#pragma once
#include "jke/core/Types.hpp"
#include "jke/terrain/TerrainType.hpp"

namespace jke {

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
};

} // namespace jke
