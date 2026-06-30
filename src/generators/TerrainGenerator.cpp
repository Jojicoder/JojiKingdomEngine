#include "jke/generators/TerrainGenerator.hpp"
#include "jke/core/Constants.hpp"
#include <algorithm>
#include <queue>
#include <cmath>

namespace jke {
namespace {
    constexpr int MAP_N = jke::constants::MAP_SIZE; // 129

    float vegetationPattern(int x, int y) {
        float v =
            0.45f * std::sin(x * 0.17f + y * 0.05f) +
            0.35f * std::cos(y * 0.15f - x * 0.04f) +
            0.20f * std::sin((x + y) * 0.09f);
        return (v + 1.0f) * 0.5f;
    }
}

TerrainGenerator::TerrainGenerator(Random& rng) : rng_(rng) {}

void TerrainGenerator::generate(WorldMap& map) {
    generateHeightmap(map);
    classifyTerrain(map);
    generateLakes(map);
    generateRivers(map);
    smoothCoastline(map);

    // Set fertility and resource richness per tile
    for (auto& tile : map.tiles()) {
        tile.fertility        = terrainFertility(tile.terrain);
        tile.resourceRichness = (tile.terrain == TerrainType::Mountain) ? 1.5f :
                                (tile.terrain == TerrainType::Hill)     ? 1.2f :
                                (tile.terrain == TerrainType::Forest)   ? 0.9f : 1.0f;
        if (tile.hasRiver) {
            tile.fertility += 0.3f;
            tile.fertility = std::min(tile.fertility, 2.0f);
        }
    }
}

void TerrainGenerator::generateHeightmap(WorldMap& map) {
    int size = MAP_N;
    std::vector<float> h(static_cast<size_t>(size * size), 0.0f);

    // Initialize corners
    h[0]                           = rng_.nextFloat(0.3f, 0.7f);
    h[size - 1]                    = rng_.nextFloat(0.3f, 0.7f);
    h[(size-1) * size]             = rng_.nextFloat(0.3f, 0.7f);
    h[(size-1) * size + (size-1)]  = rng_.nextFloat(0.3f, 0.7f);

    float scale = 0.5f;
    int step = size - 1;

    while (step > 1) {
        int half = step / 2;

        // Diamond step
        for (int y = 0; y < size - 1; y += step) {
            for (int x = 0; x < size - 1; x += step) {
                float avg = (h[y*size+x] + h[y*size+(x+step)] +
                             h[(y+step)*size+x] + h[(y+step)*size+(x+step)]) * 0.25f;
                h[(y+half)*size+(x+half)] = avg + rng_.nextFloat(-scale, scale);
            }
        }

        // Square step
        for (int y = 0; y < size; y += half) {
            for (int x = (y + half) % step; x < size; x += step) {
                float sum = 0.0f; int count = 0;
                if (y - half >= 0)   { sum += h[(y-half)*size+x]; count++; }
                if (y + half < size) { sum += h[(y+half)*size+x]; count++; }
                if (x - half >= 0)   { sum += h[y*size+(x-half)]; count++; }
                if (x + half < size) { sum += h[y*size+(x+half)]; count++; }
                h[y*size+x] = sum / count + rng_.nextFloat(-scale, scale);
            }
        }

        step = half;
        scale *= 0.55f;
    }

    // Normalize to [0,1]
    float mn = *std::min_element(h.begin(), h.end());
    float mx = *std::max_element(h.begin(), h.end());
    float range = mx - mn;
    if (range < 1e-6f) range = 1.0f;
    for (auto& v : h) v = (v - mn) / range;

    // Island generation — separates SHAPE from TERRAIN:
    //   • a warped radial mask determines the land/ocean boundary
    //   • diamond-square noise controls terrain type *within* the continent
    // The mask keeps the continent coherent while avoiding a perfect disk.
    const float cx = (size - 1) * 0.5f + rng_.nextFloat(-1.5f, 1.5f);
    const float cy = (size - 1) * 0.5f + rng_.nextFloat(-1.5f, 1.5f);
    const float baseRadius = (size - 1) * rng_.nextFloat(0.44f, 0.46f);
    const float aspectX = rng_.nextFloat(0.92f, 1.12f);
    const float aspectY = rng_.nextFloat(0.92f, 1.12f);

    const float phase2 = rng_.nextFloat(0.0f, 6.2831853f);
    const float phase3 = rng_.nextFloat(0.0f, 6.2831853f);
    const float phase5 = rng_.nextFloat(0.0f, 6.2831853f);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float noise = h[y*size+x];
            float dx = (x - cx) / aspectX;
            float dy = (y - cy) / aspectY;
            float angle = std::atan2(dy, dx);
            float dist = std::sqrt(dx * dx + dy * dy);

            float lobe =
                0.06f * std::sin(2.0f * angle + phase2) +
                0.05f * std::sin(3.0f * angle + phase3) +
                0.03f * std::cos(5.0f * angle + phase5);
            float localRadius = baseRadius * (1.0f + lobe);
            float d = dist / std::max(localRadius, 1.0f);  // 0=centre, 1=edge

            // Quadratic dome: 1 at centre, 0 at edge. The radius varies by angle.
            float landMask = std::max(0.0f, 1.0f - d * d);

            // Perturb the effective boundary slightly with noise → natural coastline
            float effective = landMask + (noise - 0.5f) * 0.20f;

            float elevation;
            const float LAND_THRESH = 0.20f;

            if (effective <= 0.0f) {
                elevation = 0.0f;
            } else if (effective < LAND_THRESH) {
                // Shallow water / coast approach
                elevation = (effective / LAND_THRESH) * 0.19f;
            } else {
                // Land: noise controls terrain type; interior depth scales the range
                float interior = std::clamp((landMask - LAND_THRESH) /
                                            (1.0f - LAND_THRESH), 0.0f, 1.0f);
                elevation = 0.20f + noise * (0.22f + interior * 0.58f);
            }

            map.at(x, y).elevation = std::clamp(elevation, 0.0f, 1.0f);
        }
    }
}

void TerrainGenerator::classifyTerrain(WorldMap& map) {
    namespace C = jke::constants;
    for (auto& tile : map.tiles()) {
        float e = tile.elevation;
        if      (e < C::OCEAN_THRESHOLD)  tile.terrain = TerrainType::Ocean;
        else if (e < C::PLAIN_THRESHOLD)  {
            auto [x, y] = tile.position;
            float veg = vegetationPattern(x, y);
            tile.terrain = (veg > 0.66f && e > 0.26f) ? TerrainType::Forest : TerrainType::Plain;
        }
        else if (e < C::FOREST_THRESHOLD) tile.terrain = TerrainType::Forest;
        else if (e < C::HILL_THRESHOLD)   tile.terrain = TerrainType::Hill;
        else                              tile.terrain = TerrainType::Mountain;
    }
}

void TerrainGenerator::generateLakes(WorldMap& map) {
    // Place lakes in low-elevation basins away from coast
    for (auto& tile : map.tiles()) {
        if (tile.terrain == TerrainType::Plain &&
            tile.elevation > 0.22f && tile.elevation < 0.32f &&
            rng_.chance(constants::LAKE_PROB)) {
            tile.terrain = TerrainType::Lake;
        }
    }
}

void TerrainGenerator::generateRivers(WorldMap& map) {
    // Find mountain tiles as potential river sources
    std::vector<TileID> mountains;
    for (const auto& tile : map.tiles()) {
        if (tile.terrain == TerrainType::Mountain ||
           (tile.terrain == TerrainType::Hill && tile.elevation > 0.62f))
            mountains.push_back(tile.id);
    }

    std::shuffle(mountains.begin(), mountains.end(), rng_.engine());
    // Scale river count with map size (more rivers on larger map)
    int scaledRivers = constants::NUM_RIVERS * (constants::MAP_SIZE / 129);
    int numRivers = std::min(scaledRivers, static_cast<int>(mountains.size()));

    for (int i = 0; i < numRivers; ++i) {
        auto path = traceRiver(map, mountains[i]);
        if (path.size() < 4) continue;

        River river;
        river.path  = path;
        river.width = rng_.nextFloat(0.8f, 2.0f);

        for (TileID tid : path) {
            auto& t = map.at(tid);
            t.hasRiver = true;
            if (t.terrain != TerrainType::Mountain &&
                t.terrain != TerrainType::Ocean &&
                t.terrain != TerrainType::Lake) {
                t.terrain = TerrainType::River;
            }
        }
        map.rivers().push_back(std::move(river));
    }
}

std::vector<TileID> TerrainGenerator::traceRiver(WorldMap& map, TileID source) {
    std::vector<TileID> path;
    TileID current = source;
    std::vector<bool> visited(map.tileCount(), false);

    for (int steps = 0; steps < 200; ++steps) {
        if (visited[current]) break;
        visited[current] = true;
        path.push_back(current);

        const Tile& cur = map.at(current);
        if (cur.terrain == TerrainType::Ocean ||
            cur.terrain == TerrainType::Lake  ||
            cur.terrain == TerrainType::Coast) break;

        // Move to lowest unvisited neighbor (with small random perturbation)
        auto neighbors = map.neighbors4v(current);
        TileID best = NO_TILE;
        float  bestElev = cur.elevation + 0.05f; // must go downhill

        for (TileID nid : neighbors) {
            if (visited[nid]) continue;
            float elev = map.at(nid).elevation + rng_.nextFloat(-0.02f, 0.02f);
            if (elev < bestElev) {
                bestElev = elev;
                best = nid;
            }
        }
        if (best == NO_TILE) break;
        current = best;
    }
    return path;
}

void TerrainGenerator::smoothCoastline(WorldMap& map) {
    // Coast is shallow water adjacent to land. Low-elevation inland tiles should
    // remain land, otherwise continents turn into large blue wedges.
    std::vector<TileID> toCoast;
    for (const auto& tile : map.tiles()) {
        if (tile.terrain != TerrainType::Ocean) continue;
        int landNeighbors = 0;
        for (TileID nid : map.neighbors8(tile.id)) {
            auto t = map.at(nid).terrain;
            if (t != TerrainType::Ocean &&
                t != TerrainType::Coast &&
                t != TerrainType::Lake) {
                landNeighbors++;
            }
        }
        if (landNeighbors >= 1) toCoast.push_back(tile.id);
    }

    for (TileID tid : toCoast) {
        map.at(tid).terrain = TerrainType::Coast;
    }
}

} // namespace jke
