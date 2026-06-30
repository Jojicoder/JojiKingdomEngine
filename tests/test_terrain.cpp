#include "jke/generators/TerrainGenerator.hpp"
#include "jke/generators/WorldGenerator.hpp"
#include "jke/world/WorldMap.hpp"
#include "jke/core/Constants.hpp"
#include "jke/core/Types.hpp"
#include <cassert>
#include <iostream>
#include <algorithm>

int main() {
    jke::Random rng(12345);
    jke::WorldMap map(jke::constants::MAP_SIZE, jke::constants::MAP_SIZE);
    jke::TerrainGenerator gen(rng);
    gen.generate(map);

    // At least some tiles should be land
    int landCount = 0, oceanCount = 0;
    for (const auto& t : map.tiles()) {
        if (t.terrain == jke::TerrainType::Ocean) oceanCount++;
        else landCount++;
    }
    assert(landCount > 100 && "Too few land tiles");
    assert(oceanCount > 100 && "Too few ocean tiles");
    assert(landCount > 8000 && "Continent is too small for the viewer map");

    // Rivers should exist
    assert(!map.rivers().empty() && "No rivers generated");

    // Elevation should be in [0, 1]
    for (const auto& t : map.tiles()) {
        assert(t.elevation >= 0.0f && t.elevation <= 1.0f && "Elevation out of range");
    }

    jke::WorldGenerator worldGen(12345);
    auto world = worldGen.generate();
    for (const auto& t : world.worldMap.tiles()) {
        if (t.terrain == jke::TerrainType::Ocean ||
            t.terrain == jke::TerrainType::Coast ||
            t.terrain == jke::TerrainType::Lake) {
            assert(t.owner == jke::NO_KINGDOM && "Water and coast tiles must not be kingdom territory");
        }
    }

    std::cout << "Terrain tests PASSED\n";
    std::cout << "  Land tiles:  " << landCount  << "\n";
    std::cout << "  Ocean tiles: " << oceanCount << "\n";
    std::cout << "  Rivers:      " << map.rivers().size() << "\n";
    return 0;
}
