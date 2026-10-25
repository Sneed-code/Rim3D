#pragma once

#include "ColonySim/Random.h"

#include <cstdint>
#include <cstring>

namespace colony {

// Builds a 64-bit hash from a sequence of values, for state hashes and def fingerprints. Order
// matters. Floats are hashed by their bits, so any difference at all shows up.
class StateHasher {
public:
    void add(uint64_t value) { hash_ = splitMix64(hash_ ^ value); }
    void add(int64_t value) { add(static_cast<uint64_t>(value)); }
    void add(int value) { add(static_cast<uint64_t>(static_cast<int64_t>(value))); }
    void add(uint32_t value) { add(static_cast<uint64_t>(value)); }
    void add(bool value) { add(static_cast<uint64_t>(value ? 1 : 0)); }
    void add(float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        add(static_cast<uint64_t>(bits));
    }
    void add(const char* text, size_t length) {
        add(static_cast<uint64_t>(length));
        for (size_t i = 0; i < length; ++i) {
            add(static_cast<uint64_t>(static_cast<unsigned char>(text[i])));
        }
    }

    uint64_t value() const { return hash_; }

private:
    uint64_t hash_ = 0x6A09E667F3BCC908ULL;
};

}  // namespace colony
