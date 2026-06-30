#pragma once
#include <string>
#include <vector>
#include <unordered_set>
#include "jke/core/Types.hpp"
#include "jke/economy/ResourceLedger.hpp"
#include "jke/nation/Ruler.hpp"

namespace jke {

enum class KingdomPersonality : uint8_t {
    Aggressive    = 0,
    Defensive     = 1,
    Economic      = 2,
    Diplomatic    = 3,
    Opportunistic = 4,
    Expansionist  = 5,
};

enum class KingdomSpecialization : uint8_t {
    Military     = 0,
    Economy      = 1,
    Agriculture  = 2,
    Technology   = 3,
    Trade        = 4,
    Defense      = 5,
};

enum class NationalPolicy : uint8_t {
    Rebuilding        = 0,
    Defending         = 1,
    Invading          = 2,
    CoalitionBuilding = 3,
    FinalWar          = 4,
};

enum class StrategyPlan : uint8_t {
    HoldAndRecover    = 0,
    TurtleDefense     = 1,
    CapitalRush       = 2,
    BorderExpansion   = 3,
    OpportunisticRaid = 4,
    RevengeWar        = 5,
    AntiHegemonWar    = 6,
    TotalConquest     = 7,
};

constexpr std::string_view personalityName(KingdomPersonality p) noexcept {
    switch(p) {
        case KingdomPersonality::Aggressive:    return "Aggressive";
        case KingdomPersonality::Defensive:     return "Defensive";
        case KingdomPersonality::Economic:      return "Economic";
        case KingdomPersonality::Diplomatic:    return "Diplomatic";
        case KingdomPersonality::Opportunistic: return "Opportunistic";
        case KingdomPersonality::Expansionist:  return "Expansionist";
    }
    return "Unknown";
}

constexpr std::string_view specializationName(KingdomSpecialization s) noexcept {
    switch(s) {
        case KingdomSpecialization::Military:    return "Military";
        case KingdomSpecialization::Economy:     return "Economy";
        case KingdomSpecialization::Agriculture: return "Agriculture";
        case KingdomSpecialization::Technology:  return "Technology";
        case KingdomSpecialization::Trade:       return "Trade";
        case KingdomSpecialization::Defense:     return "Defense";
    }
    return "Unknown";
}

constexpr std::string_view nationalPolicyName(NationalPolicy p) noexcept {
    switch(p) {
        case NationalPolicy::Rebuilding:        return "Rebuilding";
        case NationalPolicy::Defending:         return "Defending";
        case NationalPolicy::Invading:          return "Invading";
        case NationalPolicy::CoalitionBuilding: return "Coalition";
        case NationalPolicy::FinalWar:          return "Final War";
    }
    return "Unknown";
}

constexpr std::string_view strategyPlanName(StrategyPlan p) noexcept {
    switch(p) {
        case StrategyPlan::HoldAndRecover:    return "Recover";
        case StrategyPlan::TurtleDefense:     return "Turtle";
        case StrategyPlan::CapitalRush:       return "Capital Rush";
        case StrategyPlan::BorderExpansion:   return "Border Push";
        case StrategyPlan::OpportunisticRaid: return "Surprise Raid";
        case StrategyPlan::RevengeWar:        return "Revenge";
        case StrategyPlan::AntiHegemonWar:    return "Anti-Hegemon";
        case StrategyPlan::TotalConquest:     return "Total Conquest";
    }
    return "Unknown";
}

struct Kingdom {
    KingdomID              id               = NO_KINGDOM;
    std::string            name;
    KingdomPersonality     personality      = KingdomPersonality::Aggressive;
    KingdomSpecialization  specialization   = KingdomSpecialization::Military;
    NationalPolicy         policy           = NationalPolicy::Rebuilding;
    StrategyPlan           strategyPlan     = StrategyPlan::HoldAndRecover;
    KingdomID              strategicTarget  = NO_KINGDOM;
    KingdomID              revengeTarget    = NO_KINGDOM;
    TurnNumber             revengeUntil     = 0;

    // Alive state
    bool                   isAlive          = true;
    KingdomID              annexedBy        = NO_KINGDOM;
    TurnNumber             foundedTurn      = 0;
    TurnNumber             collapsedTurn    = 0;

    // Cities / armies
    CityID                 capitalCity      = NO_CITY;
    std::vector<CityID>    cities;
    std::vector<ArmyID>    armies;

    // Resources
    ResourceLedger         treasury;
    ResourceLedger         perTurnIncome;
    ResourceLedger         perTurnExpense;

    // Population
    uint32_t               totalPopulation  = 0;

    // Stats (0.0 – 1.0 unless noted)
    float                  stability        = 1.0f;
    float                  morale           = 1.0f;
    float                  legitimacy       = 1.0f;
    float                  aggression       = 0.5f;  // personality-derived

    // Technology
    std::unordered_set<TechID> researchedTechs;
    TechID                 currentResearch  = NO_TECH;
    float                  researchProgress = 0.0f;

    // Modifiers derived from tech (recomputed each turn)
    float                  combatBonus      = 1.0f;
    float                  foodBonus        = 1.0f;
    float                  goldBonus        = 1.0f;
    float                  siegeBonus       = 1.0f;
    float                  defenseBonus     = 1.0f;

    // Tracks consecutive turns below starvation threshold
    int                    starvationTurns  = 0;

    // Consecutive turns in debt (gold < 0 before clamp)
    int                    debtTurns        = 0;

    // Consecutive turns without any active war
    int                    peaceTurns       = 0;

    // War declaration cooldown — prevents rapid re-declaration spam
    TurnNumber             lastWarDeclaredTurn = 0;

    // War exhaustion: rises each turn at war, decays in peace
    float                  warWeariness     = 0.0f;  // 0.0 – 1.0

    // Disease
    int                    plagueTurns      = 0;     // 0 = healthy, >0 = plague active
    bool                   plagueImmune     = false; // survived plague recently

    // Tributary relations
    KingdomID              tributaryOf      = NO_KINGDOM;  // NO_KINGDOM if independent
    TurnNumber             tributaryUntil   = 0;

    // City-building cooldown
    TurnNumber             lastCityFounded  = 0;

    // Culture: 0-7 cultural group (assigned at generation based on starting position)
    uint8_t                cultureGroup     = 0;

    // Ruler / dynasty
    Ruler                  ruler;
    bool                   hasRuler         = false;

    // True for rebel/civil-war factions (avoids repeated string search)
    bool                   isRebel          = false;

    int                    tileCount() const { return static_cast<int>(cities.size()); }
};

} // namespace jke
