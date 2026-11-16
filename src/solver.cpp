#include "solver.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "util.h"

namespace scoop {

// ---------------------------------------------------------------- exact EV vs mixed profile

void evalProfile(const BoardSet& bs, const std::vector<Mixed>& sigma, std::vector<SplitStats>& out, int threads,
                 int maxBoards) {
    const int N = bs.N;
    const int B = maxBoards > 0 ? std::min(maxBoards, bs.B) : bs.B;
    int supN[kMaxPlayers] = {};
    int sup[kMaxPlayers][kSplits];
    double sp[kMaxPlayers][kSplits];
    for (int j = 0; j < N; j++)
        for (int s = 0; s < kSplits; s++)
            if (sigma[j][s] > 0) {
                sup[j][supN[j]] = s;
                sp[j][supN[j]] = sigma[j][s];
                supN[j]++;
            }

    constexpr int kChunk = 512;
    const int chunks = (B + kChunk - 1) / kChunk;
    const int T = std::max(1, std::min(threads, chunks));
    std::vector<std::vector<SplitStats>> acc(T, std::vector<SplitStats>(N));

    parallelFor(chunks, T, [&](int ci, int t) {
        auto& A = acc[t];
        // F[j][k][s] = P(player j's mixed split is beaten on both hands by player k's split s)
        double F[kMaxPlayers][kMaxPlayers][kSplits];
        const uint32_t* V[kMaxPlayers];
        const int lo = ci * kChunk, hi = std::min(B, lo + kChunk);
        for (int b = lo; b < hi; b++) {
            for (int j = 0; j < N; j++) V[j] = bs.at(b, j);
            for (int k = 0; k < N; k++)
                for (int j = 0; j < N; j++) {
                    if (j == k) continue;
                    double* f = F[j][k];
                    const uint32_t* vk = V[k];
                    for (int s = 0; s < kSplits; s++) f[s] = 0;
                    for (int n = 0; n < supN[j]; n++) {
                        const uint32_t o = V[j][sup[j][n]];
                        const double w = sp[j][n];
                        for (int s = 0; s < kSplits; s++) f[s] += beatsBoth(vk[s], o) ? w : 0.0;
                    }
                }
            for (int k = 0; k < N; k++)
                for (int s = 0; s < kSplits; s++) {
                    double p = 1;
                    for (int j = 0; j < N && p != 0; j++)
                        if (j != k) p *= F[j][k][s];
                    A[k].scoop[s] += p;
                }
            for (int i = 0; i < N; i++)
                for (int k = 0; k < N; k++) {
                    if (k == i) continue;
                    for (int n = 0; n < supN[k]; n++) {
                        const int u = sup[k][n];
                        double base = sp[k][n];
                        for (int j = 0; j < N && base != 0; j++)
                            if (j != i && j != k) base *= F[j][k][u];
                        if (base == 0) continue;
                        const uint32_t a = V[k][u];
                        for (int s = 0; s < kSplits; s++) A[i].lose[s] += beatsBoth(a, V[i][s]) ? base : 0.0;
                    }
                }
        }
    });

    out.assign(N, SplitStats{});
    const double inv = 1.0 / B;
    for (int i = 0; i < N; i++)
        for (int s = 0; s < kSplits; s++) {
            double sc = 0, lo = 0;
            for (int t = 0; t < T; t++) {
                sc += acc[t][i].scoop[s];
                lo += acc[t][i].lose[s];
            }
            out[i].scoop[s] = sc * inv;
            out[i].lose[s] = lo * inv;
        }
}

// ---------------------------------------------------------------- payoff tensor

Tensor buildTensor(const BoardSet& bs, const std::vector<std::vector<int>>& sup, int threads) {
    Tensor T;
    buildTensor(bs, sup, threads, T);
    return T;
}

void buildTensor(const BoardSet& bs, const std::vector<std::vector<int>>& sup, int threads, Tensor& T) {
    const int N = bs.N;
    T.N = N;
    T.sup = sup;
    T.stride.assign(N, 1);
    for (int i = N - 2; i >= 0; i--) T.stride[i] = T.stride[i + 1] * sup[i + 1].size();
    T.P = T.stride[0] * sup[0].size();

    // cnt[k] is laid out as [k's local split][profile of the others], so each task
    // (k, ui) owns a contiguous block and threads never share cache lines.
    // Worker lambdas must see this thread's buffer, so bind a plain reference to it.
    thread_local std::vector<std::vector<uint32_t>> cntStore;
    auto& cnt = cntStore;
    cnt.resize(N);
    size_t restStride[kMaxPlayers][kMaxPlayers] = {};
    size_t restSize[kMaxPlayers];
    for (int k = 0; k < N; k++) {
        size_t s = 1;
        for (int j = N - 1; j >= 0; j--)
            if (j != k) {
                restStride[k][j] = s;
                s *= sup[j].size();
            }
        restSize[k] = s;
        cnt[k].assign(T.P, 0);
    }

    std::vector<std::pair<int, int>> tasks;
    for (int k = 0; k < N; k++)
        for (int ui = 0; ui < (int)sup[k].size(); ui++) tasks.emplace_back(k, ui);

    parallelFor((int)tasks.size(), threads, [&](int ti, int) {
        const int k = tasks[ti].first, ui = tasks[ti].second, u = sup[k][ui];
        int opp[kMaxPlayers], m = 0;
        for (int j = 0; j < N; j++)
            if (j != k) opp[m++] = j;
        uint32_t* block = cnt[k].data() + size_t(ui) * restSize[k];
        size_t offs[kMaxPlayers][kSplits];
        int no[kMaxPlayers];
        auto expand = [&](auto&& self, int d, size_t idx) -> void {
            if (d == m - 1) {
                for (int t = 0; t < no[d]; t++) block[idx + offs[d][t]]++;
                return;
            }
            for (int t = 0; t < no[d]; t++) self(self, d + 1, idx + offs[d][t]);
        };
        for (int b = 0; b < bs.B; b++) {
            const uint32_t a = bs.at(b, k)[u];
            bool any = true;
            for (int d = 0; d < m && any; d++) {
                const int j = opp[d];
                const uint32_t* vj = bs.at(b, j);
                no[d] = 0;
                for (int t = 0; t < (int)sup[j].size(); t++) {
                    offs[d][no[d]] = size_t(t) * restStride[k][j];
                    no[d] += beatsBoth(a, vj[sup[j][t]]);
                }
                any = no[d] > 0;
            }
            if (any) expand(expand, 0, 0);
        }
    });

    T.pay.assign(T.P * N, 0.f);
    const double inv = 1.0 / bs.B;
    int idx[kMaxPlayers] = {};
    for (size_t p = 0; p < T.P; p++) {
        uint32_t c[kMaxPlayers], tot = 0;
        for (int k = 0; k < N; k++) {
            size_t r = 0;
            for (int j = 0; j < N; j++)
                if (j != k) r += idx[j] * restStride[k][j];
            c[k] = cnt[k][size_t(idx[k]) * restSize[k] + r];
            tot += c[k];
        }
        for (int i = 0; i < N; i++) T.pay[p * N + i] = float((double(N) * c[i] - tot) * inv);
        for (int d = N - 1; d >= 0; d--) {  // odometer, last player fastest (matches stride)
            if (++idx[d] < (int)sup[d].size()) break;
            idx[d] = 0;
        }
    }
}

// ---------------------------------------------------------------- regret matching+

namespace {

using Strat = std::vector<std::vector<double>>;

// ev[i][a] = expected payoff of player i playing local split a vs the others' sig.
// Profiles where two or more players have zero weight contribute nothing and are skipped.
void tensorEV(const Tensor& T, const Strat& sig, Strat& ev) {
    const int N = T.N;
    ev.resize(N);
    for (int i = 0; i < N; i++) ev[i].assign(T.sup[i].size(), 0.0);
    int idx[kMaxPlayers];
    auto rec = [&](auto&& self, int d, size_t base, int zeros, int zp) -> void {
        if (d == N) {
            const float* p = &T.pay[base * N];
            if (zeros == 0) {
                double all = 1;
                for (int j = 0; j < N; j++) all *= sig[j][idx[j]];
                for (int i = 0; i < N; i++) ev[i][idx[i]] += all / sig[i][idx[i]] * p[i];
            } else {
                double w = 1;
                for (int j = 0; j < N; j++)
                    if (j != zp) w *= sig[j][idx[j]];
                ev[zp][idx[zp]] += w * p[zp];
            }
            return;
        }
        const auto& sd = sig[d];
        for (int a = 0; a < (int)sd.size(); a++) {
            const bool zero = sd[a] == 0;
            if (zeros + zero >= 2) continue;
            idx[d] = a;
            self(self, d + 1, base + a * T.stride[d], zeros + zero, zero ? d : zp);
        }
    };
    rec(rec, 0, 0, 0, -1);
}

double nashConvTensor(const Tensor& T, const Strat& sig) {
    Strat ev;
    tensorEV(T, sig, ev);
    double conv = 0;
    for (int i = 0; i < T.N; i++) {
        double v = 0, best = -1e300;
        for (size_t a = 0; a < ev[i].size(); a++) {
            v += sig[i][a] * ev[i][a];
            best = std::max(best, ev[i][a]);
        }
        conv += best - v;
    }
    return conv;
}

void normaliseInto(const std::vector<double>& w, std::vector<double>& out) {
    double s = 0;
    for (double x : w) s += x;
    out.resize(w.size());
    for (size_t a = 0; a < w.size(); a++) out[a] = s > 0 ? w[a] / s : 1.0 / w.size();
}

Strat solveRM(const Tensor& T, int iters, double tol, int& used) {
    const int N = T.N;
    Strat R(N), sig(N), avg(N), ev, tmp(N), best;
    double bestConv = 1e300;
    bool trivial = true;
    for (int i = 0; i < N; i++) {
        const size_t n = T.sup[i].size();
        R[i].assign(n, 0.0);
        avg[i].assign(n, 0.0);
        sig[i].assign(n, 1.0 / n);
        trivial &= n == 1;
    }
    used = 0;
    if (trivial) return sig;
    for (int t = 1; t <= iters; t++) {
        used = t;
        tensorEV(T, sig, ev);
        for (int i = 0; i < N; i++) {
            double v = 0;
            for (size_t a = 0; a < ev[i].size(); a++) v += sig[i][a] * ev[i][a];
            for (size_t a = 0; a < ev[i].size(); a++) {
                R[i][a] = std::max(0.0, R[i][a] + ev[i][a] - v);
                avg[i][a] += t * sig[i][a];
            }
        }
        for (int i = 0; i < N; i++) normaliseInto(R[i], sig[i]);
        if (t % 50 == 0 || t == iters) {
            for (int i = 0; i < N; i++) normaliseInto(avg[i], tmp[i]);
            for (const Strat* cand : {&tmp, &sig}) {
                const double c = nashConvTensor(T, *cand);
                if (c < bestConv) {
                    bestConv = c;
                    best = *cand;
                }
            }
            if (bestConv < tol) break;
        }
    }
    return best;
}

// Marks every pure profile that is a Nash equilibrium of the tensor game.
void findPureNE(const Tensor& T, int maxKeep, long long& count, std::vector<std::vector<int>>& keep) {
    const int N = T.N;
    std::vector<uint8_t> ok(T.P, 0);
    for (int i = 0; i < N; i++) {
        const size_t st = T.stride[i], n = T.sup[i].size();
        for (size_t p = 0; p < T.P; p++) {
            if ((p / st) % n != 0) continue;
            float best = -1e30f;
            for (size_t a = 0; a < n; a++) best = std::max(best, T.pay[(p + a * st) * N + i]);
            for (size_t a = 0; a < n; a++)
                if (T.pay[(p + a * st) * N + i] >= best - 1e-7f) ok[p + a * st]++;
        }
    }
    count = 0;
    keep.clear();
    for (size_t p = 0; p < T.P; p++) {
        if (ok[p] != N) continue;
        count++;
        if ((int)keep.size() < maxKeep) {
            std::vector<int> prof(N);
            for (int i = 0; i < N; i++) prof[i] = T.sup[i][(p / T.stride[i]) % T.sup[i].size()];
            keep.push_back(prof);
        }
    }
}

}  // namespace

// ---------------------------------------------------------------- top-level solve

SolveResult solve(const BoardSet& bs, const SolveOptions& opt) {
    const int N = bs.N;
    SolveResult res;
    std::vector<std::vector<int>> sup(N);
    std::vector<Mixed> sigma(N);

    if (opt.fullTensor) {
        for (int i = 0; i < N; i++)
            for (int s = 0; s < kSplits; s++) sup[i].push_back(s);
    } else {
        // Seed the oracle with each player's best response to uniformly random splits,
        // estimated on a board subset: it is only a starting point for the oracle.
        for (auto& m : sigma) m.fill(1.0 / kSplits);
        evalProfile(bs, sigma, res.stats, opt.threads, 256);
        for (int i = 0; i < N; i++) {
            int best = 0;
            for (int s = 1; s < kSplits; s++)
                if (res.stats[i].ev(s, N) > res.stats[i].ev(best, N)) best = s;
            sup[i].push_back(best);
        }
    }

    for (int it = 0; it < std::max(1, opt.maxOracleIters); it++) {
        res.oracleIters = it + 1;
        thread_local Tensor tensorStore;
        Tensor& T = tensorStore;
        buildTensor(bs, sup, opt.threads, T);
        res.profilesEvaluated += T.P;
        int iters = opt.rmIters;
        if (opt.rmWorkBudget > 0)
            iters = int(std::clamp(opt.rmWorkBudget / double(T.P), 100.0, double(opt.rmIters)));
        int used = 0;
        const Strat local = solveRM(T, iters, opt.rmTol, used);
        res.rmItersUsed += used;

        for (int i = 0; i < N; i++) {
            sigma[i].fill(0.0);
            double s = 0;
            for (size_t a = 0; a < sup[i].size(); a++) {
                const double p = local[i][a] < 1e-4 ? 0.0 : local[i][a];  // drop RM dust
                sigma[i][sup[i][a]] = p;
                s += p;
            }
            for (auto& p : sigma[i]) p /= s;
        }
        evalProfile(bs, sigma, res.stats, opt.threads);

        if (opt.fullTensor) {
            findPureNE(T, opt.maxPureNE, res.pureNECount, res.pureNE);
            break;
        }
        bool added = false;
        for (int i = 0; i < N; i++) {
            double v = 0;
            int best = 0;
            for (int s = 0; s < kSplits; s++) {
                v += sigma[i][s] * res.stats[i].ev(s, N);
                if (res.stats[i].ev(s, N) > res.stats[i].ev(best, N)) best = s;
            }
            if (res.stats[i].ev(best, N) > v + 1e-9 &&
                std::find(sup[i].begin(), sup[i].end(), best) == sup[i].end()) {
                sup[i].push_back(best);
                added = true;
            }
        }
        if (!added) break;
    }

    res.sigma = sigma;
    res.value.assign(N, 0.0);
    res.nashConv = 0;
    for (int i = 0; i < N; i++) {
        double best = -1e300;
        for (int s = 0; s < kSplits; s++) {
            res.value[i] += sigma[i][s] * res.stats[i].ev(s, N);
            best = std::max(best, res.stats[i].ev(s, N));
        }
        res.nashConv += best - res.value[i];
    }
    return res;
}

}  // namespace scoop
