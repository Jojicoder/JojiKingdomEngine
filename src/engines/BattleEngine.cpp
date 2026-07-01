#include "jke/engines/BattleEngine.hpp"
#include "jke/core/Constants.hpp"

#include <algorithm>
#include <unordered_set>

namespace jke {
namespace {
bool isClaimableTerritory(TerrainType terrain) {
    return terrain != TerrainType::Ocean &&
           terrain != TerrainType::Coast &&
           terrain != TerrainType::Lake;
}
}

BattleEngine::BattleEngine(Random& rng) : rng_(rng) {}

void BattleEngine::update(
    std::unordered_map<KingdomID, Kingdom>& kingdoms,
    std::unordered_map<ArmyID, Army>& armies,
    std::unordered_map<CityID, City>& cities,
    WorldMap& worldMap,
    EventBus& bus,
    TurnNumber turn,
    const RelationMap& relations,
    Season season)
{
    auto battles = detectBattles(armies, cities, worldMap, kingdoms, relations, turn, season);

    for (auto& ctx : battles) {
        if (!armies.count(ctx.attackerArmy)) continue;
        if (ctx.defenderArmy != NO_ARMY && !armies.count(ctx.defenderArmy)) continue;
        if (!kingdoms.count(ctx.attackerKingdom) || !kingdoms.at(ctx.attackerKingdom).isAlive) continue;
        if (!kingdoms.count(ctx.defenderKingdom) || !kingdoms.at(ctx.defenderKingdom).isAlive) continue;

        auto result = resolveBattle(ctx, armies, kingdoms, cities);
        applyResult(result, armies, kingdoms, cities, worldMap, bus, turn);
    }
}

std::vector<BattleContext> BattleEngine::detectBattles(
    const std::unordered_map<ArmyID, Army>& armies,
    const std::unordered_map<CityID, City>& cities,
    const WorldMap& worldMap,
    const std::unordered_map<KingdomID, Kingdom>& kingdoms,
    const RelationMap& relations,
    TurnNumber turn,
    Season season) const
{
    std::vector<BattleContext> battles;
    std::unordered_set<TileID> processed;

    tileArmies_.clear();
    for (const auto& [aid, army] : armies) {
        auto kit = kingdoms.find(army.owner);
        if (kit == kingdoms.end() || !kit->second.isAlive) continue;
        tileArmies_[army.currentTile].push_back(aid);
    }

    for (const auto& [tid, armyList] : tileArmies_) {
        if (processed.count(tid)) continue;
        if (armyList.empty()) continue;

        const Tile& tile = worldMap.at(tid);

        // One army on enemy city = siege
        if (armyList.size() == 1) {
            if (tile.city == NO_CITY) continue;

            const City& city = cities.at(tile.city);
            const Army& army = armies.at(armyList[0]);

            if (city.owner == NO_KINGDOM && army.owner != NO_KINGDOM) {
                // Neutral outpost: army walks in and claims it immediately
                BattleContext ctx;
                ctx.attackerArmy    = armyList[0];
                ctx.defenderArmy    = NO_ARMY;
                ctx.attackerKingdom = army.owner;
                ctx.defenderKingdom = NO_KINGDOM;
                ctx.battleTile      = tid;
                ctx.terrain         = tile.terrain;
                ctx.contestedCity   = tile.city;
                ctx.isSiege         = true;
                ctx.turn            = turn;
                ctx.season          = season;

                battles.push_back(ctx);
                processed.insert(tid);
                continue;
            }

            if (city.owner != NO_KINGDOM &&
                city.owner != army.owner &&
                areAtWar(army.owner, city.owner, relations)) {

                BattleContext ctx;
                ctx.attackerArmy    = armyList[0];
                ctx.defenderArmy    = NO_ARMY;
                ctx.attackerKingdom = army.owner;
                ctx.defenderKingdom = city.owner;
                ctx.battleTile      = tid;
                ctx.terrain         = tile.terrain;
                ctx.contestedCity   = tile.city;
                ctx.isSiege         = true;
                ctx.turn            = turn;

                battles.push_back(ctx);
                processed.insert(tid);
            }

            continue;
        }

        // On a city tile, resolve the city owner's defense before unrelated
        // third-party fights; otherwise sieges can be starved forever.
        if (tile.city != NO_CITY) {
            const City& city = cities.at(tile.city);
            ArmyID cityDefender = NO_ARMY;
            ArmyID siegeArmy = NO_ARMY;
            ArmyID fallbackArmy = NO_ARMY;

            for (ArmyID aid : armyList) {
                const Army& army = armies.at(aid);
                if (army.owner == city.owner) {
                    cityDefender = aid;
                    continue;
                }
                if (city.owner != NO_KINGDOM &&
                    areAtWar(army.owner, city.owner, relations)) {
                    if (fallbackArmy == NO_ARMY) fallbackArmy = aid;
                    for (const auto& unit : army.units) {
                        if (unit.type == UnitType::SiegeUnit && unit.soldiers > 0) {
                            siegeArmy = aid;
                            break;
                        }
                    }
                    if (siegeArmy != NO_ARMY && cityDefender != NO_ARMY) break;
                }
            }

            ArmyID attacker = siegeArmy != NO_ARMY ? siegeArmy : fallbackArmy;
            if (attacker != NO_ARMY) {
                const Army& army = armies.at(attacker);
                BattleContext ctx;
                ctx.attackerArmy    = attacker;
                ctx.defenderArmy    = cityDefender;
                ctx.attackerKingdom = army.owner;
                ctx.defenderKingdom = city.owner;
                ctx.battleTile      = tid;
                ctx.terrain         = tile.terrain;
                ctx.contestedCity   = tile.city;
                ctx.isSiege         = cityDefender == NO_ARMY;
                ctx.turn            = turn;
                ctx.season          = season;

                battles.push_back(ctx);
                processed.insert(tid);
            }
        }

        if (processed.count(tid)) continue;

        // Multiple armies on same non-city tile, or no active city siege.
        for (size_t i = 0; i < armyList.size(); ++i) {
            for (size_t j = i + 1; j < armyList.size(); ++j) {
                const Army& a = armies.at(armyList[i]);
                const Army& b = armies.at(armyList[j]);

                if (a.owner == b.owner) continue;
                if (!areAtWar(a.owner, b.owner, relations)) continue;

                BattleContext ctx;
                ctx.attackerArmy    = armyList[i];
                ctx.defenderArmy    = armyList[j];
                ctx.attackerKingdom = a.owner;
                ctx.defenderKingdom = b.owner;
                ctx.battleTile      = tid;
                ctx.terrain         = tile.terrain;
                ctx.contestedCity   = tile.city;
                ctx.isSiege         = false;
                ctx.turn            = turn;
                ctx.season          = season;

                battles.push_back(ctx);
                processed.insert(tid);
                break;
            }

            if (processed.count(tid)) break;
        }
    }

    return battles;
}

BattleResult BattleEngine::resolveBattle(
    BattleContext ctx,
    std::unordered_map<ArmyID, Army>& armies,
    std::unordered_map<KingdomID, Kingdom>& kingdoms,
    std::unordered_map<CityID, City>& cities)
{
    BattleResult result;
    result.attackerArmy = ctx.attackerArmy;
    result.defenderArmy = ctx.defenderArmy;

    Army& attacker = armies.at(ctx.attackerArmy);

    float atkStr = attacker.combatStrength(ctx.terrain, false);
    {
        const Kingdom& ak = kingdoms.at(ctx.attackerKingdom);
        atkStr *= ak.combatBonus;
        if (ak.hasRuler) atkStr *= ak.ruler.combatMult;
        if (ak.personality == KingdomPersonality::Defensive) {
            atkStr *= 0.93f;
        }
    }

    switch (ctx.season) {
        case Season::Spring: atkStr *= 1.08f; break;  // invasion season
        case Season::Winter: atkStr *= 0.82f; break;  // hard to attack in winter
        default: break;
    }

    // Terrain modifiers for attacker
    switch (ctx.terrain) {
        case TerrainType::Forest:   atkStr *= 0.85f; break;  // dense cover hampers assault
        case TerrainType::Mountain: atkStr *= 0.70f; break;  // uphill assault is brutal
        case TerrainType::Hill:     atkStr *= 0.88f; break;
        case TerrainType::River:    atkStr *= 0.80f; break;  // river crossing under fire
        default: break;
    }
    atkStr *= 1.0f + rng_.nextFloat(
        -constants::RANDOM_COMBAT_FACTOR,
         constants::RANDOM_COMBAT_FACTOR
    );

    float defStr = 1.0f;

    if (ctx.isSiege) {
        const City& city = cities.at(ctx.contestedCity);

        // Garrison scaled so a standard army (800-2000 soldiers) can realistically
        // capture a city.  Old formula (base 300 + fort*900) made ratio ~0.1 —
        // sieges never succeeded.  New formula makes a 1200-soldier army roughly
        // equal to a lightly-fortified city.
        float populationDefense = static_cast<float>(city.population) * 0.003f;  // reduced (was 0.005)
        float fortDefense       = city.fortification * 85.0f;   // reduced (was 120)
        float baseGarrison      = 60.0f;

        defStr = baseGarrison + populationDefense + fortDefense;
        defStr *= kingdoms.at(ctx.defenderKingdom).defenseBonus;
        if (ctx.season == Season::Winter) {
            defStr *= 1.12f;  // winter favors prepared defenders
        }

        result.defenderArmy = NO_ARMY;
    } else {
        Army& defender = armies.at(ctx.defenderArmy);

        defStr = defender.combatStrength(ctx.terrain, true);
        defStr *= kingdoms.at(ctx.defenderKingdom).defenseBonus;

        if (kingdoms.at(ctx.attackerKingdom).personality == KingdomPersonality::Opportunistic &&
            defender.supplyLevel < 0.55f) {
            atkStr *= defender.supplyLevel < 0.35f ? 1.24f : 1.12f;
        }

        // Aggressive defending their own territory: warrior culture fights for home
        if (kingdoms.at(ctx.defenderKingdom).personality == KingdomPersonality::Aggressive &&
            ctx.terrain != TerrainType::Ocean && ctx.terrain != TerrainType::Lake)
            defStr *= 1.18f;

        // Expansionist defending their settlements: settlers protect what they built
        if (kingdoms.at(ctx.defenderKingdom).personality == KingdomPersonality::Expansionist)
            defStr *= 1.12f;

        // Defender gets terrain bonus (they picked the ground)
        switch (ctx.terrain) {
            case TerrainType::Forest:   defStr *= 1.20f; break;
            case TerrainType::Mountain: defStr *= 1.45f; break;
            case TerrainType::Hill:     defStr *= 1.25f; break;
            case TerrainType::River:    defStr *= 1.15f; break;
            default: break;
        }
        defStr *= 1.0f + rng_.nextFloat(
            -constants::RANDOM_COMBAT_FACTOR,
             constants::RANDOM_COMBAT_FACTOR
        );
    }

    result.attackerStrength = atkStr;
    result.defenderStrength = defStr;

    float ratio = atkStr / std::max(defStr, 1.0f);

    if (ctx.isSiege) {
        City& city = cities.at(ctx.contestedCity);

        // ── Siege food stores: deplete while besieged ────────────────────────
        city.siegeTurns++;
        city.siegeFoodStores = std::max(0.0f, city.siegeFoodStores - 0.035f);

        // Starvation weakens garrison
        float starvationMult = 1.0f;
        if (city.siegeFoodStores < 0.2f)
            starvationMult = 0.5f + city.siegeFoodStores * 2.5f; // 0→0.5, 0.2→1.0

        float defStrMod = defStr * starvationMult;

        // ── Siege progress accumulation ───────────────────────────────────────
        float siegePower = 0.0f;
        for (const auto& unit : attacker.units) {
            if (unit.type == UnitType::SiegeUnit)
                siegePower += unit.baseCombatPower();
        }
        siegePower *= kingdoms.at(ctx.attackerKingdom).siegeBonus;
        bool hasSiegeAdvantage = siegePower > 30.0f;

        // Aggressive kingdoms: relentless assault culture → +25% siege progress
        const bool aggressiveAttacker =
            kingdoms.count(ctx.attackerKingdom) &&
            kingdoms.at(ctx.attackerKingdom).personality == KingdomPersonality::Aggressive;

        // Progress rate: ratio above 0.25 contributes; fort slows it; siege engines speed it
        float progressRate = std::max(0.0f, (atkStr / std::max(defStrMod, 1.0f)) - 0.25f)
                             * 0.10f
                             / (1.0f + city.fortification * 4.5f);
        if (hasSiegeAdvantage)  progressRate *= 1.6f;
        if (aggressiveAttacker) progressRate *= 1.25f;
        if (kingdoms.count(ctx.attackerKingdom) &&
            kingdoms.at(ctx.attackerKingdom).personality == KingdomPersonality::Opportunistic) {
            progressRate *= 1.18f;
        }
        switch (ctx.season) {
            case Season::Autumn: progressRate *= 1.65f; break; // siege season
            case Season::Spring: progressRate *= 0.90f; break;
            case Season::Winter: progressRate *= 0.40f; break;
            default: break;
        }
        // Starvation accelerates collapse
        if (city.siegeFoodStores < 0.2f) progressRate *= 2.0f;

        // Sortie: defenders push back (15% chance)
        if (rng_.chance(0.15f)) progressRate -= 0.05f;

        // Accumulate on the besieging army
        Army& attackerArmy = armies.at(ctx.attackerArmy);
        attackerArmy.siegeProgress = std::clamp(
            attackerArmy.siegeProgress + progressRate, 0.0f, 1.0f);

        // Besieger also drains supply faster in enemy territory
        attackerArmy.supplyLevel = std::max(0.0f, attackerArmy.supplyLevel - 0.02f);

        int importantHoldings = 0;
        if (city.isCapital && kingdoms.count(ctx.defenderKingdom)) {
            for (CityID cid : kingdoms.at(ctx.defenderKingdom).cities) {
                auto cit = cities.find(cid);
                if (cit == cities.end()) continue;
                const City& holding = cit->second;
                if (holding.id == city.id || holding.owner != ctx.defenderKingdom) continue;
                const bool strategic =
                    holding.population >= 700 ||
                    holding.cityType == CityType::Fortress ||
                    holding.cityType == CityType::Port ||
                    holding.cityType == CityType::TradeHub ||
                    holding.cityType == CityType::Mining;
                if (strategic) ++importantHoldings;
            }
        }

        bool capitalProtected = city.isCapital && importantHoldings >= 2;

        // Complete siege when progress reaches 1.0 or food runs out entirely
        bool siegeComplete = !capitalProtected &&
            (attackerArmy.siegeProgress >= 1.0f || city.siegeFoodStores <= 0.0f);

        result.cityConquered = siegeComplete;
        result.conqueredCity = siegeComplete ? ctx.contestedCity : NO_CITY;

        result.victor = siegeComplete ? ctx.attackerKingdom : ctx.defenderKingdom;
        result.loser  = siegeComplete ? ctx.defenderKingdom : ctx.attackerKingdom;

        if (siegeComplete) {
            attackerArmy.siegeProgress = 0.0f;
            attackerArmy.siegeTarget   = NO_CITY;
            city.siegeTurns            = 0;
            city.siegeFoodStores       = 1.0f;
        }

        result.attackerCasualties   = siegeComplete ? 0.08f : 0.02f;
        result.defenderCasualties   = 0.0f;
        result.attackerMoraleChange = siegeComplete ?  0.12f : -0.04f;
        result.defenderMoraleChange = siegeComplete ? -0.35f :  0.03f;

        return result;
    }

    bool attackerWins = ratio > 1.0f;

    result.victor = attackerWins ? ctx.attackerKingdom : ctx.defenderKingdom;
    result.loser  = attackerWins ? ctx.defenderKingdom : ctx.attackerKingdom;

    if (attackerWins) {
        result.attackerCasualties = 0.05f + (1.0f / ratio) * 0.10f;
        result.defenderCasualties = 0.25f + std::min(0.50f, (ratio - 1.0f) * 0.30f);
    } else {
        result.attackerCasualties = 0.25f + std::min(0.50f, ((1.0f / ratio) - 1.0f) * 0.30f);
        result.defenderCasualties = 0.05f + ratio * 0.08f;
    }

    result.attackerMoraleChange = attackerWins ? 0.08f : -0.15f;
    result.defenderMoraleChange = attackerWins ? -0.15f : 0.08f;

    if (ctx.season == Season::Summer) {
        result.attackerCasualties *= 1.25f;
        result.defenderCasualties *= 1.25f;
        result.attackerMoraleChange += attackerWins ? 0.03f : -0.03f;
        result.defenderMoraleChange += attackerWins ? -0.03f : 0.03f;
    } else if (ctx.season == Season::Winter) {
        result.attackerCasualties *= 0.75f;
        result.defenderCasualties *= 0.75f;
    }

    result.attackerRetreated = !attackerWins;
    result.defenderRetreated = attackerWins;

    if (ctx.contestedCity != NO_CITY) {
        const City& city = cities.at(ctx.contestedCity);
        const bool cityOwnerInBattle =
            city.owner == ctx.attackerKingdom || city.owner == ctx.defenderKingdom;

        if (cityOwnerInBattle &&
            result.loser == city.owner &&
            result.victor != city.owner) {
            result.cityConquered = true;
            result.conqueredCity = ctx.contestedCity;
        }
    }

    return result;
}

void BattleEngine::applyResult(
    const BattleResult& result,
    std::unordered_map<ArmyID, Army>& armies,
    std::unordered_map<KingdomID, Kingdom>& kingdoms,
    std::unordered_map<CityID, City>& cities,
    WorldMap& worldMap,
    EventBus& bus,
    TurnNumber turn)
{
    auto applyArmyDamage = [](Army& army, float casualties, float moraleChange) {
        for (auto& unit : army.units) {
            uint32_t loss = static_cast<uint32_t>(unit.soldiers * casualties);
            unit.soldiers = unit.soldiers > loss ? unit.soldiers - loss : 0;

            unit.morale = std::clamp(unit.morale + moraleChange, 0.0f, 1.0f);
            unit.experience = std::min(1.0f, unit.experience + 0.02f);
        }
    };

    auto retreatArmy = [&](Army& army) {
        TileID from = army.currentTile;
        TileID retreatTile = NO_TILE;

        TileID rnbs[4]; int rnCount = worldMap.neighbors4(from, rnbs);
        for (int rni = 0; rni < rnCount; ++rni) {
            TileID nid = rnbs[rni];
            const Tile& tile = worldMap.at(nid);
            if (!isClaimableTerritory(tile.terrain)) continue;
            if (tile.owner != army.owner) continue;
            // Use tile.army field directly instead of scanning all armies
            bool occupiedByEnemy = (tile.army != NO_ARMY &&
                armies.count(tile.army) && armies.at(tile.army).owner != army.owner);
            if (!occupiedByEnemy) {
                retreatTile = nid;
                break;
            }
        }

        army.targetTile = NO_TILE;
        army.movementPath.clear();

        if (retreatTile == NO_TILE) {
            applyArmyDamage(army, 0.50f, -0.10f);
            return;
        }

        if (from != NO_TILE && from < static_cast<TileID>(worldMap.tileCount())) {
            Tile& oldTile = worldMap.at(from);
            if (oldTile.army == army.id) oldTile.army = NO_ARMY;
        }

        army.currentTile = retreatTile;
        army.position = worldMap.at(retreatTile).position;
        worldMap.at(retreatTile).army = army.id;
    };

    if (armies.count(result.attackerArmy)) {
        Army& atk = armies.at(result.attackerArmy);
        applyArmyDamage(atk, result.attackerCasualties, result.attackerMoraleChange);

        if (result.attackerRetreated) {
            retreatArmy(atk);
        }
    }

    if (result.defenderArmy != NO_ARMY && armies.count(result.defenderArmy)) {
        Army& def = armies.at(result.defenderArmy);
        applyArmyDamage(def, result.defenderCasualties, result.defenderMoraleChange);

        if (result.defenderRetreated) {
            retreatArmy(def);
        }
    }

    if (result.cityConquered && result.conqueredCity != NO_CITY) {
        City& city = cities.at(result.conqueredCity);

        KingdomID oldOwner = city.owner;
        KingdomID newOwner = result.victor;

        if (oldOwner != newOwner && oldOwner != NO_KINGDOM && kingdoms.count(oldOwner)) {
            auto& oldCities = kingdoms.at(oldOwner).cities;
            oldCities.erase(
                std::remove(oldCities.begin(), oldCities.end(), result.conqueredCity),
                oldCities.end()
            );

            if (kingdoms.at(oldOwner).capitalCity == result.conqueredCity) {
                const bool lateSmallStateCollapse = turn >= 250 && oldCities.size() <= 2;

                kingdoms.at(oldOwner).capitalCity =
                    (oldCities.empty() || lateSmallStateCollapse) ? NO_CITY : oldCities.front();

                if (!oldCities.empty() && !lateSmallStateCollapse) {
                    cities.at(oldCities.front()).isCapital = true;
                }

                HistoryEvent capEv;
                capEv.type = EventType::CapitalCaptured;
                capEv.turn = turn;
                capEv.primaryKingdom = newOwner;
                capEv.secondaryKingdom = oldOwner;
                capEv.relatedCity = result.conqueredCity;
                capEv.description =
                    kingdoms.at(newOwner).name + " captured the capital of " +
                    kingdoms.at(oldOwner).name + "!";
                bus.emit(std::move(capEv));

                if (lateSmallStateCollapse) {
                    auto remainingCities = oldCities;
                    auto& newCities = kingdoms.at(newOwner).cities;
                    for (CityID cid : remainingCities) {
                        if (!cities.count(cid)) continue;
                        City& annexedCity = cities.at(cid);
                        annexedCity.owner = newOwner;
                        annexedCity.isCapital = false;
                        annexedCity.happiness = std::max(0.2f, annexedCity.happiness - 0.20f);
                        if (std::find(newCities.begin(), newCities.end(), cid) == newCities.end()) {
                            newCities.push_back(cid);
                        }
                        worldMap.at(annexedCity.tile).owner = newOwner;
                    }
                    oldCities.clear();
                }
            }

            if (oldCities.empty()) {
                kingdoms.at(oldOwner).isAlive = false;
                kingdoms.at(oldOwner).annexedBy = newOwner;

                for (ArmyID aid : kingdoms.at(oldOwner).armies) {
                    if (!armies.count(aid)) continue;
                    TileID armyTile = armies.at(aid).currentTile;
                    if (armyTile != NO_TILE && armyTile < static_cast<TileID>(worldMap.tileCount())) {
                        Tile& tile = worldMap.at(armyTile);
                        if (tile.army == aid) tile.army = NO_ARMY;
                    }
                    armies.erase(aid);
                }
                kingdoms.at(oldOwner).armies.clear();

                for (auto& tile : worldMap.tiles()) {
                    if (tile.owner == oldOwner) {
                        tile.owner = newOwner;
                    }
                }

                HistoryEvent colEv;
                colEv.type = EventType::KingdomCollapsed;
                colEv.turn = turn;
                colEv.primaryKingdom = oldOwner;
                colEv.secondaryKingdom = newOwner;
                colEv.description =
                    kingdoms.at(oldOwner).name + " has collapsed and was annexed by " +
                    kingdoms.at(newOwner).name + ".";
                bus.emit(std::move(colEv));
            }
        }

        if (oldOwner != newOwner) {
            if (kingdoms.count(oldOwner) && kingdoms.at(oldOwner).isAlive) {
                kingdoms.at(oldOwner).revengeTarget = newOwner;
                kingdoms.at(oldOwner).revengeUntil = turn + 120;
            }

            city.owner = newOwner;
            city.isCapital = false;
            city.happiness = std::max(0.2f, city.happiness - 0.30f);
            city.fortification = std::max(0.0f, city.fortification - 0.20f);
            city.underSiege = false;
            city.lastConquered = 0;
            // Culture: reset assimilation; original owner keeps cultural identity
            // If re-conquered by original culture owner, start at 50%
            if (city.cultureOwner == newOwner) {
                city.cultureAssimilation = 0.5f;
            } else {
                city.cultureAssimilation = 0.0f;
                // Keep cultureOwner as the previous kingdom's culture
            }

            // War damage: buildings are looted/damaged on conquest
            for (auto& b : city.buildings) {
                b.condition = std::max(0.15f, b.condition - 0.30f);
            }

            // Pillage windfall: conqueror gains gold based on city wealth
            // Scales with personality — Aggressive/Expansionist are better looters
            if (kingdoms.count(newOwner) && kingdoms.at(newOwner).isAlive) {
                float pillage = static_cast<float>(city.population) * 0.018f + 40.0f;
                if (kingdoms.at(newOwner).personality == KingdomPersonality::Aggressive  ||
                    kingdoms.at(newOwner).personality == KingdomPersonality::Expansionist)
                    pillage *= 2.0f;
                else if (kingdoms.at(newOwner).personality == KingdomPersonality::Opportunistic)
                    pillage *= 2.2f;
                kingdoms.at(newOwner).treasury.gold += pillage;
            }

            auto& newCities = kingdoms.at(newOwner).cities;
            if (std::find(newCities.begin(), newCities.end(), result.conqueredCity) == newCities.end()) {
                newCities.push_back(result.conqueredCity);
            }

            worldMap.at(city.tile).owner = newOwner;

            // Take nearby tiles around conquered city
            TileID cnbs[4]; int cnCount = worldMap.neighbors4(city.tile, cnbs);
            for (int cni = 0; cni < cnCount; ++cni) { TileID neighbor = cnbs[cni];
                Tile& nt = worldMap.at(neighbor);
                if (isClaimableTerritory(nt.terrain) &&
                    (nt.owner == oldOwner || nt.owner == NO_KINGDOM)) {
                    nt.owner = newOwner;
                }
            }

            HistoryEvent ev;
            ev.type = EventType::CityConquered;
            ev.turn = turn;
            ev.primaryKingdom = newOwner;
            ev.secondaryKingdom = oldOwner;
            ev.relatedCity = result.conqueredCity;
            std::string fromStr = (oldOwner == NO_KINGDOM)
                ? "independent forces"
                : kingdoms.at(oldOwner).name;
            ev.description =
                kingdoms.at(newOwner).name + " conquered " +
                city.name + " from " + fromStr + ".";
            bus.emit(std::move(ev));
        }
    }

    HistoryEvent battleEv;
    battleEv.type = EventType::BattleFought;
    battleEv.turn = turn;
    battleEv.primaryKingdom = result.victor;
    battleEv.secondaryKingdom = result.loser;
    battleEv.description =
        kingdoms.at(result.victor).name + " defeated " +
        kingdoms.at(result.loser).name + " in battle.";
    battleEv.context["attacker_strength"] = result.attackerStrength;
    battleEv.context["defender_strength"] = result.defenderStrength;

    bus.emit(std::move(battleEv));

    if (kingdoms.count(result.loser)) {
        Kingdom& loser = kingdoms.at(result.loser);
        loser.recentDefeats = std::min(6, loser.recentDefeats + 1);
        loser.lastDefeatTurn = turn;
        loser.lastDefeatedBy = result.victor;
    }
    if (kingdoms.count(result.victor)) {
        Kingdom& victor = kingdoms.at(result.victor);
        victor.recentDefeats = std::max(0, victor.recentDefeats - 1);
    }
}

bool BattleEngine::areAtWar(
    KingdomID a,
    KingdomID b,
    const RelationMap& relations) const
{
    if (a == b || a == NO_KINGDOM || b == NO_KINGDOM) {
        return false;
    }

    KingdomID lo = std::min(a, b);
    KingdomID hi = std::max(a, b);

    auto it = relations.find(lo);
    if (it == relations.end()) {
        return false; // no relation = neutral
    }

    auto it2 = it->second.find(hi);
    if (it2 == it->second.end()) {
        return false; // no relation = neutral
    }

    return it2->second.state == RelationState::War;
}

} // namespace jke
