#pragma once
#include <unordered_map>
#include <vector>
#include "jke/nation/Kingdom.hpp"
#include "jke/city/City.hpp"
#include "jke/diplomacy/DiplomaticRelation.hpp"
#include "jke/core/EventBus.hpp"
#include "jke/ai/AIDecision.hpp"

namespace jke {

using RelationMap = std::unordered_map<KingdomID,
                       std::unordered_map<KingdomID, DiplomaticRelation>>;

class DiplomacyEngine {
public:
    void update(
        std::unordered_map<KingdomID, Kingdom>& kingdoms,
        RelationMap& relations,
        std::vector<TreatyProposal>& pendingProposals,
        std::unordered_map<CityID, City>& cities,
        EventBus& bus,
        TurnNumber turn
    );

    // Called by SimulationEngine when AI issues a diplomatic decision
    void submitProposal(TreatyProposal proposal,
                        std::vector<TreatyProposal>& pending);

    DiplomaticRelation& getOrCreate(RelationMap& rel, KingdomID a, KingdomID b);
    const DiplomaticRelation* get(const RelationMap& rel, KingdomID a, KingdomID b) const;

    bool areAtWar(const RelationMap& rel, KingdomID a, KingdomID b) const;
    bool areAllied(const RelationMap& rel, KingdomID a, KingdomID b) const;

private:
    void processPendingProposals(
        std::vector<TreatyProposal>& pending,
        RelationMap& relations,
        std::unordered_map<KingdomID, Kingdom>& kingdoms,
        std::unordered_map<CityID, City>& cities,
        EventBus& bus, TurnNumber turn
    );

    // Transfer border cities from loser to winner on peace
    void ceedTerritory(KingdomID winner, KingdomID loser,
                       std::unordered_map<KingdomID, Kingdom>& kingdoms,
                       std::unordered_map<CityID, City>& cities,
                       TurnNumber turn);

    void updateTrustValues(RelationMap& relations,
                           const std::unordered_map<KingdomID, Kingdom>& kingdoms) const;

    void declareWar(RelationMap& relations, KingdomID attacker, KingdomID defender,
                    EventBus& bus, TurnNumber turn);
};

} // namespace jke
