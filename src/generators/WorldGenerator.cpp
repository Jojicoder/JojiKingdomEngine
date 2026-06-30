#include "jke/generators/WorldGenerator.hpp"
#include "jke/generators/TerrainGenerator.hpp"
#include "jke/generators/NationGenerator.hpp"
#include "jke/generators/CityGenerator.hpp"
#include "jke/generators/ArmyGenerator.hpp"
#include "jke/core/Constants.hpp"

namespace jke {

WorldGenerator::WorldGenerator(uint64_t seed)
    : rng_(seed), seed_(seed) {}

GeneratedWorld WorldGenerator::generate() {
    GeneratedWorld world{
        WorldMap(constants::MAP_SIZE, constants::MAP_SIZE),
        {}, {}, {},
        TechTree{}
    };

    // Step 1: Terrain
    TerrainGenerator terrainGen(rng_);
    terrainGen.generate(world.worldMap);

    // Step 2: Nations + territory
    NationGenerator nationGen(rng_);
    nationGen.generate(world);

    // Step 3: Cities (capitals)
    CityGenerator cityGen(rng_);
    cityGen.generate(world);

    // Step 4: Starting armies
    ArmyGenerator armyGen(rng_);
    armyGen.generate(world);

    return world;
}

} // namespace jke
