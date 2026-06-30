#pragma once
#include <unordered_map>
#include "jke/nation/Kingdom.hpp"
#include "jke/city/City.hpp"
#include "jke/army/Army.hpp"
#include "jke/core/EventBus.hpp"
#include "jke/world/Season.hpp"
#include "jke/engines/DiplomacyEngine.hpp"

namespace jke {

class EconomyEngine {
public:
    void update(
        std::unordered_map<KingdomID, Kingdom>& kingdoms,
        std::unordered_map<CityID, City>& cities,
        const std::unordered_map<ArmyID, Army>& armies,
        EventBus& bus,
        TurnNumber turn,
        Season season = Season::Summer,
        const RelationMap* relations = nullptr
    );

private:
    void computeIncome(Kingdom& k,
                       const std::unordered_map<CityID, City>& cities,
                       const std::unordered_map<KingdomID, Kingdom>& kingdoms,
                       const RelationMap* relations) const;
    void computeExpenses(Kingdom& k,
                         const std::unordered_map<ArmyID, Army>& armies) const;
    void applyStarvation(Kingdom& k,
                         std::unordered_map<CityID, City>& cities,
                         EventBus& bus, TurnNumber turn) const;
    void growPopulation(Kingdom& k,
                        std::unordered_map<CityID, City>& cities) const;
    void updateCityHappiness(const Kingdom& k, City& c) const;
};

} // namespace jke
