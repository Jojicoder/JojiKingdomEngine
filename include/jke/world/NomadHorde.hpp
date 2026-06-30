#pragma once
#include "jke/core/Types.hpp"

namespace jke {

struct NomadHorde {
    bool       active     = false;
    TileID     tile       = NO_TILE;
    uint32_t   strength   = 6000;   // cavalry count
    int        moveCd     = 0;
    CityID     target     = NO_CITY;
    TurnNumber spawnTurn  = 0;
    int        raidCd     = 0;      // turns until next raid
    int        turnsActive = 0;     // dissolves after ~120 turns or <500 strength
};

} // namespace jke
