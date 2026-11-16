// Six-max split catalogue.
//
// Every 6-card hand is solved as the hero at a 6-handed table: five opponents are
// dealt at random, every player picks a Nash split, and the hero's 15 split values
// are averaged over several such deals. Suit permutations are one hand, so the file
// holds the 962,988 canonical representatives rather than all C(52,6) = 20,358,520.
//
// data/sixmax/hands.bin (little-endian)
//   64-byte header, then ncanon fixed-size records in ascending canonical-key order.
//   The key is the six card ids, sorted high to low (rank * 4 + suit, suit c=0 d=1 h=2 s=3).
//   A record with deals == 0 is a placeholder that has not been solved yet.
//
// header:
//   0  char[8]  "SCOOP6H1"
//   8  u32      ncanon
//   12 u32      completed
//   16 u16      deals
//   18 u16      boards
//   20 u32      seed
//   24 u16      record size (119)
//   26 u16      players (6)
//   28 u16      regret-matching iteration cap
//   30 u16      reserved
//   32 f32      regret-matching tolerance (BB)
//   36 f64      elapsed seconds
//   44 u8[20]   reserved
// record (119 bytes):
//   0   u8[6]   cards, descending
//   6   u8      orbit size (1..24)
//   7   u8      deals averaged (0 = unsolved)
//   8   i16[15] EV in thousandths of a BB
//   38  u16[15] P(scoop) * 10000
//   68  u16[15] P(lose) * 10000
//   98  u8[15]  equilibrium frequency * 255
//   113 i16     mean NashConv in thousandths of a BB
//   115 i16     standard error of the best split's EV, thousandths of a BB
//   117 i16     equilibrium value in thousandths of a BB (mean over deals, not a product of means)

#include "catalog.h"

#include "engine.h"
#include "solver.h"
#include "util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>
#include <share.h>
#include <stdio.h>
#else
#include <unistd.h>
#endif

namespace scoop {
namespace {

constexpr int kPlayers = 6;
constexpr int kRec = 119;
constexpr int kHdr = 64;
constexpr int kDefaultDeals = 36;
constexpr int kDefaultBoards = 80;
constexpr uint32_t kCanonHands = 962988;
constexpr uint64_t kRealHands = 20358520ull;

int gPerm[24][4];

void initPerms() {
    int p[4] = {0, 1, 2, 3};
    int n = 0;
    do {
        std::memcpy(gPerm[n++], p, sizeof p);
    } while (std::next_permutation(p, p + 4));
    if (n != 24) throw std::runtime_error("suit permutation table is incomplete");
}

void sort6(int* a) {
    for (int i = 1; i < 6; i++) {
        int v = a[i], j = i;
        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            --j;
        }
        a[j] = v;
    }
}

int cmp6(const int* a, const int* b) {
    for (int i = 0; i < 6; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

// True when the ascending 6-card set is the lexicographically smallest in its suit orbit.
bool classify(const int asc[6], int& orbit) {
    int stab = 0;
    for (int pi = 0; pi < 24; pi++) {
        int m[6];
        const int* p = gPerm[pi];
        for (int i = 0; i < 6; i++) m[i] = (asc[i] & ~3) | p[asc[i] & 3];
        sort6(m);
        const int c = cmp6(m, asc);
        if (c < 0) return false;
        if (c == 0) stab++;
    }
    if (stab <= 0 || 24 % stab) throw std::runtime_error("suit stabiliser does not divide 24");
    orbit = 24 / stab;
    return true;
}

void lexMin(const int* in, int asc[6], int& orbit) {
    int best[6];
    int stab = 0;
    bool have = false;
    for (int pi = 0; pi < 24; pi++) {
        int m[6];
        const int* p = gPerm[pi];
        for (int i = 0; i < 6; i++) m[i] = (in[i] & ~3) | p[in[i] & 3];
        sort6(m);
        if (!have || cmp6(m, best) < 0) {
            std::memcpy(best, m, sizeof best);
            have = true;
            stab = 1;
        } else if (cmp6(m, best) == 0) {
            stab++;
        }
    }
    if (stab <= 0 || 24 % stab) throw std::runtime_error("suit stabiliser does not divide 24");
    std::memcpy(asc, best, sizeof(int) * 6);
    orbit = 24 / stab;
}

struct Key {
    uint8_t c[6];
    uint8_t orbit;
};

struct Solved {
    double ev[kSplits]{};
    double scoop[kSplits]{};
    double lose[kSplits]{};
    double eq[kSplits]{};
    double nash = 0;
    double se = 0;
    double value = 0;  // mean of per-deal equilibrium values, not the dot product of averages
    int deals = 0;
};

// ---------------------------------------------------------------- file primitives

bool seekFile(FILE* f, int64_t off) {
#ifdef _WIN32
    return _fseeki64(f, off, SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(off), SEEK_SET) == 0;
#endif
}

int64_t tellFile(FILE* f) {
#ifdef _WIN32
    return _ftelli64(f);
#else
    return static_cast<int64_t>(ftello(f));
#endif
}

void commitFile(FILE* f) {
    std::fflush(f);
#ifdef _WIN32
    _commit(_fileno(f));
#else
    fsync(fileno(f));
#endif
}

FILE* openFile(const std::filesystem::path& path, bool create) {
#ifdef _WIN32
    return _fsopen(path.string().c_str(), create ? "w+b" : "r+b", _SH_DENYNO);
#else
    return std::fopen(path.string().c_str(), create ? "w+b" : "r+b");
#endif
}

void putU16(unsigned char* p, int v) {
    if (v < 0) v = 0;
    if (v > 65535) v = 65535;
    p[0] = static_cast<unsigned char>(v & 255);
    p[1] = static_cast<unsigned char>((v >> 8) & 255);
}

void putI16(unsigned char* p, int v) {
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    const uint16_t u = static_cast<uint16_t>(static_cast<int16_t>(v));
    p[0] = static_cast<unsigned char>(u & 255);
    p[1] = static_cast<unsigned char>((u >> 8) & 255);
}

int getU16(const unsigned char* p) { return p[0] | (p[1] << 8); }

int getI16(const unsigned char* p) {
    int u = getU16(p);
    if (u >= 32768) u -= 65536;
    return u;
}

struct Hdr {
    uint32_t ncanon = 0;
    uint32_t completed = 0;
    uint16_t deals = 0;
    uint16_t boards = 0;
    uint32_t seed = 0;
    uint16_t rmIters = 0;
    float rmTol = 0;
    double elapsed = 0;
};

void writeHdr(FILE* f, const Hdr& h) {
    unsigned char b[kHdr] = {};
    std::memcpy(b, "SCOOP6H1", 8);
    b[8] = h.ncanon & 255;
    b[9] = (h.ncanon >> 8) & 255;
    b[10] = (h.ncanon >> 16) & 255;
    b[11] = (h.ncanon >> 24) & 255;
    b[12] = h.completed & 255;
    b[13] = (h.completed >> 8) & 255;
    b[14] = (h.completed >> 16) & 255;
    b[15] = (h.completed >> 24) & 255;
    b[16] = h.deals & 255;
    b[17] = (h.deals >> 8) & 255;
    b[18] = h.boards & 255;
    b[19] = (h.boards >> 8) & 255;
    b[20] = h.seed & 255;
    b[21] = (h.seed >> 8) & 255;
    b[22] = (h.seed >> 16) & 255;
    b[23] = (h.seed >> 24) & 255;
    b[24] = kRec & 255;
    b[25] = (kRec >> 8) & 255;
    b[26] = kPlayers & 255;
    b[27] = 0;
    b[28] = h.rmIters & 255;
    b[29] = (h.rmIters >> 8) & 255;
    std::memcpy(b + 32, &h.rmTol, 4);
    std::memcpy(b + 36, &h.elapsed, 8);
    if (!seekFile(f, 0) || std::fwrite(b, 1, kHdr, f) != kHdr) throw std::runtime_error("could not write catalogue header");
}

Hdr readHdr(FILE* f) {
    unsigned char b[kHdr];
    if (!seekFile(f, 0) || std::fread(b, 1, kHdr, f) != kHdr) throw std::runtime_error("could not read catalogue header");
    if (std::memcmp(b, "SCOOP6H1", 8) != 0) throw std::runtime_error("data/sixmax/hands.bin is not a Scoop catalogue");
    Hdr h;
    h.ncanon = uint32_t(b[8] | (b[9] << 8) | (b[10] << 16) | (b[11] << 24));
    h.completed = uint32_t(b[12] | (b[13] << 8) | (b[14] << 16) | (b[15] << 24));
    h.deals = uint16_t(b[16] | (b[17] << 8));
    h.boards = uint16_t(b[18] | (b[19] << 8));
    h.seed = uint32_t(b[20] | (b[21] << 8) | (b[22] << 16) | (b[23] << 24));
    const int rec = b[24] | (b[25] << 8);
    const int players = b[26] | (b[27] << 8);
    h.rmIters = uint16_t(b[28] | (b[29] << 8));
    std::memcpy(&h.rmTol, b + 32, 4);
    std::memcpy(&h.elapsed, b + 36, 8);
    if (rec != kRec || players != kPlayers) throw std::runtime_error("catalogue record layout does not match this build");
    return h;
}

void encode(unsigned char* p, const Key& k, const Solved& r) {
    std::memset(p, 0, kRec);
    std::memcpy(p, k.c, 6);
    p[6] = k.orbit;
    p[7] = static_cast<unsigned char>(std::clamp(r.deals, 0, 255));
    for (int s = 0; s < kSplits; s++) {
        putI16(p + 8 + 2 * s, static_cast<int>(std::lround(r.ev[s] * 1000.0)));
        putU16(p + 38 + 2 * s, static_cast<int>(std::lround(r.scoop[s] * 10000.0)));
        putU16(p + 68 + 2 * s, static_cast<int>(std::lround(r.lose[s] * 10000.0)));
        int eq = static_cast<int>(std::lround(r.eq[s] * 255.0));
        p[98 + s] = static_cast<unsigned char>(std::clamp(eq, 0, 255));
    }
    putI16(p + 113, static_cast<int>(std::lround(r.nash * 1000.0)));
    putI16(p + 115, static_cast<int>(std::lround(r.se * 1000.0)));
    putI16(p + 117, static_cast<int>(std::lround(r.value * 1000.0)));
}

Solved decode(const unsigned char* p) {
    Solved r;
    r.deals = p[7];
    double es = 0;
    for (int s = 0; s < kSplits; s++) {
        r.ev[s] = getI16(p + 8 + 2 * s) / 1000.0;
        r.scoop[s] = getU16(p + 38 + 2 * s) / 10000.0;
        r.lose[s] = getU16(p + 68 + 2 * s) / 10000.0;
        r.eq[s] = p[98 + s] / 255.0;
        es += r.eq[s];
    }
    if (es > 0)
        for (int s = 0; s < kSplits; s++) r.eq[s] /= es;
    r.nash = getI16(p + 113) / 1000.0;
    r.se = getI16(p + 115) / 1000.0;
    r.value = getI16(p + 117) / 1000.0;
    return r;
}

void writeRec(FILE* f, uint32_t index, const unsigned char* rec) {
    const int64_t off = kHdr + int64_t(index) * kRec;
    if (!seekFile(f, off) || std::fwrite(rec, 1, kRec, f) != kRec)
        throw std::runtime_error("could not write catalogue record");
}

// ---------------------------------------------------------------- hand features (same labels as the stats command)

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
        if (r1 >= 9) return std::string(2, kRankChars[r1]);
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

int maxHoldemSplit(const Hand& h) {
    for (int p = 0; p < kSplits; p++)
        if (rankOf(h[kPairA[p]]) == rankOf(h[kPairB[p]])) return p;
    return 0;
}

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

// ---------------------------------------------------------------- enumeration

struct Part {
    std::vector<Key> keys;
    uint64_t weight = 0;
    int orbits[25] = {};
};

std::vector<Key> enumerateKeys(int threads, uint64_t& weight) {
    std::vector<Part> parts(47);
    parallelFor(47, threads, [&](int a, int) {
        Part& part = parts[a];
        for (int b = a + 1; b < 52; b++)
            for (int c = b + 1; c < 52; c++)
                for (int d = c + 1; d < 52; d++)
                    for (int e = d + 1; e < 52; e++)
                        for (int f = e + 1; f < 52; f++) {
                            const int asc[6] = {a, b, c, d, e, f};
                            int orbit = 0;
                            if (!classify(asc, orbit)) continue;
                            Key k;
                            for (int i = 0; i < 6; i++) k.c[i] = static_cast<uint8_t>(asc[5 - i]);
                            k.orbit = static_cast<uint8_t>(orbit);
                            part.keys.push_back(k);
                            part.weight += uint64_t(orbit);
                            if (orbit < 25) part.orbits[orbit]++;
                        }
    });
    std::vector<Key> keys;
    weight = 0;
    int hist[25] = {};
    for (auto& part : parts) {
        weight += part.weight;
        keys.insert(keys.end(), part.keys.begin(), part.keys.end());
        for (int i = 0; i < 25; i++) hist[i] += part.orbits[i];
    }
    std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return std::memcmp(a.c, b.c, 6) < 0; });
    for (size_t i = 1; i < keys.size(); i++)
        if (std::memcmp(keys[i - 1].c, keys[i].c, 6) == 0) throw std::runtime_error("duplicate canonical hand");
    std::printf("canonical hands %zu  real hands represented %llu\n", keys.size(), (unsigned long long)weight);
    std::printf("orbit sizes:");
    for (int i = 1; i < 25; i++)
        if (hist[i]) std::printf("  %d:%d", i, hist[i]);
    std::printf("\n");
    std::fflush(stdout);
    if (keys.size() != kCanonHands || weight != kRealHands)
        throw std::runtime_error("suit-isomorphism count is not C(52,6) collapsed by S4 — refusing to build a bad catalogue");
    return keys;
}

// ---------------------------------------------------------------- solve one hand

Solved evaluate(const Key& key, int deals, int boards, uint64_t seed, const SolveOptions& base) {
    Hand hero;
    for (int i = 0; i < 6; i++) hero[i] = key.c[i];
    uint64_t handSeed = seed;
    for (int i = 0; i < 6; i++) handSeed = mixSeed(handSeed, uint64_t(hero[i]) + 1);

    double evS[kSplits] = {}, ev2[kSplits] = {}, scS[kSplits] = {}, loS[kSplits] = {}, eqS[kSplits] = {};
    double conv = 0, valueSum = 0;
    SolveOptions opt = base;
    opt.fullTensor = false;
    opt.threads = 1;
    for (int d = 0; d < deals; d++) {
        Rng rng(mixSeed(handSeed, uint64_t(d) + 1));
        std::vector<Hand> hands(kPlayers);
        hands[0] = hero;
        for (int i = 1; i < kPlayers; i++) hands[i].fill(-1);
        dealRandom(hands, rng);
        thread_local BoardSet bs;
        buildBoards(hands, boards, false, rng.next(), 1, bs);
        const SolveResult res = solve(bs, opt);
        for (int s = 0; s < kSplits; s++) {
            const double ev = res.stats[0].ev(s, kPlayers);
            evS[s] += ev;
            ev2[s] += ev * ev;
            scS[s] += res.stats[0].scoop[s];
            loS[s] += res.stats[0].lose[s];
            eqS[s] += res.sigma[0][s];
        }
        conv += res.nashConv;
        valueSum += res.value[0];
    }
    Solved r;
    r.deals = deals;
    r.nash = conv / deals;
    r.value = valueSum / deals;
    int best = 0;
    for (int s = 0; s < kSplits; s++) {
        r.ev[s] = evS[s] / deals;
        r.scoop[s] = scS[s] / deals;
        r.lose[s] = loS[s] / deals;
        r.eq[s] = eqS[s] / deals;
        if (r.ev[s] > r.ev[best]) best = s;
    }
    const double var = std::max(0.0, ev2[best] / deals - r.ev[best] * r.ev[best]);
    r.se = std::sqrt(var / deals);
    return r;
}

// ---------------------------------------------------------------- aggregates

struct Spot {
    double ev = 0;
    double value = 0;
    double eq = 0;
    int orbit = 0;
    std::string cards, best, shape;
};

struct Agg {
    double weight = 0, evSum = 0, bestSum = 0, nashSum = 0, seSum = 0, gapSum = 0;
    double heurLoss = 0, randLoss = 0, pureW = 0, topIn = 0, topTwo = 0;
    int hands = 0;
    struct SW {
        double w = 0, ev = 0, avail = 0;
    };
    struct Pair {
        double w = 0, ev = 0, he = 0, plo = 0, split = 0, suited = 0, ds = 0;
    };
    struct Suit {
        double w = 0, ev = 0, suited = 0, ds = 0, ss = 0, rain = 0, three = 0, mono = 0;
    };
    struct CE {
        double w = 0, ev = 0;
    };
    std::map<std::string, SW> shape;
    std::map<std::string, Pair> pairing;
    std::map<std::string, Suit> suits;
    std::map<std::string, CE> hec;
    std::map<std::string, std::map<std::string, double>> plo;
    std::vector<Spot> strong, weak;
};

void pushSpot(std::vector<Spot>& v, Spot s, bool high) {
    constexpr int kCap = 12;
    if ((int)v.size() == kCap) {
        if (high && s.ev <= v.back().ev) return;
        if (!high && s.ev >= v.back().ev) return;
    }
    v.push_back(std::move(s));
    std::sort(v.begin(), v.end(), [&](const Spot& a, const Spot& b) { return high ? a.ev > b.ev : a.ev < b.ev; });
    if ((int)v.size() > kCap) v.pop_back();
}

void addHand(Agg& A, const Hand& h, int orbit, const Solved& r) {
    const double W = orbit;
    double eqEv = 0, maxEv = -1e300, second = -1e300, mean = 0, maxEq = 0;
    int best = 0;
    double eqN[kSplits];
    double es = 0;
    for (int s = 0; s < kSplits; s++) es += r.eq[s];
    for (int s = 0; s < kSplits; s++) eqN[s] = es > 0 ? r.eq[s] / es : 1.0 / kSplits;
    for (int s = 0; s < kSplits; s++) {
        mean += r.ev[s] / kSplits;
        eqEv += eqN[s] * r.ev[s];
        maxEq = std::max(maxEq, eqN[s]);
        if (r.ev[s] > maxEv) {
            second = maxEv;
            maxEv = r.ev[s];
            best = s;
        } else if (r.ev[s] > second) {
            second = r.ev[s];
        }
    }
    A.weight += W;
    A.hands++;
    A.evSum += W * r.value;
    A.bestSum += W * maxEv;
    A.nashSum += W * r.nash;
    A.seSum += W * r.se;
    A.gapSum += W * (maxEv - second);
    A.heurLoss += W * (maxEv - r.ev[maxHoldemSplit(h)]);
    A.randLoss += W * (maxEv - mean);
    if (maxEq > 0.999) A.pureW += W;
    const int topRank = rankOf(h[0]);
    for (int s = 0; s < kSplits; s++) {
        if (rankOf(h[kPairA[s]]) == topRank || rankOf(h[kPairB[s]]) == topRank) A.topIn += W * eqN[s];
        if (s == 0) A.topTwo += W * eqN[s];
        int he[2], plo[4];
        splitCards(h, s, he, plo);
        const double w = W * eqN[s];
        const std::string shape = heShape(he);
        auto& sh = A.shape[shape];
        sh.w += w;
        sh.ev += w * r.ev[s];
        A.hec[heClass(he)].w += w;
        A.hec[heClass(he)].ev += w * r.ev[s];
        const std::string ps = ploSuit(plo);
        A.plo[ps][ploPairing(plo)] += w;
        auto& su = A.suits[suitPattern(h)];
        su.suited += shape == "suited" ? w : 0;
        su.ds += ps == "double-suited" ? w : 0;
        su.ss += ps == "single-suited" ? w : 0;
        su.rain += ps == "rainbow" ? w : 0;
        su.three += ps == "three-flush" ? w : 0;
        su.mono += ps == "monotone" ? w : 0;
        const std::string place = topPairPlacement(h, s);
        if (!place.empty()) {
            auto& pr = A.pairing[pairClass(h)];
            if (place == "HE") pr.he += w;
            else if (place == "PLO") pr.plo += w;
            else pr.split += w;
            pr.suited += shape == "suited" ? w : 0;
            pr.ds += ps == "double-suited" ? w : 0;
        }
    }
    for (int s = 0; s < kSplits; s++) {
        int he[2], plo[4];
        splitCards(h, s, he, plo);
        A.shape[heShape(he)].avail += W / kSplits;
    }
    auto& pr = A.pairing[pairClass(h)];
    pr.w += W;
    pr.ev += W * eqEv;
    auto& su = A.suits[suitPattern(h)];
    su.w += W;
    su.ev += W * eqEv;

    Spot spot;
    spot.ev = maxEv;
    spot.value = r.value;
    spot.eq = eqN[best];
    spot.orbit = orbit;
    spot.cards = handStr(h);
    spot.best = splitStr(h, best);
    int he[2], plo[4];
    splitCards(h, best, he, plo);
    spot.shape = heShape(he) + " / " + ploSuit(plo);
    pushSpot(A.strong, spot, true);
    pushSpot(A.weak, spot, false);
}

std::string q(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '\\' || c == '"') r.push_back('\\');
        r.push_back(c);
    }
    r.push_back('"');
    return r;
}

std::string num(double x) {
    if (!std::isfinite(x)) return "0";
    char b[64];
    std::snprintf(b, sizeof b, "%.6g", x);
    return b;
}

double frac(double part, double whole) { return whole > 0 ? part / whole : 0; }

void writeSummary(const std::filesystem::path& path, const Hdr& h, const Agg& A, int todoLeft, double rate) {
    const double W = A.weight;
    std::ostringstream o;
    o << "{\n";
    o << "  \"players\": 6,\n";
    o << "  \"deals\": " << h.deals << ",\n";
    o << "  \"boards\": " << h.boards << ",\n";
    o << "  \"seed\": " << h.seed << ",\n";
    o << "  \"canonical\": " << h.ncanon << ",\n";
    o << "  \"realHands\": " << kRealHands << ",\n";
    o << "  \"completed\": " << h.completed << ",\n";
    o << "  \"completedWeight\": " << num(W) << ",\n";
    o << "  \"finished\": " << (h.completed == h.ncanon ? "true" : "false") << ",\n";
    o << "  \"elapsedSec\": " << num(h.elapsed) << ",\n";
    o << "  \"handsPerSec\": " << num(rate) << ",\n";
    o << "  \"remaining\": " << todoLeft << ",\n";
    o << "  \"meanEv\": " << num(frac(A.evSum, W)) << ",\n";
    o << "  \"meanBestEv\": " << num(frac(A.bestSum, W)) << ",\n";
    o << "  \"meanNashConv\": " << num(frac(A.nashSum, W)) << ",\n";
    o << "  \"typicalSe\": " << num(frac(A.seSum, W)) << ",\n";
    o << "  \"meanTopGap\": " << num(frac(A.gapSum, W)) << ",\n";
    o << "  \"heurLoss\": " << num(frac(A.heurLoss, W)) << ",\n";
    o << "  \"randLoss\": " << num(frac(A.randLoss, W)) << ",\n";
    o << "  \"pureRate\": " << num(frac(A.pureW, W)) << ",\n";
    o << "  \"topInHoldem\": " << num(frac(A.topIn, W)) << ",\n";
    o << "  \"topTwoInHoldem\": " << num(frac(A.topTwo, W)) << ",\n";
    o << "  \"method\": \"Six players ante 1 BB and each receive 6 cards. The hero hand is fixed; the other five hands are random. "
         "On every sampled deal all six players choose a Nash equilibrium split, knowing every hole card. "
         "Hold'em uses the best 5 of 7. PLO uses exactly 2 hole cards and 3 board cards. "
         "A scoop is strictly winning both hands; otherwise the antes are returned. "
         "Each row averages the hero's 15 splits over the sampled deals and boards. "
         "One representative is stored per suit-isomorphism class.\",\n";

    o << "  \"heShape\": [\n";
    {
        bool first = true;
        for (const char* name : {"pocket pair", "suited", "offsuit"}) {
            const auto it = A.shape.find(name);
            Agg::SW sw;
            if (it != A.shape.end()) sw = it->second;
            if (!first) o << ",\n";
            first = false;
            o << "    {\"label\": " << q(name) << ", \"share\": " << num(frac(sw.w, W))
              << ", \"available\": " << num(frac(sw.avail, W)) << ", \"ev\": " << num(frac(sw.ev, sw.w)) << "}";
        }
        o << "\n  ],\n";
    }

    o << "  \"pairing\": [\n";
    {
        bool first = true;
        for (const char* name : {"no pair", "one pair", "two pair", "three pair", "trips", "trips + pair", "two trips", "quads",
                                 "quads + pair"}) {
            auto it = A.pairing.find(name);
            if (it == A.pairing.end() || it->second.w <= 0) continue;
            const auto& pr = it->second;
            const bool paired = std::string(name) != "no pair";
            if (!first) o << ",\n";
            first = false;
            o << "    {\"label\": " << q(name) << ", \"share\": " << num(frac(pr.w, W)) << ", \"ev\": " << num(frac(pr.ev, pr.w));
            if (paired) {
                o << ", \"pairHe\": " << num(frac(pr.he, pr.w)) << ", \"pairPlo\": " << num(frac(pr.plo, pr.w))
                  << ", \"pairSplit\": " << num(frac(pr.split, pr.w));
            }
            o << ", \"heSuited\": " << num(frac(pr.suited, pr.w)) << ", \"ploDs\": " << num(frac(pr.ds, pr.w)) << "}";
        }
        o << "\n  ],\n";
    }

    o << "  \"suits\": [\n";
    {
        std::vector<std::pair<double, std::string>> order;
        for (auto& [k, v] : A.suits) order.emplace_back(-v.w, k);
        std::sort(order.begin(), order.end());
        bool first = true;
        for (auto& [neg, k] : order) {
            const auto& su = A.suits.at(k);
            if (!first) o << ",\n";
            first = false;
            o << "    {\"label\": " << q(k) << ", \"share\": " << num(frac(su.w, W)) << ", \"ev\": " << num(frac(su.ev, su.w))
              << ", \"heSuited\": " << num(frac(su.suited, su.w)) << ", \"ds\": " << num(frac(su.ds, su.w))
              << ", \"ss\": " << num(frac(su.ss, su.w)) << ", \"rainbow\": " << num(frac(su.rain, su.w))
              << ", \"three\": " << num(frac(su.three, su.w)) << ", \"mono\": " << num(frac(su.mono, su.w)) << "}";
        }
        o << "\n  ],\n";
    }

    o << "  \"heClass\": [\n";
    {
        std::vector<std::pair<double, std::string>> order;
        for (auto& [k, v] : A.hec) order.emplace_back(-v.w, k);
        std::sort(order.begin(), order.end());
        bool first = true;
        for (auto& [neg, k] : order) {
            const auto& c = A.hec.at(k);
            if (!first) o << ",\n";
            first = false;
            o << "    {\"label\": " << q(k) << ", \"share\": " << num(frac(c.w, W)) << ", \"ev\": " << num(frac(c.ev, c.w))
              << "}";
        }
        o << "\n  ],\n";
    }

    o << "  \"plo\": [\n";
    {
        const char* pairs[] = {"unpaired", "one pair", "two pair", "trips+"};
        bool first = true;
        for (const char* s : {"double-suited", "single-suited", "rainbow", "three-flush", "monotone"}) {
            if (!first) o << ",\n";
            first = false;
            double tot = 0;
            o << "    {\"suits\": " << q(s);
            for (const char* p : pairs) {
                double v = 0;
                auto it = A.plo.find(s);
                if (it != A.plo.end()) {
                    auto jt = it->second.find(p);
                    if (jt != it->second.end()) v = jt->second;
                }
                tot += v;
                o << ", " << q(p) << ": " << num(frac(v, W));
            }
            o << ", \"total\": " << num(frac(tot, W)) << "}";
        }
        o << "\n  ],\n";
    }

    auto emitSpots = [&](const char* name, const std::vector<Spot>& v) {
        o << "  \"" << name << "\": [\n";
        for (size_t i = 0; i < v.size(); i++) {
            const auto& s = v[i];
            if (i) o << ",\n";
            o << "    {\"cards\": " << q(s.cards) << ", \"ev\": " << num(s.ev) << ", \"value\": " << num(s.value)
              << ", \"best\": " << q(s.best) << ", \"eq\": " << num(s.eq) << ", \"orbit\": " << s.orbit
              << ", \"shape\": " << q(s.shape) << "}";
        }
        o << "\n  ]";
    };
    emitSpots("strongest", A.strong);
    o << ",\n";
    emitSpots("weakest", A.weak);
    o << "\n}\n";

    const auto dest = path;
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) throw std::runtime_error("could not write " + tmp);
        out << o.str();
    }
    std::error_code ec;
    std::filesystem::remove(dest, ec);
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        std::filesystem::copy_file(tmp, dest, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
    }
}

// ---------------------------------------------------------------- catalogue driver

Key keyFromHand(const Hand& h, int orbit) {
    Key k;
    for (int i = 0; i < 6; i++) k.c[i] = static_cast<uint8_t>(h[i]);
    k.orbit = static_cast<uint8_t>(orbit);
    return k;
}

int runOne(const CatalogOptions& opt, int deals, int boards, const SolveOptions& base) {
    const auto cards = parseCards(opt.only);
    if (cards.size() != 6) throw std::runtime_error("--only needs exactly 6 cards");
    bool used[52] = {};
    for (int c : cards) {
        if (used[c]) throw std::runtime_error("duplicate card " + cardStr(c));
        used[c] = true;
    }
    int asc[6], orbit = 0;
    lexMin(cards.data(), asc, orbit);
    Hand hero;
    for (int i = 0; i < 6; i++) hero[i] = asc[5 - i];
    const Key key = keyFromHand(hero, orbit);
    const auto t0 = std::chrono::steady_clock::now();
    const Solved r = evaluate(key, deals, boards, opt.seed, base);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("Scoop catalogue - one hand, 6 players\n");
    std::printf("Input: %s\n", opt.only.c_str());
    std::printf("Representative: %s   orbit %d (stands for %d real hands)   %d deals x %d boards   [%.2fs]\n",
                handStr(hero).c_str(), orbit, orbit, deals, boards, sec);
    double maxEq = 0;
    int mode = 0;
    std::vector<int> order(kSplits);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](int a, int b) { return r.ev[a] > r.ev[b]; });
    for (int s = 0; s < kSplits; s++) {
        if (r.eq[s] > maxEq) {
            maxEq = r.eq[s];
            mode = s;
        }
    }
    std::printf("Equilibrium EV %+.4f BB   best committed split %+.4f BB   NashConv %.4f BB   SE %.4f BB\n", r.value,
                r.ev[order[0]], r.nash, r.se);
    std::printf("Most-played equilibrium split: %s (%.1f%%)\n\n", splitStr(hero, mode).c_str(), 100 * maxEq);
    std::printf("  %-4s %-24s %-28s %9s %8s %8s %8s %8s\n", "rank", "split  HE | PLO", "shape", "EV(BB)", "scoop", "chop",
                "lose", "eq");
    for (int i = 0; i < kSplits; i++) {
        const int s = order[i];
        int he[2], plo[4];
        splitCards(hero, s, he, plo);
        const double chop = 1.0 - r.scoop[s] - r.lose[s];
        std::printf("  %-4d %-24s %-28s %+9.4f %7.1f%% %7.1f%% %7.1f%% %7.1f%%\n", i + 1, splitStr(hero, s).c_str(),
                    (heShape(he) + " / " + ploSuit(plo)).c_str(), r.ev[s], 100 * r.scoop[s], 100 * chop, 100 * r.lose[s],
                    100 * r.eq[s]);
    }
    return 0;
}

}  // namespace

int runCatalog(const CatalogOptions& opt) {
    initPerms();
    const int deals = opt.deals > 0 ? opt.deals : kDefaultDeals;
    const int boards = opt.boards > 0 ? opt.boards : kDefaultBoards;
    if (deals > 255) throw std::runtime_error("--deals must be 1..255");
    if (boards < 1) throw std::runtime_error("--boards must be positive");
    SolveOptions base;
    base.rmIters = opt.rmIters > 0 ? opt.rmIters : 250;
    base.rmTol = opt.rmTol > 0 ? opt.rmTol : 0.004;
    base.rmWorkBudget = 3e6;
    base.threads = 1;
    if (!opt.only.empty()) return runOne(opt, deals, boards, base);

    namespace fs = std::filesystem;
    fs::create_directories(opt.out);
    const fs::path binPath = fs::path(opt.out) / "hands.bin";
    const fs::path sumPath = fs::path(opt.out) / "summary.json";
    const bool exists = fs::exists(binPath);

    Hdr hdr;
    std::vector<Key> keys;
    std::vector<uint8_t> done;
    FILE* f = nullptr;
    if (!exists) {
        std::printf("Enumerating suit-canonical 6-card hands...\n");
        std::fflush(stdout);
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t weight = 0;
        keys = enumerateKeys(opt.threads, weight);
        std::printf("Enumeration %.1fs. Writing placeholders...\n",
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        std::fflush(stdout);
        f = openFile(binPath, true);
        if (!f) throw std::runtime_error("could not create " + binPath.string());
        hdr.ncanon = uint32_t(keys.size());
        hdr.deals = uint16_t(deals);
        hdr.boards = uint16_t(boards);
        hdr.seed = uint32_t(opt.seed);
        hdr.rmIters = uint16_t(base.rmIters);
        hdr.rmTol = float(base.rmTol);
        writeHdr(f, hdr);
        std::vector<unsigned char> buf(size_t(kRec) * 2048);
        int nbuf = 0;
        auto flushBuf = [&] {
            if (!nbuf) return;
            if (std::fwrite(buf.data(), kRec, nbuf, f) != size_t(nbuf)) throw std::runtime_error("could not write placeholders");
            nbuf = 0;
        };
        Solved empty;
        for (const auto& k : keys) {
            encode(buf.data() + nbuf * kRec, k, empty);
            if (++nbuf == 2048) flushBuf();
        }
        flushBuf();
        commitFile(f);
        done.assign(keys.size(), 0);
        std::printf("Placeholder file %.1f MB\n", (kHdr + keys.size() * double(kRec)) / 1e6);
    } else {
        f = openFile(binPath, false);
        if (!f) throw std::runtime_error("could not open " + binPath.string());
        hdr = readHdr(f);
        if (hdr.deals != deals || hdr.boards != boards || hdr.seed != opt.seed || hdr.rmIters != base.rmIters ||
            std::fabs(hdr.rmTol - float(base.rmTol)) > 1e-6) {
            throw std::runtime_error(
                "existing catalogue was built with different --deals/--boards/--seed/--rm-iters/--rm-tol. "
                "Rerun with those settings, or delete the output directory to start over.");
        }
        const int64_t want = kHdr + int64_t(hdr.ncanon) * kRec;
        if (!seekFile(f, 0) || tellFile(f) < 0) throw std::runtime_error("could not measure catalogue file");
        seekFile(f, 0);
#ifdef _WIN32
        if (_fseeki64(f, 0, SEEK_END) != 0) throw std::runtime_error("could not measure catalogue file");
#else
        if (fseeko(f, 0, SEEK_END) != 0) throw std::runtime_error("could not measure catalogue file");
#endif
        if (tellFile(f) != want) throw std::runtime_error("catalogue file has the wrong length");
        keys.resize(hdr.ncanon);
        done.assign(hdr.ncanon, 0);
        if (!seekFile(f, kHdr)) throw std::runtime_error("could not read catalogue");
        std::vector<unsigned char> buf(size_t(kRec) * 2048);
        uint32_t got = 0;
        while (got < hdr.ncanon) {
            const uint32_t n = std::min<uint32_t>(2048, hdr.ncanon - got);
            if (std::fread(buf.data(), kRec, n, f) != n) throw std::runtime_error("catalogue file ended early");
            for (uint32_t i = 0; i < n; i++) {
                const unsigned char* p = buf.data() + i * kRec;
                std::memcpy(keys[got + i].c, p, 6);
                keys[got + i].orbit = p[6];
                done[got + i] = p[7] ? 1 : 0;
            }
            got += n;
        }
        std::printf("Resuming %s (%u / %u hands already solved)\n", binPath.string().c_str(),
                    uint32_t(std::count(done.begin(), done.end(), 1)), hdr.ncanon);
    }

    struct FileClose {
        FILE* f;
        ~FileClose() {
            if (f) std::fclose(f);
        }
    } guard{f};

    Agg agg;
    // Rebuild charts from rows already on disk so a resumed run does not forget them.
    {
        std::vector<unsigned char> buf(size_t(kRec) * 2048);
        if (!seekFile(f, kHdr)) throw std::runtime_error("could not reread catalogueue");
        uint32_t got = 0;
        while (got < hdr.ncanon) {
            const uint32_t n = std::min<uint32_t>(2048, hdr.ncanon - got);
            if (std::fread(buf.data(), kRec, n, f) != n) throw std::runtime_error("catalogue file ended early");
            for (uint32_t i = 0; i < n; i++) {
                if (!done[got + i]) continue;
                Hand h;
                for (int c = 0; c < 6; c++) h[c] = keys[got + i].c[c];
                addHand(agg, h, keys[got + i].orbit, decode(buf.data() + i * kRec));
            }
            got += n;
        }
    }

    std::vector<int> order(keys.size());
    std::iota(order.begin(), order.end(), 0);
    Rng shuffle(mixSeed(opt.seed, 0xCA7A10ull));
    for (int i = int(order.size()) - 1; i > 0; --i) {
        const int j = int(shuffle.below(uint32_t(i + 1)));
        std::swap(order[i], order[j]);
    }
    std::vector<int> todo;
    todo.reserve(keys.size());
    for (int i : order)
        if (!done[i]) todo.push_back(i);
    if (opt.limit > 0 && (int)todo.size() > opt.limit) todo.resize(opt.limit);
    hdr.completed = uint32_t(std::count(done.begin(), done.end(), 1));

    auto wall = [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const double session0 = wall();
    const double elapsedBase = hdr.elapsed;
    auto currentElapsed = [&] { return elapsedBase + (wall() - session0); };
    auto unsolved = [&] { return int(keys.size() - std::count(done.begin(), done.end(), 1)); };

    writeHdr(f, hdr);
    writeSummary(sumPath, hdr, agg, unsolved(), 0);
    commitFile(f);

    if (todo.empty()) {
        std::printf("Catalogue already complete: %u hands in %s\n", hdr.ncanon, binPath.string().c_str());
        return 0;
    }

    std::printf("Solving %zu hands at 6 players, %d deals x %d boards, %d threads. %d already done, %d still unsolved.\n",
                todo.size(), deals, boards, opt.threads, int(hdr.completed), unsolved());
    std::fflush(stdout);

    constexpr int kChunk = 128;
    uint32_t sessionDone = 0;
    for (size_t at = 0; at < todo.size(); at += kChunk) {
        const int m = int(std::min(size_t(kChunk), todo.size() - at));
        std::vector<Solved> sols(m);
        const auto c0 = std::chrono::steady_clock::now();
        parallelFor(m, opt.threads, [&](int j, int) { sols[j] = evaluate(keys[todo[at + j]], deals, boards, opt.seed, base); });
        unsigned char rec[kRec];
        for (int j = 0; j < m; j++) {
            const int idx = todo[at + j];
            encode(rec, keys[idx], sols[j]);
            writeRec(f, uint32_t(idx), rec);
            Hand h;
            for (int c = 0; c < 6; c++) h[c] = keys[idx].c[c];
            addHand(agg, h, keys[idx].orbit, sols[j]);
            done[idx] = 1;
        }
        sessionDone += uint32_t(m);
        const double chunkSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
        const double sessionSec = std::max(1e-6, wall() - session0);
        const double rate = sessionDone / sessionSec;
        hdr.elapsed = currentElapsed();
        hdr.completed = uint32_t(std::count(done.begin(), done.end(), 1));
        writeHdr(f, hdr);
        writeSummary(sumPath, hdr, agg, unsolved(), rate);
        commitFile(f);
        const double etaH = unsolved() / rate / 3600.0;
        std::printf("  %u / %u (%.1f%%)   %.1f hands/s   chunk %.0f ms/hand   eta %.2f h   mean EV %+.4f   NashConv %.4f   SE %.4f\n",
                    hdr.completed, hdr.ncanon, 100.0 * hdr.completed / hdr.ncanon, rate, chunkSec / m * 1000.0, etaH,
                    frac(agg.evSum, agg.weight), frac(agg.nashSum, agg.weight), frac(agg.seSum, agg.weight));
        std::fflush(stdout);
    }

    std::printf("Wrote %s and %s  (%u / %u hands).\n", binPath.string().c_str(), sumPath.string().c_str(), hdr.completed,
                hdr.ncanon);
    return 0;
}

}  // namespace scoop
