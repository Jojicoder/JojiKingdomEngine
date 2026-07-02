#include "jke/generators/WorldGenerator.hpp"
#include "jke/generators/TerrainGenerator.hpp"
#include "jke/generators/NationGenerator.hpp"
#include "jke/generators/CityGenerator.hpp"
#include "jke/generators/ArmyGenerator.hpp"
#include "jke/core/Constants.hpp"
#include <algorithm>
#include <cmath>

namespace jke {

namespace {

bool isLandStrategicTile(const Tile& tile) {
    return tile.terrain != TerrainType::Ocean &&
           tile.terrain != TerrainType::Lake &&
           tile.city == NO_CITY;
}

float nearestCityDistance(const GeneratedWorld& world, TileID tileId) {
    const auto pos = world.worldMap.at(tileId).position;
    float best = 1e9f;
    for (const auto& [cid, city] : world.cities) {
        (void)cid;
        if (city.tile == NO_TILE) continue;
        const auto cpos = world.worldMap.at(city.tile).position;
        best = std::min(best, std::hypot(float(pos.x - cpos.x), float(pos.y - cpos.y)));
    }
    return best;
}

bool nearTerrain(const WorldMap& map, TileID tileId, TerrainType terrain) {
    for (TileID nid : map.neighbors8(tileId)) {
        if (map.at(nid).terrain == terrain) return true;
    }
    return false;
}

void placeStrategicPoints(GeneratedWorld& world) {
    struct Candidate {
        TileID tile = NO_TILE;
        StrategicPointType type = StrategicPointType::None;
        float score = 0.0f;
    };
    std::vector<Candidate> candidates;
    const WorldMap& map = world.worldMap;

    for (const Tile& tile : map.tiles()) {
        if (!isLandStrategicTile(tile)) continue;

        int mountainNeighbors = 0;
        int hillNeighbors = 0;
        int riverNeighbors = tile.hasRiver || tile.terrain == TerrainType::River ? 1 : 0;
        int coastNeighbors = tile.terrain == TerrainType::Coast ? 1 : 0;
        int landNeighbors = 0;
        int ownedNeighborChanges = 0;
        KingdomID firstOwner = NO_KINGDOM;

        for (TileID nid : map.neighbors8(tile.id)) {
            const Tile& nt = map.at(nid);
            if (nt.terrain != TerrainType::Ocean && nt.terrain != TerrainType::Lake) ++landNeighbors;
            if (nt.terrain == TerrainType::Mountain) ++mountainNeighbors;
            if (nt.terrain == TerrainType::Hill) ++hillNeighbors;
            if (nt.hasRiver || nt.terrain == TerrainType::River) ++riverNeighbors;
            if (nt.terrain == TerrainType::Coast || nt.terrain == TerrainType::Ocean) ++coastNeighbors;
            if (nt.owner != NO_KINGDOM) {
                if (firstOwner == NO_KINGDOM) firstOwner = nt.owner;
                else if (firstOwner != nt.owner) ++ownedNeighborChanges;
            }
        }

        const float cityDist = nearestCityDistance(world, tile.id);
        if (cityDist < 4.0f) continue;

        auto addCandidate = [&](StrategicPointType type, float score) {
            if (score <= 0.0f) return;
            candidates.push_back({tile.id, type, score});
        };

        if ((tile.terrain == TerrainType::Hill || tile.terrain == TerrainType::Mountain) &&
            mountainNeighbors >= 2 && landNeighbors >= 3) {
            addCandidate(StrategicPointType::MountainPass,
                         30.0f + mountainNeighbors * 3.0f + ownedNeighborChanges * 5.0f -
                         std::max(0.0f, cityDist - 22.0f) * 0.4f);
        }
        if ((tile.hasRiver || tile.terrain == TerrainType::River) && riverNeighbors >= 3) {
            addCandidate(StrategicPointType::Bridge,
                         28.0f + riverNeighbors * 2.5f + ownedNeighborChanges * 4.0f -
                         std::max(0.0f, cityDist - 24.0f) * 0.35f);
        } else if (nearTerrain(map, tile.id, TerrainType::River) && landNeighbors >= 5) {
            addCandidate(StrategicPointType::RiverFord,
                         22.0f + riverNeighbors * 2.0f + ownedNeighborChanges * 3.0f -
                         std::max(0.0f, cityDist - 22.0f) * 0.30f);
        }
        if (coastNeighbors >= 2 &&
            tile.terrain != TerrainType::Coast &&
            tile.terrain != TerrainType::Mountain) {
            addCandidate(StrategicPointType::HarborSite,
                         26.0f + coastNeighbors * 2.5f + tile.resourceRichness * 4.0f -
                         std::max(0.0f, cityDist - 28.0f) * 0.25f);
        }
        if (tile.terrain == TerrainType::Plain || tile.terrain == TerrainType::Forest ||
            tile.terrain == TerrainType::Hill) {
            const float supplyScore =
                tile.fertility * 6.0f + tile.resourceRichness * 5.0f +
                hillNeighbors * 1.5f + ownedNeighborChanges * 4.0f -
                std::abs(cityDist - 14.0f) * 0.65f;
            addCandidate(StrategicPointType::SupplyDepot, supplyScore);
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    const int targetCount = std::clamp(map.tileCount() / 180, 18, 58);
    int placed = 0;
    for (const Candidate& candidate : candidates) {
        if (placed >= targetCount) break;
        Tile& tile = world.worldMap.at(candidate.tile);
        if (tile.strategicPoint != StrategicPointType::None || tile.city != NO_CITY) continue;

        bool tooClose = false;
        for (TileID nid : world.worldMap.neighbors8(candidate.tile)) {
            if (world.worldMap.at(nid).strategicPoint != StrategicPointType::None) {
                tooClose = true;
                break;
            }
        }
        if (tooClose) continue;

        tile.strategicPoint = candidate.type;
        tile.strategicValue = std::clamp(candidate.score, 12.0f, 70.0f);
        ++placed;
    }
}

} // namespace

WorldGenerator::WorldGenerator(uint64_t seed, int kingdomCount)
    : rng_(seed)
    , kingdomCount_(std::clamp(kingdomCount, 4, constants::NUM_KINGDOMS)) {}

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
    nationGen.generate(world, kingdomCount_);

    // Step 3: Cities (capitals)
    CityGenerator cityGen(rng_);
    cityGen.generate(world);

    // Step 4: Non-city strategic points
    placeStrategicPoints(world);

    // Step 5: Starting armies
    ArmyGenerator armyGen(rng_);
    armyGen.generate(world);

    return world;
}

} // namespace jke
