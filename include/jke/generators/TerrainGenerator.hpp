#pragma once
#include "jke/world/WorldMap.hpp"
#include "jke/core/Random.hpp"

namespace jke {

class TerrainGenerator {
public:
    explicit TerrainGenerator(Random& rng);
    void generate(WorldMap& map);

private:
    Random& rng_;

    // Diamond-square heightmap
    void  generateHeightmap(WorldMap& map);
    float diamondSquare(std::vector<float>& heights, int size, int step, float scale);
    void  classifyTerrain(WorldMap& map);
    void  generateRivers(WorldMap& map);
    void  generateLakes(WorldMap& map);
    void  smoothCoastline(WorldMap& map);

    // River pathfinding: greedy descent from mountain peak to coast
    std::vector<TileID> traceRiver(WorldMap& map, TileID source);
};

} // namespace jke
