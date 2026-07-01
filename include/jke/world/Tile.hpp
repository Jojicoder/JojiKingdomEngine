#pragma once
#include "jke/core/Types.hpp"
#include "jke/terrain/TerrainType.hpp"
#include <string_view>

namespace jke {

enum class StrategicPointType : uint8_t {
    None = 0,
    MountainPass,
    Bridge,
    RiverFord,
    HarborSite,
    SupplyDepot
};

constexpr std::string_view strategicPointName(StrategicPointType t) noexcept {
    switch (t) {
        case StrategicPointType::None:         return "None";
        case StrategicPointType::MountainPass: return "Mountain Pass";
        case StrategicPointType::Bridge:       return "Bridge";
        case StrategicPointType::RiverFord:    return "River Ford";
        case StrategicPointType::HarborSite:   return "Harbor Site";
        case StrategicPointType::SupplyDepot:  return "Supply Depot";
    }
    return "Unknown";
}

struct Tile {
    TileID       id               = NO_TILE;
    Coordinate   position         = {0, 0};
    TerrainType  terrain          = TerrainType::Plain;
    KingdomID    owner            = NO_KINGDOM;
    CityID       city             = NO_CITY;
    ArmyID       army             = NO_ARMY;  // army currently on tile (or NO_ARMY)
    float        elevation        = 0.0f;
    float        fertility        = 1.0f;
    float        resourceRichness = 1.0f;
    bool         hasRiver         = false;
    bool         isContested      = false;
    StrategicPointType strategicPoint = StrategicPointType::None;
    float        strategicValue   = 0.0f;
};

} // namespace jke
