#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "jke/nation/Kingdom.hpp"
#include "jke/army/Army.hpp"
#include "jke/city/City.hpp"
#include "jke/world/WorldMap.hpp"
#include "jke/world/Season.hpp"
#include "jke/core/EventBus.hpp"

namespace jke {

struct RecruitOrder {
    KingdomID kingdom   = NO_KINGDOM;
    CityID    city      = NO_CITY;
    UnitType  unitType  = UnitType::Infantry;
    uint32_t  soldiers  = 0;
};

class MilitaryEngine {
public:
    void update(
        std::unordered_map<KingdomID, Kingdom>& kingdoms,
        std::unordered_map<ArmyID, Army>& armies,
        const std::unordered_map<CityID, City>& cities,
        WorldMap& worldMap,
        EventBus& bus,
        TurnNumber turn,
        Season season = Season::Summer
    );

    ArmyID spawnArmy(
        std::unordered_map<KingdomID, Kingdom>& kingdoms,
        std::unordered_map<ArmyID, Army>& armies,
        WorldMap& worldMap,
        ArmyID& nextArmyID,
        UnitID& nextUnitID,
        const RecruitOrder& order,
        const std::unordered_map<CityID, City>& cities
    );

    void removeEmptyArmies(std::unordered_map<KingdomID, Kingdom>& kingdoms,
                           std::unordered_map<ArmyID, Army>& armies,
                           WorldMap& worldMap) const;

private:
    void moveArmies(std::unordered_map<ArmyID, Army>& armies,
                    WorldMap& worldMap,
                    Season season) const;
    void decaySupply(std::unordered_map<ArmyID, Army>& armies,
                     const std::unordered_map<CityID, City>& cities,
                     const WorldMap& worldMap,
                     Season season) const;

    // A* / greedy pathfinding on the tile grid
    std::vector<TileID> findPath(const WorldMap& map, TileID from, TileID to) const;

    mutable std::vector<float> pathGCost_;
    mutable std::vector<TileID> pathCameFrom_;
    mutable std::vector<uint32_t> pathSeenStamp_;
    mutable std::vector<uint32_t> pathClosedStamp_;
    mutable uint32_t pathSearchStamp_ = 1;
};

} // namespace jke
