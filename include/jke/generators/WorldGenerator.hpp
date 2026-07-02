#pragma once
#include <memory>
#include "jke/world/WorldMap.hpp"
#include "jke/nation/Kingdom.hpp"
#include "jke/city/City.hpp"
#include "jke/army/Army.hpp"
#include "jke/technology/TechTree.hpp"
#include "jke/core/Random.hpp"
#include "jke/core/Constants.hpp"

namespace jke {

struct GeneratedWorld {
    WorldMap                            worldMap;
    std::unordered_map<KingdomID, Kingdom> kingdoms;
    std::unordered_map<CityID, City>       cities;
    std::unordered_map<ArmyID, Army>       armies;
    TechTree                            techTree;
    KingdomID                           nextKingdomID = 1;
    CityID                              nextCityID    = 1;
    ArmyID                              nextArmyID    = 1;
    UnitID                              nextUnitID    = 1;
};

class WorldGenerator {
public:
    explicit WorldGenerator(uint64_t seed,
                            int kingdomCount = constants::NUM_KINGDOMS);
    GeneratedWorld generate();

private:
    Random rng_;
    int kingdomCount_;
};

} // namespace jke
