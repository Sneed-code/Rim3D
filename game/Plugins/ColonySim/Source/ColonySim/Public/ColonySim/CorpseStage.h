#pragma once

#include <cstdint>

namespace colony {

// How far a dead body has rotted (docs/design/spoilage.md "Corpses"). None for the living and machines.
enum class CorpseStage : uint8_t { None, Fresh, Rotting, Dessicated };

}  // namespace colony
