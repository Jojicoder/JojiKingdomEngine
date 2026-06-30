#pragma once
#include <string_view>
#include "jke/core/Types.hpp"

namespace jke {

enum class Season : uint8_t {
    Spring = 0,
    Summer = 1,
    Autumn = 2,
    Winter = 3,
};

constexpr std::string_view seasonName(Season s) noexcept {
    switch (s) {
        case Season::Spring: return "Spring";
        case Season::Summer: return "Summer";
        case Season::Autumn: return "Autumn";
        case Season::Winter: return "Winter";
    }
    return "?";
}

struct SeasonModifiers {
    float foodMult   = 1.0f;  // multiplier on food income
    float goldMult   = 1.0f;
    float armySpeed  = 1.0f;  // multiplier on movement speed
    float supplyDecay= 1.0f;  // multiplier on supply decay rate
    float moraleDelta= 0.0f;  // per-turn morale change
};

inline SeasonModifiers seasonMods(Season s) noexcept {
    switch (s) {
        case Season::Spring: return {1.20f, 1.00f, 1.10f, 0.85f,  0.005f};
        case Season::Summer: return {1.30f, 1.10f, 1.00f, 1.00f,  0.000f};
        case Season::Autumn: return {1.10f, 1.05f, 0.90f, 1.10f,  0.000f};
        case Season::Winter: return {0.60f, 0.90f, 0.60f, 1.40f, -0.008f};
    }
    return {};
}

} // namespace jke
