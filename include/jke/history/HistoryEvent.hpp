#pragma once
#include <string>
#include <unordered_map>
#include "jke/core/Types.hpp"

namespace jke {

enum class EventType {
    KingdomFounded,
    CityFounded,
    WarDeclared,
    PeaceSigned,
    AllianceFormed,
    AllianceBroken,
    TreatyFormed,
    BattleFought,
    CityConquered,
    CityDestroyed,
    CapitalCaptured,
    RebellionStarted,
    RebellionSuppressed,
    CivilWar,
    KingdomCollapsed,
    KingdomAnnexed,
    ContinentUnified,
    TechResearched,
    VassalFormed,
    VassalLiberated,
    WorldEventPositive,  // harvest, boom, general, unity
    WorldEventNegative,  // plague, drought, revolt, scandal
    SeasonChanged,       // spring/summer/autumn/winter transition
    PlagueSpread,        // plague spreading to neighboring kingdom
    BanditRaid,          // bandits raid a city
    NonAggressionSigned, // non-aggression pact established
    TributaryEstablished,// kingdom becomes tributary
    CityFounded2,        // city founded by expansion (AI-built)
};

struct HistoryEvent {
    EventID       id              = 0;
    EventType     type            = EventType::BattleFought;
    TurnNumber    turn            = 0;
    KingdomID     primaryKingdom  = NO_KINGDOM;
    KingdomID     secondaryKingdom= NO_KINGDOM;
    CityID        relatedCity     = NO_CITY;
    ArmyID        relatedArmy     = NO_ARMY;
    std::string   description;
    std::unordered_map<std::string, float> context; // numeric metadata
};

} // namespace jke
