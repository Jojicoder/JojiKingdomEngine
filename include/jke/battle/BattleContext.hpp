#pragma once
#include "jke/core/Types.hpp"
#include "jke/terrain/TerrainType.hpp"
#include "jke/world/Season.hpp"

namespace jke {

struct BattleContext {
    ArmyID      attackerArmy      = NO_ARMY;
    ArmyID      defenderArmy      = NO_ARMY;
    KingdomID   attackerKingdom   = NO_KINGDOM;
    KingdomID   defenderKingdom   = NO_KINGDOM;
    TileID      battleTile        = NO_TILE;
    TerrainType terrain           = TerrainType::Plain;
    CityID      contestedCity     = NO_CITY;
    bool        isSiege           = false;
    TurnNumber  turn              = 0;
    Season      season            = Season::Summer;
};

} // namespace jke
