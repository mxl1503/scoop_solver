#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace scoop {

// ---------------------------------------------------------------- cards
// card = rank * 4 + suit, rank 0 = '2' ... 12 = 'A', suits c d h s.
inline constexpr char kRankChars[] = "23456789TJQKA";
inline constexpr char kSuitChars[] = "cdhs";

inline int rankOf(int c) { return c >> 2; }
inline int suitOf(int c) { return c & 3; }

inline std::string cardStr(int c) { return {kRankChars[rankOf(c)], kSuitChars[suitOf(c)]}; }

inline int parseRank(char ch) {
    ch = char(std::toupper((unsigned char)ch));
    for (int r = 0; r < 13; r++)
        if (kRankChars[r] == ch) return r;
    return -1;
}
inline int parseSuit(char ch) {
    ch = char(std::tolower((unsigned char)ch));
    for (int s = 0; s < 4; s++)
        if (kSuitChars[s] == ch) return s;
    return -1;
}

// Parses "AsKd7h7c2s3d" (spaces / commas ignored, "10" accepted for T).
inline std::vector<int> parseCards(const std::string& text) {
    std::string t;
    for (size_t i = 0; i < text.size(); i++) {
        char ch = text[i];
        if (ch == ' ' || ch == ',' || ch == '-' || ch == '[' || ch == ']') continue;
        if (ch == '1' && i + 1 < text.size() && text[i + 1] == '0') { t += 'T'; i++; continue; }
        t += ch;
    }
    if (t.size() % 2) throw std::runtime_error("bad card string: " + text);
    std::vector<int> out;
    for (size_t i = 0; i < t.size(); i += 2) {
        const int r = parseRank(t[i]), s = parseSuit(t[i + 1]);
        if (r < 0 || s < 0) throw std::runtime_error("bad card '" + t.substr(i, 2) + "' in: " + text);
        out.push_back(r * 4 + s);
    }
    return out;
}

// ---------------------------------------------------------------- rng
struct Rng {
    uint64_t s[4];
    explicit Rng(uint64_t seed) {
        for (auto& x : s) {
            uint64_t z = (seed += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            x = z ^ (z >> 31);
        }
    }
    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
    uint64_t next() {  // xoshiro256**
        const uint64_t r = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3]; s[2] ^= t; s[3] = rotl(s[3], 45);
        return r;
    }
    uint32_t below(uint32_t n) { return uint32_t(((next() >> 32) * uint64_t(n)) >> 32); }
};

inline uint64_t mixSeed(uint64_t a, uint64_t b) {
    uint64_t z = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// ---------------------------------------------------------------- threads
inline int defaultThreads() { return std::max(1u, std::thread::hardware_concurrency()); }

// Runs f(taskIndex, threadIndex) for taskIndex in [0, n) with dynamic scheduling.
template <class F>
void parallelFor(int n, int threads, F&& f) {
    threads = std::min(threads, n);
    if (threads <= 1) {
        for (int i = 0; i < n; i++) f(i, 0);
        return;
    }
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (int t = 0; t < threads; t++)
        pool.emplace_back([&, t] {
            for (int i; (i = next.fetch_add(1, std::memory_order_relaxed)) < n;) f(i, t);
        });
    for (auto& th : pool) th.join();
}

}  // namespace scoop
