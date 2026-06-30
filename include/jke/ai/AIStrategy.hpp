#pragma once
#include <vector>
#include <unordered_map>
#include "jke/ai/AIDecision.hpp"
#include "jke/nation/Kingdom.hpp"
#include "jke/city/City.hpp"
#include "jke/army/Army.hpp"
#include "jke/world/WorldMap.hpp"
#include "jke/history/Timeline.hpp"
#include "jke/diplomacy/DiplomaticRelation.hpp"

namespace jke {

using RelationMap = std::unordered_map<KingdomID,
                       std::unordered_map<KingdomID, DiplomaticRelation>>;

// Read-only view of simulation state passed to AI each turn
struct AIContext {
    const Kingdom&                                    self;
    const std::unordered_map<KingdomID, Kingdom>&     kingdoms;
    const std::unordered_map<CityID, City>&           cities;
    const std::unordered_map<ArmyID, Army>&           armies;
    const WorldMap&                                   worldMap;
    const Timeline&                                   timeline;
    const RelationMap&                                relations;
    TurnNumber                                        turn;
};

// Abstract base — one subclass per personality
class AIStrategy {
public:
    virtual ~AIStrategy() = default;

    // Returns prioritized list of decisions (SimulationEngine executes top feasible ones)
    virtual std::vector<AIDecision> evaluate(const AIContext& ctx) const = 0;

protected:
    // Shared utility helpers
    static KingdomID findWeakestNeighbor(const AIContext& ctx);
    static bool atWarWith(const AIContext& ctx, KingdomID other);
    static bool alliedWith(const AIContext& ctx, KingdomID other);
    static float threatLevel(const AIContext& ctx, KingdomID from);
    static std::vector<KingdomID> enemies(const AIContext& ctx);
    static std::vector<KingdomID> allies(const AIContext& ctx);
    static KingdomID findBestAllyTarget(const AIContext& ctx);
    static TechID    bestAvailableTech(const AIContext& ctx);
};

} // namespace jke
