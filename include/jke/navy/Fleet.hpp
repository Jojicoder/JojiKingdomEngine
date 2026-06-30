#pragma once
#include "jke/core/Types.hpp"

namespace jke {

struct Fleet {
    FleetID   id         = NO_FLEET;
    KingdomID owner      = NO_KINGDOM;
    TileID    tile       = NO_TILE;   // current position — must be Coast or Ocean
    ArmyID    cargo      = NO_ARMY;   // embarked army (NO_ARMY = empty transport)
    uint32_t  hull       = 1000;      // structural integrity; 0 = sunk
    int       moveCd     = 0;         // cooldown before next move (turns)
    bool      inPort     = true;      // anchored at a Port city
    CityID    homePort   = NO_CITY;   // Port city this fleet belongs to
};

} // namespace jke
