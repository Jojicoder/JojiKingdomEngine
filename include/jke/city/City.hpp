#pragma once
#include <string>
#include <vector>
#include "jke/core/Types.hpp"
#include "jke/economy/ResourceLedger.hpp"

namespace jke {

enum class CityType : uint8_t {
    Generic     = 0,  // default
    Port        = 1,  // coastal — gold income, extended supply
    Fortress    = 2,  // hill/mountain — high fortification, hard to siege
    Agricultural= 3,  // river/fertile plain — food + population
    Mining      = 4,  // resource-rich — iron + stone
    TradeHub    = 5,  // capital or crossroads — gold + stability
};

constexpr std::string_view cityTypeName(CityType t) noexcept {
    switch (t) {
        case CityType::Generic:      return "Town";
        case CityType::Port:         return "Port";
        case CityType::Fortress:     return "Fortress";
        case CityType::Agricultural: return "Farmland";
        case CityType::Mining:       return "Mine";
        case CityType::TradeHub:     return "Trade Hub";
    }
    return "Unknown";
}

enum class BuildingType : uint8_t {
    Farm         = 0,
    LumberMill   = 1,
    Quarry       = 2,
    IronMine     = 3,
    Market       = 4,
    Barracks     = 5,
    Walls        = 6,
    Fortress     = 7,
    Temple       = 8,
    Library      = 9,
    Aqueduct     = 10,
    Granary      = 11,
    Workshop     = 12,
};

constexpr std::string_view buildingName(BuildingType b) noexcept {
    switch(b) {
        case BuildingType::Farm:        return "Farm";
        case BuildingType::LumberMill:  return "Lumber Mill";
        case BuildingType::Quarry:      return "Quarry";
        case BuildingType::IronMine:    return "Iron Mine";
        case BuildingType::Market:      return "Market";
        case BuildingType::Barracks:    return "Barracks";
        case BuildingType::Walls:       return "Walls";
        case BuildingType::Fortress:    return "Fortress";
        case BuildingType::Temple:      return "Temple";
        case BuildingType::Library:     return "Library";
        case BuildingType::Aqueduct:    return "Aqueduct";
        case BuildingType::Granary:     return "Granary";
        case BuildingType::Workshop:    return "Workshop";
    }
    return "Unknown";
}

struct CityBuilding {
    BuildingType type      = BuildingType::Farm;
    uint8_t      level     = 1;   // 1–5
    float        condition = 1.0f; // 0.0 – 1.0
};

struct City {
    CityID       id              = NO_CITY;
    KingdomID    owner           = NO_KINGDOM;
    KingdomID    originalOwner   = NO_KINGDOM;
    std::string  name;
    Coordinate   position        = {0, 0};
    TileID       tile            = NO_TILE;
    CityType     cityType        = CityType::Generic;

    uint32_t     population      = 1000;
    float        happiness       = 0.8f;   // 0.0 – 1.0
    float        fortification   = 0.1f;   // 0.0 – 1.0
    float        taxRate         = 0.15f;  // 0.0 – 0.5

    ResourceLedger baseProduction;         // per-turn before modifiers

    std::vector<CityBuilding> buildings;

    bool         isCapital       = false;
    bool         isRuined        = false;
    bool         underSiege      = false;
    TurnNumber   foundedTurn     = 0;
    TurnNumber   lastConquered   = 0;

    // Culture system
    // 0.0 = fully foreign, 1.0 = fully assimilated to current owner
    float        cultureAssimilation = 1.0f;
    KingdomID    cultureOwner        = NO_KINGDOM; // original culture source

    // Siege state
    float        siegeFoodStores     = 1.0f;  // 0.0-1.0; depletes when besieged
    int          siegeTurns          = 0;     // consecutive turns under siege

    // Compute effective production (base * building bonuses)
    ResourceLedger effectiveProduction() const;
    float          rebellionRisk() const;
};

} // namespace jke
