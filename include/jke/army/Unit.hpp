#pragma once
#include <string_view>
#include "jke/core/Types.hpp"

namespace jke {

enum class UnitType : uint8_t {
    Militia    = 0,
    Infantry   = 1,
    Spearmen   = 2,
    Archers    = 3,
    Cavalry    = 4,
    SiegeUnit  = 5,
};

constexpr std::string_view unitName(UnitType t) noexcept {
    switch(t) {
        case UnitType::Militia:   return "Militia";
        case UnitType::Infantry:  return "Infantry";
        case UnitType::Spearmen:  return "Spearmen";
        case UnitType::Archers:   return "Archers";
        case UnitType::Cavalry:   return "Cavalry";
        case UnitType::SiegeUnit: return "Siege Unit";
    }
    return "Unknown";
}

// Recruitment cost per soldier (base values)
struct UnitCost {
    float gold  = 0;
    float food  = 0;
    float iron  = 0;
    float wood  = 0;
};

constexpr UnitCost unitRecruitCost(UnitType t) noexcept {
    switch(t) {
        case UnitType::Militia:   return {0.02f, 0.05f, 0.00f, 0.00f};
        case UnitType::Infantry:  return {0.05f, 0.05f, 0.03f, 0.00f};
        case UnitType::Spearmen:  return {0.04f, 0.05f, 0.04f, 0.01f};
        case UnitType::Archers:   return {0.05f, 0.05f, 0.01f, 0.02f};
        case UnitType::Cavalry:   return {0.10f, 0.08f, 0.03f, 0.00f};
        case UnitType::SiegeUnit: return {0.08f, 0.03f, 0.05f, 0.08f};
    }
    return {0,0,0,0};
}

// Upkeep per turn per 1000 soldiers
constexpr float unitUpkeepPerK(UnitType t) noexcept {
    switch(t) {
        case UnitType::Militia:   return 1.0f;
        case UnitType::Infantry:  return 2.0f;
        case UnitType::Spearmen:  return 2.0f;
        case UnitType::Archers:   return 2.5f;
        case UnitType::Cavalry:   return 5.0f;
        case UnitType::SiegeUnit: return 3.0f;
    }
    return 1.0f;
}

struct Unit {
    UnitID   id          = 0;
    UnitType type        = UnitType::Infantry;
    uint32_t soldiers    = 1000;
    float    training    = 0.5f;   // 0.0 – 1.0
    float    equipment   = 0.5f;
    float    experience  = 0.0f;
    float    morale      = 1.0f;
    float    supply      = 1.0f;

    // Base combat power for this unit (without terrain/tech)
    float baseCombatPower() const noexcept {
        return soldiers * training * equipment * (0.5f + experience * 0.5f) * morale * supply;
    }
};

} // namespace jke
