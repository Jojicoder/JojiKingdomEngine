#pragma once
#include <string>
#include <vector>
#include "jke/core/Types.hpp"

namespace jke {

enum class RulerTrait : uint8_t {
    Brave       = 0,  // +10% combat
    Cowardly    = 1,  // -10% combat
    Greedy      = 2,  // +15% gold, -stability
    Generous    = 3,  // -5% gold, +stability
    Wise        = 4,  // +stability, +research
    Foolish     = 5,  // -stability
    Ambitious   = 6,  // +aggression
    Diplomatic  = 7,  // +trust gains
    Cruel       = 8,  // +combat, -foreign culture assimilation
    Pious       = 9,  // +legitimacy
};

constexpr std::string_view traitName(RulerTrait t) noexcept {
    switch (t) {
        case RulerTrait::Brave:      return "Brave";
        case RulerTrait::Cowardly:   return "Cowardly";
        case RulerTrait::Greedy:     return "Greedy";
        case RulerTrait::Generous:   return "Generous";
        case RulerTrait::Wise:       return "Wise";
        case RulerTrait::Foolish:    return "Foolish";
        case RulerTrait::Ambitious:  return "Ambitious";
        case RulerTrait::Diplomatic: return "Diplomatic";
        case RulerTrait::Cruel:      return "Cruel";
        case RulerTrait::Pious:      return "Pious";
    }
    return "Unknown";
}

struct Ruler {
    std::string              name;
    std::string              dynastyName;
    uint8_t                  age        = 35;
    TurnNumber               reignStart = 0;
    std::vector<RulerTrait>  traits;   // 1-3 traits
    bool                     hasHeir    = false;
    std::string              heirName;
    uint8_t                  heirAge    = 0;

    // Precomputed one-time multipliers from traits (avoids per-turn accumulation)
    float combatMult = 1.0f;  // Brave +12%, Cowardly -12%
    float goldMult   = 1.0f;  // Greedy +18%, Generous -8%
};

} // namespace jke
