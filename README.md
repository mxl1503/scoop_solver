# Scoop poker split solver

A fast C++20 solver for **Scoop**: N players (2–6) each ante 1 BB and receive 6 hole cards. Each player
splits them into a 2-card **Hold'em** hand and a 4-card **PLO** hand, then five community cards are dealt.
A player who wins **both** hands takes the whole pot; if nobody does, every ante is returned.

## Build

Requires CMake ≥ 3.16 and a C++20 compiler (MSVC 2022, GCC 11+, or Clang 14+).

```powershell
cmake -S . -B build
cmake --build build --config Release
```

The executable is `build/Release/scoop.exe` with Visual Studio generators, or `build/scoop` with
single-config generators such as Ninja or Make. The build uses `-march=native` (or `/arch:AVX2` on MSVC);
pass `-DSCOOP_NATIVE=OFF` to build a portable binary.

Run `scoop selftest` first. It checks the evaluators against every 5-card and 7-card poker hand and
cross-checks the solver (about 2 seconds).

## Usage

```text
scoop hand <6 cards> [-n 4] [--deals 1000] [--boards 2000]   rank a hand's 15 splits vs random opponents
scoop deal [-n 4] [--hands "AsAhKdQd7c6c,?,?,?"] [--boards 20000 | --exact] [--solver full|oracle]
scoop stats [-n 4] [--deals 2000] [--boards 1500]           aggregate split-shape preferences
scoop catalog [--deals 36] [--boards 80] [--out data/sixmax]
scoop selftest | scoop bench
common: --seed S  --threads T  --top K  --rm-iters I  --rm-tol X
```

Cards are written as rank `23456789TJQKA` followed by suit `cdhs`, e.g. `AsAhKdQd7c6c`.

- **`hand`** samples `--deals` random opponent hands. For each deal it solves the full game (every
  player picks a Nash-equilibrium split), then reports your EV for each of your 15 splits against the
  opponents' equilibrium splits. The output ranks all 15 by EV, with a standard error, scoop rate, chop
  rate, the rate at which an opponent scoops (`lose`), and how often the equilibrium actually chose that
  split (`eq-freq`).
- **`deal`** solves one fully specified deal (`?` = random hand) and prints a ranked split table for
  every player. Boards are enumerated exactly when there are at most `--boards` possible boards (always
  the case at 6 players). By default, with 2–4 players the solver evaluates all 15^N split profiles and also counts
  pure-strategy Nash equilibria. The output ends with a table that compares the true scoop rate with
  P(win Hold'em) × P(win PLO), showing how much the two hands are correlated.
- **`stats`** solves thousands of random deals and tabulates which split shapes the equilibrium
  prefers, broken down by pairing structure and suit distribution: pocket pair, suited, or offsuit in
  Hold'em; double-suited, single-suited, rainbow, and so on in PLO; whether the top pair goes to Hold'em,
  stays in PLO, or is split; where the high cards go; and how much EV the naive "best Hold'em hand"
  heuristic gives up.

## Six-max split book

`scoop catalog` is the way to ask the solver how to split every hand. It deals a 6-player game, gives the hero one specific 6-card hand, and gives the other five players random hands. On each sampled deal everybody chooses a Nash split with all hole cards known. The catalogue records, for every suit-canonical hero hand, the hero's average result for each of the 15 ways to cut the hand.

There are C(52,6) = 20,358,520 six-card hands. Relabelling the four suits leaves 962,988 representatives. The defaults are 36 opponent deals and 80 sampled boards per deal. The catalogue checkpoints to disk and resumes when rerun with the same sampling and solver settings. At 60 hands/s, a full run takes about 4.5 hours.

```powershell
build\Release\scoop.exe catalog
python -m http.server 8765
```

Open `http://localhost:8765/web/`. Type any six cards. The page maps them onto the stored suit pattern and ranks the 15 splits. A link such as `http://localhost:8765/web/?hand=AsAhKdKc7d3c` opens that hand. `web/` reads `data/sixmax/hands.bin` and `summary.json`.

Git ignores the local catalogue and generated Pages bundle. With Node.js installed, run `node tools/pack_pages.mjs` after the solve finishes. It creates a small manifest, compressed hand chunks, a Browse index, and precomputed Insights under `data/pages/`. The published app loads the manifest on startup, one chunk per lookup, and the Browse index or Insights only when opened. It caches data by content hash, so a new solve updates only assets whose contents changed.

To publish, stage the generated bundle with `git add -f -A -- data/pages/` and use the branch root as the GitHub Pages source. On localhost, the default still reads the live catalogue in `data/sixmax/`. To test the generated bundle locally, open `http://localhost:8765/web/?pack=1`.

A row's EV is what you get by **committing** to that split while the opponents play the equilibrium for the deal. The separate equilibrium value is higher, because a player who can see every hand adapts the split to that deal. The book is zero-sum, so that adaptive value averages to 0 across all hands.

## Rules as implemented

- **Hold'em hand:** the best 5 of the 7 cards (2 hole cards + 5 board cards), using any combination,
  including playing the board.
- **PLO hand:** standard Omaha, exactly 2 of the 4 hole cards plus exactly 3 board cards.
- **Scoop:** strictly beating every other player in both hands. A tie for best in either hand means no
  scoop.
- **Payoffs (net, in BB):** scooper +(N−1), everyone else −1. If nobody scoops, all antes are returned
  and everyone gets 0. The game is constant-sum.
- **Board:** 5 cards from the deck minus all hole cards, with no burn cards.

## How it works

**Evaluator (`src/evaluator.*`).** Every card has a 64-bit additive key. It holds 13 three-bit
rank-count fields and 4 four-bit suit-count fields, plus an OR-able rank mask for each suit. A hand's key
is the sum of its card keys, so a PLO combination costs one 64-bit add of a precomputed hole-pair key
and a precomputed board-triple key.

Flushes are detected with a single add-and-mask operation and resolved through an 8192-entry table.
Everything else goes through a perfect hash over the rank-count field (hash-and-displace, built at
startup in about 80 ms). There are two tables: 32 KB for 5-card hands and 256 KB for 7-card hands. A
lookup costs two multiplies and two loads, with no probing.

**Engine (`src/engine.*`).** For each board, each player gets all 15 split outcomes in one pass:

- 150 five-card evaluations: 15 hole pairs × 10 board triples. Each PLO split takes the best of the 6
  pairs inside its 4 cards.
- 15 seven-card evaluations for the Hold'em pairs.

Results are stored as `(holdem << 16) | plo` for every (board, player, split). Every question the
solver asks is answered from that **one shared board**, so the scoop event is always joint. Marginal
equities are never multiplied.

**Solver (`src/solver.*`).** Because players randomise independently, the EV of any split against any
mixed opponent profile is exact over the sampled boards:

```text
P(k scoops with u) = Σ_boards Π_{j≠k} P(j's split loses both hands to k's u)
EV_i(s)            = (N−1)·P(i scoops | s) − Σ_{k≠i} P(k scoops | i plays s)
```

The solver builds the payoff tensor over split profiles, either all 15^N or a restricted subset, by
expanding per-board dominance masks. It then runs regret-matching+ on that tensor. There are two modes:

- **`full`:** the entire 15^N game, with every pure Nash equilibrium enumerated. This is the default in
  `deal` mode for N ≤ 4.
- **`oracle`:** double oracle. Starting from best responses to random play, it solves the restricted
  game, adds each player's best response over all 15 splits, and repeats until no player gains.

Each solution reports its **NashConv**: the total EV all players could gain by deviating. 0 means an
exact Nash equilibrium on the board sample.

## Performance (i5-12600KF, 16 threads)

| task | time |
|---|---|
| 5-card evaluator | ~650–850 M evals/s per thread |
| board engine, 4 players | ~3.6–4 M player-boards/s per thread (~2.3–3 G 5-card evals/s on 16 threads) |
| `deal`, 4 players, full 15^4 = 50,625-profile game × 20k boards | 0.1–0.15 s |
| `deal`, 5 players, full 15^5 = 759,375 profiles × 20k boards | 1.3 s |
| `deal`, 2 players, all 658,008 boards enumerated | 0.25 s |
| `hand`, 4 players, 1000 deals × 2000 boards | 1.1 s |
| `hand`, 6 players, 1000 deals × 2000 boards | 4.7 s |
| `stats`, 4 players, 2000 / 8000 deals | 2.5 s / 8.6 s |
| `selftest` (all 133,784,560 seven-card hands, etc.) | 2 s |

On Windows 11 the program opts out of "EcoQoS" power throttling at startup. Without that, a run
launched from a background terminal is moved to efficiency cores after about 3 seconds and runs about
3× slower. `scoop bench sustained` prints throughput once per second, so you can check your machine.

## Example: `scoop hand AsAhKdQd7c6c`

```text
Scoop solver - hand analysis
Hand: AsAhKdQd7c6c   (one pair, suits 2-2-1-1)   players: 4   pot: 4 BB
Opponent deals: 1000 random, 2000 sampled boards each (120M Hold'em+PLO split outcomes)   [1.16s, 16 threads]
Per deal every player picks a Nash-equilibrium split (simultaneous moves, perfect play);
each row is this hand's EV for committing to that split vs. the opponents' equilibrium splits.
Pure equilibrium in 52% of deals, mean NashConv 0.00033 BB, hero equilibrium EV +0.2689 BB
Solver per deal: 3.3 double-oracle rounds, 41 restricted profiles, 1007 regret-matching iters

  rank split  HE | PLO          shape                           EV(BB)     +/-    scoop     chop     lose  eq-freq
  1    AsAh | KdQd7c6c          pocket pair / double-suited    +0.2671  0.0057    12.7%    76.0%    11.3%    86.2%
  2    KdQd | AsAh7c6c          suited / single-suited         +0.1918  0.0054    10.8%    75.9%    13.3%    10.9%
  3    7c6c | AsAhKdQd          suited / single-suited         +0.0439  0.0044     6.2%    79.6%    14.2%     1.7%
  4    Kd7c | AsAhQd6c          offsuit / rainbow              +0.0409  0.0047     7.0%    76.2%    16.8%     0.2%
  5    Kd6c | AsAhQd7c          offsuit / rainbow              +0.0389  0.0047     6.9%    76.2%    16.9%     0.4%
  6    Qd7c | AsAhKd6c          offsuit / rainbow              +0.0199  0.0044     6.3%    76.7%    17.0%     0.3%
  7    Qd6c | AsAhKd7c          offsuit / rainbow              +0.0196  0.0044     6.3%    76.8%    16.9%     0.0%
  8    AhQd | AsKd7c6c          offsuit / single-suited        +0.0130  0.0044     6.5%    75.3%    18.2%     0.0%
  9    AsQd | AhKd7c6c          offsuit / single-suited        +0.0130  0.0044     6.5%    75.3%    18.2%     0.1%
  10   AsKd | AhQd7c6c          offsuit / single-suited        +0.0080  0.0045     6.4%    75.3%    18.3%     0.0%
  11   AhKd | AsQd7c6c          offsuit / single-suited        +0.0080  0.0045     6.4%    75.3%    18.3%     0.1%
  12   As7c | AhKdQd6c          offsuit / single-suited        -0.0078  0.0046     6.1%    74.7%    19.1%     0.1%
  13   Ah7c | AsKdQd6c          offsuit / single-suited        -0.0078  0.0046     6.1%    74.7%    19.1%     0.0%
  14   As6c | AhKdQd7c          offsuit / single-suited        -0.0122  0.0046     6.0%    74.8%    19.2%     0.1%
  15   Ah6c | AsKdQd7c          offsuit / single-suited        -0.0122  0.0046     6.0%    74.8%    19.2%     0.1%
```

Aces in Hold'em with a double-suited KQ76 in PLO is clearly best (+0.27 BB per hand, about 13 standard
errors ahead of KQ-suited in Hold'em). Splitting the aces costs almost all of the hand's value.
Suit-isomorphic splits, such as `AhQd` and `AsQd`, come out identical, which is a useful consistency
check.

`examples/` also contains a full `deal` run and a `stats` run. Some headline aggregates (4 players):

- Pocket pairs are chosen for Hold'em 30% of the time, against 6% for a random split.
- With one pair, the pair goes to Hold'em 40% of the time and stays in PLO 46% of the time.
- The "always play the best Hold'em hand" heuristic gives up 0.037 BB/hand against the best split. A
  random split gives up 0.096 BB/hand.

## Modelling notes and limits

- **Perfect play** means the full-information simultaneous-move game for each deal: every player knows
  all hole cards and picks an equilibrium split. There is no range or Bayesian modelling.
- **What `hand` measures:** EV(split | opponents play the equilibrium for that deal), averaged over
  random opponent deals. The "hero equilibrium EV" line is higher than the top row because the
  equilibrium lets the hero adapt the split to each deal.
- **Equilibrium quality:** for N > 2, regret matching is not guaranteed to reach an exact Nash
  equilibrium, so NashConv is always printed. Typical values are around 1e-3 BB, far below the
  Monte Carlo noise.
- **Mixed equilibria** are common. They mostly mix between nearly identical splits (for example,
  swapping same-rank cards of different suits), or between splits that only matter on boards where
  nobody scoops.
- **Large games:** `--solver full` at 6 players is 11.4M profiles and about 550 MB. Use the default
  double oracle there.
