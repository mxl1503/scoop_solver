// Scoop poker split solver - command line front end.
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <numeric>
#include <string>
#include <vector>

#include "catalog.h"
#include "engine.h"
#include "evaluator.h"
#include "solver.h"
#include "util.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace scoop;

namespace {

// Windows 11 "EcoQoS" throttles long-running processes that are not in the foreground
// (e.g. launched from a background terminal) onto efficiency cores after a few seconds,
// cutting throughput ~3x. Opt out: this is a short, deliberately CPU-bound job.
void disablePowerThrottling() {
#ifdef _WIN32
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    s.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof s);
#endif
}

// ================================================================ CLI

struct Args {
    std::string mode;
    std::vector<std::string> positional;
    int players = 4;
    int boards = -1;
    int deals = -1;
    uint64_t seed = 1;
    int threads = defaultThreads();
    int top = kSplits;
    bool exact = false;
    std::string solver = "auto";
    std::string hands;
    int rmIters = -1;
    double rmTol = -1;
    int limit = 0;
    std::string out = "data/sixmax";
    std::string only;

    // Per-deal solver settings; hand/stats solve hundreds of deals, so they default to
    // a looser tolerance that is still well below the Monte Carlo noise of one deal.
    SolveOptions solveOptions(bool manyDeals) const {
        SolveOptions o;
        if (manyDeals) {
            o.rmIters = 1500;
            o.rmTol = 2e-4;
            o.rmWorkBudget = 3e6;
        }
        if (rmIters > 0) o.rmIters = rmIters;
        if (rmTol > 0) o.rmTol = rmTol;
        return o;
    }
};

void usage() {
    std::printf(
        "Scoop poker split solver\n\n"
        "usage:\n"
        "  scoop hand <6 cards> [options]   rank all 15 splits of a hand vs random opponents\n"
        "  scoop deal [options]             solve one full deal (all hands known)\n"
        "  scoop stats [options]            aggregate split-shape preferences over random deals\n"
        "  scoop catalog [options]          solve every suit-canonical 6-card hand at 6 players\n"
        "  scoop selftest                   verify evaluators and solver\n"
        "  scoop bench                      evaluator / engine throughput\n\n"
        "options:\n"
        "  -n, --players N    players at the table, 2..6 (default 4)\n"
        "  --boards B         Monte Carlo boards per deal (deal 20000, hand 2000, stats 1500)\n"
        "  --exact            enumerate every board instead of sampling\n"
        "  --deals D          random deals to sample (hand 1000, stats 2000)\n"
        "  --hands LIST       deal mode: comma-separated hands, '?' = random, e.g. \"AsAhKd7c6c2s,?,?,?\"\n"
        "  --solver S         deal mode: full (all 15^N profiles) | oracle (double oracle) | auto\n"
        "  --top K            rows to print per split table (default 15)\n"
        "  --seed S           RNG seed (default 1)\n"
        "  --threads T        worker threads (default: all cores)\n"
        "  --rm-iters I       regret-matching+ iteration cap per restricted game\n"
        "                     (default 4000 for deal, 1500 for hand/stats, 250 for catalog)\n"
        "  --rm-tol X         stop regret matching below this NashConv in BB\n"
        "                     (default 2e-5 for deal, 2e-4 for hand/stats, 0.004 for catalog)\n"
        "  --out DIR          catalog output directory (default data/sixmax)\n"
        "  --limit N          catalog: solve at most N new hands this run (0 = all)\n"
        "  --only CARDS       catalog: print one 6-card hand and do not write the dataset\n\n"
        "cards: rank 23456789TJQKA + suit cdhs, e.g. AsKd7h7c2s3d\n"
        "catalog writes hands.bin + summary.json. View them with: python -m http.server 8765\n"
        "then open http://localhost:8765/web/\n");
}

Args parseArgs(int argc, char** argv) {
    Args a;
    if (argc < 2) return a;
    a.mode = argv[1];
    for (int i = 2; i < argc; i++) {
        std::string s = argv[i];
        auto need = [&](const char* what) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + what);
            return argv[++i];
        };
        if (s == "-n" || s == "--players") a.players = std::stoi(need("--players"));
        else if (s == "--boards") a.boards = std::stoi(need("--boards"));
        else if (s == "--deals") a.deals = std::stoi(need("--deals"));
        else if (s == "--seed") a.seed = std::stoull(need("--seed"));
        else if (s == "--threads") a.threads = std::max(1, std::stoi(need("--threads")));
        else if (s == "--top") a.top = std::clamp(std::stoi(need("--top")), 1, kSplits);
        else if (s == "--exact") a.exact = true;
        else if (s == "--solver") a.solver = need("--solver");
        else if (s == "--hands") a.hands = need("--hands");
        else if (s == "--rm-iters") a.rmIters = std::stoi(need("--rm-iters"));
        else if (s == "--rm-tol") a.rmTol = std::stod(need("--rm-tol"));
        else if (s == "--out") a.out = need("--out");
        else if (s == "--limit") a.limit = std::max(0, std::stoi(need("--limit")));
        else if (s == "--only") a.only = need("--only");
        else if (s == "-h" || s == "--help") a.mode = "help";
        else if (!s.empty() && s[0] == '-') throw std::runtime_error("unknown option " + s);
        else a.positional.push_back(s);
    }
    if (a.players < 2 || a.players > kMaxPlayers) throw std::runtime_error("--players must be 2..6");
    return a;
}

struct Timer {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    double sec() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
};

Hand toHand(const std::vector<int>& cards) {
    if (cards.size() != 6) throw std::runtime_error("a hand needs exactly 6 cards");
    Hand h;
    std::copy(cards.begin(), cards.end(), h.begin());
    return sortedHand(h);
}

// Fills hands[i] == {-1...} slots with random cards not already used.
void dealRandom(std::vector<Hand>& hands, Rng& rng) {
    bool used[52] = {};
    for (auto& h : hands)
        for (int c : h)
            if (c >= 0) {
                if (used[c]) throw std::runtime_error("duplicate card " + cardStr(c));
                used[c] = true;
            }
    std::vector<int> deck;
    for (int c = 0; c < 52; c++)
        if (!used[c]) deck.push_back(c);
    int top = 0;
    for (auto& h : hands) {
        if (h[0] >= 0) continue;
        for (int k = 0; k < 6; k++) {
            const int j = top + int(rng.below(uint32_t(deck.size() - top)));
            std::swap(deck[top], deck[j]);
            h[k] = deck[top++];
        }
        h = sortedHand(h);
    }
}

std::string pct(double x) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%5.1f%%", 100.0 * x);
    return buf;
}

// ================================================================ split shape features

int rankCounts(const int* cards, int n, int* cnt) {
    std::fill(cnt, cnt + 13, 0);
    for (int i = 0; i < n; i++) cnt[rankOf(cards[i])]++;
    return 0;
}

std::string signature(std::vector<int> v) {
    std::sort(v.rbegin(), v.rend());
    std::string s;
    for (int x : v)
        if (x) s += (s.empty() ? "" : "-") + std::to_string(x);
    return s;
}

std::string pairClass(const Hand& h) {
    int cnt[13];
    rankCounts(h.data(), 6, cnt);
    const std::string sig = signature(std::vector<int>(cnt, cnt + 13));
    static const std::map<std::string, std::string> names = {
        {"1-1-1-1-1-1", "no pair"}, {"2-1-1-1-1", "one pair"}, {"2-2-1-1", "two pair"},
        {"2-2-2", "three pair"},    {"3-1-1-1", "trips"},      {"3-2-1", "trips + pair"},
        {"3-3", "two trips"},       {"4-1-1", "quads"},        {"4-2", "quads + pair"}};
    auto it = names.find(sig);
    return it == names.end() ? sig : it->second;
}

std::string suitPattern(const Hand& h) {
    std::vector<int> c(4, 0);
    for (int x : h) c[suitOf(x)]++;
    return signature(c);
}

void splitCards(const Hand& h, int q, int he[2], int plo[4]) {
    he[0] = h[kPairA[q]];
    he[1] = h[kPairB[q]];
    int n = 0;
    for (int i = 0; i < 6; i++)
        if (i != kPairA[q] && i != kPairB[q]) plo[n++] = h[i];
}

std::string heShape(const int he[2]) {
    if (rankOf(he[0]) == rankOf(he[1])) return "pocket pair";
    return suitOf(he[0]) == suitOf(he[1]) ? "suited" : "offsuit";
}

std::string heClass(const int he[2]) {
    int r1 = rankOf(he[0]), r2 = rankOf(he[1]);
    if (r1 < r2) std::swap(r1, r2);
    if (r1 == r2) {
        if (r1 >= 9) return std::string(2, kRankChars[r1]);  // AA KK QQ JJ
        return r1 >= 6 ? "TT-88" : "77-22";
    }
    const char* sfx = suitOf(he[0]) == suitOf(he[1]) ? " suited" : " offsuit";
    if (r2 >= 8) return std::string("two broadway") + sfx;
    if (r1 == 12) return std::string("Ax") + sfx;
    if (r1 - r2 <= 2) return std::string("connector/gapper") + sfx;
    return std::string("other") + sfx;
}

std::string ploSuit(const int plo[4]) {
    std::vector<int> c(4, 0);
    for (int i = 0; i < 4; i++) c[suitOf(plo[i])]++;
    const std::string s = signature(c);
    if (s == "2-2") return "double-suited";
    if (s == "2-1-1") return "single-suited";
    if (s == "1-1-1-1") return "rainbow";
    if (s == "3-1") return "three-flush";
    return "monotone";
}

std::string ploPairing(const int plo[4]) {
    int cnt[13];
    rankCounts(plo, 4, cnt);
    const std::string s = signature(std::vector<int>(cnt, cnt + 13));
    if (s == "1-1-1-1") return "unpaired";
    if (s == "2-1-1") return "one pair";
    if (s == "2-2") return "two pair";
    return "trips+";
}

// Where the hand's highest-ranked pair (or better) went: "HE", "PLO" or "split".
std::string topPairPlacement(const Hand& h, int q) {
    int cnt[13];
    rankCounts(h.data(), 6, cnt);
    int r = -1;
    for (int x = 12; x >= 0 && r < 0; x--)
        if (cnt[x] >= 2) r = x;
    if (r < 0) return "";
    int he[2], plo[4];
    splitCards(h, q, he, plo);
    const int inHe = (rankOf(he[0]) == r) + (rankOf(he[1]) == r);
    if (inHe == 2) return "HE";
    return inHe == 0 ? "PLO" : "split";
}

// Naive heuristic: best Hold'em starting hand (highest pocket pair, else two highest cards).
int maxHoldemSplit(const Hand& h) {
    for (int p = 0; p < kSplits; p++)  // hand is sorted high->low so the first pair found is highest
        if (rankOf(h[kPairA[p]]) == rankOf(h[kPairB[p]])) return p;
    return 0;
}

// ================================================================ reporting

struct Row {
    int split;
    double ev, se, scoop, chop, lose, eq;
};

void printSplitTable(const Hand& h, std::vector<Row> rows, int top, bool withSE) {
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ev > b.ev; });
    std::printf("  %-4s %-24s %-28s %9s%s %8s %8s %8s %8s\n", "rank", "split  HE | PLO", "shape", "EV(BB)",
                withSE ? "     +/-" : "", "scoop", "chop", "lose", "eq-freq");
    for (int r = 0; r < std::min(top, (int)rows.size()); r++) {
        const Row& x = rows[r];
        int he[2], plo[4];
        splitCards(h, x.split, he, plo);
        const std::string shape = heShape(he) + " / " + ploSuit(plo);
        char se[16] = "";
        if (withSE) std::snprintf(se, sizeof se, "  %6.4f", x.se);
        std::printf("  %-4d %-24s %-28s %+9.4f%s %8s %8s %8s %8s\n", r + 1, splitStr(h, x.split).c_str(),
                    shape.c_str(), x.ev, se, pct(x.scoop).c_str(), pct(x.chop).c_str(), pct(x.lose).c_str(),
                    pct(x.eq).c_str());
    }
}

std::string strategyStr(const Hand& h, const Mixed& m) {
    std::vector<std::pair<double, int>> v;
    for (int s = 0; s < kSplits; s++)
        if (m[s] > 0) v.emplace_back(m[s], s);
    std::sort(v.rbegin(), v.rend());
    std::string out;
    for (auto& [p, s] : v) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%s%s (%.1f%%)", out.empty() ? "" : ", ", splitStr(h, s).c_str(), 100 * p);
        out += buf;
    }
    return out;
}

int argmax(const Mixed& m) { return int(std::max_element(m.begin(), m.end()) - m.begin()); }

// ================================================================ mode: deal

int runDeal(const Args& a) {
    const int N = a.players;
    std::vector<Hand> hands(N);
    for (auto& h : hands) h.fill(-1);
    if (!a.hands.empty()) {
        std::vector<std::string> parts;
        std::string cur;
        for (char ch : a.hands + ",") {
            if (ch == ',' || ch == ';' || ch == '/') {
                parts.push_back(cur);
                cur.clear();
            } else if (ch != ' ') {
                cur += ch;
            }
        }
        if ((int)parts.size() > N) throw std::runtime_error("more hands given than --players");
        for (size_t i = 0; i < parts.size(); i++)
            if (!parts[i].empty() && parts[i] != "?" && parts[i] != "random") hands[i] = toHand(parseCards(parts[i]));
    }
    Rng rng(mixSeed(a.seed, 0xDEA1));
    dealRandom(hands, rng);

    const uint64_t total = choose(52 - 6 * N, 5);
    const int B = a.boards > 0 ? a.boards : 20000;
    const bool exact = a.exact || total <= uint64_t(B);
    const bool full = a.solver == "full" || (a.solver == "auto" && N <= 4);
    if (a.solver != "full" && a.solver != "oracle" && a.solver != "auto")
        throw std::runtime_error("--solver must be full, oracle or auto");
    if (full && N == 6)
        std::fprintf(stderr, "warning: the full 15^6 game has 11.4M profiles (~550 MB, slow); "
                             "--solver oracle is recommended for 6 players\n");

    Timer tb;
    const BoardSet bs = buildBoards(hands, B, exact, mixSeed(a.seed, 0xB0A2D), a.threads);
    const double tBoards = tb.sec();

    SolveOptions opt = a.solveOptions(false);
    opt.fullTensor = full;
    opt.threads = a.threads;
    Timer ts;
    const SolveResult res = solve(bs, opt);
    const double tSolve = ts.sec();

    std::printf("Scoop solver - %d players, ante 1 BB each, pot %d BB\n", N, N);
    std::printf("Boards: %d %s (of %llu possible), shared by all players and both hands   [%.2fs]\n", bs.B,
                exact ? "enumerated exactly" : "sampled", (unsigned long long)total, tBoards);
    if (full)
        std::printf("Solver: full game, %zu split profiles (15^%d) x %d boards, regret-matching+ %d iters   [%.2fs]\n",
                    res.profilesEvaluated, N, bs.B, res.rmItersUsed, tSolve);
    else
        std::printf("Solver: double oracle, %d iterations, %zu restricted profiles, regret-matching+ %d iters   [%.2fs]\n",
                    res.oracleIters, res.profilesEvaluated, res.rmItersUsed, tSolve);
    if (res.pureNECount >= 0) std::printf("Pure-strategy Nash equilibria found: %lld\n", res.pureNECount);
    std::printf("Equilibrium exploitability (NashConv): %.5f BB\n", res.nashConv);

    double pScoop = 0;
    for (int i = 0; i < N; i++)
        for (int s = 0; s < kSplits; s++) pScoop += res.sigma[i][s] * res.stats[i].scoop[s];
    std::printf("At equilibrium: someone scoops %.1f%% of boards, antes returned %.1f%%\n", 100 * pScoop,
                100 * (1 - pScoop));

    for (int i = 0; i < N; i++) {
        std::printf("\nPlayer %d: %s   (%s, suits %s)\n", i + 1, handStr(hands[i]).c_str(), pairClass(hands[i]).c_str(),
                    suitPattern(hands[i]).c_str());
        std::printf("  equilibrium split: %s\n", strategyStr(hands[i], res.sigma[i]).c_str());
        std::printf("  equilibrium EV: %+.4f BB\n", res.value[i]);
        std::vector<Row> rows;
        for (int s = 0; s < kSplits; s++)
            rows.push_back({s, res.stats[i].ev(s, N), 0, res.stats[i].scoop[s], res.stats[i].chop(s),
                            res.stats[i].lose[s], res.sigma[i][s]});
        printSplitTable(hands[i], rows, a.top, false);
    }

    // Joint vs independent: why the scoop must be measured on one shared board.
    std::vector<int> prof(N);
    for (int i = 0; i < N; i++) prof[i] = argmax(res.sigma[i]);
    std::vector<double> winHE(N), winPLO(N), sc(N);
    for (int b = 0; b < bs.B; b++) {
        for (int i = 0; i < N; i++) {
            const uint32_t me = bs.at(b, i)[prof[i]];
            bool he = true, plo = true;
            for (int j = 0; j < N; j++) {
                if (j == i) continue;
                const uint32_t o = bs.at(b, j)[prof[j]];
                he &= heOf(me) > heOf(o);
                plo &= ploOf(me) > ploOf(o);
            }
            winHE[i] += he;
            winPLO[i] += plo;
            sc[i] += he && plo;
        }
    }
    std::printf("\nJoint scoop vs independence assumption (equilibrium splits, outright wins only):\n");
    std::printf("  %-7s %-24s %9s %9s %14s %12s\n", "player", "split", "win HE", "win PLO", "HE x PLO", "true scoop");
    for (int i = 0; i < N; i++)
        std::printf("  %-7d %-24s %9s %9s %14s %12s\n", i + 1, splitStr(hands[i], prof[i]).c_str(),
                    pct(winHE[i] / bs.B).c_str(), pct(winPLO[i] / bs.B).c_str(),
                    pct(winHE[i] * winPLO[i] / (double(bs.B) * bs.B)).c_str(), pct(sc[i] / bs.B).c_str());
    return 0;
}

// ================================================================ mode: hand

int runHand(const Args& a) {
    if (a.positional.empty()) throw std::runtime_error("hand mode needs 6 cards, e.g. scoop hand AsKs7h7d4c2c");
    std::string text;
    for (auto& p : a.positional) text += p;
    const Hand hero = toHand(parseCards(text));
    const int N = a.players;
    const int D = a.deals > 0 ? a.deals : 1000;
    const int B = a.boards > 0 ? a.boards : 2000;

    struct Acc {
        double ev[kSplits] = {}, ev2[kSplits] = {}, scoop[kSplits] = {}, lose[kSplits] = {}, eq[kSplits] = {};
        double eqValue = 0, conv = 0, oracleIters = 0, rmIters = 0, profiles = 0;
        int pure = 0;
    };
    std::vector<Acc> acc(a.threads);
    Timer t;
    parallelFor(D, a.threads, [&](int d, int th) {
        Rng rng(mixSeed(a.seed, uint64_t(d)));
        std::vector<Hand> hands(N);
        hands[0] = hero;
        for (int i = 1; i < N; i++) hands[i].fill(-1);
        dealRandom(hands, rng);
        thread_local BoardSet bs;
        buildBoards(hands, B, a.exact, rng.next(), 1, bs);
        const SolveResult res = solve(bs, a.solveOptions(true));
        Acc& A = acc[th];
        const SplitStats& st = res.stats[0];
        bool pure = true;
        for (int s = 0; s < kSplits; s++) {
            const double ev = st.ev(s, N);
            A.ev[s] += ev;
            A.ev2[s] += ev * ev;
            A.scoop[s] += st.scoop[s];
            A.lose[s] += st.lose[s];
            A.eq[s] += res.sigma[0][s];
        }
        for (int i = 0; i < N; i++) pure &= *std::max_element(res.sigma[i].begin(), res.sigma[i].end()) > 0.999;
        A.pure += pure;
        A.eqValue += res.value[0];
        A.conv += res.nashConv;
        A.oracleIters += res.oracleIters;
        A.rmIters += res.rmItersUsed;
        A.profiles += double(res.profilesEvaluated);
    });
    Acc tot;
    for (auto& x : acc) {
        for (int s = 0; s < kSplits; s++) {
            tot.ev[s] += x.ev[s];
            tot.ev2[s] += x.ev2[s];
            tot.scoop[s] += x.scoop[s];
            tot.lose[s] += x.lose[s];
            tot.eq[s] += x.eq[s];
        }
        tot.eqValue += x.eqValue;
        tot.conv += x.conv;
        tot.pure += x.pure;
        tot.oracleIters += x.oracleIters;
        tot.rmIters += x.rmIters;
        tot.profiles += x.profiles;
    }
    std::vector<Row> rows;
    for (int s = 0; s < kSplits; s++) {
        const double m = tot.ev[s] / D, var = std::max(0.0, tot.ev2[s] / D - m * m);
        const double sc = tot.scoop[s] / D, lo = tot.lose[s] / D;
        rows.push_back({s, m, std::sqrt(var / D), sc, 1 - sc - lo, lo, tot.eq[s] / D});
    }

    std::printf("Scoop solver - hand analysis\n");
    std::printf("Hand: %s   (%s, suits %s)   players: %d   pot: %d BB\n", handStr(hero).c_str(),
                pairClass(hero).c_str(), suitPattern(hero).c_str(), N, N);
    std::printf("Opponent deals: %d random, %d %s boards each (%.0fM Hold'em+PLO split outcomes)   [%.2fs, %d threads]\n",
                D, B, a.exact ? "enumerated" : "sampled", double(D) * B * N * kSplits / 1e6, t.sec(), a.threads);
    std::printf("Per deal every player picks a Nash-equilibrium split (simultaneous moves, perfect play);\n");
    std::printf("each row is this hand's EV for committing to that split vs. the opponents' equilibrium splits.\n");
    std::printf("Pure equilibrium in %.0f%% of deals, mean NashConv %.5f BB, hero equilibrium EV %+.4f BB\n",
                100.0 * tot.pure / D, tot.conv / D, tot.eqValue / D);
    std::printf("Solver per deal: %.1f double-oracle rounds, %.0f restricted profiles, %.0f regret-matching iters\n\n",
                tot.oracleIters / D, tot.profiles / D, tot.rmIters / D);
    printSplitTable(hero, rows, a.top, true);
    std::printf("\n  EV is net of the 1 BB ante: scoop pays +%d, an opponent scoop costs -1, chops return the ante.\n",
                N - 1);
    std::printf("  eq-freq = share of deals in which the solver's equilibrium uses that split.\n");
    return 0;
}

// ================================================================ mode: stats

struct Record {
    Hand h;
    int chosen;
    double value, maxEv, heurEv, meanEv, chop;
    bool pure;
};

struct Agg {
    int n = 0;
    double ev = 0;
    std::map<std::string, int> tally;
    void add(double e, std::initializer_list<std::string> keys) {
        n++;
        ev += e;
        for (auto& k : keys)
            if (!k.empty()) tally[k]++;
    }
    double frac(const std::string& k) const {
        auto it = tally.find(k);
        return n && it != tally.end() ? double(it->second) / n : 0.0;
    }
};

int runStats(const Args& a) {
    const int N = a.players;
    const int D = a.deals > 0 ? a.deals : 2000;
    const int B = a.boards > 0 ? a.boards : 1500;
    std::vector<std::vector<Record>> per(a.threads);
    std::vector<double> convSum(a.threads, 0);
    Timer t;
    parallelFor(D, a.threads, [&](int d, int th) {
        Rng rng(mixSeed(a.seed, 0x57A75000ull + d));
        std::vector<Hand> hands(N);
        for (auto& h : hands) h.fill(-1);
        dealRandom(hands, rng);
        thread_local BoardSet bs;
        buildBoards(hands, B, a.exact, rng.next(), 1, bs);
        const SolveResult res = solve(bs, a.solveOptions(true));
        convSum[th] += res.nashConv;
        double pScoop = 0;
        for (int i = 0; i < N; i++)
            for (int s = 0; s < kSplits; s++) pScoop += res.sigma[i][s] * res.stats[i].scoop[s];
        for (int i = 0; i < N; i++) {
            Record r;
            r.h = hands[i];
            r.chosen = argmax(res.sigma[i]);
            r.value = res.value[i];
            r.maxEv = -1e9;
            r.meanEv = 0;
            for (int s = 0; s < kSplits; s++) {
                r.maxEv = std::max(r.maxEv, res.stats[i].ev(s, N));
                r.meanEv += res.stats[i].ev(s, N) / kSplits;
            }
            r.heurEv = res.stats[i].ev(maxHoldemSplit(hands[i]), N);
            r.chop = 1 - pScoop;
            r.pure = res.sigma[i][r.chosen] > 0.999;
            per[th].push_back(r);
        }
    });
    std::vector<Record> recs;
    for (auto& v : per) recs.insert(recs.end(), v.begin(), v.end());
    const double conv = std::accumulate(convSum.begin(), convSum.end(), 0.0) / D;

    std::printf("Scoop solver - aggregate split preferences\n");
    std::printf("%d players, %d random deals x %d boards, %zu equilibrium hand decisions   [%.2fs, %d threads]\n", N, D,
                B, recs.size(), t.sec(), a.threads);
    double chop = 0, pure = 0, lossHeur = 0, lossRand = 0;
    for (auto& r : recs) {
        chop += r.chop;
        pure += r.pure;
        lossHeur += r.maxEv - r.heurEv;
        lossRand += r.maxEv - r.meanEv;
    }
    const double n = double(recs.size());
    std::printf("Antes returned (no scoop) %.1f%% of boards | pure equilibrium splits %.1f%% | mean NashConv %.5f BB\n",
                100 * chop / n, 100 * pure / n, conv);
    std::printf("EV given up vs. the best split: 'best Hold'em hand' heuristic %.4f BB/hand, random split %.4f BB/hand\n",
                lossHeur / n, lossRand / n);

    // 1. Hold'em shape chosen vs. what was available.
    {
        std::map<std::string, Agg> chosen;
        std::map<std::string, double> avail;
        for (auto& r : recs) {
            int he[2], plo[4];
            splitCards(r.h, r.chosen, he, plo);
            chosen[heShape(he)].add(r.value, {});
            for (int s = 0; s < kSplits; s++) {
                splitCards(r.h, s, he, plo);
                avail[heShape(he)] += 1.0 / kSplits;
            }
        }
        std::printf("\n[1] Hold'em hand shape chosen\n  %-14s %9s %12s %10s\n", "shape", "chosen", "available*", "mean EV");
        for (const char* k : {"pocket pair", "suited", "offsuit"}) {
            const Agg& g = chosen[k];
            std::printf("  %-14s %9s %12s %+10.4f\n", k, pct(g.n / n).c_str(), pct(avail[k] / n).c_str(),
                        g.n ? g.ev / g.n : 0.0);
        }
        std::printf("  * share of all 15 splits per hand with that shape (what a random split would give)\n");
    }

    // 2. By pairing structure of the 6 cards.
    {
        std::map<std::string, Agg> g;
        for (auto& r : recs) {
            int he[2], plo[4];
            splitCards(r.h, r.chosen, he, plo);
            g[pairClass(r.h)].add(r.value, {"pp:" + topPairPlacement(r.h, r.chosen), "ps:" + ploSuit(plo),
                                            "he:" + heShape(he)});
        }
        std::printf("\n[2] By pairing of the 6 cards (where the highest pair goes)\n");
        std::printf("  %-13s %7s %9s %9s %10s %9s %10s %10s\n", "hand", "share", "mean EV", "pair->HE", "pair->PLO",
                    "pair split", "HE suited", "PLO ds");
        for (const char* k : {"no pair", "one pair", "two pair", "three pair", "trips", "trips + pair", "two trips",
                              "quads", "quads + pair"}) {
            auto it = g.find(k);
            if (it == g.end()) continue;
            const Agg& x = it->second;
            const bool paired = std::string(k) != "no pair";
            std::printf("  %-13s %7s %+9.4f %9s %10s %9s %10s %10s\n", k, pct(x.n / n).c_str(), x.ev / x.n,
                        paired ? pct(x.frac("pp:HE")).c_str() : "    -", paired ? pct(x.frac("pp:PLO")).c_str() : "    -",
                        paired ? pct(x.frac("pp:split")).c_str() : "    -", pct(x.frac("he:suited")).c_str(),
                        pct(x.frac("ps:double-suited")).c_str());
        }
    }

    // 3. By suit distribution of the 6 cards.
    {
        std::map<std::string, Agg> g;
        for (auto& r : recs) {
            int he[2], plo[4];
            splitCards(r.h, r.chosen, he, plo);
            g[suitPattern(r.h)].add(r.value, {"ps:" + ploSuit(plo), "he:" + heShape(he)});
        }
        std::vector<std::pair<int, std::string>> order;
        for (auto& [k, v] : g) order.emplace_back(-v.n, k);
        std::sort(order.begin(), order.end());
        std::printf("\n[3] By suit distribution of the 6 cards (PLO suit structure chosen)\n");
        std::printf("  %-9s %7s %9s %10s %9s %9s %9s %9s %9s\n", "suits", "share", "mean EV", "HE suited", "PLO ds",
                    "PLO ss", "rainbow", "3-flush", "mono");
        for (auto& [neg, k] : order) {
            const Agg& x = g[k];
            std::printf("  %-9s %7s %+9.4f %10s %9s %9s %9s %9s %9s\n", k.c_str(), pct(x.n / n).c_str(), x.ev / x.n,
                        pct(x.frac("he:suited")).c_str(), pct(x.frac("ps:double-suited")).c_str(),
                        pct(x.frac("ps:single-suited")).c_str(), pct(x.frac("ps:rainbow")).c_str(),
                        pct(x.frac("ps:three-flush")).c_str(), pct(x.frac("ps:monotone")).c_str());
        }
    }

    // 4. Hold'em hand classes chosen.
    {
        std::map<std::string, Agg> g;
        for (auto& r : recs) {
            int he[2], plo[4];
            splitCards(r.h, r.chosen, he, plo);
            g[heClass(he)].add(r.value, {});
        }
        std::vector<std::pair<int, std::string>> order;
        for (auto& [k, v] : g) order.emplace_back(-v.n, k);
        std::sort(order.begin(), order.end());
        std::printf("\n[4] Hold'em hand class chosen\n  %-26s %8s %10s\n", "class", "share", "mean EV");
        for (auto& [neg, k] : order)
            std::printf("  %-26s %8s %+10.4f\n", k.c_str(), pct(-neg / n).c_str(), g[k].ev / g[k].n);
    }

    // 5. PLO shape chosen (suits x pairing) and high-card allocation.
    {
        std::map<std::string, std::map<std::string, int>> cross;
        int topToHe = 0, top2ToHe = 0;
        for (auto& r : recs) {
            int he[2], plo[4];
            splitCards(r.h, r.chosen, he, plo);
            cross[ploSuit(plo)][ploPairing(plo)]++;
            const int hi = rankOf(r.h[0]);
            topToHe += rankOf(he[0]) == hi || rankOf(he[1]) == hi;
            top2ToHe += r.chosen == 0;
        }
        const char* pairs[] = {"unpaired", "one pair", "two pair", "trips+"};
        std::printf("\n[5] PLO hand shape chosen (share of all decisions)\n  %-15s", "suits \\ ranks");
        for (auto* p : pairs) std::printf(" %10s", p);
        std::printf(" %10s\n", "total");
        for (const char* s : {"double-suited", "single-suited", "rainbow", "three-flush", "monotone"}) {
            int rowTot = 0;
            std::printf("  %-15s", s);
            for (auto* p : pairs) {
                rowTot += cross[s][p];
                std::printf(" %10s", pct(cross[s][p] / n).c_str());
            }
            std::printf(" %10s\n", pct(rowTot / n).c_str());
        }
        std::printf("\n[6] High-card allocation\n");
        std::printf("  Hold'em hand contains the hand's top-ranked card: %s\n", pct(topToHe / n).c_str());
        std::printf("  Hold'em hand is exactly the two highest cards:    %s\n", pct(top2ToHe / n).c_str());
    }
    return 0;
}

// ================================================================ selftest / bench

bool checkEq(const char* what, long long got, long long want) {
    std::printf("  %-34s %14lld  %s\n", what, got, got == want ? "ok" : "MISMATCH");
    return got == want;
}

uint16_t bruteBest(const std::vector<int>& cards, int mustHole, int holeCount) {
    // Best 5-card hand from `cards`; if mustHole > 0 exactly mustHole cards come from the
    // first holeCount entries (Omaha rule), otherwise any 5.
    const int n = int(cards.size());
    uint16_t best = 0;
    for (int m = 0; m < (1 << n); m++) {
        if (std::popcount(unsigned(m)) != 5) continue;
        if (mustHole > 0 && std::popcount(unsigned(m & ((1 << holeCount) - 1))) != mustHole) continue;
        int rk[5], k = 0, suit = -1;
        bool fl = true;
        for (int i = 0; i < n; i++)
            if (m >> i & 1) {
                rk[k++] = rankOf(cards[i]);
                if (suit < 0) suit = suitOf(cards[i]);
                fl &= suitOf(cards[i]) == suit;
            }
        best = std::max(best, denseFromSlow(slowEval5(rk, fl)));
    }
    return best;
}

int runSelftest() {
    bool ok = true;
    Timer t;
    std::printf("5-card evaluator, all C(52,5) hands:\n");
    {
        long long cat[9] = {};
        std::vector<char> seen(7463, 0);
        for (int a = 0; a < 52; a++)
            for (int b = a + 1; b < 52; b++)
                for (int c = b + 1; c < 52; c++)
                    for (int d = c + 1; d < 52; d++)
                        for (int e = d + 1; e < 52; e++) {
                            const CardKey k = cardKey(a) + cardKey(b) + cardKey(c) + cardKey(d) + cardKey(e);
                            const uint16_t v = eval5(k.key, k.mask);
                            cat[handCategory(v)]++;
                            seen[v] = 1;
                        }
        const long long want[9] = {1302540, 1098240, 123552, 54912, 10200, 5108, 3744, 624, 40};
        for (int i = 0; i < 9; i++) ok &= checkEq(categoryName(i), cat[i], want[i]);
        ok &= checkEq("distinct hand classes", std::count(seen.begin(), seen.end(), 1), 7462);
    }
    std::printf("7-card evaluator, all C(52,7) hands:\n");
    {
        std::vector<std::array<long long, 9>> cat(52);
        for (auto& c : cat) c.fill(0);
        parallelFor(52, defaultThreads(), [&](int a, int) {
            for (int b = a + 1; b < 52; b++) {
                const CardKey kab = cardKey(a) + cardKey(b);
                for (int c = b + 1; c < 52; c++) {
                    const CardKey kc = kab + cardKey(c);
                    for (int d = c + 1; d < 52; d++) {
                        const CardKey kd = kc + cardKey(d);
                        for (int e = d + 1; e < 52; e++) {
                            const CardKey ke = kd + cardKey(e);
                            for (int f = e + 1; f < 52; f++) {
                                const CardKey kf = ke + cardKey(f);
                                for (int g = f + 1; g < 52; g++) {
                                    const CardKey kg = kf + cardKey(g);
                                    cat[a][handCategory(eval7(kg.key, kg.mask))]++;
                                }
                            }
                        }
                    }
                }
            }
        });
        const long long want[9] = {23294460, 58627800, 31433400, 6461620, 6180020, 4047644, 3473184, 224848, 41584};
        for (int i = 0; i < 9; i++) {
            long long s = 0;
            for (auto& c : cat) s += c[i];
            ok &= checkEq(categoryName(i), s, want[i]);
        }
    }
    std::printf("Split engine vs brute force (Hold'em best 5 of 7, PLO exactly 2+3):\n");
    {
        Rng rng(7);
        int bad = 0;
        const int trials = 20000;
        for (int it = 0; it < trials; it++) {
            int deck[52];
            std::iota(deck, deck + 52, 0);
            for (int i = 0; i < 11; i++) std::swap(deck[i], deck[i + rng.below(52 - i)]);
            Hand h;
            std::copy(deck, deck + 6, h.begin());
            const int* bd = deck + 6;
            uint32_t v[kSplits];
            splitValues(prepHand(h), prepBoard(bd), v);
            for (int q = 0; q < kSplits; q++) {
                int he[2], plo[4];
                splitCards(h, q, he, plo);
                const uint16_t wantHe = bruteBest({he[0], he[1], bd[0], bd[1], bd[2], bd[3], bd[4]}, 0, 0);
                const uint16_t wantPlo =
                    bruteBest({plo[0], plo[1], plo[2], plo[3], bd[0], bd[1], bd[2], bd[3], bd[4]}, 2, 4);
                bad += heOf(v[q]) != wantHe || ploOf(v[q]) != wantPlo;
            }
        }
        ok &= checkEq("split/board mismatches", bad, 0);
    }
    std::printf("Solver consistency (random deals):\n");
    {
        Rng rng(11);
        for (int N : {2, 3, 4}) {
            std::vector<Hand> hands(N);
            for (auto& h : hands) h.fill(-1);
            dealRandom(hands, rng);
            const BoardSet bs = buildBoards(hands, 3000, false, 99, defaultThreads());
            SolveOptions full;
            full.fullTensor = true;
            full.threads = defaultThreads();
            const SolveResult rf = solve(bs, full);
            const SolveResult ro = solve(bs, SolveOptions{});
            double sum = 0;
            for (double v : rf.value) sum += v;
            // Tensor payoffs vs. the independent board-level evaluator, at random pure profiles.
            std::vector<std::vector<int>> all(N);
            for (auto& s : all)
                for (int q = 0; q < kSplits; q++) s.push_back(q);
            const Tensor T = buildTensor(bs, all, defaultThreads());
            double maxDiff = 0;
            for (int trial = 0; trial < 50; trial++) {
                std::vector<Mixed> sig(N);
                size_t p = 0;
                for (int i = 0; i < N; i++) {
                    const int s = int(rng.below(kSplits));
                    sig[i].fill(0);
                    sig[i][s] = 1;
                    p += s * T.stride[i];
                }
                std::vector<SplitStats> st;
                evalProfile(bs, sig, st, 1);
                for (int i = 0; i < N; i++) {
                    const int s = argmax(sig[i]);
                    maxDiff = std::max(maxDiff, std::abs(st[i].ev(s, N) - T.pay[p * N + i]));
                }
            }
            std::printf("  N=%d: zero-sum check %+.2e | tensor vs evaluator max diff %.2e | NashConv full %.5f, oracle %.5f\n",
                        N, sum, maxDiff, rf.nashConv, ro.nashConv);
            ok &= std::abs(sum) < 1e-6 && maxDiff < 1e-5;
        }
    }
    std::printf("\n%s   [%.1fs]\n", ok ? "ALL TESTS PASSED" : "SOME TESTS FAILED", t.sec());
    return ok ? 0 : 1;
}

int runBench(const Args& a) {
    Rng rng(a.seed);
    if (a.positional.size() == 1 && a.positional[0] == "sustained") {
        std::vector<Hand> hands(a.players);
        for (auto& h : hands) h.fill(-1);
        dealRandom(hands, rng);
        BoardSet bs;
        Timer total;
        for (int it = 0; total.sec() < 12; it++) {
            Timer t;
            int reps = 0;
            while (t.sec() < 1.0) {
                buildBoards(hands, 100000, false, it * 1000 + reps, a.threads, bs);
                reps++;
            }
            std::printf("t=%5.1fs  %.2f M player-boards/s\n", total.sec(), reps * 100000.0 * a.players / t.sec() / 1e6);
        }
        return 0;
    }
    {
        const int n = 20'000'000;
        std::vector<CardKey> keys(4096);
        for (auto& k : keys) {
            int d[52];
            std::iota(d, d + 52, 0);
            for (int i = 0; i < 7; i++) std::swap(d[i], d[i + rng.below(52 - i)]);
            k = CardKey{};
            for (int i = 0; i < 5; i++) k = k + cardKey(d[i]);
        }
        Timer t;
        uint64_t sink = 0;
        for (int i = 0; i < n; i++) {
            const CardKey& k = keys[(i * 7) & 4095];
            sink += eval5(k.key, k.mask);
        }
        std::printf("eval5:  %.1f M evals/s (single thread)  [checksum %llu]\n", n / t.sec() / 1e6,
                    (unsigned long long)(sink & 0xFFFF));
    }
    const int N = a.players, B = a.boards > 0 ? a.boards : 200000;
    std::vector<Hand> hands(N);
    for (auto& h : hands) h.fill(-1);
    dealRandom(hands, rng);
    for (int th : {1, a.threads}) {
        Timer t;
        const BoardSet bs = buildBoards(hands, B, false, 5, th);
        const double s = t.sec();
        std::printf("engine: %d players x %d boards in %.3fs on %d thread(s) = %.2f M player-boards/s "
                    "(%.0f M 5-card evals/s)\n",
                    N, B, s, th, double(N) * B / s / 1e6, double(N) * B * 165 / s / 1e6);
    }
    {
        const BoardSet small = buildBoards(hands, 2000, false, 5, 1);
        std::vector<Mixed> uni(N), pure(N);
        for (auto& m : uni) m.fill(1.0 / kSplits);
        for (auto& m : pure) {
            m.fill(0);
            m[0] = 1;
        }
        std::vector<SplitStats> st;
        for (auto* prof : {&uni, &pure}) {
            Timer t;
            for (int r = 0; r < 20; r++) evalProfile(small, *prof, st, 1);
            std::printf("evalProfile (%s opponents), %d players x 2000 boards: %.2f ms\n",
                        prof == &uni ? "uniform" : "pure", N, t.sec() / 20 * 1e3);
        }
        Timer t;
        for (int r = 0; r < 20; r++) solve(small, SolveOptions{});
        std::printf("solve (oracle, 1 thread), %d players x 2000 boards: %.2f ms\n", N, t.sec() / 20 * 1e3);
    }
    const BoardSet bs = buildBoards(hands, 20000, false, 5, a.threads);
    for (bool full : {true, false}) {
        if (full && N > 4) continue;
        SolveOptions o;
        o.fullTensor = full;
        o.threads = a.threads;
        Timer t;
        const SolveResult r = solve(bs, o);
        std::printf("solve:  %s, %d players x 20000 boards in %.3fs (NashConv %.5f)\n",
                    full ? "full 15^N tensor" : "double oracle", N, t.sec(), r.nashConv);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Args a = parseArgs(argc, argv);
        if (a.mode.empty() || a.mode == "help") {
            usage();
            return a.mode.empty() ? 1 : 0;
        }
        disablePowerThrottling();
        Timer init;
        initEvaluator();
        if (a.mode == "deal") return runDeal(a);
        if (a.mode == "hand") return runHand(a);
        if (a.mode == "stats") return runStats(a);
        if (a.mode == "catalog") {
            CatalogOptions c;
            c.deals = a.deals;
            c.boards = a.boards;
            c.threads = a.threads;
            c.seed = a.seed;
            c.limit = a.limit;
            c.rmIters = a.rmIters;
            c.rmTol = a.rmTol;
            c.out = a.out;
            c.only = a.only;
            return runCatalog(c);
        }
        if (a.mode == "selftest") return runSelftest();
        if (a.mode == "bench") {
            std::printf("table init: %.3fs\n", init.sec());
            return runBench(a);
        }
        usage();
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
