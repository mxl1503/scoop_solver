#pragma once
// Lookup-table poker hand evaluator.
//
// Every card has a 64-bit additive key:
//   bits  0..38 : 13 rank-count fields, 3 bits each (count <= 4 never carries)
//   bits 40..55 : 4 suit-count fields, 4 bits each (count <= 7 never carries)
// plus a 64-bit OR-able suit/rank mask (16 bits per suit). Summing the keys of any
// 5 or 7 cards yields the rank multiset and the per-suit counts in one integer, so
// combining a precomputed "hole pair" key with a precomputed "board triple" key is
// a single addition. Non-flush hands are resolved through a perfect hash over the
// rank-count field (2 multiplies + 2 L1/L2 loads, no probing); flushes through an
// 8192-entry table indexed by the 13-bit rank mask of the flush suit.
//
// Returned values are dense hand strengths in [1, 7462]; higher is better.

#include <bit>
#include <cstdint>

namespace scoop {

struct CardKey {
    uint64_t key = 0;
    uint64_t mask = 0;
    CardKey operator+(const CardKey& o) const { return {key + o.key, mask | o.mask}; }
};

constexpr uint64_t kRankFieldMask = (1ull << 39) - 1;
constexpr uint64_t kSuitAdd = (3ull << 40) | (3ull << 44) | (3ull << 48) | (3ull << 52);
constexpr uint64_t kSuitTest = (8ull << 40) | (8ull << 44) | (8ull << 48) | (8ull << 52);

constexpr int kH5BucketBits = 12, kH5SlotBits = 14;
constexpr int kH7BucketBits = 15, kH7SlotBits = 17;

namespace tables {
alignas(64) inline uint16_t h5disp[1 << kH5BucketBits];
alignas(64) inline uint16_t h5val[1 << kH5SlotBits];
alignas(64) inline uint16_t h7disp[1 << kH7BucketBits];
alignas(64) inline uint16_t h7val[1 << kH7SlotBits];
alignas(64) inline uint16_t flush[8192];
inline uint64_t h5m1, h5m2, h7m1, h7m2;
inline CardKey cards[52];
inline uint8_t category[7463];
}  // namespace tables

void initEvaluator();

inline CardKey cardKey(int c) { return tables::cards[c]; }

inline uint16_t flushValue(uint64_t f, uint64_t mask) {
    const int suit = (std::countr_zero(f) - 43) >> 2;
    return tables::flush[(mask >> (16 * suit)) & 0x1FFF];
}

// Exactly 5 cards.
inline uint16_t eval5(uint64_t key, uint64_t mask) {
    const uint64_t f = (key + kSuitAdd) & kSuitTest;
    if (f) [[unlikely]] return flushValue(f, mask);
    const uint64_t k = key & kRankFieldMask;
    const uint32_t b = uint32_t((k * tables::h5m1) >> (64 - kH5BucketBits));
    const uint32_t s = (uint32_t((k * tables::h5m2) >> (64 - kH5SlotBits)) + tables::h5disp[b]) &
                       ((1u << kH5SlotBits) - 1);
    return tables::h5val[s];
}

// Exactly 7 cards (best 5 of 7). With 7 cards a flush excludes quads / full house,
// so the flush table alone is authoritative when a 5+ suit is present.
inline uint16_t eval7(uint64_t key, uint64_t mask) {
    const uint64_t f = (key + kSuitAdd) & kSuitTest;
    if (f) [[unlikely]] return flushValue(f, mask);
    const uint64_t k = key & kRankFieldMask;
    const uint32_t b = uint32_t((k * tables::h7m1) >> (64 - kH7BucketBits));
    const uint32_t s = (uint32_t((k * tables::h7m2) >> (64 - kH7SlotBits)) + tables::h7disp[b]) &
                       ((1u << kH7SlotBits) - 1);
    return tables::h7val[s];
}

// 0 = high card ... 8 = straight flush
inline int handCategory(uint16_t v) { return tables::category[v]; }
const char* categoryName(int cat);

// Reference (slow, obviously-correct) evaluator used for table construction and tests.
// Returns a comparable 32-bit value: category << 20 | tiebreak.
uint32_t slowEval5(const int ranks[5], bool flush);
uint16_t denseFromSlow(uint32_t slow);

}  // namespace scoop
