#pragma once
#include <string>
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
    Vanguard = 4,
    Flanker = 5,
    Garrison = 6,
    SupplyGuard = 7,
};

constexpr std::string_view armyRoleName(ArmyRole r) noexcept {
    switch(r) {
        case ArmyRole::Reserve: return "Reserve";
        case ArmyRole::Defense: return "Defense";
        case ArmyRole::Attack:  return "Attack";
        case ArmyRole::Siege:   return "Siege";
        case ArmyRole::Vanguard: return "Vanguard";
        case ArmyRole::Flanker: return "Flanker";
        case ArmyRole::Garrison: return "Garrison";
        case ArmyRole::SupplyGuard: return "SupplyGuard";
    }
    return "Unknown";
}

struct ArmyCommander {
    std::string name;
    float attack     = 1.0f;
    float defense    = 1.0f;
    float cavalry    = 1.0f;
    float morale     = 1.0f;
    float pursuit    = 1.0f;
    float experience = 0.0f;
    float fame       = 0.0f;
    bool  fameEventLogged = false;
    bool  wounded    = false;
    bool  alive      = false;
};

struct ArmyStrategist {
    std::string name;
    float siege      = 1.0f;
    float ambush     = 1.0f;
    float retreat    = 1.0f;
    float logistics  = 1.0f;
    float lure       = 1.0f;
    float experience = 0.0f;
    float fame       = 0.0f;
    bool  fameEventLogged = false;
    bool  wounded    = false;
    bool  alive      = false;
};

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
    ArmyCommander     commander;
    ArmyStrategist    strategist;

    float             supplyLevel        = 1.0f;   // 0.0 – 1.0
    float             movementPoints     = 3.0f;   // refreshed each turn; spent per tile moved
    uint16_t          pathCursor         = 0;      // next index into movementPath (avoids erase)
    bool              recoveringSupply   = false;
    TurnNumber        supplyRetreatUntil = 0;
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
    bool hasCommander() const noexcept { return commander.alive; }
    bool hasStrategist() const noexcept { return strategist.alive; }
    bool hasNotableLeader() const noexcept {
        return (commander.alive && commander.fame >= 0.45f) ||
               (strategist.alive && strategist.fame >= 0.45f);
    }
};

} // namespace jke
