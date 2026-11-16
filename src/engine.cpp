#include "engine.h"

#include <algorithm>
#include <stdexcept>

#include "util.h"

namespace scoop {

const int kPairA[kSplits] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3, 4};
const int kPairB[kSplits] = {1, 2, 3, 4, 5, 2, 3, 4, 5, 3, 4, 5, 4, 5, 5};

const std::array<std::array<int, 6>, kSplits> kPloPairs = [] {
    std::array<std::array<int, 6>, kSplits> t{};
    for (int q = 0; q < kSplits; q++) {
        int n = 0;
        for (int p = 0; p < kSplits; p++)
            if (kPairA[p] != kPairA[q] && kPairA[p] != kPairB[q] && kPairB[p] != kPairA[q] &&
                kPairB[p] != kPairB[q])
                t[q][n++] = p;
    }
    return t;
}();

static const int kTriA[10] = {0, 0, 0, 0, 0, 0, 1, 1, 1, 2};
static const int kTriB[10] = {1, 1, 1, 2, 2, 3, 2, 2, 3, 3};
static const int kTriC[10] = {2, 3, 4, 3, 4, 4, 3, 4, 4, 4};

HandPre prepHand(const Hand& h) {
    HandPre p;
    for (int q = 0; q < kSplits; q++) p.pair[q] = cardKey(h[kPairA[q]]) + cardKey(h[kPairB[q]]);
    return p;
}

BoardPre prepBoard(const int bd[5]) {
    BoardPre b;
    CardKey k[5];
    for (int i = 0; i < 5; i++) k[i] = cardKey(bd[i]);
    b.full = k[0] + k[1] + k[2] + k[3] + k[4];
    for (int t = 0; t < 10; t++) b.tri[t] = k[kTriA[t]] + k[kTriB[t]] + k[kTriC[t]];
    return b;
}

void splitValues(const HandPre& h, const BoardPre& b, uint32_t out[kSplits]) {
    uint16_t pairBest[kSplits], he[kSplits];
    for (int p = 0; p < kSplits; p++) {
        const uint64_t pk = h.pair[p].key, pm = h.pair[p].mask;
        uint16_t m = 0;
        for (int t = 0; t < 10; t++) {
            const uint16_t v = eval5(pk + b.tri[t].key, pm | b.tri[t].mask);
            m = v > m ? v : m;
        }
        pairBest[p] = m;
        he[p] = eval7(pk + b.full.key, pm | b.full.mask);
    }
    for (int q = 0; q < kSplits; q++) {
        const auto& pp = kPloPairs[q];
        uint16_t m = pairBest[pp[0]];
        for (int i = 1; i < 6; i++) m = std::max(m, pairBest[pp[i]]);
        out[q] = (uint32_t(he[q]) << 16) | m;
    }
}

uint64_t choose(int n, int k) {
    if (k < 0 || k > n) return 0;
    uint64_t r = 1;
    for (int i = 1; i <= k; i++) r = r * uint64_t(n - k + i) / uint64_t(i);
    return r;
}

BoardSet buildBoards(const std::vector<Hand>& hands, int samples, bool exact, uint64_t seed, int threads) {
    BoardSet bs;
    buildBoards(hands, samples, exact, seed, threads, bs);
    return bs;
}

void buildBoards(const std::vector<Hand>& hands, int samples, bool exact, uint64_t seed, int threads,
                 BoardSet& bs) {
    const int N = int(hands.size());
    if (N < 2 || N > kMaxPlayers) throw std::runtime_error("player count must be 2..6");
    bool used[52] = {};
    for (const auto& h : hands)
        for (int c : h) {
            if (c < 0 || c >= 52 || used[c]) throw std::runtime_error("duplicate or invalid card in deal");
            used[c] = true;
        }
    std::vector<int> deck;
    for (int c = 0; c < 52; c++)
        if (!used[c]) deck.push_back(c);
    const int R = int(deck.size());

    if (samples <= 0) exact = true;
    // Worker lambdas must see this thread's buffer, so bind a plain reference to it.
    thread_local std::vector<std::array<uint8_t, 5>> boardStore;
    auto& boards = boardStore;
    boards.clear();
    if (exact) {
        boards.reserve(size_t(choose(R, 5)));
        for (int a = 0; a < R; a++)
            for (int b = a + 1; b < R; b++)
                for (int c = b + 1; c < R; c++)
                    for (int d = c + 1; d < R; d++)
                        for (int e = d + 1; e < R; e++)
                            boards.push_back({uint8_t(deck[a]), uint8_t(deck[b]), uint8_t(deck[c]),
                                              uint8_t(deck[d]), uint8_t(deck[e])});
    } else {
        Rng rng(seed);
        boards.resize(samples);
        for (auto& bd : boards) {
            for (int i = 0; i < 5; i++) {  // partial Fisher-Yates: uniform 5-subset
                const int j = i + int(rng.below(uint32_t(R - i)));
                std::swap(deck[i], deck[j]);
                bd[i] = uint8_t(deck[i]);
            }
        }
    }

    bs.N = N;
    bs.B = int(boards.size());
    bs.exact = exact;
    bs.v.resize(size_t(bs.B) * N * kSplits);
    std::vector<HandPre> pre(N);
    for (int i = 0; i < N; i++) pre[i] = prepHand(hands[i]);

    constexpr int kChunk = 1024;
    const int chunks = (bs.B + kChunk - 1) / kChunk;
    parallelFor(chunks, threads, [&](int ci, int) {
        const int lo = ci * kChunk, hi = std::min(bs.B, lo + kChunk);
        for (int b = lo; b < hi; b++) {
            const int bd[5] = {boards[b][0], boards[b][1], boards[b][2], boards[b][3], boards[b][4]};
            const BoardPre bp = prepBoard(bd);
            for (int i = 0; i < N; i++) splitValues(pre[i], bp, bs.v.data() + (size_t(b) * N + i) * kSplits);
        }
    });
}

Hand sortedHand(Hand h) {
    std::sort(h.begin(), h.end(), [](int a, int b) { return a > b; });
    return h;
}

std::string handStr(const Hand& h) {
    std::string s;
    for (int c : h) s += cardStr(c);
    return s;
}

std::string splitStr(const Hand& h, int q) {
    std::string he = cardStr(h[kPairA[q]]) + cardStr(h[kPairB[q]]), plo;
    for (int i = 0; i < 6; i++)
        if (i != kPairA[q] && i != kPairB[q]) plo += cardStr(h[i]);
    return he + " | " + plo;
}

}  // namespace scoop
