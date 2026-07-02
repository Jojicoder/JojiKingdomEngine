#pragma once
#include "jke/core/Types.hpp"

namespace jke {

struct BattleResult {
    KingdomID victor               = NO_KINGDOM;
    KingdomID loser                = NO_KINGDOM;
    ArmyID    attackerArmy         = NO_ARMY;
    ArmyID    defenderArmy         = NO_ARMY;
    float     attackerCasualties   = 0.0f;  // proportion 0.0–1.0
    float     defenderCasualties   = 0.0f;
    float     attackerMoraleChange = 0.0f;
    float     defenderMoraleChange = 0.0f;
    bool      cityConquered        = false;
    CityID    conqueredCity        = NO_CITY;
    bool      capitulated          = false;  // 無血開城 (民衆疲弊による)
    bool      attackerRetreated    = false;
    bool      defenderRetreated    = false;
    EventID   historyEventID       = 0;
    float     attackerStrength     = 0.0f;  // for history logging
    float     defenderStrength     = 0.0f;
};

} // namespace jke
