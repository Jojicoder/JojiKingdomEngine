#pragma once
#include "jke/core/Types.hpp"

namespace jke {

struct BanditGroup {
    uint32_t   id         = 0;
    TileID     tile       = NO_TILE;
    uint32_t   strength   = 200;   // pseudo-soldiers
    int        moveCd     = 0;     // cooldown before next move
    int        raidCd     = 0;     // cooldown before next raid
};

} // namespace jke
