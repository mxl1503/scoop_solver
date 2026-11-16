#include "evaluator.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scoop {

namespace {

std::unordered_map<uint32_t, uint16_t> g_dense;

uint64_t splitmix(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

using Items = std::vector<std::pair<uint64_t, uint16_t>>;

// Hash-and-displace perfect hash: slot = (h2(k) + disp[h1(k)]) mod 2^slotBits.
bool buildPerfectHash(const Items& items, int bucketBits, int slotBits, uint64_t m1, uint64_t m2,
                      uint16_t* disp, uint16_t* vals) {
    const uint32_t nb = 1u << bucketBits, ns = 1u << slotBits, smask = ns - 1;
    std::vector<std::vector<int>> buckets(nb);
    for (int i = 0; i < (int)items.size(); i++)
        buckets[uint32_t((items[i].first * m1) >> (64 - bucketBits))].push_back(i);
    std::vector<uint32_t> order(nb);
    for (uint32_t i = 0; i < nb; i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](uint32_t a, uint32_t b) { return buckets[a].size() > buckets[b].size(); });
    std::vector<char> used(ns, 0);
    std::fill(disp, disp + nb, 0);
    std::vector<uint32_t> h2, placed;
    for (uint32_t b : order) {
        const auto& bk = buckets[b];
        if (bk.empty()) break;
        h2.clear();
        for (int i : bk) h2.push_back(uint32_t((items[i].first * m2) >> (64 - slotBits)));
        bool found = false;
        for (uint32_t d = 0; d < 65536 && !found; d++) {
            placed.clear();
            bool ok = true;
            for (uint32_t h : h2) {
                const uint32_t s = (h + d) & smask;
                if (used[s]) { ok = false; break; }
                used[s] = 1;
                placed.push_back(s);
            }
            if (ok) {
                disp[b] = uint16_t(d);
                for (size_t j = 0; j < bk.size(); j++) vals[placed[j]] = items[bk[j]].second;
                found = true;
            } else {
                for (uint32_t s : placed) used[s] = 0;
            }
        }
        if (!found) return false;
    }
    return true;
}

void buildHashOrDie(const Items& items, int bucketBits, int slotBits, uint64_t& m1, uint64_t& m2,
                    uint16_t* disp, uint16_t* vals) {
    uint64_t seed = 0xC0FFEE ^ (uint64_t(slotBits) << 32);
    for (int attempt = 0; attempt < 200; attempt++) {
        m1 = splitmix(seed) | 1;
        m2 = splitmix(seed) | 1;
        if (buildPerfectHash(items, bucketBits, slotBits, m1, m2, disp, vals)) return;
    }
    throw std::runtime_error("perfect hash construction failed");
}

// Enumerate rank-count vectors with total `n` and each count <= 4.
void enumMultisets(int n, std::vector<std::array<int, 13>>& out) {
    std::array<int, 13> c{};
    auto rec = [&](auto&& self, int r, int left) -> void {
        if (r == 13) {
            if (left == 0) out.push_back(c);
            return;
        }
        for (int k = 0; k <= std::min(4, left); k++) {
            c[r] = k;
            self(self, r + 1, left - k);
        }
        c[r] = 0;
    };
    rec(rec, 0, n);
}

uint64_t rankKey(const std::array<int, 13>& c) {
    uint64_t k = 0;
    for (int r = 0; r < 13; r++) k += uint64_t(c[r]) << (3 * r);
    return k;
}

std::once_flag g_initOnce;

void doInit() {
    using namespace tables;
    for (int c = 0; c < 52; c++) {
        const int r = c >> 2, s = c & 3;
        cards[c].key = (1ull << (3 * r)) | (1ull << (40 + 4 * s));
        cards[c].mask = 1ull << (16 * s + r);
    }

    // Collect every 5-card equivalence class and assign dense ranks 1..7462.
    std::vector<std::array<int, 13>> ms5, ms7;
    enumMultisets(5, ms5);
    enumMultisets(7, ms7);
    std::vector<uint32_t> all;
    auto toRanks = [](const std::array<int, 13>& c, int* out) {
        int n = 0;
        for (int r = 12; r >= 0; r--)
            for (int k = 0; k < c[r]; k++) out[n++] = r;
        return n;
    };
    for (auto& m : ms5) {
        int rk[5];
        toRanks(m, rk);
        all.push_back(slowEval5(rk, false));
    }
    for (int mask = 0; mask < 8192; mask++) {
        if (std::popcount(unsigned(mask)) != 5) continue;
        int rk[5], n = 0;
        for (int r = 12; r >= 0; r--)
            if (mask >> r & 1) rk[n++] = r;
        all.push_back(slowEval5(rk, true));
    }
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    if (all.size() != 7462) throw std::runtime_error("expected 7462 hand classes");
    g_dense.clear();
    for (size_t i = 0; i < all.size(); i++) {
        g_dense[all[i]] = uint16_t(i + 1);
        category[i + 1] = uint8_t(all[i] >> 20);
    }

    // Flush table: best flush / straight flush among 5..7 cards of one suit.
    for (int mask = 0; mask < 8192; mask++) {
        flush[mask] = 0;
        int bits[13], n = 0;
        for (int r = 12; r >= 0; r--)
            if (mask >> r & 1) bits[n++] = r;
        if (n < 5) continue;
        uint16_t best = 0;
        for (int a = 0; a < n; a++)
            for (int b = a + 1; b < n; b++)
                for (int c = b + 1; c < n; c++)
                    for (int d = c + 1; d < n; d++)
                        for (int e = d + 1; e < n; e++) {
                            const int rk[5] = {bits[a], bits[b], bits[c], bits[d], bits[e]};
                            best = std::max(best, g_dense.at(slowEval5(rk, true)));
                        }
        flush[mask] = best;
    }

    Items items5, items7;
    for (auto& m : ms5) {
        int rk[5];
        toRanks(m, rk);
        items5.emplace_back(rankKey(m), g_dense.at(slowEval5(rk, false)));
    }
    for (auto& m : ms7) {
        int rk[7];
        toRanks(m, rk);
        uint16_t best = 0;
        for (int skip1 = 0; skip1 < 7; skip1++)
            for (int skip2 = skip1 + 1; skip2 < 7; skip2++) {
                int sub[5], n = 0;
                for (int i = 0; i < 7; i++)
                    if (i != skip1 && i != skip2) sub[n++] = rk[i];
                best = std::max(best, g_dense.at(slowEval5(sub, false)));
            }
        items7.emplace_back(rankKey(m), best);
    }
    buildHashOrDie(items5, kH5BucketBits, kH5SlotBits, h5m1, h5m2, h5disp, h5val);
    buildHashOrDie(items7, kH7BucketBits, kH7SlotBits, h7m1, h7m2, h7disp, h7val);
}

}  // namespace

uint32_t slowEval5(const int ranks[5], bool isFlush) {
    int cnt[13] = {0};
    for (int i = 0; i < 5; i++) cnt[ranks[i]]++;
    std::array<std::pair<int, int>, 5> groups{};  // (count, rank)
    int ng = 0;
    unsigned bits = 0;
    for (int r = 12; r >= 0; r--)
        if (cnt[r]) {
            groups[ng++] = {cnt[r], r};
            bits |= 1u << r;
        }
    std::sort(groups.begin(), groups.begin() + ng, [](auto a, auto b) {
        return a.first != b.first ? a.first > b.first : a.second > b.second;
    });
    int straightHigh = -1;
    if (ng == 5) {
        for (int top = 12; top >= 4; top--)
            if (((bits >> (top - 4)) & 0x1F) == 0x1F) { straightHigh = top; break; }
        if (straightHigh < 0 && bits == 0x100F) straightHigh = 3;  // A-2-3-4-5
    }
    uint32_t tb = 0;
    for (int g = 0; g < ng; g++) tb |= uint32_t(groups[g].second) << (4 * (4 - g));
    int cat;
    if (straightHigh >= 0 && isFlush) { cat = 8; tb = straightHigh; }
    else if (groups[0].first == 4) cat = 7;
    else if (groups[0].first == 3 && groups[1].first == 2) cat = 6;
    else if (isFlush) cat = 5;
    else if (straightHigh >= 0) { cat = 4; tb = straightHigh; }
    else if (groups[0].first == 3) cat = 3;
    else if (groups[0].first == 2 && groups[1].first == 2) cat = 2;
    else if (groups[0].first == 2) cat = 1;
    else cat = 0;
    return (uint32_t(cat) << 20) | tb;
}

uint16_t denseFromSlow(uint32_t slow) { return g_dense.at(slow); }

void initEvaluator() { std::call_once(g_initOnce, doInit); }

const char* categoryName(int cat) {
    static const char* names[] = {"high card",  "pair",       "two pair",   "trips",         "straight",
                                  "flush",      "full house", "quads",      "straight flush"};
    return (cat >= 0 && cat <= 8) ? names[cat] : "?";
}

}  // namespace scoop
