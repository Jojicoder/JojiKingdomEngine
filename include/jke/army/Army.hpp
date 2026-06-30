#pragma once
#include <string_view>
#include <vector>
#include "jke/army/Unit.hpp"
#include "jke/terrain/TerrainType.hpp"

namespace jke {

enum class ArmyRole : uint8_t {
    Reserve = 0,
    Defense = 1,
    Attack  = 2,
    Siege   = 3,
};

constexpr std::string_view armyRoleName(ArmyRole r) noexcept {
    switch(r) {
        case ArmyRole::Reserve: return "Reserve";
        case ArmyRole::Defense: return "Defense";
        case ArmyRole::Attack:  return "Attack";
        case ArmyRole::Siege:   return "Siege";
    }
    return "Unknown";
}

// Per-unit terrain combat multiplier table
// [UnitType][TerrainType]
constexpr float TERRAIN_MULT[6][8] = {
    //            Ocean   Coast   Plain   Forest  Hill    Mountain  River   Lake
    /* Militia  */{ 0.0f, 0.8f,  1.0f,   0.9f,   0.8f,   0.6f,   0.9f,   0.0f },
    /* Infantry */{ 0.0f, 0.9f,  1.0f,   1.0f,   1.1f,   1.1f,   0.9f,   0.0f },
    /* Spearmen */{ 0.0f, 0.9f,  1.0f,   0.9f,   1.0f,   1.0f,   0.9f,   0.0f },
    /* Archers  */{ 0.0f, 1.0f,  1.0f,   0.9f,   1.2f,   1.2f,   1.0f,   0.0f },
    /* Cavalry  */{ 0.0f, 0.7f,  1.3f,   0.8f,   0.7f,   0.5f,   0.8f,   0.0f },
    /* Siege    */{ 0.0f, 0.8f,  1.0f,   0.8f,   0.7f,   0.6f,   0.8f,   0.0f },
};

struct Army {
    ArmyID            id            = NO_ARMY;
    KingdomID         owner         = NO_KINGDOM;
    std::vector<Unit> units;

    Coordinate        position      = {0, 0};
    TileID            currentTile   = NO_TILE;
    TileID            targetTile    = NO_TILE;
    std::vector<TileID> movementPath;
    ArmyRole          role          = ArmyRole::Reserve;

    float             supplyLevel        = 1.0f;   // 0.0 – 1.0
    float             movementPoints     = 3.0f;   // refreshed each turn; spent per tile moved
    uint16_t          pathCursor         = 0;      // next index into movementPath (avoids erase)
    bool              isInBattle    = false;
    bool              isBesieging   = false;
    // Mercenary fields
    bool              isMercenary   = false;
    TurnNumber        contractUntil = 0;    // turn when contract expires
    CityID            siegeTarget   = NO_CITY;
    float             siegeProgress = 0.0f;   // 0.0 – 1.0

    uint32_t          totalSoldiers() const noexcept {
        uint32_t total = 0;
        for (const auto& u : units) total += u.soldiers;
        return total;
    }

    // Dynamic combat strength (never cached)
    float combatStrength(TerrainType terrain, bool isDefender) const noexcept {
        float strength = 0.0f;
        for (const auto& u : units) {
            int tt = static_cast<int>(u.type);
            int tr = static_cast<int>(terrain);
            float terrMult = (tt < 6 && tr < 8) ? TERRAIN_MULT[tt][tr] : 1.0f;
            float base = u.baseCombatPower() * terrMult;
            // Defenders get +20% on non-plain terrain
            if (isDefender && terrain != TerrainType::Plain && terrain != TerrainType::Ocean) {
                base *= 1.2f;
            }
            strength += base;
        }
        return strength;
    }

    bool isEmpty() const noexcept { return totalSoldiers() == 0; }
};

} // namespace jke
