#include "jke/generators/NationGenerator.hpp"
#include "jke/core/Constants.hpp"
#include <algorithm>
#include <queue>
#include <cmath>

namespace jke {
namespace {
bool isClaimableTerritory(TerrainType terrain) {
    return terrain != TerrainType::Ocean &&
           terrain != TerrainType::Coast &&
           terrain != TerrainType::Lake;
}
}

static const std::string KINGDOM_NAMES[] = {
    "Valdoria", "Ironspire", "Aethermoor", "Sunhaven",
    "Blackthorn", "Goldenveil", "Stonegate", "Rivermark",
    "Ashford", "Crystalholm", "Embervast", "Dawnreach",
    "Thornwall", "Silvershard", "Highcrest", "Mistwood",
    "Duskfell", "Crownsreach", "Ironhollow", "Embermarch"
};

NationGenerator::NationGenerator(Random& rng) : rng_(rng) {}

void NationGenerator::generate(GeneratedWorld& world) {
    auto sites = pickCapitalSites(world.worldMap, constants::NUM_KINGDOMS);

    for (int i = 0; i < constants::NUM_KINGDOMS; ++i) {
        KingdomID kid = world.nextKingdomID++;
        Kingdom k;
        k.id              = kid;
        k.name            = KINGDOM_NAMES[i];
        k.personality     = assignPersonality(i);
        k.specialization  = assignSpecialization(i);
        k.foundedTurn     = 0;

        // Starting resources — balanced with small random variation
        k.treasury.food  = constants::START_FOOD  * rng_.nextFloat(0.9f, 1.1f);
        k.treasury.gold  = constants::START_GOLD  * rng_.nextFloat(0.9f, 1.1f);
        k.treasury.wood  = constants::START_WOOD  * rng_.nextFloat(0.9f, 1.1f);
        k.treasury.stone = constants::START_STONE * rng_.nextFloat(0.9f, 1.1f);
        k.treasury.iron  = constants::START_IRON  * rng_.nextFloat(0.9f, 1.1f);

        k.stability   = rng_.nextFloat(0.85f, 1.0f);
        k.morale      = rng_.nextFloat(0.85f, 1.0f);
        k.legitimacy  = 1.0f;

        applySpecializationBonuses(k);

        // Assign starting technologies by specialization
        switch (k.specialization) {
            case KingdomSpecialization::Military:
                k.researchedTechs = {7, 10};  // Iron Working, Shield Wall
                break;
            case KingdomSpecialization::Economy:
                k.researchedTechs = {13, 14}; // Taxation, Trade Routes
                break;
            case KingdomSpecialization::Agriculture:
                k.researchedTechs = {1, 3};   // Crop Rotation, Selective Breeding
                break;
            case KingdomSpecialization::Technology:
                k.researchedTechs = {11, 16}; // Writing, Mining
                break;
            case KingdomSpecialization::Trade:
                k.researchedTechs = {14, 4};  // Trade Routes, Roads
                break;
            case KingdomSpecialization::Defense:
                k.researchedTechs = {19, 4};  // Fortification, Roads
                break;
        }

        // Store capital tile reference (city will be built by CityGenerator)
        world.worldMap.at(sites[i]).owner = kid;

        world.kingdoms[kid] = std::move(k);
    }

    // Assign territories via BFS Voronoi expansion from capital sites
    assignTerritories(world);
}

std::vector<TileID> NationGenerator::pickCapitalSites(const WorldMap& map, int count) const {
    // Score each land tile — prefer high fertility, not on border, not ocean/mountain
    std::vector<std::pair<float, TileID>> candidates;

    for (const auto& tile : map.tiles()) {
        if (tile.terrain == TerrainType::Ocean   ||
            tile.terrain == TerrainType::Mountain ||
            tile.terrain == TerrainType::Lake     ||
            tile.terrain == TerrainType::Coast)
            continue;

        // Prefer tiles away from edges
        int margin = 8;
        auto [x, y] = tile.position;
        if (x < margin || y < margin ||
            x >= map.width() - margin ||
            y >= map.height() - margin) continue;

        float score = tile.fertility + tile.elevation * 0.5f;
        if (tile.hasRiver) score += 0.3f;
        score += rng_.nextFloat(0.0f, 0.2f);
        candidates.push_back({score, tile.id});
    }

    // Sort descending by score
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b){ return a.first > b.first; });

    // Pick sites with minimum separation
    const float minSep = 20.0f;
    std::vector<TileID> chosen;
    chosen.reserve(count);

    for (auto& [score, tid] : candidates) {
        if (static_cast<int>(chosen.size()) >= count) break;
        auto [cx, cy] = map.at(tid).position;

        bool tooClose = false;
        for (TileID prev : chosen) {
            auto [px, py] = map.at(prev).position;
            float d = std::hypot(float(cx - px), float(cy - py));
            if (d < minSep) { tooClose = true; break; }
        }
        if (!tooClose) chosen.push_back(tid);
    }

    // If not enough sites found, relax constraints
    if (static_cast<int>(chosen.size()) < count) {
        for (auto& [score, tid] : candidates) {
            if (static_cast<int>(chosen.size()) >= count) break;
            if (std::find(chosen.begin(), chosen.end(), tid) == chosen.end())
                chosen.push_back(tid);
        }
    }
    return chosen;
}

void NationGenerator::assignTerritories(GeneratedWorld& world) const {
    WorldMap& map = world.worldMap;

    // Multi-source BFS from each kingdom's capital tile
    struct Node { TileID tile; float cost; KingdomID kingdom; };
    auto cmp = [](const Node& a, const Node& b){ return a.cost > b.cost; };
    std::priority_queue<Node, std::vector<Node>, decltype(cmp)> pq(cmp);
    std::vector<float> dist(map.tileCount(), 1e9f);

    for (const auto& [kid, k] : world.kingdoms) {
        // Find the tile owned by this kingdom (capital site)
        for (const auto& tile : map.tiles()) {
            if (tile.owner == kid) {
                pq.push({tile.id, 0.0f, kid});
                dist[tile.id] = 0.0f;
                break;
            }
        }
    }

    while (!pq.empty()) {
        auto [tid, cost, kid] = pq.top(); pq.pop();
        if (cost > dist[tid]) continue;

        Tile& t = map.at(tid);
        if (!isClaimableTerritory(t.terrain)) continue;

        t.owner = kid;

        for (TileID nid : map.neighbors4v(tid)) {
            Tile& nb = map.at(nid);
            if (!isClaimableTerritory(nb.terrain)) continue;

            float moveCost = terrainMoveCost(nb.terrain) * terrainBorderStrength(nb.terrain);
            float newCost  = cost + moveCost;
            if (newCost < dist[nid]) {
                dist[nid] = newCost;
                pq.push({nid, newCost, kid});
            }
        }
    }

    // Second pass: claim any remaining unclaimed land tiles via uniform-cost BFS.
    // Mountains and rivers can block the weighted BFS but every land tile
    // must eventually belong to some kingdom.
    std::queue<TileID> frontier;
    for (const auto& tile : map.tiles()) {
        if (tile.owner != NO_KINGDOM) frontier.push(tile.id);
    }
    while (!frontier.empty()) {
        TileID cur = frontier.front(); frontier.pop();
        KingdomID owner = map.at(cur).owner;
        for (TileID nid : map.neighbors4v(cur)) {
            Tile& nb = map.at(nid);
            if (nb.owner != NO_KINGDOM) continue;
            if (!isClaimableTerritory(nb.terrain)) continue;
            nb.owner = owner;
            frontier.push(nid);
        }
    }

    for (auto& tile : map.tiles()) {
        if (!isClaimableTerritory(tile.terrain)) {
            tile.owner = NO_KINGDOM;
        }
    }
}

void NationGenerator::applySpecializationBonuses(Kingdom& k) const {
    switch (k.specialization) {
        case KingdomSpecialization::Military:
            k.treasury.iron  *= 1.3f;
            k.combatBonus     = 1.15f;
            k.treasury.gold  *= 0.85f;
            break;
        case KingdomSpecialization::Economy:
            k.treasury.gold  *= 1.4f;
            k.goldBonus       = 1.2f;
            k.treasury.iron  *= 0.8f;
            break;
        case KingdomSpecialization::Agriculture:
            k.treasury.food  *= 1.4f;
            k.foodBonus       = 1.2f;
            k.treasury.stone *= 0.85f;
            break;
        case KingdomSpecialization::Technology:
            k.treasury.gold  *= 1.1f;
            k.treasury.iron  *= 1.1f;
            break;
        case KingdomSpecialization::Trade:
            k.treasury.gold  *= 1.3f;
            k.goldBonus       = 1.15f;
            k.treasury.iron  *= 0.85f;
            break;
        case KingdomSpecialization::Defense:
            k.defenseBonus    = 1.2f;
            k.treasury.stone *= 1.4f;
            k.treasury.gold  *= 0.85f;
            break;
    }
}

KingdomPersonality NationGenerator::assignPersonality(int /*index*/) const {
    // Fully random personality per run — seed controls the outcome
    int r = static_cast<int>(rng_.nextInt(0, 5));
    switch (r) {
        case 0: return KingdomPersonality::Aggressive;
        case 1: return KingdomPersonality::Defensive;
        case 2: return KingdomPersonality::Economic;
        case 3: return KingdomPersonality::Diplomatic;
        case 4: return KingdomPersonality::Opportunistic;
        default: return KingdomPersonality::Expansionist;
    }
}

KingdomSpecialization NationGenerator::assignSpecialization(int index) const {
    static const KingdomSpecialization specs[] = {
        KingdomSpecialization::Military,
        KingdomSpecialization::Defense,
        KingdomSpecialization::Economy,
        KingdomSpecialization::Trade,
        KingdomSpecialization::Agriculture,
        KingdomSpecialization::Technology,
        KingdomSpecialization::Military,
        KingdomSpecialization::Economy,
        KingdomSpecialization::Agriculture,
        KingdomSpecialization::Defense,
        KingdomSpecialization::Trade,
        KingdomSpecialization::Technology,
        KingdomSpecialization::Military,
        KingdomSpecialization::Agriculture,
        KingdomSpecialization::Economy,
        KingdomSpecialization::Defense,
    };
    return specs[index % 16];
}

} // namespace jke
