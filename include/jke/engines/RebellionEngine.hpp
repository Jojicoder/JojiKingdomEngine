#pragma once
#include <unordered_map>
#include <vector>
#include "jke/nation/Kingdom.hpp"
#include "jke/city/City.hpp"
#include "jke/army/Army.hpp"
#include "jke/rebellion/Rebellion.hpp"
#include "jke/core/Random.hpp"
#include "jke/core/EventBus.hpp"
#include "jke/diplomacy/DiplomaticRelation.hpp"

namespace jke { using RelationMap = std::unordered_map<KingdomID,
    std::unordered_map<KingdomID, DiplomaticRelation>>; }

namespace jke {

class RebellionEngine {
public:
    explicit RebellionEngine(Random& rng);

    void update(
        std::unordered_map<KingdomID, Kingdom>& kingdoms,
        std::unordered_map<CityID, City>& cities,
        std::unordered_map<ArmyID, Army>& armies,
        std::vector<Rebellion>& rebellions,
        std::vector<CivilWar>& civilWars,
        RelationMap& relations,
        KingdomID& nextKingdomID,
        ArmyID& nextArmyID,
        UnitID& nextUnitID,
        EventBus& bus,
        TurnNumber turn
    );

private:
    Random& rng_;

    float computeRebellionPressure(const City& city,
                                   const Kingdom& owner) const;

    void escalateToCivilWar(
        Rebellion& rebellion,
        std::unordered_map<KingdomID, Kingdom>& kingdoms,
        std::unordered_map<CityID, City>& cities,
        std::unordered_map<ArmyID, Army>& armies,
        std::vector<CivilWar>& civilWars,
        RelationMap& relations,
        KingdomID& nextKingdomID,
        ArmyID& nextArmyID,
        UnitID& nextUnitID,
        EventBus& bus,
        TurnNumber turn
    );

    void suppressRebellion(Rebellion& r,
                           std::unordered_map<CityID, City>& cities,
                           EventBus& bus, TurnNumber turn) const;
};

} // namespace jke
