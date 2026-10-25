#pragma once

#include <cstdint>

namespace colony {

// SplitMix64: turns any 64-bit value into a well-mixed one. Used to derive
// independent seeds and to hash coordinates for map generation.
inline constexpr uint64_t splitMix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

// PCG32 random stream. All sim randomness goes through one of these so a
// seed plus the command log always replays the same game.
class Rng {
public:
    explicit Rng(uint64_t seed, uint64_t stream = 1) : state_(0), inc_((stream << 1u) | 1u) {
        next();
        state_ += seed;
        next();
    }

    uint32_t next() {
        const uint64_t old = state_;
        state_ = old * 6364136223846793005ULL + inc_;
        const uint32_t xorShifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot = static_cast<uint32_t>(old >> 59u);
        return (xorShifted >> rot) | (xorShifted << ((32u - rot) & 31u));
    }

    // Uniform integer in [0, bound), without modulo bias.
    uint32_t below(uint32_t bound) {
        if (bound == 0) {
            return 0;
        }
        const uint32_t threshold = (0u - bound) % bound;
        for (;;) {
            const uint32_t r = next();
            if (r >= threshold) {
                return r % bound;
            }
        }
    }

    // Uniform float in [0, 1).
    float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }

    float range(float lo, float hi) { return lo + (hi - lo) * unit(); }

    // The whole state, for state hashes and saves.
    uint64_t state() const { return state_; }
    uint64_t increment() const { return inc_; }

private:
    uint64_t state_;
    uint64_t inc_;
};

}  // namespace colony
