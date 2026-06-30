#pragma once
#include "jke/core/Types.hpp"

namespace jke {

enum class RebellionCause : uint8_t {
    Famine           = 0,
    EconomicCollapse = 1,
    LowLegitimacy    = 2,
    Oppression       = 3,
    ForeignInfluence = 4,
    ConqueredCity    = 5,
};

struct Rebellion {
    KingdomID      targetKingdom      = NO_KINGDOM;
    CityID         epicenterCity      = NO_CITY;
    RebellionCause cause              = RebellionCause::Famine;
    float          strength           = 0.0f;    // 0.0 – 1.0
    TurnNumber     startedTurn        = 0;
    bool           escalatedToCivilWar = false;
    bool           suppressed         = false;
};

struct CivilWar {
    KingdomID            parentKingdom    = NO_KINGDOM;
    KingdomID            rebelFactionID   = NO_KINGDOM;
    std::vector<CityID>  rebelCities;
    std::vector<ArmyID>  rebelArmies;
    TurnNumber           startedTurn      = 0;
    bool                 resolved         = false;
};

} // namespace jke
