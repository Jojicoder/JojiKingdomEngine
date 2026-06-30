#pragma once
#include <memory>
#include "jke/world/WorldMap.hpp"
#include "jke/nation/Kingdom.hpp"
#include "jke/city/City.hpp"
#include "jke/army/Army.hpp"
#include "jke/technology/TechTree.hpp"
#include "jke/core/Random.hpp"

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
    explicit WorldGenerator(uint64_t seed);
    GeneratedWorld generate();

private:
    Random rng_;
    uint64_t seed_;
};

} // namespace jke
