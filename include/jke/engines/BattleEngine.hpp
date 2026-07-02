#pragma once
#include <unordered_map>
#include "jke/nation/Kingdom.hpp"
#include "jke/army/Army.hpp"
#include "jke/city/City.hpp"
#include "jke/world/WorldMap.hpp"
#include "jke/world/Season.hpp"
#include "jke/battle/BattleContext.hpp"
#include "jke/battle/BattleResult.hpp"
#include "jke/core/Random.hpp"
#include "jke/core/EventBus.hpp"
#include "jke/engines/DiplomacyEngine.hpp"

namespace jke {

class BattleEngine {
public:
    explicit BattleEngine(Random& rng);

    void update(
        std::unordered_map<KingdomID, Kingdom>& kingdoms,
        std::unordered_map<ArmyID, Army>& armies,
        std::unordered_map<CityID, City>& cities,
        WorldMap& worldMap,
        EventBus& bus,
        TurnNumber turn,
        const RelationMap& relations,
        Season season = Season::Summer
    );

private:
    Random& rng_;
    // Reused each turn to avoid per-turn allocation (mutable: modified in const detectBattles)
    mutable std::unordered_map<TileID, std::vector<ArmyID>> tileArmies_;

    std::vector<BattleContext> detectBattles(
        const std::unordered_map<ArmyID, Army>& armies,
        const std::unordered_map<CityID, City>& cities,
        const WorldMap& worldMap,
        const std::unordered_map<KingdomID, Kingdom>& kingdoms,
        const RelationMap& relations,
        TurnNumber turn,
        Season season
    ) const;

    BattleResult resolveBattle(
        BattleContext ctx,
        std::unordered_map<ArmyID, Army>& armies,
        std::unordered_map<KingdomID, Kingdom>& kingdoms,
        std::unordered_map<CityID, City>& cities,
        const WorldMap& worldMap
    );

    void applyResult(
        const BattleResult& result,
        std::unordered_map<ArmyID, Army>& armies,
        std::unordered_map<KingdomID, Kingdom>& kingdoms,
        std::unordered_map<CityID, City>& cities,
        WorldMap& worldMap,
        EventBus& bus,
        TurnNumber turn
    );

    bool areAtWar(KingdomID a, KingdomID b, const RelationMap& relations) const;
};

} // namespace jke
