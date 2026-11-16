#pragma once
// Monte Carlo / exhaustive board engine.
//
// For every sampled board and every player we store all 15 split outcomes
// (Hold'em strength, PLO strength) computed on that same board, so any joint
// question ("does player i win BOTH hands with split s while the others play
// splits t_j?") is answered from correlated, shared-board data.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "evaluator.h"

namespace scoop {

constexpr int kSplits = 15;   // C(6,2) ways to pick the Hold'em pair
constexpr int kMaxPlayers = 6;

using Hand = std::array<int, 6>;

// Split q puts hand[kPairA[q]], hand[kPairB[q]] into Hold'em and the other 4 into PLO.
extern const int kPairA[kSplits];
extern const int kPairB[kSplits];
// For split q: the 6 pair indices whose both cards lie in the PLO part.
extern const std::array<std::array<int, 6>, kSplits> kPloPairs;

// Packed outcome of one split on one board: (holdem << 16) | plo, higher = better.
inline uint16_t heOf(uint32_t v) { return uint16_t(v >> 16); }
inline uint16_t ploOf(uint32_t v) { return uint16_t(v & 0xFFFF); }
// True when `a` strictly beats `b` in both the Hold'em and the PLO hand.
// Non-short-circuit '&' keeps hot loops branchless (outcomes are ~50/50 and unpredictable).
inline bool beatsBoth(uint32_t a, uint32_t b) { return ((a >> 16) > (b >> 16)) & ((a & 0xFFFF) > (b & 0xFFFF)); }

struct HandPre {
    CardKey pair[kSplits];
};
struct BoardPre {
    CardKey full;
    CardKey tri[10];
};
HandPre prepHand(const Hand& h);
BoardPre prepBoard(const int board[5]);
// Writes the 15 packed split outcomes for one hand on one board.
void splitValues(const HandPre& h, const BoardPre& b, uint32_t out[kSplits]);

struct BoardSet {
    int N = 0;
    int B = 0;
    bool exact = false;
    std::vector<uint32_t> v;  // [board][player][split]
    const uint32_t* at(int b, int i) const { return v.data() + (size_t(b) * N + i) * kSplits; }
};

uint64_t choose(int n, int k);
// samples <= 0 or exact=true enumerates every board from the remaining deck.
BoardSet buildBoards(const std::vector<Hand>& hands, int samples, bool exact, uint64_t seed, int threads);
// Same, filling `out` in place so its storage is reused across many deals.
void buildBoards(const std::vector<Hand>& hands, int samples, bool exact, uint64_t seed, int threads, BoardSet& out);

// Formatting helpers.
std::string handStr(const Hand& h);
std::string splitStr(const Hand& h, int split);  // "AsKs | QhJh7c2d"
Hand sortedHand(Hand h);                         // high cards first

}  // namespace scoop
