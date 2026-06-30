#pragma once
#include <string>
#include "jke/core/Types.hpp"

namespace jke {

enum class AIDecisionType : uint8_t {
    DoNothing       = 0,
    Expand          = 1,
    Recruit         = 2,
    Attack          = 3,
    Defend          = 4,
    UpgradeCity     = 5,
    ImproveEconomy  = 6,
    ResearchTech    = 7,
    FormAlliance    = 8,
    BreakAlliance   = 9,
    NegotiatePeace  = 10,
    DeclareWar      = 11,
    BuildSiegeArmy  = 12,
    SiegeCity       = 13,
    MergeArmies     = 14,
    OfferVassal     = 15,
    HireMercenary  = 16,
};

struct AIDecision {
    AIDecisionType type            = AIDecisionType::DoNothing;
    KingdomID      actor           = NO_KINGDOM;
    KingdomID      target          = NO_KINGDOM;   // target kingdom
    CityID         targetCity      = NO_CITY;
    TileID         targetTile      = NO_TILE;
    ArmyID         targetArmy      = NO_ARMY;
    TechID         targetTech      = NO_TECH;
    float          priority        = 0.0f;
    std::string    reasoning;
};

} // namespace jke
