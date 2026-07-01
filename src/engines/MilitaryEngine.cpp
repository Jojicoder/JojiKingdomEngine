#include "jke/engines/MilitaryEngine.hpp"
#include "jke/terrain/TerrainType.hpp"
#include "jke/city/City.hpp"
#include <queue>
#include <unordered_map>
#include <algorithm>
#include <cmath>

namespace jke {
namespace {
bool isClaimableTerritory(TerrainType terrain) {
    return terrain != TerrainType::Ocean &&
           terrain != TerrainType::Coast &&
           terrain != TerrainType::Lake;
}
}

void MilitaryEngine::update(
    std::unordered_map<KingdomID, Kingdom>& kingdoms,
    std::unordered_map<ArmyID, Army>& armies,
    const std::unordered_map<CityID, City>& cities,
    WorldMap& worldMap,
    EventBus& /*bus*/,
    TurnNumber /*turn*/,
    Season season)
{
    decaySupply(armies, cities, worldMap, season);
    moveArmies(armies, worldMap, season);
    removeEmptyArmies(kingdoms, armies, worldMap);
}

void MilitaryEngine::moveArmies(
    std::unordered_map<ArmyID, Army>& armies,
    WorldMap& worldMap,
    Season season) const
{
    // Base movement budget per turn (plain = 1 cost, mountain = 4 cost)
    constexpr float BASE_MOVEMENT = 4.0f;
    const SeasonModifiers mods = seasonMods(season);

    for (auto& [aid, army] : armies) {
        // Refresh movement points each turn
        army.movementPoints = BASE_MOVEMENT * mods.armySpeed;

        if (army.isInBattle || army.isBesieging) continue;
        if (army.targetTile == NO_TILE || army.targetTile == army.currentTile) continue;

        // Compute path if not cached or stale (cursor past end = path exhausted)
        bool pathStale = army.movementPath.empty() ||
                         army.pathCursor >= army.movementPath.size() ||
                         army.movementPath.back() != army.targetTile;
        if (pathStale) {
            army.movementPath = findPath(worldMap, army.currentTile, army.targetTile);
            army.pathCursor   = 0;
        }

        if (army.movementPath.empty()) {
            army.targetTile = NO_TILE;
            continue;
        }

        // Move as many tiles as movement budget allows (no erase — use cursor index)
        while (army.pathCursor < army.movementPath.size() && army.movementPoints > 0.0f) {
            TileID next = army.movementPath[army.pathCursor];
            const Tile& nextTile = worldMap.at(next);
            float cost = terrainMoveCost(nextTile.terrain);

            if (cost > army.movementPoints) break;

            army.movementPoints -= cost;
            ++army.pathCursor;

            // Remove from old tile
            if (army.currentTile != NO_TILE &&
                army.currentTile < static_cast<TileID>(worldMap.tileCount())) {
                Tile& oldTile = worldMap.at(army.currentTile);
                if (oldTile.army == aid) oldTile.army = NO_ARMY;
            }

            // Move to new tile
            army.currentTile = next;
            army.position    = worldMap.at(next).position;
            worldMap.at(next).army = aid;

            // Claim land while advancing; city tiles only change after conquest
            Tile& newTile = worldMap.at(next);
            if (newTile.owner != army.owner &&
                newTile.city == NO_CITY &&
                isClaimableTerritory(newTile.terrain)) {
                newTile.owner = army.owner;
            }

            if (army.currentTile == army.targetTile) {
                army.targetTile = NO_TILE;
                army.movementPath.clear();
                army.pathCursor = 0;
                break;
            }
        }
    }
}

void MilitaryEngine::decaySupply(
    std::unordered_map<ArmyID, Army>& armies,
    const std::unordered_map<CityID, City>& cities,
    const WorldMap& worldMap,
    Season season) const
{
    const SeasonModifiers mods = seasonMods(season);

    for (auto& [aid, army] : armies) {
        if (army.currentTile == NO_TILE ||
            army.currentTile >= static_cast<TileID>(worldMap.tileCount())) {
            continue;
        }

        const Tile& t = worldMap.at(army.currentTile);
        auto armyPos = t.position;
        // Compare squared distances — sqrt only on the winner to compute effectiveRange
        float nearestDistSq = 1e18f;
        bool  nearestIsPort = false;
        for (const auto& [cid, city] : cities) {
            if (city.owner != army.owner || city.tile == NO_TILE) continue;
            float dx = float(armyPos.x - city.position.x);
            float dy = float(armyPos.y - city.position.y);
            float distSq = dx*dx + dy*dy;
            if (distSq < nearestDistSq) {
                nearestDistSq = distSq;
                nearestIsPort = (city.cityType == CityType::Port);
                if (distSq < 64.0f) break;  // < 8 tiles away — close enough
            }
        }
        for (const Tile& supplyTile : worldMap.tiles()) {
            if (supplyTile.owner != army.owner) continue;
            if (supplyTile.strategicPoint != StrategicPointType::SupplyDepot &&
                supplyTile.strategicPoint != StrategicPointType::HarborSite &&
                supplyTile.strategicPoint != StrategicPointType::Bridge) {
                continue;
            }
            float dx = float(armyPos.x - supplyTile.position.x);
            float dy = float(armyPos.y - supplyTile.position.y);
            float distSq = dx*dx + dy*dy;
            if (distSq < nearestDistSq) {
                nearestDistSq = distSq;
                nearestIsPort = supplyTile.strategicPoint == StrategicPointType::HarborSite;
                if (distSq < 49.0f) break;
            }
        }
        float nearestCity = std::sqrt(nearestDistSq);
        // Port cities extend effective supply range by 8 tiles (coastal logistics)
        float effectiveRange = nearestCity;
        if (nearestIsPort) effectiveRange = std::max(0.0f, nearestCity - 8.0f);

        float delta = 0.05f;
        if (effectiveRange > 28.0f) {
            delta = -0.055f;
        } else if (effectiveRange > 20.0f) {
            delta = -0.035f;
        } else if (effectiveRange > 13.0f) {
            delta = -0.015f;
        } else if (t.owner == army.owner) {
            delta = 0.075f;
        }

        if (t.owner != army.owner && t.owner != NO_KINGDOM) {
            delta -= 0.02f;
        }

        // Terrain modifiers: rivers ease supply, mountains drain it
        if (t.terrain == TerrainType::River || t.hasRiver) delta += 0.018f;
        if (t.terrain == TerrainType::Mountain)             delta -= 0.025f;
        if (t.terrain == TerrainType::Hill)                 delta -= 0.010f;
        if (t.owner == army.owner) {
            if (t.strategicPoint == StrategicPointType::SupplyDepot) delta += 0.030f;
            if (t.strategicPoint == StrategicPointType::HarborSite)  delta += 0.024f;
            if (t.strategicPoint == StrategicPointType::Bridge ||
                t.strategicPoint == StrategicPointType::RiverFord)   delta += 0.014f;
        }

        if (delta < 0.0f) {
            delta *= mods.supplyDecay;
        } else if (season == Season::Winter && t.owner == army.owner) {
            delta += 0.035f;
        } else if (season == Season::Spring) {
            delta += 0.010f;
        }

        army.supplyLevel += delta;
        army.supplyLevel = std::clamp(army.supplyLevel, 0.08f, 1.0f);

        if (army.supplyLevel < 0.18f && season != Season::Winter) {
            for (auto& u : army.units) {
                uint32_t loss = std::max<uint32_t>(1, static_cast<uint32_t>(u.soldiers * 0.006f));
                u.soldiers = u.soldiers > loss ? u.soldiers - loss : 0;
                u.morale = std::max(0.1f, u.morale - 0.015f);
            }
        } else {
            for (auto& u : army.units) {
                if (t.owner == army.owner && effectiveRange <= 8.0f) {
                    const float restMorale = season == Season::Winter ? 0.02f : 0.01f;
                    u.morale = std::min(1.0f, u.morale + restMorale);
                }
            }
        }

        for (auto& u : army.units) {
            u.supply = army.supplyLevel;
        }
    }
}

void MilitaryEngine::removeEmptyArmies(
    std::unordered_map<KingdomID, Kingdom>& kingdoms,
    std::unordered_map<ArmyID, Army>& armies,
    WorldMap& worldMap) const
{
    std::vector<ArmyID> toRemove;
    for (const auto& [aid, army] : armies) {
        if (army.isEmpty()) toRemove.push_back(aid);
    }
    for (ArmyID aid : toRemove) {
        auto it = armies.find(aid);
        if (it == armies.end()) continue;
        const Army& army = it->second;

        // O(1) tile clear via stored position instead of O(tiles) full scan
        TileID t = army.currentTile;
        if (t != NO_TILE && t < static_cast<TileID>(worldMap.tileCount()) &&
            worldMap.at(t).army == aid) {
            worldMap.at(t).army = NO_ARMY;
        }

        // Only update the owning kingdom, not all kingdoms
        auto kit = kingdoms.find(army.owner);
        if (kit != kingdoms.end()) {
            auto& vec = kit->second.armies;
            vec.erase(std::remove(vec.begin(), vec.end(), aid), vec.end());
        }

        armies.erase(it);
    }
}

// Greedy best-first pathfinding (good enough for strategic movement)
std::vector<TileID> MilitaryEngine::findPath(
    const WorldMap& map, TileID from, TileID to) const
{
    if (from == to || from == NO_TILE || to == NO_TILE) return {};
    if (from >= static_cast<TileID>(map.tileCount())) return {};
    if (to   >= static_cast<TileID>(map.tileCount())) return {};

    struct Node {
        float    priority;
        TileID   id;
        bool operator>(const Node& o) const { return priority > o.priority; }
    };

    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    std::unordered_map<TileID, TileID>  cameFrom;
    std::unordered_map<TileID, float>   gCost;
    cameFrom.reserve(256);
    gCost.reserve(256);

    auto heuristic = [&](TileID a, TileID b) -> float {
        auto [ax, ay] = map.coordOf(a);
        auto [bx, by] = map.coordOf(b);
        return std::hypot(float(ax-bx), float(ay-by));
    };

    open.push({0.0f, from});
    gCost[from] = 0.0f;

    int iterations = 0;
    constexpr int MAX_ITER = 3000; // prevent freeze on unreachable targets
    while (!open.empty() && iterations++ < MAX_ITER) {
        TileID cur = open.top().id; open.pop();
        if (cur == to) break;

        TileID nbs[4]; int nCount = map.neighbors4(cur, nbs);
        for (int ni = 0; ni < nCount; ++ni) {
            TileID nb = nbs[ni];
            const Tile& nt = map.at(nb);
            if (nt.terrain == TerrainType::Ocean ||
                nt.terrain == TerrainType::Lake) continue;

            float cost = gCost[cur] + terrainMoveCost(nt.terrain);
            if (!gCost.count(nb) || cost < gCost[nb]) {
                gCost[nb]    = cost;
                cameFrom[nb] = cur;
                open.push({cost + heuristic(nb, to), nb});
            }
        }
    }

    // Reconstruct path
    std::vector<TileID> path;
    TileID cur = to;
    while (cur != from) {
        auto it = cameFrom.find(cur);
        if (it == cameFrom.end()) return {}; // no path
        path.push_back(cur);
        cur = it->second;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

ArmyID MilitaryEngine::spawnArmy(
    std::unordered_map<KingdomID, Kingdom>& kingdoms,
    std::unordered_map<ArmyID, Army>& armies,
    WorldMap& worldMap,
    ArmyID& nextArmyID,
    UnitID& nextUnitID,
    const RecruitOrder& order,
    const std::unordered_map<CityID, City>& cities)
{
    auto cityIt = cities.find(order.city);
    if (cityIt == cities.end()) return NO_ARMY;

    // Must spawn at a city we actually own
    if (cityIt->second.owner != order.kingdom) {
        // Fall back to any owned city
        cityIt = cities.end();
        for (auto it = cities.begin(); it != cities.end(); ++it) {
            if (it->second.owner == order.kingdom) { cityIt = it; break; }
        }
        if (cityIt == cities.end()) return NO_ARMY;
    }

    TileID spawnTile = cityIt->second.tile;

    // Prefer a city tile that has no garrison yet
    auto& kRef = kingdoms.at(order.kingdom);
    for (CityID cid : kRef.cities) {
        auto cit2 = cities.find(cid);
        if (cit2 == cities.end()) continue;
        if (cit2->second.owner != order.kingdom) continue;
        TileID ct = cit2->second.tile;
        if (ct == NO_TILE) continue;
        if (worldMap.at(ct).army == NO_ARMY ||
            !armies.count(worldMap.at(ct).army)) {
            spawnTile = ct;
            break;
        }
    }

    // If chosen tile is occupied, try adjacent land tiles
    if (worldMap.at(spawnTile).army != NO_ARMY &&
        armies.count(worldMap.at(spawnTile).army)) {
        bool found = false;
        TileID snbs[4]; int snCount = worldMap.neighbors4(spawnTile, snbs);
        for (int sni = 0; sni < snCount; ++sni) { TileID nb = snbs[sni];
            const Tile& t = worldMap.at(nb);
            if (t.terrain == TerrainType::Ocean || t.terrain == TerrainType::Lake) continue;
            if (t.army != NO_ARMY && armies.count(t.army)) continue;
            spawnTile = nb;
            found = true;
            break;
        }
        if (!found) return NO_ARMY; // no room to spawn
    }

    ArmyID aid = nextArmyID++;
    Army army;
    army.id          = aid;
    army.owner       = order.kingdom;
    army.supplyLevel = 1.0f;

    const Kingdom& kRef2 = kingdoms.at(order.kingdom);
    // Diplomatic kingdoms have less military culture → lower training
    const float trainingBase = (kRef2.personality == KingdomPersonality::Diplomatic) ? 0.48f
                             : (kRef2.personality == KingdomPersonality::Aggressive)  ? 0.95f
                             : (kRef2.personality == KingdomPersonality::Expansionist)? 0.85f
                             : (kRef2.personality == KingdomPersonality::Opportunistic)? 0.90f
                             : 0.75f;

    Unit u;
    u.id        = nextUnitID++;
    u.type      = order.unitType;
    u.soldiers  = order.soldiers;
    u.training  = trainingBase;
    u.equipment = 0.75f;
    u.experience= 0.0f;
    u.morale    = 1.0f;
    u.supply    = 1.0f;
    army.units.push_back(u);

    army.currentTile = spawnTile;
    army.position    = worldMap.at(spawnTile).position;
    worldMap.at(spawnTile).army = aid;

    auto& k = kingdoms.at(order.kingdom);
    k.armies.push_back(aid);
    armies[aid] = std::move(army);
    return aid;
}

} // namespace jke
