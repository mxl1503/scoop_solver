#pragma once
// Split-selection game solver.
//
// Payoffs (in BB, net of the 1 BB ante) on a single board:
//   player i scoops            : +(N-1)
//   another player scoops      : -1
//   nobody scoops (antes back) : 0
// A scoop means strictly winning BOTH the Hold'em and the PLO hand; any tie in
// either hand means no scoop. The game is constant-sum (payoffs sum to 0).

#include <array>
#include <vector>

#include "engine.h"

namespace scoop {

using Mixed = std::array<double, kSplits>;  // probability of each split

struct SplitStats {
    Mixed scoop{};  // P(this player scoops | plays split s, others play their profile)
    Mixed lose{};   // P(some other player scoops | ...)
    double ev(int s, int N) const { return (N - 1) * scoop[s] - lose[s]; }
    double chop(int s) const { return 1.0 - scoop[s] - lose[s]; }
};

// Exact (over the board set) scoop / lose probabilities for every player and every
// split against the other players' mixed strategies. Uses the factorisation
//   P(k scoops with u) = sum_boards prod_{j != k} P_j(j's split is beaten by k's u on both)
// which is exact because players randomise independently given the board.
// maxBoards > 0 evaluates only the first maxBoards boards (cheap estimate).
void evalProfile(const BoardSet& bs, const std::vector<Mixed>& sigma, std::vector<SplitStats>& out, int threads,
                 int maxBoards = 0);

// Payoff tensor over a restricted set of splits per player (all 15 = full 15^N game).
struct Tensor {
    int N = 0;
    std::vector<std::vector<int>> sup;  // sup[i] = split ids available to player i
    std::vector<size_t> stride;
    size_t P = 0;            // number of profiles
    std::vector<float> pay;  // [profile][player] EV in BB
};
Tensor buildTensor(const BoardSet& bs, const std::vector<std::vector<int>>& sup, int threads);
// Same, filling `T` in place so its storage is reused across oracle rounds and deals.
void buildTensor(const BoardSet& bs, const std::vector<std::vector<int>>& sup, int threads, Tensor& T);

struct SolveOptions {
    bool fullTensor = false;  // enumerate all 15^N profiles instead of double oracle
    int rmIters = 4000;       // regret-matching+ iterations cap per restricted game
    double rmTol = 2e-5;      // stop RM+ when restricted NashConv falls below this (BB)
    double rmWorkBudget = 0;  // > 0: cap RM+ iterations at budget / profiles (bounds tail cost)
    int maxOracleIters = 60;
    int threads = 1;
    int maxPureNE = 8;  // pure equilibria to record (full tensor only)
};

struct SolveResult {
    std::vector<Mixed> sigma;
    std::vector<SplitStats> stats;  // every split vs the others' equilibrium strategies
    std::vector<double> value;      // equilibrium EV per player
    double nashConv = 0;            // sum over players of best-response gain (0 = exact Nash)
    int oracleIters = 0;
    int rmItersUsed = 0;
    size_t profilesEvaluated = 0;
    long long pureNECount = -1;  // -1 = not computed
    std::vector<std::vector<int>> pureNE;
};

SolveResult solve(const BoardSet& bs, const SolveOptions& opt);

}  // namespace scoop
