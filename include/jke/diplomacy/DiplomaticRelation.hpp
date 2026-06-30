#pragma once
#include "jke/core/Types.hpp"

namespace jke {

enum class RelationState : uint8_t {
    Neutral         = 0,
    Alliance        = 1,
    TradeAgreement  = 2,
    War             = 3,
    Vassal          = 4,    // secondaryKingdom is vassal of primaryKingdom
    Peace           = 5,    // explicit peace treaty (still not alliance)
};

struct DiplomaticRelation {
    KingdomID    kingdomA           = NO_KINGDOM;
    KingdomID    kingdomB           = NO_KINGDOM;
    RelationState state             = RelationState::Neutral;
    float        trust              = 0.0f;  // -1.0 – +1.0
    TurnNumber   turnEstablished    = 0;
    TurnNumber   turnsAtWar         = 0;
    bool         tradeRouteActive   = false;
    // Non-aggression pact
    bool         nonAggressionPact  = false;
    TurnNumber   nonAggressionUntil = 0;
};

struct TreatyProposal {
    KingdomID    proposer           = NO_KINGDOM;
    KingdomID    recipient          = NO_KINGDOM;
    RelationState proposedState     = RelationState::Peace;
    float        goldTransfer       = 0.0f;
    TurnNumber   proposedTurn       = 0;
    bool         accepted           = false;
    bool         processed          = false;
};

} // namespace jke
