#pragma once

#include "ColonySim/Api.h"

#include <cstdint>

namespace colony {

// How long a bill goes on, as RimWorld's repeat modes (docs/design/bills.md). Here rather than in
// Bills.h so commands and the snapshot can name it without the bill system. New modes go at the
// end: saves store the number.
enum class BillMode : uint8_t {
    Count,      // "Do X times": `count` more times; the bill removes itself when it reaches 0
    UntilHave,  // "Do until you have X": while fewer than `count` of the product lie on the map
    Forever,    // "Do forever"
};

COLONY_API const char* billModeName(BillMode mode);

}  // namespace colony
