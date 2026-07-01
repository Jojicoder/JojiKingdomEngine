#include "jke/SimulationEngine.hpp"
#include <algorithm>
#include <cmath>

namespace jke {

bool SimulationEngine::canFoundCity(const Kingdom& k, TileID tile) const {
    if (tile == NO_TILE || tile >= static_cast<TileID>(worldMap_.tileCount())) return false;
    const auto& t = worldMap_.at(tile);

    // Expansionist/Opportunistic: settler doctrine — plant colonies anywhere habitable
    // (NationGenerator owns all land so "unclaimed" check is useless; just allow any land)
    const bool isSettler = (k.personality == KingdomPersonality::Expansionist ||
                            k.personality == KingdomPersonality::Opportunistic);
    if (!isSettler) {
        if (t.owner != k.id) return false;
    }

    if (t.city  != NO_CITY) return false;
    if (t.terrain == TerrainType::Ocean ||
        t.terrain == TerrainType::Lake  ||
        t.terrain == TerrainType::Coast) return false;
    if (t.terrain == TerrainType::Mountain && !isSettler) return false;
    // Minimum founding distance: Expansionist uses dense colonial network (4 tiles),
    // others use normal spacing (16 tiles)
    const float minDist = (k.personality == KingdomPersonality::Expansionist ||
                           k.personality == KingdomPersonality::Opportunistic) ? 4.0f : 16.0f;
    for (const auto& [cid, city] : cities_) {
        float d = std::hypot(float(t.position.x - city.position.x),
                             float(t.position.y - city.position.y));
        if (d < minDist) return false;
    }
    return true;
}

void SimulationEngine::runAICityBuilding() {
    namespace C = constants;

    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        // Diplomatic kingdoms use slow civic expansion (60-turn cooldown, high cost, no hard cap)
        const bool isExpansionist  = (k.personality == KingdomPersonality::Expansionist);
        const bool isOpportunistic = (k.personality == KingdomPersonality::Opportunistic);
        // Expansionist/Opportunistic: can found starting from their 1 capital; others need 3
        const size_t minCities = (isExpansionist || isOpportunistic) ? 1u : 3u;
        if (k.cities.size() < minCities) continue;
        const TurnNumber cooldown = isExpansionist  ? 25u :   // aggressive settlement pace
                                    isOpportunistic ? 40u : 60u;
        // Expansionist/Opportunistic: gold-only (lightweight frontier outposts)
        const float goldCost  = isExpansionist ? 80.0f : isOpportunistic ? 120.0f : 180.0f;
        const float woodCost  = (isExpansionist || isOpportunistic) ? 0.0f : 100.0f;
        const float stoneCost = (isExpansionist || isOpportunistic) ? 0.0f :  60.0f;
        if (currentTurn_ - k.lastCityFounded < cooldown) continue;
        if (k.treasury.gold  < goldCost)  continue;
        if (!isExpansionist && !isOpportunistic && k.treasury.wood  < woodCost)  continue;
        if (!isExpansionist && !isOpportunistic && k.treasury.stone < stoneCost) continue;
        if (k.stability < 0.40f) continue;  // slightly more tolerant of instability

        auto nearCoast = [&](TileID tileId) {
            if (tileId == NO_TILE || tileId >= static_cast<TileID>(worldMap_.tileCount())) return false;
            const Tile& tile = worldMap_.at(tileId);
            if (tile.terrain == TerrainType::Coast) return true;
            for (TileID nid : worldMap_.neighbors4v(tileId)) {
                const TerrainType terrain = worldMap_.at(nid).terrain;
                if (terrain == TerrainType::Coast || terrain == TerrainType::Ocean) return true;
            }
            return false;
        };

        auto nearestEnemyCityDistance = [&](TileID tileId) {
            float best = 1e9f;
            const auto pos = worldMap_.at(tileId).position;
            for (const auto& [cid, city] : cities_) {
                if (city.owner == kid || city.owner == NO_KINGDOM) continue;
                if (!kingdoms_.count(city.owner) || !kingdoms_.at(city.owner).isAlive) continue;
                const auto cityPos = worldMap_.at(city.tile).position;
                best = std::min(best, std::hypot(float(pos.x - cityPos.x), float(pos.y - cityPos.y)));
            }
            return best;
        };

        auto applyFoundedCityType = [&](City& city, const Tile& tile) {
            const bool coastal = nearCoast(city.tile);
            const bool highResource = tile.resourceRichness > 0.75f;
            const bool onRiver = tile.hasRiver || tile.terrain == TerrainType::River;
            const bool hillOrMountain =
                tile.terrain == TerrainType::Hill ||
                tile.terrain == TerrainType::Mountain;
            const bool fertile = tile.fertility > 0.75f || onRiver;

            if (hillOrMountain && city.fortification >= 0.24f) {
                city.cityType = CityType::Fortress;
                city.fortification = std::min(1.0f, city.fortification + 0.24f);
                city.baseProduction.stone *= 1.5f;
                city.baseProduction.iron *= 1.25f;
                city.buildings.push_back({BuildingType::Walls, 1, 1.0f});
            } else if (coastal) {
                city.cityType = CityType::Port;
                city.baseProduction.gold *= 1.55f;
                city.baseProduction.food *= 0.90f;
                city.buildings.push_back({BuildingType::Market, 1, 1.0f});
            } else if (highResource) {
                city.cityType = CityType::Mining;
                city.baseProduction.iron *= 2.0f;
                city.baseProduction.stone *= 1.7f;
                city.buildings.push_back({BuildingType::IronMine, 1, 1.0f});
            } else if (fertile) {
                city.cityType = CityType::Agricultural;
                city.baseProduction.food *= 1.65f;
                city.population = static_cast<uint32_t>(city.population * 1.12f);
                city.buildings.push_back({BuildingType::Granary, 1, 1.0f});
            } else if (k.cities.size() >= 6) {
                city.cityType = CityType::TradeHub;
                city.baseProduction.gold *= 1.25f;
                city.happiness = std::min(1.0f, city.happiness + 0.03f);
                city.buildings.push_back({BuildingType::Market, 1, 1.0f});
            }
        };

        // Find best buildable tile
        TileID best = NO_TILE;
        float  bestScore = 0.0f;
        for (const auto& t : worldMap_.tiles()) {
            if (!canFoundCity(k, t.id)) continue;
            const bool coastal = nearCoast(t.id);
            const bool hillOrMountain =
                t.terrain == TerrainType::Hill ||
                t.terrain == TerrainType::Mountain;
            const float enemyDist = nearestEnemyCityDistance(t.id);
            float score = t.fertility * 1.25f + t.resourceRichness * 1.15f;
            if (t.hasRiver || t.terrain == TerrainType::River) score += 1.2f;
            if (t.terrain == TerrainType::Hill) score += 1.1f;
            if (t.terrain == TerrainType::Mountain) score += 1.5f;
            if (coastal) score += 0.9f;
            if (t.resourceRichness > 0.75f) score += 0.8f;
            if (hillOrMountain && (isExpansionist || isOpportunistic)) score += 0.7f;
            if (enemyDist < 36.0f) score += std::max(0.0f, 36.0f - enemyDist) * 0.035f;
            if (enemyDist < 12.0f && !isExpansionist && !isOpportunistic) score -= 1.2f;
            if (score > bestScore) { bestScore = score; best = t.id; }
        }
        // Expansionist/Opportunistic settle more easily (lower quality standards)
        const float minBuildScore = (isExpansionist || isOpportunistic) ? 0.6f : 1.2f;
        if (best == NO_TILE || bestScore < minBuildScore) continue;

        // Found the city
        k.treasury.gold  -= goldCost;
        k.treasury.wood  -= woodCost;
        k.treasury.stone -= stoneCost;
        k.lastCityFounded = currentTurn_;

        const Tile& tile = worldMap_.at(best);
        City city;
        city.owner         = kid;
        city.originalOwner = kid;
        city.tile          = best;
        city.position      = tile.position;
        city.isCapital     = false;
        city.foundedTurn   = currentTurn_;
        city.name          = k.name.substr(0, 4) +
                             (rng_.chance(0.5f) ? "wick" : "ford");
        city.population    = isExpansionist ? rng_.nextInt(500, 800) :
                             isOpportunistic ? rng_.nextInt(400, 700) :
                             rng_.nextInt(300, 600);
        city.baseProduction.food  = 16.0f * tile.fertility;
        city.baseProduction.gold  = 5.0f;
        city.baseProduction.wood  = (tile.terrain == TerrainType::Forest) ? 10.0f : 3.0f;
        city.baseProduction.stone = (tile.terrain == TerrainType::Hill)   ? 10.0f : 3.0f;
        city.baseProduction.iron  = tile.resourceRichness * 3.0f;
        city.happiness     = 0.70f;
        // Expansionist/Opportunistic build sturdier frontier outposts
        city.fortification = (isExpansionist || isOpportunistic) ? 0.26f : 0.12f;
        city.buildings.push_back({BuildingType::Farm, 1, 1.0f});
        applyFoundedCityType(city, tile);

        CityID cid = nextCityID_++;
        city.id = cid;
        worldMap_.at(best).city  = cid;
        worldMap_.at(best).owner = kid;   // settler claims the tile on founding
        k.cities.push_back(cid);
        k.totalPopulation += city.population;
        cities_[cid] = std::move(city);

        HistoryEvent ev;
        ev.type           = EventType::CityFounded2;
        ev.turn           = currentTurn_;
        ev.primaryKingdom = kid;
        ev.relatedCity    = cid;
        ev.description    = k.name + " founded a new settlement.";
        eventBus_.emit(std::move(ev));
        eventBus_.flush();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  履歴スナップショット（グラフ用）
// ─────────────────────────────────────────────────────────────────────────────

} // namespace jke
