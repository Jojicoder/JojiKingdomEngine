#include "jke/SimulationEngine.hpp"
#include "jke/generators/WorldGenerator.hpp"
#include "jke/generators/CityGenerator.hpp"
#include "jke/ai/personalities/AggressiveAI.hpp"
#include "jke/ai/personalities/DefensiveAI.hpp"
#include "jke/ai/personalities/EconomicAI.hpp"
#include "jke/ai/personalities/DiplomaticAI.hpp"
#include "jke/ai/personalities/OpportunisticAI.hpp"
#include "jke/ai/personalities/ExpansionistAI.hpp"
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <cmath>

namespace jke {

SimulationEngine::SimulationEngine(SimulationConfig config)
    : config_(std::move(config))
    , rng_(config_.seed)
    , worldMap_(1, 1)          // replaced by initializeWorld()
    , battleEngine_(rng_)
    , rebellionEngine_(rng_)
    , historyEngine_(timeline_)
{
    historyEngine_.registerHandlers(eventBus_);
}

void SimulationEngine::setSerializer(std::unique_ptr<SnapshotSerializer> s) {
    serializer_ = std::move(s);
}

bool SimulationEngine::step() {
    if (simulationOver_ || currentTurn_ >= config_.maxTurns) return false;

    currentTurn_++;
    runTurn();
    collectSnapshot();

    // Victory check every 5 turns (unification doesn't happen between heartbeats)
    if (currentTurn_ % 5 == 0 &&
        historyEngine_.checkContinentUnified(kingdoms_, eventBus_, currentTurn_)) {
        eventBus_.flush();
        simulationOver_ = true;
        collectSnapshot();
        return false;
    }
    return true;
}

void SimulationEngine::run() {
    if (kingdoms_.empty()) initializeWorld();

    if (config_.verbose) {
        std::cout << "=== JojiKingdomEngine ===\n";
        std::cout << "Seed: " << config_.seed << "\n";
        std::cout << "Max turns: " << config_.maxTurns << "\n";
        std::cout << "Starting kingdoms: " << kingdoms_.size() << "\n\n";
    }

    // Initial snapshot (turn 0)
    collectSnapshot();

    while (step()) {
        if (config_.verbose && currentTurn_ % 10 == 0) {
            printTurnSummary();
        }
    }

    if (serializer_) serializer_->finalize();

    if (config_.verbose) {
        std::cout << "\n=== Simulation Complete ===\n";
        std::cout << "Total turns: " << currentTurn_ << "\n";
        std::cout << "Total events: " << timeline_.all().size() << "\n";
        int alive = 0;
        for (auto& [kid, k] : kingdoms_) if (k.isAlive) alive++;
        std::cout << "Surviving kingdoms: " << alive << "\n";
    }
}

void SimulationEngine::initializeWorld() {
    WorldGenerator gen(config_.seed);
    GeneratedWorld world = gen.generate();

    worldMap_       = std::move(world.worldMap);
    kingdoms_       = std::move(world.kingdoms);
    cities_         = std::move(world.cities);
    armies_         = std::move(world.armies);
    techTree_       = std::move(world.techTree);
    nextKingdomID_  = world.nextKingdomID;
    nextCityID_     = world.nextCityID;
    nextArmyID_     = world.nextArmyID;
    nextUnitID_     = world.nextUnitID;

    // Save initial tile state for delta detection
    prevTileState_ = worldMap_.tiles();

    // Create AI strategy for each kingdom
    for (auto& [kid, k] : kingdoms_) {
        switch (k.personality) {
            case KingdomPersonality::Aggressive:
                aiStrategies_[kid] = std::make_unique<AggressiveAI>();
                break;
            case KingdomPersonality::Defensive:
                aiStrategies_[kid] = std::make_unique<DefensiveAI>();
                break;
            case KingdomPersonality::Economic:
                aiStrategies_[kid] = std::make_unique<EconomicAI>();
                break;
            case KingdomPersonality::Diplomatic:
                aiStrategies_[kid] = std::make_unique<DiplomaticAI>();
                break;
            case KingdomPersonality::Opportunistic:
                aiStrategies_[kid] = std::make_unique<OpportunisticAI>();
                break;
            case KingdomPersonality::Expansionist:
                aiStrategies_[kid] = std::make_unique<ExpansionistAI>();
                break;
        }

        // Emit founding event
        HistoryEvent ev;
        ev.id             = timeline_.nextEventID();
        ev.type           = EventType::KingdomFounded;
        ev.turn           = 0;
        ev.primaryKingdom = kid;
        ev.description    = k.name + " was founded.";
        timeline_.record(std::move(ev));
    }

    // Emit city founding events
    for (auto& [cid, c] : cities_) {
        HistoryEvent ev;
        ev.id             = timeline_.nextEventID();
        ev.type           = EventType::CityFounded;
        ev.turn           = 0;
        ev.primaryKingdom = c.owner;
        ev.relatedCity    = cid;
        if (c.owner == NO_KINGDOM) {
            ev.description = c.name + " stands as an independent stronghold.";
        } else {
            ev.description = c.name + " was founded as " +
                             (c.isCapital ? "the capital of " : "a regional strongpoint of ") +
                             kingdoms_.at(c.owner).name + ".";
        }
        timeline_.record(std::move(ev));
    }

    // Assign cultural groups based on starting capital positions
    assignCultureGroups();

    // Assign initial rulers
    assignInitialRulers();

    // Aggressive/Expansionist/Opportunistic kingdoms start with a second standing army.
    // Opportunistic gets a small raiding band so its weak-target logic can convert into action.
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        if (k.personality != KingdomPersonality::Aggressive &&
            k.personality != KingdomPersonality::Expansionist &&
            k.personality != KingdomPersonality::Opportunistic) continue;
        RecruitOrder order;
        order.kingdom  = kid;
        order.city     = k.capitalCity;
        order.unitType = UnitType::Militia;
        order.soldiers = (k.personality == KingdomPersonality::Aggressive) ? 700u :
            (k.personality == KingdomPersonality::Expansionist) ? 750u : 650u;
        militaryEngine_.spawnArmy(kingdoms_, armies_, worldMap_,
                                  nextArmyID_, nextUnitID_, order, cities_);
    }

    // Expansionist kingdoms start with extra resources AND claimed surrounding tiles
    // (their scouts and settlers push the frontier immediately from Turn 1)
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive || k.personality != KingdomPersonality::Expansionist) continue;
        k.treasury.wood  += 150.0f;
        k.treasury.stone += 80.0f;
        k.treasury.gold  += 60.0f;

        // Claim 8 tiles outward from capital so city founding can work from Turn 30
        if (k.capitalCity != NO_CITY && cities_.count(k.capitalCity)) {
            TileID capTile = cities_.at(k.capitalCity).tile;
            if (capTile != NO_TILE) {
                // BFS up to 3 steps from capital, claim land tiles
                std::vector<TileID> frontier = {capTile};
                std::unordered_set<TileID> visited = {capTile};
                for (int depth = 0; depth < 3 && !frontier.empty(); ++depth) {
                    std::vector<TileID> next;
                    for (TileID t : frontier) {
                        TileID nbs[4]; int nc = worldMap_.neighbors4(t, nbs);
                        for (int ni = 0; ni < nc; ++ni) {
                            TileID nb = nbs[ni];
                            if (visited.count(nb)) continue;
                            visited.insert(nb);
                            auto& tile = worldMap_.at(nb);
                            if (tile.terrain == TerrainType::Ocean ||
                                tile.terrain == TerrainType::Lake  ||
                                tile.terrain == TerrainType::Coast) continue;
                            if (tile.owner == NO_KINGDOM) tile.owner = kid;
                            next.push_back(nb);
                        }
                    }
                    frontier = std::move(next);
                }
            }
        }
    }

    // Opportunistic kingdoms start with extra gold and can hire their first mercenary immediately
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive || k.personality != KingdomPersonality::Opportunistic) continue;
        k.treasury.gold += 360.0f;  // enough for a mercenary + war chest
    }
}

void SimulationEngine::runTurn() {
    // 0. Random world events (~4% chance per kingdom per turn)
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        if (!rng_.chance(0.04f)) continue;
        int roll = static_cast<int>(rng_.nextInt(0, 11));

        std::string evDesc;
        bool positive = false;

        switch (roll) {
            case 0: { // Gold windfall
                float g = rng_.nextFloat(60.0f, 150.0f);
                k.treasury.gold += g;
                evDesc = k.name + " received a gold windfall.";
                positive = true;
                break;
            }
            case 1: { // Iron vein discovered
                float fe = rng_.nextFloat(30.0f, 80.0f);
                k.treasury.iron += fe;
                evDesc = k.name + " discovered a rich iron vein.";
                positive = true;
                break;
            }
            case 2: { // Drought
                k.treasury.food = std::max(0.0f, k.treasury.food - rng_.nextFloat(40.0f, 100.0f));
                k.starvationTurns++;
                evDesc = k.name + " suffered a severe drought.";
                break;
            }
            case 3: { // Plague
                k.morale    = std::max(0.0f, k.morale    - rng_.nextFloat(0.1f, 0.25f));
                k.stability = std::max(0.0f, k.stability - rng_.nextFloat(0.05f, 0.15f));
                evDesc = k.name + " was struck by a devastating plague.";
                break;
            }
            case 4: { // Military revolt
                if (!k.armies.empty()) {
                    ArmyID victim = k.armies[rng_.nextInt(0, static_cast<int>(k.armies.size()) - 1)];
                    if (armies_.count(victim)) {
                        for (auto& u : armies_.at(victim).units)
                            u.soldiers = u.soldiers / 2;
                    }
                    evDesc = k.name + "'s army suffered an internal revolt.";
                }
                break;
            }
            case 5: { // Trade boom
                k.treasury.gold += rng_.nextFloat(30.0f, 80.0f);
                k.treasury.gold *= 1.1f;
                evDesc = k.name + " experienced a prosperous trade boom.";
                positive = true;
                break;
            }
            case 6: { // Diplomatic scandal
                k.stability = std::max(0.0f, k.stability - 0.08f);
                evDesc = k.name + " was rocked by a diplomatic scandal.";
                break;
            }
            case 7: { // Fortification collapse
                if (!k.cities.empty()) {
                    CityID cid = k.cities[rng_.nextInt(0, static_cast<int>(k.cities.size()) - 1)];
                    if (cities_.count(cid)) {
                        cities_.at(cid).fortification = std::max(0.0f, cities_.at(cid).fortification - 0.3f);
                        evDesc = k.name + "'s fortifications crumbled in " + cities_.at(cid).name + ".";
                    }
                }
                break;
            }
            case 8: { // Harvest festival — food & morale surge
                k.treasury.food += rng_.nextFloat(50.0f, 120.0f);
                k.morale = std::min(1.0f, k.morale + 0.08f);
                evDesc = k.name + " celebrated a bountiful harvest festival.";
                positive = true;
                break;
            }
            case 9: { // Talented general — lead army gets experience
                if (!k.armies.empty()) {
                    ArmyID lead = k.armies.front();
                    if (armies_.count(lead)) {
                        for (auto& u : armies_.at(lead).units)
                            u.experience = std::min(1.0f, u.experience + 0.20f);
                    }
                    evDesc = k.name + " produced a brilliant general, inspiring its armies.";
                    positive = true;
                }
                break;
            }
            case 10: { // Stability surge — religious revival or good harvest
                k.stability = std::min(1.0f, k.stability + 0.10f);
                k.morale    = std::min(1.0f, k.morale    + 0.05f);
                evDesc = k.name + " experienced a wave of national pride and unity.";
                positive = true;
                break;
            }
            case 11: { // Building repairs — random city buildings improved
                if (!k.cities.empty()) {
                    CityID cid = k.cities[rng_.nextInt(0, static_cast<int>(k.cities.size()) - 1)];
                    if (cities_.count(cid)) {
                        for (auto& b : cities_.at(cid).buildings)
                            b.condition = std::min(1.0f, b.condition + 0.20f);
                        evDesc = k.name + " restored its infrastructure in " + cities_.at(cid).name + ".";
                        positive = true;
                    }
                }
                break;
            }
        }

        if (!evDesc.empty()) {
            HistoryEvent ev;
            ev.type           = positive ? EventType::WorldEventPositive : EventType::WorldEventNegative;
            ev.turn           = currentTurn_;
            ev.primaryKingdom = kid;
            ev.description    = evDesc;
            eventBus_.emit(std::move(ev));
        }
    }

    // 0.5 Succession crisis (~1.2% chance per kingdom per turn, turn > 30)
    if (currentTurn_ > 30) {
        for (auto& [kid, k] : kingdoms_) {
            if (!k.isAlive || !k.hasRuler) continue;
            // Chance of sudden death scales with ruler age
            float deathChance = 0.004f + (k.ruler.age > 55 ? (k.ruler.age - 55) * 0.001f : 0.0f);
            if (!rng_.chance(deathChance)) continue;

            std::string rulerName = k.ruler.name;
            bool hadHeir = k.ruler.hasHeir;
            std::string heirName = k.ruler.heirName;

            if (hadHeir) {
                // Smooth succession
                k.stability  = std::max(0.0f, k.stability  - 0.10f);
                k.legitimacy = std::max(0.0f, k.legitimacy - 0.10f);
                k.ruler      = generateRuler(currentTurn_);
                k.ruler.name = heirName;

                HistoryEvent ev;
                ev.type           = EventType::WorldEventNegative;
                ev.turn           = currentTurn_;
                ev.primaryKingdom = kid;
                ev.description    = rulerName + " of " + k.name + " has died. " +
                    heirName + " ascends the throne.";
                eventBus_.emit(std::move(ev));
            } else {
                // Crisis: no heir
                k.stability  = std::max(0.0f, k.stability  - 0.28f);
                k.morale     = std::max(0.0f, k.morale     - 0.18f);
                k.legitimacy = std::max(0.0f, k.legitimacy - 0.35f);
                if (k.stability < 0.30f)
                    k.stability = std::max(0.0f, k.stability - 0.10f);
                k.ruler = generateRuler(currentTurn_);

                HistoryEvent ev;
                ev.type           = EventType::WorldEventNegative;
                ev.turn           = currentTurn_;
                ev.primaryKingdom = kid;
                ev.description    = rulerName + " of " + k.name +
                    " died without an heir! Succession crisis!";
                ev.context["crisis"] = 1.0f;
                eventBus_.emit(std::move(ev));
            }
        }
        eventBus_.flush();
    }

    // 1. Economy
    economyEngine_.update(kingdoms_, cities_, armies_, eventBus_, currentTurn_, currentSeason(), &relations_);
    eventBus_.flush();

    // 1.4 Long peace military atrophy (mutable armies, so done here not in EconomyEngine)
    if (currentTurn_ % 10 == 0) {
        for (auto& [kid, k] : kingdoms_) {
            if (!k.isAlive || k.peaceTurns < 150) continue;
            float decay = std::min(0.0008f, (k.peaceTurns - 150) * 0.000005f);
            for (ArmyID aid : k.armies) {
                auto ait = armies_.find(aid);
                if (ait == armies_.end()) continue;
                for (auto& u : ait->second.units)
                    u.training = std::max(0.35f, u.training - decay);
            }
        }
    }

    // 1.5 Bankruptcy: disband smallest army after 3+ turns in debt
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive || k.debtTurns < 3 || k.armies.empty()) continue;
        // Find the army with fewest soldiers
        ArmyID smallestAid = NO_ARMY;
        uint32_t fewest = UINT32_MAX;
        for (ArmyID aid : k.armies) {
            auto ait = armies_.find(aid);
            if (ait == armies_.end()) continue;
            uint32_t s = ait->second.totalSoldiers();
            if (s < fewest) { fewest = s; smallestAid = aid; }
        }
        if (smallestAid == NO_ARMY) continue;
        // Disband: zero out soldiers so removeEmptyArmies cleans up next turn
        auto& disbandArmy = armies_.at(smallestAid);
        for (auto& u : disbandArmy.units) u.soldiers = 0;

        HistoryEvent ev;
        ev.type           = EventType::WorldEventNegative;
        ev.turn           = currentTurn_;
        ev.primaryKingdom = kid;
        ev.description    = k.name + " disbanded an army due to bankruptcy.";
        eventBus_.emit(std::move(ev));
        k.debtTurns = 0; // reset so we don't cascade-disband every turn
    }
    eventBus_.flush();

    // 1.8 Fortification decay: walls need upkeep or they slowly degrade
    if (currentTurn_ % 5 == 0) {
        for (auto& [cid, city] : cities_) {
            if (city.isRuined || city.fortification <= 0.05f) continue;
            auto kit = kingdoms_.find(city.owner);
            if (kit == kingdoms_.end() || !kit->second.isAlive) continue;
            const Kingdom& owner = kit->second;
            // Decay rate: faster if owner is poor or at war and neglecting upkeep
            float decay = 0.006f;
            if (owner.treasury.stone < 5.0f) decay += 0.008f;  // no stone → crumbles faster
            if (owner.warWeariness > 0.6f)   decay += 0.004f;  // war distraction
            city.fortification = std::max(0.02f, city.fortification - decay);
        }
    }

    // 2. Technology
    techEngine_.update(kingdoms_, techTree_, eventBus_, currentTurn_);
    eventBus_.flush();

    // 2.5 War momentum: Aggressive get combat boost while fighting (applied after tech reset)
    {
        auto isWarring = [&](KingdomID a, KingdomID b) {
            auto lo = std::min(a,b), hi = std::max(a,b);
            auto it = relations_.find(lo);
            if (it == relations_.end()) return false;
            auto it2 = it->second.find(hi);
            return it2 != it->second.end() && it2->second.state == RelationState::War;
        };
        for (auto& [kid, k] : kingdoms_) {
            if (!k.isAlive || k.personality != KingdomPersonality::Aggressive) continue;
            bool fighting = false;
            for (const auto& [oid, ok] : kingdoms_) {
                if (oid == kid || !ok.isAlive) continue;
                if (isWarring(kid, oid)) { fighting = true; break; }
            }
            // combatBonus was just reset by TechEngine; add war momentum on top
            if (fighting) {
                k.combatBonus *= std::min(1.50f, 1.0f + k.warWeariness * 0.50f + 0.12f);
            }
        }
    }

    // 3. Clean up armies destroyed last turn so AI sees accurate army counts
    militaryEngine_.removeEmptyArmies(kingdoms_, armies_, worldMap_);
    updateStrategicPostures();
    assignArmyRoles();

    // 4. AI decisions
    runAIPhase();
    eventBus_.flush();

    // 4.5 Supply retreats: redirect starving armies home before they move
    enforceSupplyRetreats();

    // 5. Military movement
    militaryEngine_.update(kingdoms_, armies_, cities_, worldMap_, eventBus_, currentTurn_, currentSeason());
    eventBus_.flush();

    // 6. Battles (only between kingdoms actually at war)
    battleEngine_.update(kingdoms_, armies_, cities_, worldMap_, eventBus_, currentTurn_, relations_, currentSeason());
    eventBus_.flush();

    // 7. Diplomacy (process proposals submitted by AI)
    diplomacyEngine_.update(kingdoms_, relations_, pendingProposals_, cities_, eventBus_, currentTurn_);
    eventBus_.flush();

    // 7.5 Alliance obligations: allies auto-join active wars
    enforceAllianceObligations();
    diplomacyEngine_.update(kingdoms_, relations_, pendingProposals_, cities_, eventBus_, currentTurn_);
    eventBus_.flush();

    // 7.6 Aggressive hegemon counter-declaration: if the dominant Aggressive kingdom
    // is attacked by a coalition, it immediately declares war on ALL enemies at once.
    // This forces the coalition to fight the full military machine head-on.
    {
        // Recompute hegemony share quickly
        size_t totalCitiesNow = 0, maxCitiesNow = 0;
        KingdomID aggressiveHegemon = NO_KINGDOM;
        for (const auto& [kid, k] : kingdoms_) {
            if (!k.isAlive || k.isRebel) continue;
            totalCitiesNow += k.cities.size();
            if (k.cities.size() > maxCitiesNow) {
                maxCitiesNow = k.cities.size();
                aggressiveHegemon = kid;
            }
        }
        const float ahShare = (totalCitiesNow > 0)
            ? static_cast<float>(maxCitiesNow) / static_cast<float>(totalCitiesNow) : 0.0f;

        if (aggressiveHegemon != NO_KINGDOM &&
            ahShare >= 0.22f &&
            kingdoms_.at(aggressiveHegemon).personality == KingdomPersonality::Aggressive) {
            // Collect current enemies and their allies (the actual coalition)
            std::vector<KingdomID> currentEnemies;
            for (const auto& [kid, k] : kingdoms_) {
                if (kid == aggressiveHegemon || !k.isAlive || k.isRebel) continue;
                auto lo = std::min(aggressiveHegemon, kid), hi = std::max(aggressiveHegemon, kid);
                auto rit = relations_.find(lo);
                if (rit != relations_.end()) {
                    auto rit2 = rit->second.find(hi);
                    if (rit2 != rit->second.end() && rit2->second.state == RelationState::War)
                        currentEnemies.push_back(kid);
                }
            }
            // If coalition is attacking, pre-emptively hit their allies too
            // (targeted: only enemies OF enemies, not the entire world)
            if (currentEnemies.size() >= 2) {
                std::vector<KingdomID> alliesToHit;
                for (KingdomID enemy : currentEnemies) {
                    for (const auto& [kid, k] : kingdoms_) {
                        if (kid == aggressiveHegemon || !k.isAlive || k.isRebel) continue;
                        auto lo2 = std::min(enemy, kid), hi2 = std::max(enemy, kid);
                        auto rit = relations_.find(lo2);
                        if (rit == relations_.end()) continue;
                        auto rit2 = rit->second.find(hi2);
                        if (rit2 == rit->second.end()) continue;
                        if (rit2->second.state != RelationState::Alliance) continue;
                        // This kingdom is allied with our enemy — preemptive strike
                        auto lo3 = std::min(aggressiveHegemon, kid), hi3 = std::max(aggressiveHegemon, kid);
                        auto& rel3 = relations_[lo3][hi3];
                        if (rel3.state != RelationState::War && rel3.state != RelationState::Alliance)
                            alliesToHit.push_back(kid);
                    }
                }
                for (KingdomID target : alliesToHit) {
                    TreatyProposal p;
                    p.proposer      = aggressiveHegemon;
                    p.recipient     = target;
                    p.proposedState = RelationState::War;
                    p.accepted      = true;
                    pendingProposals_.push_back(p);
                }
                if (!alliesToHit.empty())
                    kingdoms_.at(aggressiveHegemon).lastWarDeclaredTurn = currentTurn_;
            }
        }
    }

    // 8. Rebellions
    rebellionEngine_.update(kingdoms_, cities_, armies_, rebellions_, civilWars_,
                             relations_, nextKingdomID_, nextArmyID_, nextUnitID_,
                             eventBus_, currentTurn_);
    eventBus_.flush();

    // Check for kingdom collapses (no cities left)
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        if (k.cities.empty()) {
            k.isAlive     = false;
            k.collapsedTurn = currentTurn_;

            HistoryEvent ev;
            ev.id              = timeline_.nextEventID();
            ev.type            = EventType::KingdomCollapsed;
            ev.turn            = currentTurn_;
            ev.primaryKingdom  = kid;
            ev.description     = k.name + " has collapsed with no remaining cities.";
            timeline_.record(std::move(ev));

            if (config_.verbose) {
                std::cout << "[Turn " << currentTurn_ << "] "
                          << k.name << " has COLLAPSED\n";
            }
        }
    }

    std::vector<KingdomID> aliveKingdoms;
    for (const auto& [kid, k] : kingdoms_) {
        if (k.isAlive) aliveKingdoms.push_back(kid);
    }

    if (checkDominationVictory()) {
        return;
    }

    if (currentTurn_ >= 1000 && aliveKingdoms.size() == 2) {
        KingdomID a = aliveKingdoms[0];
        KingdomID b = aliveKingdoms[1];
        KingdomID winner = kingdoms_.at(a).totalPopulation >= kingdoms_.at(b).totalPopulation ? a : b;
        KingdomID loser  = winner == a ? b : a;

        const auto winnerPop = static_cast<float>(kingdoms_.at(winner).totalPopulation);
        const auto loserPop  = std::max(1.0f, static_cast<float>(kingdoms_.at(loser).totalPopulation));
        if (winnerPop / loserPop >= 1.15f) {
            auto& winnerKingdom = kingdoms_.at(winner);
            auto& loserKingdom = kingdoms_.at(loser);
            auto remainingCities = loserKingdom.cities;

            for (CityID cid : remainingCities) {
                if (!cities_.count(cid)) continue;
                City& city = cities_.at(cid);
                city.owner = winner;
                city.isCapital = false;
                if (std::find(winnerKingdom.cities.begin(), winnerKingdom.cities.end(), cid) ==
                    winnerKingdom.cities.end()) {
                    winnerKingdom.cities.push_back(cid);
                }
                worldMap_.at(city.tile).owner = winner;
            }

            for (ArmyID aid : loserKingdom.armies) {
                auto ait = armies_.find(aid);
                if (ait == armies_.end()) continue;
                TileID armyTile = ait->second.currentTile;
                if (armyTile != NO_TILE && armyTile < static_cast<TileID>(worldMap_.tileCount())) {
                    Tile& tile = worldMap_.at(armyTile);
                    if (tile.army == aid) tile.army = NO_ARMY;
                }
                armies_.erase(aid);
            }

            for (auto& tile : worldMap_.tiles()) {
                if (tile.owner == loser) tile.owner = winner;
            }

            loserKingdom.cities.clear();
            loserKingdom.armies.clear();
            loserKingdom.capitalCity = NO_CITY;
            loserKingdom.isAlive = false;
            loserKingdom.annexedBy = winner;
            loserKingdom.collapsedTurn = currentTurn_;

            HistoryEvent ev;
            ev.id = timeline_.nextEventID();
            ev.type = EventType::KingdomCollapsed;
            ev.turn = currentTurn_;
            ev.primaryKingdom = loser;
            ev.secondaryKingdom = winner;
            ev.description = loserKingdom.name + " accepted domination by " +
                             winnerKingdom.name + ".";
            timeline_.record(std::move(ev));
        }
    }

    // ── Mercenary contract expiry ─────────────────────────────────────────────
    for (auto& [aid, army] : armies_) {
        if (!army.isMercenary || army.contractUntil > currentTurn_) continue;
        // Contract expired — disband
        if (kingdoms_.count(army.owner)) {
            auto& k = kingdoms_.at(army.owner);
            auto& av = k.armies;
            av.erase(std::remove(av.begin(), av.end(), aid), av.end());
            HistoryEvent ev;
            ev.type           = EventType::WorldEventNegative;
            ev.turn           = currentTurn_;
            ev.primaryKingdom = army.owner;
            ev.description    = k.name + "'s mercenary contract expired.";
            eventBus_.emit(std::move(ev));
        }
        if (army.currentTile != NO_TILE &&
            army.currentTile < static_cast<TileID>(worldMap_.tileCount()) &&
            worldMap_.at(army.currentTile).army == aid) {
            worldMap_.at(army.currentTile).army = NO_ARMY;
        }
        for (auto& u : army.units) u.soldiers = 0;  // removeEmptyArmies will clean up
    }
    eventBus_.flush();

    // ── Season effects ────────────────────────────────────────────────────────
    updateSeasonEffects();

    // ── Bandit groups ─────────────────────────────────────────────────────────
    updateBandits();

    // ── Plague propagation ────────────────────────────────────────────────────
    updatePlaguePropagation();

    // ── AI city building ── runs every 3 turns (cooldown is 30+ turns anyway)
    if (currentTurn_ > 80 && currentTurn_ % 3 == 0) runAICityBuilding();

    // ── Ruler aging & trait effects ───────────────────────────────────────────
    if (currentTurn_ % 4 == 0) updateRulerAging();
    applyRulerTraits();

    // ── Nomad horde ───────────────────────────────────────────────────────────
    updateNomadHorde();
    eventBus_.flush();

    // ── Culture assimilation tick ─────────────────────────────────────────────
    updateCultureAssimilation();

    // ── Navy update ───────────────────────────────────────────────────────────
    updateNavy();
    eventBus_.flush();

    // ── History graph snapshot (every 5 turns) ────────────────────────────────
    if (currentTurn_ % 5 == 0) recordHistorySnapshot();

    // Assign new AI strategies for rebel factions
    for (auto& [kid, k] : kingdoms_) {
        if (k.isAlive && !aiStrategies_.count(kid)) {
            aiStrategies_[kid] = std::make_unique<AggressiveAI>();
        }
    }
}

void SimulationEngine::updateStrategicPostures() {
    std::vector<KingdomID> alive;
    for (const auto& [kid, k] : kingdoms_) {
        if (k.isAlive) alive.push_back(kid);
    }

    auto atWar = [&](KingdomID a, KingdomID b) {
        KingdomID lo = std::min(a, b);
        KingdomID hi = std::max(a, b);
        auto it = relations_.find(lo);
        if (it == relations_.end()) return false;
        auto it2 = it->second.find(hi);
        if (it2 == it->second.end()) return false;
        return it2->second.state == RelationState::War;
    };

    // Pre-build invaded set in O(n_armies): which kingdoms have enemy armies on their tiles?
    invaded_.clear();
    for (const auto& [aid, army] : armies_) {
        if (army.currentTile == NO_TILE ||
            army.currentTile >= static_cast<TileID>(worldMap_.tileCount())) continue;
        KingdomID tileOwner = worldMap_.at(army.currentTile).owner;
        if (tileOwner == NO_KINGDOM || tileOwner == army.owner) continue;
        auto kit = kingdoms_.find(army.owner);
        if (kit == kingdoms_.end() || !kit->second.isAlive) continue;
        if (atWar(army.owner, tileOwner)) invaded_.insert(tileOwner);
    }

    // Refresh weakTarget cache every 5 turns
    const bool refreshWeak = (weakTargetCacheTurn_ == ~0u || currentTurn_ % 5 == 0);

    auto armyCount = [&](const Kingdom& k) {
        int count = 0;
        for (ArmyID aid : k.armies) {
            auto it = armies_.find(aid);
            if (it != armies_.end() && !it->second.isEmpty()) ++count;
        }
        return count;
    };

    KingdomID hegemon = NO_KINGDOM;
    uint64_t totalPopulation = 0;
    size_t   hegemonCities   = 0;
    size_t   totalCitiesAlive = 0;
    for (KingdomID kid : alive) {
        totalPopulation    += kingdoms_.at(kid).totalPopulation;
        totalCitiesAlive   += kingdoms_.at(kid).cities.size();
        if (hegemon == NO_KINGDOM ||
            kingdoms_.at(kid).cities.size() > hegemonCities) {
            hegemon       = kid;
            hegemonCities = kingdoms_.at(kid).cities.size();
        }
    }
    // Balance-of-power: hegemon with >32% of all cities triggers universal coalition
    const float hegemonyShare = (totalCitiesAlive > 0)
        ? static_cast<float>(hegemonCities) / static_cast<float>(totalCitiesAlive)
        : 0.0f;
    // Expansionist can hold many cities before triggering a coalition (their whole identity is expansion)
    // Aggressive gets slightly more room; others trigger at 25%
    const float hegThreshold = (hegemon != NO_KINGDOM &&
        kingdoms_.at(hegemon).personality == KingdomPersonality::Expansionist)  ? 0.55f :
        (hegemon != NO_KINGDOM &&
        kingdoms_.at(hegemon).personality == KingdomPersonality::Aggressive)    ? 0.35f : 0.25f;
    const bool hegemonIsThreaten = (hegemon != NO_KINGDOM && hegemonyShare >= hegThreshold && alive.size() >= 3);

    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;

        bool hasWar = false;
        bool hasInvader = false;
        int  numEnemies = 0;
        for (const auto& [otherId, other] : kingdoms_) {
            if (otherId == kid || !other.isAlive) continue;
            if (atWar(kid, otherId)) { hasWar = true; ++numEnemies; }
        }

        // War weariness: rises each turn at war, decays in peace
        // Peace turns: counts consecutive turns without war (for peace dividend)
        if (hasWar) {
            float wearRate = 0.006f * static_cast<float>(numEnemies);
            if (k.cities.size() >= 5) wearRate *= 0.7f;
            if (k.morale < 0.4f)      wearRate *= 1.5f;
            k.warWeariness = std::min(1.0f, k.warWeariness + wearRate);
            if (k.warWeariness > 0.6f) {
                k.morale    = std::max(0.05f, k.morale    - 0.004f);
                k.stability = std::max(0.05f, k.stability - 0.003f);
            }
            k.peaceTurns = 0;  // reset peace counter when at war
        } else {
            k.warWeariness = std::max(0.0f, k.warWeariness - 0.015f);
            k.peaceTurns++;    // accumulate peace dividend
        }

        hasInvader = invaded_.count(kid) > 0;

        const bool weakEconomy =
            k.treasury.gold < 25.0f ||
            k.starvationTurns >= 2 ||
            k.morale < 0.35f ||
            k.stability < 0.35f;

        // Compute weakTarget only when cache is stale (every 5 turns)
        const int selfArmies = armyCount(k);
        // Militarist personalities: patience phase — build up before striking
        const bool militaristPersonality =
            k.personality == KingdomPersonality::Aggressive  ||
            k.personality == KingdomPersonality::Expansionist ||
            k.personality == KingdomPersonality::Opportunistic;
        // Buildup: < 2 armies or < 5 cities in early game → seek allies, not war
        // Buildup phase: militarists seek allies rather than attacking — only if they're
        // genuinely weak (< 2 armies). With 2+ armies they're ready to expand from turn 1.
        const bool inBuildupPhase = militaristPersonality &&
            selfArmies < 2 &&
            currentTurn_ < 100 && !hasInvader && !hasWar;

        if (refreshWeak) {
            KingdomID weakTarget = NO_KINGDOM;
            float weakScore = -1.0f;

            // Defensive kingdoms only go to war when already being invaded or in an active war.
            // They do not initiate offensive wars in peacetime.
            const bool defensiveCanAttack = (k.personality != KingdomPersonality::Defensive) ||
                                            hasInvader || hasWar;

            for (const auto& [otherId, other] : kingdoms_) {
                if (otherId == kid || !other.isAlive) continue;
                if (atWar(kid, otherId)) continue;
                if (other.cities.empty()) continue;

                // Defensive: never pre-emptively attack in peacetime
                if (!defensiveCanAttack) { weakTargetCache_[kid] = NO_KINGDOM; continue; }

                // During buildup: militarists don't hunt weak targets
                if (inBuildupPhase) { weakTargetCache_[kid] = NO_KINGDOM; continue; }

                const int otherArmies = armyCount(other);
                const float cityCount = std::max(1.0f, static_cast<float>(other.cities.size()));
                const bool thinDefense = otherArmies <= 2 ||
                    static_cast<float>(otherArmies) / cityCount < 0.80f ||
                    otherArmies + 1 < selfArmies;
                const bool raidablePeer =
                    currentTurn_ > 30 &&
                    selfArmies >= 2 &&        // can raid with 2 armies (not just 3+)
                    otherArmies < selfArmies &&
                    militaristPersonality;

                // Opportunistic STRONGLY prefers attacking kingdoms already at war
                // (guaranteed distraction). In peaceful times they can still attack weak targets.
                const bool targetIsDistracted = [&]() {
                    for (const auto& [xid, xk] : kingdoms_) {
                        if (xid == otherId || !xk.isAlive) continue;
                        if (atWar(otherId, xid)) return true;
                    }
                    return false;
                }();


                if (!thinDefense && !raidablePeer) continue;

                float score = cityCount * 12.0f -
                              static_cast<float>(otherArmies) * 18.0f +
                              (other.stability < 0.55f ? 18.0f : 0.0f) +
                              (other.morale < 0.55f ? 12.0f : 0.0f);
                if (raidablePeer)
                    score += static_cast<float>(selfArmies - otherArmies) * 28.0f;
                // Opportunistic: massive bonus for distracted enemies, mild penalty otherwise
                if (k.personality == KingdomPersonality::Opportunistic) {
                    if (targetIsDistracted) score += 90.0f;  // perfect timing — pile on!
                    else                    score -= 12.0f;  // risky but possible in quiet times
                    if (otherArmies <= 2) score += 25.0f;
                }
                // Personality attack penalties — certain kingdoms are less worth picking on
                if (other.personality == KingdomPersonality::Aggressive)
                    score -= 35.0f;   // warrior culture: high retaliation risk
                if (other.personality == KingdomPersonality::Expansionist)
                    score -= 35.0f;   // colony network + defense bonus = expensive target
                if (other.personality == KingdomPersonality::Opportunistic)
                    score -= 40.0f;   // known to retaliate viciously + mercenary surprise
                // Expansionist/Opportunistic are extra hard to attack when small
                // (they have startup bonus armies that make early conquest dangerous)
                if ((other.personality == KingdomPersonality::Expansionist ||
                     other.personality == KingdomPersonality::Opportunistic) &&
                    other.cities.size() <= 3)
                    score -= 30.0f;   // early game: they punch above their weight
                if (score > weakScore) { weakScore = score; weakTarget = otherId; }
            }
            if (!inBuildupPhase) weakTargetCache_[kid] = weakTarget;
        }
        const KingdomID weakTarget = [&]() -> KingdomID {
            auto it = weakTargetCache_.find(kid);
            return (it != weakTargetCache_.end()) ? it->second : NO_KINGDOM;
        }();

        const bool hasRevengeTarget =
            k.revengeTarget != NO_KINGDOM &&
            currentTurn_ <= k.revengeUntil &&
            kingdoms_.count(k.revengeTarget) &&
            kingdoms_.at(k.revengeTarget).isAlive;

        KingdomID lateWarTarget = NO_KINGDOM;
        // Earlier trigger (800 turns, ≤8 alive) — endgame should converge
        if (currentTurn_ >= 800 && alive.size() <= 8) {
            for (KingdomID otherId : alive) {
                if (otherId == kid) continue;
                if (lateWarTarget == NO_KINGDOM ||
                    kingdoms_.at(otherId).totalPopulation <
                        kingdoms_.at(lateWarTarget).totalPopulation) {
                    lateWarTarget = otherId;
                }
            }
        }

        k.strategicTarget = NO_KINGDOM;

        // FinalWar when ≤5 alive (not just ≤3) — breaks 3-5 way stalemates
        if (alive.size() <= 5) {
            k.policy = NationalPolicy::FinalWar;
            k.strategyPlan = StrategyPlan::TotalConquest;
            if (lateWarTarget != NO_KINGDOM) k.strategicTarget = lateWarTarget;
        } else if (hasInvader) {
            k.policy = NationalPolicy::Defending;
            k.strategyPlan = StrategyPlan::TurtleDefense;
            k.strategicTarget = NO_KINGDOM;
        } else if (lateWarTarget != NO_KINGDOM) {
            k.policy = NationalPolicy::FinalWar;
            k.strategyPlan = StrategyPlan::TotalConquest;
            k.strategicTarget = lateWarTarget;
        } else if (inBuildupPhase) {
            // Militarist early game: seek alliances and grow before striking
            k.policy = NationalPolicy::Rebuilding;
            k.strategyPlan = StrategyPlan::HoldAndRecover;
            k.strategicTarget = NO_KINGDOM;
        } else if (hasRevengeTarget) {
            k.policy = hasWar ? NationalPolicy::Invading : NationalPolicy::Rebuilding;
            k.strategyPlan = StrategyPlan::RevengeWar;
            k.strategicTarget = k.revengeTarget;
        } else if (weakTarget != NO_KINGDOM &&
                   selfArmies >= 2 &&   // need at least 2 armies before striking
                   militaristPersonality) {
            k.policy = NationalPolicy::Invading;
            k.strategyPlan = (k.personality == KingdomPersonality::Expansionist)
                ? StrategyPlan::BorderExpansion
                : StrategyPlan::OpportunisticRaid;
            k.strategicTarget = weakTarget;
        } else if (weakEconomy && !hasInvader) {
            k.policy = NationalPolicy::Rebuilding;
            k.strategyPlan = StrategyPlan::HoldAndRecover;
        } else if (hegemonIsThreaten && hegemon != kid) {
            // Balance-of-power: ALL kingdoms gang up on the dominant power
            // regardless of personality — this is self-preservation
            k.policy = NationalPolicy::CoalitionBuilding;
            k.strategyPlan = StrategyPlan::AntiHegemonWar;
            k.strategicTarget = hegemon;
        } else if (hegemon != NO_KINGDOM && hegemon != kid) {
            // Softer coalition: personality-gated moderate threat detection
            const Kingdom& hk = kingdoms_.at(hegemon);
            float hegemonPopShare = totalPopulation > 0
                ? static_cast<float>(hk.totalPopulation) / static_cast<float>(totalPopulation)
                : 0.0f;
            const bool cityThreat = hegemonCities >= k.cities.size() + 4;
            const bool popThreat  = hegemonPopShare >= 0.45f;
            const bool sensitivePersonality =
                k.personality == KingdomPersonality::Diplomatic ||
                k.personality == KingdomPersonality::Defensive   ||
                k.personality == KingdomPersonality::Opportunistic;
            if ((cityThreat || popThreat) && sensitivePersonality) {
                k.policy = NationalPolicy::CoalitionBuilding;
                k.strategyPlan = StrategyPlan::AntiHegemonWar;
                k.strategicTarget = hegemon;
            }
        } else if (hasWar) {
            k.policy = NationalPolicy::Invading;
            k.strategyPlan = (k.personality == KingdomPersonality::Aggressive ||
                              k.personality == KingdomPersonality::Expansionist)
                ? StrategyPlan::CapitalRush
                : StrategyPlan::BorderExpansion;
        } else {
            k.policy = NationalPolicy::Rebuilding;
            k.strategyPlan = StrategyPlan::HoldAndRecover;
        }
    }
    if (refreshWeak) weakTargetCacheTurn_ = currentTurn_;
}

void SimulationEngine::assignArmyRoles() {
    for (auto& [aid, army] : armies_) {
        army.role = ArmyRole::Reserve;
    }

    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;

        std::vector<ArmyID> living;
        living.reserve(k.armies.size());
        for (ArmyID aid : k.armies) {
            auto ait = armies_.find(aid);
            if (ait != armies_.end() && !ait->second.isEmpty()) living.push_back(aid);
        }
        if (living.empty()) continue;

        // Extract sort keys before sorting to avoid repeated map lookups
        std::vector<std::pair<uint32_t, ArmyID>> sizeKeyed;
        sizeKeyed.reserve(living.size());
        for (ArmyID aid : living) {
            sizeKeyed.emplace_back(armies_.at(aid).totalSoldiers(), aid);
        }
        std::sort(sizeKeyed.begin(), sizeKeyed.end(),
                  [](const auto& x, const auto& y){ return x.first > y.first; });
        for (size_t i = 0; i < living.size(); ++i) living[i] = sizeKeyed[i].second;

        int defenseSlots = 0;
        if (k.policy == NationalPolicy::Defending) {
            defenseSlots = std::max(1, static_cast<int>(living.size()) / 2);
        } else if (k.policy == NationalPolicy::Rebuilding ||
                   k.personality == KingdomPersonality::Defensive) {
            defenseSlots = 1;
        } else if (k.policy == NationalPolicy::Invading &&
                   k.cities.size() >= 2 &&
                   living.size() >= 3) {
            // Keep a home guard while campaigning so one front can advance
            // without leaving the opposite side of the realm empty.
            defenseSlots = 1;
        }
        defenseSlots = std::min(defenseSlots, static_cast<int>(living.size()));

        for (int i = 0; i < defenseSlots; ++i) {
            armies_.at(living[i]).role = ArmyRole::Defense;
        }

        ArmyID siegeArmy = NO_ARMY;
        for (ArmyID aid : living) {
            if (armies_.at(aid).role == ArmyRole::Defense) continue;
            for (const auto& unit : armies_.at(aid).units) {
                if (unit.type == UnitType::SiegeUnit && unit.soldiers > 0) {
                    siegeArmy = aid;
                    break;
                }
            }
            if (siegeArmy != NO_ARMY) break;
        }

        if ((k.policy == NationalPolicy::Invading || k.policy == NationalPolicy::FinalWar) &&
            siegeArmy != NO_ARMY) {
            armies_.at(siegeArmy).role = ArmyRole::Siege;
        }

        // Late wars often hit the army cap before a dedicated siege army exists.
        // Refit one non-defense field army so campaigns do not stall outside cities.
        if ((k.policy == NationalPolicy::Invading || k.policy == NationalPolicy::FinalWar) &&
            siegeArmy == NO_ARMY &&
            living.size() >= 2) {
            for (ArmyID aid : living) {
                Army& candidate = armies_.at(aid);
                if (candidate.role == ArmyRole::Defense || candidate.units.empty()) continue;
                if (candidate.totalSoldiers() < 650 && k.policy != NationalPolicy::FinalWar) continue;
                candidate.units.front().type = UnitType::SiegeUnit;
                candidate.units.front().equipment = std::max(candidate.units.front().equipment, 0.62f);
                candidate.role = ArmyRole::Siege;
                siegeArmy = aid;
                break;
            }
        }

        for (ArmyID aid : living) {
            Army& army = armies_.at(aid);
            if (army.role != ArmyRole::Reserve) continue;

            const bool depleted = army.totalSoldiers() < 450 || army.supplyLevel < 0.25f;
            if (depleted && k.policy != NationalPolicy::FinalWar) {
                army.role = ArmyRole::Reserve;
            } else if (k.policy == NationalPolicy::Invading ||
                       k.policy == NationalPolicy::FinalWar) {
                army.role = ArmyRole::Attack;
            }
        }
    }
}

void SimulationEngine::enforceAllianceObligations() {
    // If A is allied with B and A is at war with C, B auto-joins the war against C.
    // Collect new war proposals first to avoid modifying relations_ while iterating.
    std::vector<std::pair<KingdomID, KingdomID>> newWars; // {supporter, enemy}

    for (const auto& [lo, row] : relations_) {
        for (const auto& [hi, rel] : row) {
            if (rel.state != RelationState::Alliance) continue;

            KingdomID allyA = lo, allyB = hi;
            if (!kingdoms_.count(allyA) || !kingdoms_.at(allyA).isAlive) continue;
            if (!kingdoms_.count(allyB) || !kingdoms_.at(allyB).isAlive) continue;

            // For each war involving allyA, check if allyB should join
            auto checkJoin = [&](KingdomID belligerent, KingdomID supporter) {
                for (const auto& [lo2, row2] : relations_) {
                    for (const auto& [hi2, rel2] : row2) {
                        if (rel2.state != RelationState::War) continue;
                        KingdomID enemy = NO_KINGDOM;
                        if (lo2 == belligerent && hi2 != supporter) enemy = hi2;
                        else if (hi2 == belligerent && lo2 != supporter) enemy = lo2;
                        else continue;

                        if (!kingdoms_.count(enemy) || !kingdoms_.at(enemy).isAlive) continue;

                        // Supporter already at war with this enemy?
                        KingdomID x = std::min(supporter, enemy);
                        KingdomID y = std::max(supporter, enemy);
                        auto it = relations_.find(x);
                        if (it != relations_.end()) {
                            auto it2 = it->second.find(y);
                            if (it2 != it->second.end() &&
                                it2->second.state == RelationState::War) continue;
                        }
                        newWars.emplace_back(supporter, enemy);
                    }
                }
            };

            checkJoin(allyA, allyB);
            checkJoin(allyB, allyA);
        }
    }

    for (auto& [supporter, enemy] : newWars) {
        TreatyProposal p;
        p.proposer      = supporter;
        p.recipient     = enemy;
        p.proposedState = RelationState::War;
        p.accepted      = true;
        pendingProposals_.push_back(p);
    }
}

void SimulationEngine::enforceSupplyRetreats() {
    for (auto& [aid, army] : armies_) {
        if (army.supplyLevel > 0.35f) continue;
        if (army.currentTile == NO_TILE ||
            army.currentTile >= static_cast<TileID>(worldMap_.tileCount())) continue;
        if (!kingdoms_.count(army.owner) || !kingdoms_.at(army.owner).isAlive) continue;

        const Tile& current = worldMap_.at(army.currentTile);
        const bool activeSiege =
            current.city != NO_CITY &&
            cities_.count(current.city) &&
            cities_.at(current.city).owner != army.owner &&
            army.role == ArmyRole::Siege &&
            army.supplyLevel > 0.22f;
        if (activeSiege) continue;

        const Kingdom& k = kingdoms_.at(army.owner);
        auto armyPos = worldMap_.at(army.currentTile).position;
        float bestDist = 1e9f;
        TileID retreatTile = NO_TILE;

        for (CityID cid : k.cities) {
            if (!cities_.count(cid) || cities_.at(cid).owner != army.owner) continue;
            if (cities_.at(cid).tile == NO_TILE) continue;
            auto cp = worldMap_.at(cities_.at(cid).tile).position;
            float d = std::hypot(float(armyPos.x - cp.x), float(armyPos.y - cp.y));
            if (d < bestDist) { bestDist = d; retreatTile = cities_.at(cid).tile; }
        }

        if (retreatTile != NO_TILE && retreatTile != army.currentTile) {
            army.targetTile = retreatTile;
            army.movementPath.clear();
            army.pathCursor = 0;
            army.role = ArmyRole::Reserve;
        }
    }
}

bool SimulationEngine::checkDominationVictory() {
    std::vector<KingdomID> alive;
    uint64_t totalPop = 0;
    int totalCities = 0;
    for (const auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        alive.push_back(kid);
        totalPop += k.totalPopulation;
        totalCities += static_cast<int>(k.cities.size());
    }

    if (alive.size() <= 1 || currentTurn_ < 250 || totalPop == 0 || totalCities == 0) {
        return false;
    }

    auto militaryPower = [&](const Kingdom& k) {
        // Effective power = soldiers × average training — raw headcount ignores quality
        double power = 0.0;
        for (ArmyID aid : k.armies) {
            auto ait = armies_.find(aid);
            if (ait == armies_.end()) continue;
            for (const auto& u : ait->second.units) {
                double quality = u.training * 0.6 + u.experience * 0.3 + u.morale * 0.1;
                // Mercenaries count less — except Opportunistic who masters mercenary command
                if (ait->second.isMercenary) {
                    quality *= (k.personality == KingdomPersonality::Opportunistic) ? 0.90 : 0.60;
                }
                power += u.soldiers * quality;
            }
        }
        return std::max<uint64_t>(1, static_cast<uint64_t>(power));
    };

    uint64_t maxPopulation = 1;
    int maxCities = 1;
    uint64_t maxMilitary = 1;
    for (KingdomID kid : alive) {
        const Kingdom& k = kingdoms_.at(kid);
        maxPopulation = std::max(maxPopulation, static_cast<uint64_t>(k.totalPopulation));
        maxCities = std::max(maxCities, static_cast<int>(k.cities.size()));
        maxMilitary = std::max(maxMilitary, militaryPower(k));
    }

    auto dominanceScore = [&](const Kingdom& k) {
        const float popScore  = static_cast<float>(k.totalPopulation) /
                                static_cast<float>(maxPopulation);
        const float cityScore = static_cast<float>(k.cities.size()) /
                                static_cast<float>(maxCities);
        const float armyScore = static_cast<float>(militaryPower(k)) /
                                static_cast<float>(maxMilitary);
        // Military is now dominant in scoring — can't win without armies
        return popScore * 0.20f + cityScore * 0.25f + armyScore * 0.55f;
    };

    KingdomID leader = NO_KINGDOM;
    float leaderScore = -1.0f;
    float secondScore = -1.0f;
    for (KingdomID kid : alive) {
        const float score = dominanceScore(kingdoms_.at(kid));
        if (score > leaderScore) {
            secondScore = leaderScore;
            leaderScore = score;
            leader = kid;
        } else {
            secondScore = std::max(secondScore, score);
        }
    }
    if (leader == NO_KINGDOM) return false;

    Kingdom& winner = kingdoms_.at(leader);
    const float popShare = static_cast<float>(winner.totalPopulation) /
                           std::max(1.0f, static_cast<float>(totalPop));
    const float cityShare = static_cast<float>(winner.cities.size()) /
                            std::max(1.0f, static_cast<float>(totalCities));

    uint64_t rivalPop = 0;
    uint64_t rivalMilitary = 0;
    for (KingdomID kid : alive) {
        if (kid == leader) continue;
        rivalPop += kingdoms_.at(kid).totalPopulation;
        rivalMilitary += militaryPower(kingdoms_.at(kid));
    }

    const float popRatio = static_cast<float>(winner.totalPopulation) /
                           std::max(1.0f, static_cast<float>(rivalPop));
    const float militaryRatio = static_cast<float>(militaryPower(winner)) /
                                std::max(1.0f, static_cast<float>(rivalMilitary));

    const bool continentHegemon =
        popShare >= 0.68f &&
        cityShare >= 0.62f &&
        militaryRatio >= 1.15f;

    const bool twoPowerSubmission =
        alive.size() == 2 &&
        popRatio >= 1.45f &&
        cityShare >= 0.55f &&
        militaryRatio >= 1.05f;

    const bool exhaustedLateGame =
        currentTurn_ >= 850 &&
        alive.size() <= 3 &&
        popRatio >= 1.25f &&
        cityShare >= 0.55f;

    // imperialSettlement: military dominance required (not just economic lead)
    const bool imperialSettlement =
        currentTurn_ >= 1600 &&
        alive.size() <= 5 &&
        leaderScore >= std::max(0.01f, secondScore) * 1.35f &&
        cityShare >= 0.38f &&
        militaryRatio >= 1.25f;

    // forcedConclusion: Turn 1900 hard cap — winner is whoever has the strongest ARMY
    // Pure military power determines the final ruler
    KingdomID militaryLeader = NO_KINGDOM;
    uint64_t  topMilitary = 0;
    for (KingdomID kid : alive) {
        uint64_t mp = militaryPower(kingdoms_.at(kid));
        if (mp > topMilitary) { topMilitary = mp; militaryLeader = kid; }
    }
    const bool forcedConclusion = currentTurn_ >= 1900 && militaryLeader != NO_KINGDOM;

    if (!continentHegemon && !twoPowerSubmission && !exhaustedLateGame &&
        !imperialSettlement && !forcedConclusion) {
        return false;
    }

    // For forcedConclusion: override leader with the military champion
    if (forcedConclusion && !continentHegemon && !twoPowerSubmission &&
        !exhaustedLateGame && !imperialSettlement) {
        leader = militaryLeader;
    }

    Kingdom& finalWinner = kingdoms_.at(leader);

    std::vector<KingdomID> losers;
    for (KingdomID kid : alive) {
        if (kid != leader) losers.push_back(kid);
    }

    for (KingdomID loser : losers) {
        Kingdom& loserKingdom = kingdoms_.at(loser);
        auto remainingCities = loserKingdom.cities;

        for (CityID cid : remainingCities) {
            if (!cities_.count(cid)) continue;
            City& city = cities_.at(cid);
            city.owner = leader;
            city.isCapital = false;
            if (std::find(finalWinner.cities.begin(), finalWinner.cities.end(), cid) == finalWinner.cities.end()) {
                finalWinner.cities.push_back(cid);
            }
            if (city.tile != NO_TILE && city.tile < static_cast<TileID>(worldMap_.tileCount())) {
                worldMap_.at(city.tile).owner = leader;
            }
        }

        for (ArmyID aid : loserKingdom.armies) {
            auto ait = armies_.find(aid);
            if (ait == armies_.end()) continue;
            TileID armyTile = ait->second.currentTile;
            if (armyTile != NO_TILE && armyTile < static_cast<TileID>(worldMap_.tileCount())) {
                Tile& tile = worldMap_.at(armyTile);
                if (tile.army == aid) tile.army = NO_ARMY;
            }
            armies_.erase(aid);
        }

        for (auto& tile : worldMap_.tiles()) {
            if (tile.owner == loser) tile.owner = leader;
        }

        loserKingdom.cities.clear();
        loserKingdom.armies.clear();
        loserKingdom.capitalCity = NO_CITY;
        loserKingdom.isAlive = false;
        loserKingdom.annexedBy = leader;
        loserKingdom.collapsedTurn = currentTurn_;
        loserKingdom.policy = NationalPolicy::Rebuilding;

        HistoryEvent ev;
        ev.id = timeline_.nextEventID();
        ev.type = EventType::KingdomCollapsed;
        ev.turn = currentTurn_;
        ev.primaryKingdom = loser;
        ev.secondaryKingdom = leader;
        ev.description = loserKingdom.name + " submitted to " + finalWinner.name +
                         " after losing continental dominance.";
        timeline_.record(std::move(ev));
    }

    finalWinner.policy = NationalPolicy::FinalWar;

    simulationOver_ = true;
    return true;
}

void SimulationEngine::runAIPhase() {
    // Balance-of-power: kingdoms targeting the same hegemon auto-propose alliances
    // This runs before normal AI so coalitions form before war declarations
    if (currentTurn_ % 5 == 0) {
        std::vector<KingdomID> coalitionMembers;
        KingdomID targetHegemon = NO_KINGDOM;
        for (const auto& [kid, k] : kingdoms_) {
            if (!k.isAlive || k.isRebel) continue;
            if (k.policy == NationalPolicy::CoalitionBuilding &&
                k.strategicTarget != NO_KINGDOM) {
                if (targetHegemon == NO_KINGDOM) targetHegemon = k.strategicTarget;
                if (k.strategicTarget == targetHegemon)
                    coalitionMembers.push_back(kid);
            }
        }
        // Propose alliances between all pairs of coalition members
        for (size_t i = 0; i < coalitionMembers.size(); ++i) {
            for (size_t j = i + 1; j < coalitionMembers.size(); ++j) {
                KingdomID a = coalitionMembers[i];
                KingdomID b = coalitionMembers[j];
                auto relIt = relations_.find(a);
                if (relIt != relations_.end()) {
                    auto relIt2 = relIt->second.find(b);
                    if (relIt2 != relIt->second.end()) {
                        const RelationState rs = relIt2->second.state;
                        if (rs == RelationState::Alliance || rs == RelationState::War) continue;
                    }
                }
                // Boost mutual trust and propose alliance
                auto& relAB = relations_[a][b];
                auto& relBA = relations_[b][a];
                relAB.trust = std::min(1.0f, relAB.trust + 0.08f);
                relBA.trust = std::min(1.0f, relBA.trust + 0.08f);
                if (relAB.trust >= 0.25f && rng_.chance(0.35f)) {
                    TreatyProposal p;
                    p.proposer      = a;
                    p.recipient     = b;
                    p.proposedState = RelationState::Alliance;
                    diplomacyEngine_.submitProposal(std::move(p), pendingProposals_);
                }
            }
        }
    }

    // Militarist buildup: Aggressive/Expansionist/Opportunistic kingdoms seek allies early
    // They need partners to survive while they grow — real "strategic patience"
    auto isAtWar = [&](KingdomID a, KingdomID b) {
        KingdomID lo = std::min(a, b), hi = std::max(a, b);
        auto it = relations_.find(lo);
        if (it == relations_.end()) return false;
        auto it2 = it->second.find(hi);
        if (it2 == it->second.end()) return false;
        return it2->second.state == RelationState::War;
    };
    if (currentTurn_ < 240 && currentTurn_ % 8 == 0) {
        for (auto& [kid, k] : kingdoms_) {
            if (!k.isAlive || k.isRebel) continue;
            if (k.personality != KingdomPersonality::Aggressive  &&
                k.personality != KingdomPersonality::Expansionist &&
                k.personality != KingdomPersonality::Opportunistic) continue;
            if (k.policy != NationalPolicy::Rebuilding) continue;  // only in buildup phase
            // Find the closest kingdom that isn't at war with us, pick one to ally with
            KingdomID bestAllyTarget = NO_KINGDOM;
            float bestTrust = -2.0f;
            for (const auto& [oid, ok] : kingdoms_) {
                if (oid == kid || !ok.isAlive || ok.isRebel) continue;
                if (isAtWar(kid, oid)) continue;
                const auto& rel = relations_[kid][oid];
                if (rel.state == RelationState::Alliance) continue;
                if (rel.state == RelationState::War) continue;
                if (rel.trust > bestTrust) { bestTrust = rel.trust; bestAllyTarget = oid; }
            }
            if (bestAllyTarget == NO_KINGDOM) continue;
            // Boost trust and propose trade agreement as stepping stone
            relations_[kid][bestAllyTarget].trust =
                std::min(1.0f, relations_[kid][bestAllyTarget].trust + 0.10f);
            relations_[bestAllyTarget][kid].trust =
                std::min(1.0f, relations_[bestAllyTarget][kid].trust + 0.10f);
            if (relations_[kid][bestAllyTarget].trust >= 0.20f && rng_.chance(0.45f)) {
                TreatyProposal p;
                p.proposer      = kid;
                p.recipient     = bestAllyTarget;
                p.proposedState = RelationState::TradeAgreement;
                diplomacyEngine_.submitProposal(std::move(p), pendingProposals_);
            }
        }
    }

    // Aggressive: veteran armies gain experience; also auto-spawn second army at 3+ cities
    if (currentTurn_ % 20 == 0) {
        for (auto& [aid, army] : armies_) {
            auto kit = kingdoms_.find(army.owner);
            if (kit == kingdoms_.end() || !kit->second.isAlive) continue;
            if (kit->second.personality != KingdomPersonality::Aggressive) continue;
            if (currentTurn_ < 200) continue;
            float expBonus = std::min(0.18f, (currentTurn_ - 200) * 0.0003f);
            for (auto& u : army.units) {
                u.training  = std::min(1.0f, u.training  + expBonus * 0.5f);
                u.experience= std::min(1.0f, u.experience + expBonus);
            }
        }
    }
    // Aggressive kingdoms with 3+ cities but only 1 army spawn a second one
    // (warrior culture always keeps standing armies — they don't wait to recruit)
    if (currentTurn_ % 15 == 0 && currentTurn_ < 400) {
        for (auto& [kid, k] : kingdoms_) {
            if (!k.isAlive || k.isRebel) continue;
            if (k.personality != KingdomPersonality::Aggressive) continue;
            if (k.cities.size() < 3) continue;
            // Count actual non-empty armies
            int liveArmies = 0;
            for (ArmyID aid : k.armies) {
                auto ait = armies_.find(aid);
                if (ait != armies_.end() && !ait->second.isEmpty()) ++liveArmies;
            }
            if (liveArmies >= 2) continue;
            if (k.treasury.gold < 80.0f) continue;
            // Spawn free militia garrison
            RecruitOrder order;
            order.kingdom  = kid;
            order.city     = k.capitalCity;
            order.unitType = UnitType::Militia;
            order.soldiers = 600u + static_cast<uint32_t>(k.cities.size()) * 80u;
            k.treasury.gold -= 80.0f;
            militaryEngine_.spawnArmy(kingdoms_, armies_, worldMap_,
                                      nextArmyID_, nextUnitID_, order, cities_);
        }
    }

    // Opportunistic: stir up conflict — reduce trust between other kingdoms
    // They profit from chaos, so they plant distrust between potential allies
    if (currentTurn_ % 30 == 0) {
        for (auto& [kid, k] : kingdoms_) {
            if (!k.isAlive || k.isRebel) continue;
            if (k.personality != KingdomPersonality::Opportunistic) continue;
            if (k.cities.size() < 2) continue;
            // Pick two non-enemy kingdoms at random and reduce their trust
            std::vector<KingdomID> candidates;
            for (const auto& [oid, ok] : kingdoms_) {
                if (oid == kid || !ok.isAlive || ok.isRebel) continue;
                if (!isAtWar(kid, oid)) candidates.push_back(oid);
            }
            if (candidates.size() < 2) continue;
            std::shuffle(candidates.begin(), candidates.end(), rng_.engine());
            KingdomID a = candidates[0], b = candidates[1];
            if (a == b) continue;
            // Sow distrust between a and b (they don't know who did it)
            auto lo = std::min(a, b), hi = std::max(a, b);
            relations_[lo][hi].trust = std::max(-1.0f, relations_[lo][hi].trust - 0.12f);
            relations_[hi][lo].trust = std::max(-1.0f, relations_[hi][lo].trust - 0.12f);
        }
    }


    // Shuffle kingdom order each turn to prevent first-mover bias
    std::vector<KingdomID> order;
    order.reserve(kingdoms_.size());
    for (const auto& [kid, k] : kingdoms_)
        if (k.isAlive) order.push_back(kid);

    std::shuffle(order.begin(), order.end(), rng_.engine());

    for (KingdomID kid : order) {
        Kingdom& k = kingdoms_.at(kid);
        if (!k.isAlive || !aiStrategies_.count(kid)) continue;
        const bool underInvasion = invaded_.count(kid) > 0;

        AIContext ctx{
            k, kingdoms_, cities_, armies_,
            worldMap_, timeline_, relations_, currentTurn_
        };
        const Season season = currentSeason();

        auto decisions = aiStrategies_.at(kid)->evaluate(ctx);

        // Add small random noise to priorities so same-state doesn't always
        // produce identical decision order across different runs / kingdoms
        for (auto& dec : decisions)
            dec.priority = std::clamp(dec.priority + rng_.nextFloat(-0.12f, 0.12f), 0.0f, 1.0f);
        std::sort(decisions.begin(), decisions.end(),
                  [](const AIDecision& a, const AIDecision& b){ return a.priority > b.priority; });

        // Execute top 3 strategic decisions per turn
        int executed = 0;
        bool issuedAttack = false;
        for (const auto& d : decisions) {
            if (executed >= 3) break;
            if (d.type == AIDecisionType::Recruit) continue; // handled separately below
            if (season == Season::Winter && d.type == AIDecisionType::DeclareWar) {
                continue;
            }
            if ((k.policy == NationalPolicy::FinalWar ||
                 k.strategyPlan == StrategyPlan::TotalConquest) &&
                d.type == AIDecisionType::NegotiatePeace) {
                continue;
            }
            if (underInvasion &&
                d.type == AIDecisionType::DeclareWar) {
                continue;
            }
            executeDecision(d);
            if (d.type == AIDecisionType::Attack) issuedAttack = true;
            executed++;
        }

        auto relationBetween = [&](KingdomID a, KingdomID b) -> const DiplomaticRelation* {
            KingdomID lo = std::min(a, b);
            KingdomID hi = std::max(a, b);
            auto it = relations_.find(lo);
            if (it == relations_.end()) return nullptr;
            auto it2 = it->second.find(hi);
            if (it2 == it->second.end()) return nullptr;
            return &it2->second;
        };

        KingdomID warEnemy = NO_KINGDOM;
        for (const auto& [otherId, other] : kingdoms_) {
            if (otherId == kid || !other.isAlive) continue;
            const DiplomaticRelation* rel = relationBetween(kid, otherId);
            if (rel && rel->state == RelationState::War) {
                warEnemy = otherId;
                break;
            }
        }

        if (!underInvasion &&
            k.strategicTarget != NO_KINGDOM &&
            kingdoms_.count(k.strategicTarget) &&
            kingdoms_.at(k.strategicTarget).isAlive) {
            const DiplomaticRelation* rel = relationBetween(kid, k.strategicTarget);
            const bool alreadyAtWar = rel && rel->state == RelationState::War;
            const bool shouldStrike =
                k.strategyPlan == StrategyPlan::OpportunisticRaid ||
                k.strategyPlan == StrategyPlan::RevengeWar       ||
                k.strategyPlan == StrategyPlan::AntiHegemonWar   ||
                k.strategyPlan == StrategyPlan::CapitalRush       ||
                k.strategyPlan == StrategyPlan::TotalConquest     ||
                k.policy == NationalPolicy::FinalWar;

            // Anti-hegemon wars bypass cooldown — existential threat demands immediate response
            const bool attackingHegemon = (k.strategyPlan == StrategyPlan::AntiHegemonWar);
            const size_t aliveCount = std::count_if(kingdoms_.begin(), kingdoms_.end(),
                [](const auto& p){ return p.second.isAlive && !p.second.isRebel; });
            const TurnNumber cooldown = (aliveCount <= 5) ? 8u : 25u;
            const bool onCooldown = !attackingHegemon &&
                (currentTurn_ - k.lastWarDeclaredTurn < cooldown);
            if (!alreadyAtWar && shouldStrike && !onCooldown && season != Season::Winter) {
                AIDecision war;
                war.type = AIDecisionType::DeclareWar;
                war.actor = kid;
                war.target = k.strategicTarget;
                executeDecision(war);
                k.lastWarDeclaredTurn = currentTurn_;
                warEnemy = k.strategicTarget;
            } else if (alreadyAtWar) {
                warEnemy = k.strategicTarget;
            }
        }

        // Hire mercenaries: at war with few troops, OR rich non-Diplomatic kingdoms
        // Diplomatic kingdoms rely on alliances, not bought swords
        {
            const bool canHireMerc = k.personality != KingdomPersonality::Diplomatic;
            const bool atWarNeedsTroops = (warEnemy != NO_KINGDOM &&
                                           k.armies.size() <= (k.personality == KingdomPersonality::Opportunistic ? 2u : 1u) &&
                                           canHireMerc);
            const bool richAndPeaceful  = (warEnemy == NO_KINGDOM &&
                k.treasury.gold >= 600.0f &&
                (k.personality == KingdomPersonality::Economic ||
                 k.personality == KingdomPersonality::Opportunistic) &&
                k.armies.size() < 2);
            const float mercCostThreshold = (k.personality == KingdomPersonality::Economic) ? 250.0f :
                (k.personality == KingdomPersonality::Opportunistic ? 220.0f : 320.0f);
            if ((atWarNeedsTroops || richAndPeaceful) &&
                k.treasury.gold >= mercCostThreshold &&
                rng_.chance(k.personality == KingdomPersonality::Opportunistic ? 0.48f : 0.30f)) {
                int mercCount = 0;
                for (ArmyID aid : k.armies) {
                    auto ait = armies_.find(aid);
                    if (ait != armies_.end() && ait->second.isMercenary) mercCount++;
                }
                if (mercCount == 0) {
                    AIDecision hire;
                    hire.type  = AIDecisionType::HireMercenary;
                    hire.actor = kid;
                    executeDecision(hire);
                }
            }
        }

        if (warEnemy != NO_KINGDOM && !issuedAttack) {
            const Kingdom& enemy = kingdoms_.at(warEnemy);
            if (enemy.capitalCity != NO_CITY) {
                AIDecision attack;
                attack.type = AIDecisionType::Attack;
                attack.actor = kid;
                attack.target = warEnemy;
                attack.targetCity = enemy.capitalCity;
                executeDecision(attack);
            }
        } else if (warEnemy == NO_KINGDOM && currentTurn_ > 30) {
            // Diplomatic AI: propose non-aggression pact with neighbors when at peace
            if (k.personality == KingdomPersonality::Diplomatic && rng_.chance(0.15f)) {
                for (const auto& [otherId, other] : kingdoms_) {
                    if (otherId == kid || !other.isAlive) continue;
                    const DiplomaticRelation* rel = relationBetween(kid, otherId);
                    if (rel && (rel->state == RelationState::War || rel->nonAggressionPact)) continue;
                    if (rel && rel->trust < -0.2f) continue;
                    TreatyProposal p;
                    p.proposer       = kid;
                    p.recipient      = otherId;
                    p.proposedState  = RelationState::Neutral;
                    p.goldTransfer   = 40.0f;  // pact duration = 40 turns
                    p.proposedTurn   = currentTurn_;
                    p.accepted       = false;
                    diplomacyEngine_.submitProposal(std::move(p), pendingProposals_);
                    break;
                }
            }

            if (season != Season::Winter) {
                KingdomID nearestEnemy = NO_KINGDOM;
                float bestDist = 1e9f;

                if (k.capitalCity != NO_CITY && cities_.count(k.capitalCity)) {
                    auto selfPos = worldMap_.at(cities_.at(k.capitalCity).tile).position;
                    for (const auto& [otherId, other] : kingdoms_) {
                        if (otherId == kid || !other.isAlive || other.capitalCity == NO_CITY) continue;
                        const DiplomaticRelation* rel = relationBetween(kid, otherId);
                        if (order.size() > 3 && rel && rel->state == RelationState::Alliance) continue;
                        // Don't attack non-aggression pact partners
                        if (rel && rel->nonAggressionPact && rel->nonAggressionUntil > currentTurn_) continue;
                        if (!cities_.count(other.capitalCity)) continue;

                        auto otherPos = worldMap_.at(cities_.at(other.capitalCity).tile).position;
                        float dist = std::hypot(float(selfPos.x - otherPos.x), float(selfPos.y - otherPos.y));
                        if (dist < bestDist) {
                            bestDist = dist;
                            nearestEnemy = otherId;
                        }
                    }
                }

                if (nearestEnemy != NO_KINGDOM) {
                    AIDecision war;
                    war.type = AIDecisionType::DeclareWar;
                    war.actor = kid;
                    war.target = nearestEnemy;
                    executeDecision(war);
                }
            }
        }

        // Recruitment runs independently — fill ALL empty army slots this turn
        // (loop so multiple dead armies are replaced in one pass)
        {
            CityID recruitCity = NO_CITY;
            if (k.capitalCity != NO_CITY &&
                cities_.count(k.capitalCity) &&
                cities_.at(k.capitalCity).owner == kid)
                recruitCity = k.capitalCity;
            else {
                for (CityID cid : k.cities) {
                    if (cities_.count(cid) && cities_.at(cid).owner == kid) {
                        recruitCity = cid; break;
                    }
                }
            }
            if (recruitCity != NO_CITY) {
                int bonusArmies =
                    (k.strategyPlan == StrategyPlan::OpportunisticRaid ||
                     k.strategyPlan == StrategyPlan::RevengeWar ||
                     k.strategyPlan == StrategyPlan::TotalConquest) ? 1 : 0;
                int maxArmies = std::clamp(static_cast<int>(k.cities.size()) + 1 + bonusArmies, 3, 8);
                int attempts = maxArmies; // at most maxArmies spawns per turn
                while (attempts-- > 0) {
                    AIDecision rec;
                    rec.type       = AIDecisionType::Recruit;
                    rec.actor      = kid;
                    rec.targetCity = recruitCity;
                    int beforeCount = static_cast<int>(k.armies.size());
                    executeDecision(rec);
                    // Stop if no new army was spawned (cap reached or insufficient funds)
                    if (static_cast<int>(k.armies.size()) == beforeCount) break;
                }
            }
        }
    }
}

void SimulationEngine::executeDecision(const AIDecision& d) {
    if (!kingdoms_.count(d.actor)) return;
    Kingdom& actor = kingdoms_.at(d.actor);
    if (!actor.isAlive) return;

    switch (d.type) {
        case AIDecisionType::DeclareWar: {
            if (d.target == NO_KINGDOM || !kingdoms_.count(d.target)) break;
            if (!kingdoms_.at(d.target).isAlive) break;
            // Non-aggression pact: Diplomatic/Defensive kingdoms refuse to break it
            // unless they have a revenge motive
            {
                const auto* rel = diplomacyEngine_.get(relations_, d.actor, d.target);
                if (rel && rel->nonAggressionPact && rel->nonAggressionUntil > currentTurn_) {
                    const Kingdom& ak = kingdoms_.at(d.actor);
                    bool hasRevenge = (ak.revengeTarget == d.target &&
                                       ak.revengeUntil > currentTurn_);
                    if (!hasRevenge &&
                        (ak.personality == KingdomPersonality::Diplomatic ||
                         ak.personality == KingdomPersonality::Defensive)) {
                        break;  // honor the pact
                    }
                }
            }
            TreatyProposal p;
            p.proposer       = d.actor;
            p.recipient      = d.target;
            p.proposedState  = RelationState::War;
            p.proposedTurn   = currentTurn_;
            p.accepted       = true;
            diplomacyEngine_.submitProposal(std::move(p), pendingProposals_);
            break;
        }
        case AIDecisionType::NegotiatePeace: {
            if (d.target == NO_KINGDOM || !kingdoms_.count(d.target)) break;
            if (actor.policy == NationalPolicy::FinalWar ||
                kingdoms_.at(d.target).policy == NationalPolicy::FinalWar) {
                break;
            }
            TreatyProposal p;
            p.proposer       = d.actor;
            p.recipient      = d.target;
            p.proposedState  = RelationState::Peace;
            p.proposedTurn   = currentTurn_;
            p.accepted       = false;
            diplomacyEngine_.submitProposal(std::move(p), pendingProposals_);
            break;
        }
        case AIDecisionType::FormAlliance: {
            if (d.target == NO_KINGDOM || !kingdoms_.count(d.target)) break;
            TreatyProposal p;
            p.proposer       = d.actor;
            p.recipient      = d.target;
            p.proposedState  = RelationState::Alliance;
            p.proposedTurn   = currentTurn_;
            p.accepted       = false;
            diplomacyEngine_.submitProposal(std::move(p), pendingProposals_);
            break;
        }
        case AIDecisionType::Attack: {
            std::vector<ArmyID> ownArmies;
            for (ArmyID eid : actor.armies) {
                if (armies_.count(eid) && !armies_.at(eid).isEmpty()) {
                    ownArmies.push_back(eid);
                }
            }
            if (ownArmies.empty()) break;

            auto atWar = [&](KingdomID a, KingdomID b) {
                KingdomID lo = std::min(a, b);
                KingdomID hi = std::max(a, b);
                auto it = relations_.find(lo);
                if (it == relations_.end()) return false;
                auto it2 = it->second.find(hi);
                if (it2 == it->second.end()) return false;
                return it2->second.state == RelationState::War;
            };

            auto distanceBetween = [&](jke::TileID a, jke::TileID b) {
                auto ap = worldMap_.at(a).position;
                auto bp = worldMap_.at(b).position;
                return std::hypot(float(ap.x - bp.x), float(ap.y - bp.y));
            };

            auto nearestOwnedCityTile = [&](KingdomID owner, TileID origin) {
                TileID bestTile = NO_TILE;
                float bestDist = 1e9f;
                for (CityID cid : kingdoms_.at(owner).cities) {
                    if (!cities_.count(cid) || cities_.at(cid).owner != owner) continue;
                    if (cities_.at(cid).tile == NO_TILE) continue;
                    float dist = distanceBetween(origin, cities_.at(cid).tile);
                    if (dist < bestDist) {
                        bestDist = dist;
                        bestTile = cities_.at(cid).tile;
                    }
                }
                return bestTile;
            };

            auto nearestOwnedCityDistanceTo = [&](KingdomID owner, TileID target) {
                float bestDist = 1e9f;
                for (CityID cid : kingdoms_.at(owner).cities) {
                    if (!cities_.count(cid) || cities_.at(cid).owner != owner) continue;
                    if (cities_.at(cid).tile == NO_TILE) continue;
                    bestDist = std::min(bestDist, distanceBetween(cities_.at(cid).tile, target));
                }
                return bestDist > 1e8f ? 0.0f : bestDist;
            };

            std::vector<TileID> invaderTiles;
            auto nearestInvaderTo = [&](TileID origin) {
                TileID bestTarget = invaderTiles.empty() ? NO_TILE : invaderTiles.front();
                float bestDist = bestTarget == NO_TILE ? 1e9f : distanceBetween(origin, bestTarget);
                for (TileID tid : invaderTiles) {
                    float dist = distanceBetween(origin, tid);
                    if (dist < bestDist) {
                        bestDist = dist;
                        bestTarget = tid;
                    }
                }
                return bestTarget;
            };

            // Priority 0: defend city priorities.  Defense armies protect the
            // threatened city; mobile armies intercept the nearest invader.
            for (const auto& [eid, enemy] : armies_) {
                if (enemy.owner == actor.id) continue;
                if (!kingdoms_.count(enemy.owner) || !kingdoms_.at(enemy.owner).isAlive) continue;
                if (!atWar(actor.id, enemy.owner)) continue;
                const Tile& et = worldMap_.at(enemy.currentTile);
                if (et.owner != actor.id) continue;
                invaderTiles.push_back(enemy.currentTile);
            }

            if (!invaderTiles.empty()) {
                struct DefensePoint { CityID city; TileID tile; float score; };
                std::vector<DefensePoint> defensePoints;

                for (CityID cid : actor.cities) {
                    if (!cities_.count(cid)) continue;
                    const City& city = cities_.at(cid);
                    if (city.owner != actor.id || city.tile == NO_TILE) continue;

                    float nearestThreat = 1e9f;
                    for (TileID invaderTile : invaderTiles) {
                        nearestThreat = std::min(nearestThreat, distanceBetween(city.tile, invaderTile));
                    }

                    float score = 100.0f / std::max(1.0f, nearestThreat);
                    if (city.isCapital || actor.capitalCity == cid) score += 45.0f;
                    score += static_cast<float>(city.population) * 0.0025f;
                    score += (1.0f - city.fortification) * 22.0f;
                    defensePoints.push_back({cid, city.tile, score});
                }

                std::sort(defensePoints.begin(), defensePoints.end(),
                          [](const DefensePoint& a, const DefensePoint& b) {
                              return a.score > b.score;
                          });

                std::vector<ArmyID> defenders;
                for (ArmyID eid : ownArmies) {
                    const ArmyRole role = armies_.at(eid).role;
                    if (role == ArmyRole::Defense || role == ArmyRole::Reserve) {
                        defenders.push_back(eid);
                    }
                }
                if (defenders.empty()) {
                    std::sort(ownArmies.begin(), ownArmies.end(), [&](ArmyID a, ArmyID b) {
                        return distanceBetween(armies_.at(a).currentTile, invaderTiles.front()) <
                               distanceBetween(armies_.at(b).currentTile, invaderTiles.front());
                    });
                    const size_t responseCount = std::max<size_t>(1, ownArmies.size() / 2);
                    defenders.assign(ownArmies.begin(), ownArmies.begin() + responseCount);
                }

                for (ArmyID eid : defenders) {
                    TileID bestTarget = nearestInvaderTo(armies_.at(eid).currentTile);
                    if (armies_.at(eid).role == ArmyRole::Defense && !defensePoints.empty()) {
                        auto closestCity = std::min_element(
                            defensePoints.begin(), defensePoints.end(),
                            [&](const DefensePoint& a, const DefensePoint& b) {
                                return distanceBetween(armies_.at(eid).currentTile, a.tile) <
                                       distanceBetween(armies_.at(eid).currentTile, b.tile);
                            });
                        bestTarget = closestCity->tile;
                    }
                    armies_.at(eid).targetTile = bestTarget;
                    armies_.at(eid).movementPath.clear();
                    armies_.at(eid).pathCursor = 0;
                }
                if (defenders.size() >= ownArmies.size()) break;
            }

            if (currentSeason() == Season::Winter) {
                for (ArmyID eid : ownArmies) {
                    Army& army = armies_.at(eid);
                    bool canContinueSiege = false;
                    if (army.role == ArmyRole::Siege &&
                        army.currentTile != NO_TILE &&
                        army.currentTile < static_cast<TileID>(worldMap_.tileCount())) {
                        const Tile& current = worldMap_.at(army.currentTile);
                        canContinueSiege =
                            current.city != NO_CITY &&
                            cities_.count(current.city) &&
                            cities_.at(current.city).owner != actor.id &&
                            atWar(actor.id, cities_.at(current.city).owner);
                    }
                    if (!canContinueSiege &&
                        (army.role == ArmyRole::Attack || army.role == ArmyRole::Siege)) {
                        army.targetTile = NO_TILE;
                        army.movementPath.clear();
                        army.pathCursor = 0;
                    }
                }
                break;
            }

            // Priority 1: attack nearest enemy city (sequential conquest)
            // Logistics first: tired armies return to a friendly base instead of
            // joining another deep push and bouncing back a few turns later.
            for (ArmyID eid : ownArmies) {
                Army& army = armies_.at(eid);
                if (army.supplyLevel >= 0.52f) continue;
                if (army.role == ArmyRole::Defense) continue;
                bool activeSiege = false;
                if (army.role == ArmyRole::Siege &&
                    army.currentTile != NO_TILE &&
                    army.currentTile < static_cast<TileID>(worldMap_.tileCount())) {
                    const Tile& current = worldMap_.at(army.currentTile);
                    activeSiege =
                        current.city != NO_CITY &&
                        cities_.count(current.city) &&
                        cities_.at(current.city).owner != actor.id &&
                        atWar(actor.id, cities_.at(current.city).owner) &&
                        army.supplyLevel > 0.28f;
                }
                if (activeSiege) continue;
                TileID base = nearestOwnedCityTile(actor.id, army.currentTile);
                if (base != NO_TILE && base != army.currentTile) {
                    army.targetTile = base;
                    army.movementPath.clear();
                    army.pathCursor = 0;
                    army.role = ArmyRole::Reserve;
                }
            }

            std::vector<ArmyID> attackArmies;
            for (ArmyID eid : ownArmies) {
                const Army& army = armies_.at(eid);
                if (army.supplyLevel < 0.52f && actor.policy != NationalPolicy::FinalWar) continue;
                ArmyRole role = armies_.at(eid).role;
                if (role == ArmyRole::Attack || role == ArmyRole::Siege ||
                    actor.policy == NationalPolicy::FinalWar) {
                    attackArmies.push_back(eid);
                }
            }
            if (attackArmies.empty()) {
                for (ArmyID eid : ownArmies) {
                    if (armies_.at(eid).role != ArmyRole::Defense &&
                        armies_.at(eid).supplyLevel >= 0.58f) attackArmies.push_back(eid);
                }
            }
            if (attackArmies.empty()) break;

            KingdomID enemyFilter = d.target != NO_KINGDOM ? d.target : actor.strategicTarget;

            auto isSupplyHub = [](const City& city) {
                return city.cityType == CityType::Fortress ||
                       city.cityType == CityType::TradeHub ||
                       city.cityType == CityType::Port;
            };

            struct RearThreat {
                CityID city = NO_CITY;
                TileID tile = NO_TILE;
                float score = 0.0f;
            };
            std::vector<RearThreat> rearThreats;
            for (CityID cid : actor.cities) {
                if (!cities_.count(cid)) continue;
                const City& city = cities_.at(cid);
                if (city.owner != actor.id || city.tile == NO_TILE) continue;
                const bool vitalHub = city.isCapital || isSupplyHub(city) ||
                    city.cityType == CityType::Mining || city.population >= 900;
                if (!vitalHub) continue;

                float score = 0.0f;
                for (const auto& [enemyAid, enemyArmy] : armies_) {
                    if (enemyArmy.owner == actor.id) continue;
                    if (!kingdoms_.count(enemyArmy.owner) || !kingdoms_.at(enemyArmy.owner).isAlive) continue;
                    if (!atWar(actor.id, enemyArmy.owner)) continue;
                    if (enemyArmy.currentTile == NO_TILE ||
                        enemyArmy.currentTile >= static_cast<TileID>(worldMap_.tileCount())) continue;
                    float dist = distanceBetween(city.tile, enemyArmy.currentTile);
                    if (dist > 24.0f) continue;
                    score += (24.0f - dist);
                    score += static_cast<float>(enemyArmy.totalSoldiers()) * 0.004f;
                    if (worldMap_.at(enemyArmy.currentTile).owner == actor.id) score += 14.0f;
                }

                if (score <= 0.0f) continue;
                if (city.isCapital) score += 18.0f;
                if (isSupplyHub(city)) score += 12.0f;
                if (city.cityType == CityType::Mining) score += 6.0f;
                rearThreats.push_back({cid, city.tile, score});
            }
            std::sort(rearThreats.begin(), rearThreats.end(),
                      [](const RearThreat& a, const RearThreat& b) {
                          return a.score > b.score;
                      });

            if (!rearThreats.empty()) {
                std::vector<ArmyID> rearCandidates;
                for (ArmyID eid : ownArmies) {
                    const Army& army = armies_.at(eid);
                    if (army.role == ArmyRole::Siege) continue;
                    if (army.supplyLevel < 0.30f) continue;
                    if (army.role == ArmyRole::Defense || army.role == ArmyRole::Reserve) {
                        rearCandidates.push_back(eid);
                    }
                }
                if (rearCandidates.empty() && attackArmies.size() >= 3) {
                    for (ArmyID eid : attackArmies) {
                        const Army& army = armies_.at(eid);
                        if (army.role != ArmyRole::Siege && army.supplyLevel >= 0.55f) {
                            rearCandidates.push_back(eid);
                        }
                    }
                }

                std::vector<ArmyID> assignedRear;
                const size_t responseLimit = std::min(
                    rearThreats.size(),
                    std::max<size_t>(1, ownArmies.size() / 3));
                for (size_t i = 0; i < responseLimit && !rearCandidates.empty(); ++i) {
                    const RearThreat& threat = rearThreats[i];
                    auto best = std::min_element(
                        rearCandidates.begin(), rearCandidates.end(),
                        [&](ArmyID a, ArmyID b) {
                            return distanceBetween(armies_.at(a).currentTile, threat.tile) <
                                   distanceBetween(armies_.at(b).currentTile, threat.tile);
                        });
                    if (best == rearCandidates.end()) break;
                    ArmyID aid = *best;
                    Army& army = armies_.at(aid);
                    army.role = ArmyRole::Defense;
                    army.targetTile = threat.tile;
                    army.movementPath.clear();
                    army.pathCursor = 0;
                    assignedRear.push_back(aid);
                    rearCandidates.erase(best);
                }

                if (!assignedRear.empty()) {
                    attackArmies.erase(
                        std::remove_if(attackArmies.begin(), attackArmies.end(),
                            [&](ArmyID aid) {
                                return std::find(assignedRear.begin(), assignedRear.end(), aid) != assignedRear.end();
                            }),
                        attackArmies.end());
                    if (attackArmies.empty()) break;
                }
            }

            struct InterdictionTarget {
                CityID city = NO_CITY;
                float pressure = 0.0f;
            };
            std::vector<InterdictionTarget> interdictionTargets;
            auto addInterdictionPressure = [&](CityID cid, float pressure) {
                if (cid == NO_CITY || pressure <= 0.0f) return;
                for (auto& t : interdictionTargets) {
                    if (t.city == cid) {
                        t.pressure = std::max(t.pressure, pressure);
                        return;
                    }
                }
                interdictionTargets.push_back({cid, pressure});
            };
            auto interdictionPressureFor = [&](CityID cid) {
                float pressure = 0.0f;
                for (const auto& t : interdictionTargets) {
                    if (t.city == cid) pressure = std::max(pressure, t.pressure);
                }
                return pressure;
            };

            // Supply interdiction: if an enemy field army is overextended,
            // prefer taking the nearest enemy hub that keeps it supplied.
            for (const auto& [enemyAid, enemyArmy] : armies_) {
                if (enemyArmy.owner == actor.id) continue;
                if (!kingdoms_.count(enemyArmy.owner) || !kingdoms_.at(enemyArmy.owner).isAlive) continue;
                if (enemyFilter != NO_KINGDOM && enemyArmy.owner != enemyFilter) continue;
                if (!atWar(actor.id, enemyArmy.owner)) continue;
                if (enemyArmy.currentTile == NO_TILE ||
                    enemyArmy.currentTile >= static_cast<TileID>(worldMap_.tileCount())) continue;

                const float enemyBaseDist =
                    nearestOwnedCityDistanceTo(enemyArmy.owner, enemyArmy.currentTile);
                const Tile& enemyTile = worldMap_.at(enemyArmy.currentTile);
                const bool overextended =
                    enemyArmy.supplyLevel < 0.62f ||
                    enemyBaseDist > 24.0f ||
                    enemyTile.owner == actor.id;
                if (!overextended) continue;

                CityID bestHub = NO_CITY;
                float bestHubScore = 1e9f;
                for (CityID cid : kingdoms_.at(enemyArmy.owner).cities) {
                    if (!cities_.count(cid)) continue;
                    const City& hub = cities_.at(cid);
                    if (hub.owner != enemyArmy.owner || hub.tile == NO_TILE) continue;
                    if (hub.isCapital && kingdoms_.at(enemyArmy.owner).cities.size() > 1) continue;

                    const bool strategicHub = isSupplyHub(hub) ||
                        hub.cityType == CityType::Mining ||
                        hub.population >= 900;
                    if (!strategicHub) continue;

                    float hubToArmy = distanceBetween(hub.tile, enemyArmy.currentTile);
                    float ourReach = nearestOwnedCityDistanceTo(actor.id, hub.tile);
                    float score = hubToArmy + std::max(0.0f, ourReach - 28.0f) * 1.6f;
                    if (isSupplyHub(hub)) score -= 8.0f;
                    if (score < bestHubScore) {
                        bestHubScore = score;
                        bestHub = cid;
                    }
                }

                if (bestHub != NO_CITY && bestHubScore < 46.0f) {
                    float pressure = 18.0f;
                    pressure += std::max(0.0f, 0.70f - enemyArmy.supplyLevel) * 70.0f;
                    pressure += std::max(0.0f, enemyBaseDist - 18.0f) * 1.8f;
                    if (enemyTile.owner == actor.id) pressure += 18.0f;
                    addInterdictionPressure(bestHub, pressure);
                }
            }

            struct FrontlineBase {
                CityID city;
                TileID tile;
                float score;
            };
            std::vector<FrontlineBase> frontlineBases;
            for (CityID cid : actor.cities) {
                if (!cities_.count(cid)) continue;
                const City& city = cities_.at(cid);
                if (city.owner != actor.id || city.tile == NO_TILE) continue;

                float nearestEnemyCity = 1e9f;
                for (const auto& [ecid, enemyCity] : cities_) {
                    if (enemyCity.owner == actor.id || enemyCity.owner == NO_KINGDOM) continue;
                    if (enemyFilter != NO_KINGDOM && enemyCity.owner != enemyFilter) continue;
                    if (!kingdoms_.count(enemyCity.owner) || !kingdoms_.at(enemyCity.owner).isAlive) continue;
                    if (!atWar(actor.id, enemyCity.owner)) continue;
                    nearestEnemyCity = std::min(nearestEnemyCity, distanceBetween(city.tile, enemyCity.tile));
                }
                if (nearestEnemyCity > 42.0f) continue;

                float score = 100.0f / std::max(1.0f, nearestEnemyCity);
                if (city.isCapital) score += 0.8f;
                if (city.cityType == CityType::Fortress) score += 1.3f;
                if (city.cityType == CityType::Port) score += 0.8f;
                if (city.cityType == CityType::TradeHub) score += 0.7f;
                score += city.fortification * 0.8f;
                frontlineBases.push_back({cid, city.tile, score});
            }
            std::sort(frontlineBases.begin(), frontlineBases.end(),
                      [](const FrontlineBase& a, const FrontlineBase& b) {
                          return a.score > b.score;
                      });

            auto bestFrontlineFor = [&](TileID target) {
                TileID best = NO_TILE;
                float bestDist = 1e9f;
                for (const auto& base : frontlineBases) {
                    float dist = distanceBetween(base.tile, target);
                    if (dist < bestDist) {
                        bestDist = dist;
                        best = base.tile;
                    }
                }
                return best;
            };

            if (!frontlineBases.empty()) {
                for (ArmyID eid : ownArmies) {
                    Army& army = armies_.at(eid);
                    if (army.role != ArmyRole::Defense) continue;
                    auto closest = std::min_element(
                        frontlineBases.begin(), frontlineBases.end(),
                        [&](const FrontlineBase& a, const FrontlineBase& b) {
                            return distanceBetween(army.currentTile, a.tile) <
                                   distanceBetween(army.currentTile, b.tile);
                        });
                    if (closest != frontlineBases.end() &&
                        distanceBetween(army.currentTile, closest->tile) > 2.0f) {
                        army.targetTile = closest->tile;
                        army.movementPath.clear();
                        army.pathCursor = 0;
                    }
                }
            }

            const auto& leadArmy = armies_.at(attackArmies.front());
            auto leadPos = worldMap_.at(leadArmy.currentTile).position;

            // Supply range: how far the army can safely advance from its nearest base
            float distToBase = 1e9f;
            for (CityID cid : actor.cities) {
                if (!cities_.count(cid) || cities_.at(cid).owner != actor.id) continue;
                auto cp = worldMap_.at(cities_.at(cid).tile).position;
                float baseDist = std::hypot(float(leadPos.x - cp.x), float(leadPos.y - cp.y));
                distToBase = std::min(distToBase, baseDist);
            }
            if (distToBase > 1e8f) distToBase = 0.0f;
            // Safe reach: base + budget from current supply (full supply = 28 more tiles)
            float safeRange = distToBase + leadArmy.supplyLevel * 28.0f;

            struct TargetCandidate {
                CityID city;
                TileID tile;
                float score;
                float supplyDistance;
            };
            std::vector<TargetCandidate> targets;
            for (const auto& [cid, city] : cities_) {
                if (city.owner == actor.id || city.owner == NO_KINGDOM) continue;
                if (enemyFilter != NO_KINGDOM && city.owner != enemyFilter) continue;
                if (!kingdoms_.count(city.owner) || !kingdoms_.at(city.owner).isAlive) continue;
                if (!atWar(actor.id, city.owner)) continue;
                if (city.isCapital && kingdoms_.at(city.owner).cities.size() > 1) continue;
                auto cp = worldMap_.at(city.tile).position;
                float dist = std::hypot(float(leadPos.x - cp.x), float(leadPos.y - cp.y));
                // Penalise targets beyond supply range — prefer stepping-stone cities first
                float supplyPenalty = std::max(0.0f, dist - safeRange) * 2.5f;
                float supplyBaseDist = nearestOwnedCityDistanceTo(actor.id, city.tile);
                float basePenalty = std::max(0.0f, supplyBaseDist - 18.0f) * 4.0f;
                float score = dist + supplyPenalty;
                score += basePenalty;

                const bool supplyHub =
                    city.cityType == CityType::Fortress ||
                    city.cityType == CityType::TradeHub ||
                    city.cityType == CityType::Port;
                if (city.cityType == CityType::Fortress) score -= 22.0f;
                if (city.cityType == CityType::TradeHub) score -= 16.0f;
                if (city.cityType == CityType::Port) score -= 14.0f;
                if (supplyHub && supplyBaseDist <= 26.0f) score -= 18.0f;
                if (!supplyHub && supplyBaseDist > 24.0f) score += 20.0f;
                const float interdictionPressure = interdictionPressureFor(cid);
                if (interdictionPressure > 0.0f) {
                    score -= interdictionPressure;
                    if (supplyHub) score -= 12.0f;
                }

                float enemyFieldPressure = 0.0f;
                float exhaustedEnemyOpportunity = 0.0f;
                for (const auto& [enemyAid, enemyArmy] : armies_) {
                    if (enemyArmy.owner != city.owner) continue;
                    if (enemyArmy.currentTile == NO_TILE ||
                        enemyArmy.currentTile >= static_cast<TileID>(worldMap_.tileCount())) continue;
                    float armyDist = distanceBetween(enemyArmy.currentTile, city.tile);
                    if (armyDist > 14.0f) continue;
                    if (enemyArmy.supplyLevel < 0.35f) {
                        exhaustedEnemyOpportunity +=
                            static_cast<float>(enemyArmy.totalSoldiers()) * 0.004f;
                    } else {
                        enemyFieldPressure +=
                            static_cast<float>(enemyArmy.totalSoldiers()) *
                            (enemyArmy.supplyLevel > 0.55f ? 0.007f : 0.004f);
                    }
                }
                score += enemyFieldPressure;
                score -= std::min(18.0f, exhaustedEnemyOpportunity);

                targets.push_back({cid, city.tile, score, supplyBaseDist});
            }
            std::sort(targets.begin(), targets.end(),
                      [](const TargetCandidate& a, const TargetCandidate& b) {
                          return a.score < b.score;
                      });

            if (targets.empty()) {
                bool targetAllowed = false;
                if (d.targetCity != NO_CITY &&
                    cities_.count(d.targetCity) &&
                    cities_.at(d.targetCity).owner != actor.id) {
                    const KingdomID owner = cities_.at(d.targetCity).owner;
                    const bool capitalOpen =
                        !cities_.at(d.targetCity).isCapital ||
                        (kingdoms_.count(owner) && kingdoms_.at(owner).cities.size() <= 1);
                    targetAllowed = owner == NO_KINGDOM ||
                        (atWar(actor.id, owner) && capitalOpen);
                }
                if (targetAllowed) {
                    const float supplyBaseDist =
                        nearestOwnedCityDistanceTo(actor.id, cities_.at(d.targetCity).tile);
                    targets.push_back({d.targetCity, cities_.at(d.targetCity).tile,
                                       supplyBaseDist, supplyBaseDist});
                } else {
                    break;
                }
            }

            auto passable = [&](TileID tid) {
                const Tile& t = worldMap_.at(tid);
                return t.terrain != TerrainType::Ocean &&
                       t.terrain != TerrainType::Lake;
            };

            const size_t desiredFronts = attackArmies.size() >= 6 ? 3u :
                                         attackArmies.size() >= 3 ? 2u : 1u;
            const size_t frontCount = std::min(desiredFronts, targets.size());

            std::sort(attackArmies.begin(), attackArmies.end(), [&](ArmyID a, ArmyID b) {
                return distanceBetween(armies_.at(a).currentTile, targets.front().tile) <
                       distanceBetween(armies_.at(b).currentTile, targets.front().tile);
            });

            bool assignedCityAssault = false;
            for (ArmyID eid : attackArmies) {
                if (armies_.at(eid).role == ArmyRole::Siege) {
                    assignedCityAssault = true;
                    break;
                }
            }

            size_t mobileIndex = 0;
            for (size_t i = 0; i < attackArmies.size(); ++i) {
                ArmyID eid = attackArmies[i];
                const bool siege = armies_.at(eid).role == ArmyRole::Siege ||
                    (!assignedCityAssault && i == 0);
                const TargetCandidate& target = siege
                    ? targets.front()
                    : targets[mobileIndex++ % frontCount];

                std::vector<TileID> stagingTargets{target.tile};
                for (TileID nid : worldMap_.neighbors8(target.tile)) {
                    if (!passable(nid)) continue;
                    stagingTargets.push_back(nid);
                }

                TileID assignedTarget = siege
                    ? target.tile
                    : stagingTargets[(mobileIndex + i) % stagingTargets.size()];

                if (siege && !frontlineBases.empty() && attackArmies.size() >= 2) {
                    float nearestSupport = 1e9f;
                    for (ArmyID supportId : attackArmies) {
                        if (supportId == eid) continue;
                        if (armies_.at(supportId).role == ArmyRole::Defense) continue;
                        nearestSupport = std::min(nearestSupport,
                            distanceBetween(armies_.at(supportId).currentTile, target.tile));
                    }
                    if (nearestSupport > 10.0f &&
                        distanceBetween(armies_.at(eid).currentTile, target.tile) > 8.0f) {
                        TileID frontline = bestFrontlineFor(target.tile);
                        if (frontline != NO_TILE) assignedTarget = frontline;
                    }
                }
                if (!frontlineBases.empty() &&
                    target.supplyDistance > 22.0f &&
                    armies_.at(eid).supplyLevel < 0.82f) {
                    TileID frontline = bestFrontlineFor(target.tile);
                    if (frontline != NO_TILE &&
                        distanceBetween(armies_.at(eid).currentTile, frontline) > 4.0f) {
                        assignedTarget = frontline;
                    }
                }
                if (siege) assignedCityAssault = true;
                armies_.at(eid).targetTile = assignedTarget;
                armies_.at(eid).movementPath.clear();
                armies_.at(eid).pathCursor = 0;
            }
            break;
        }
        case AIDecisionType::Recruit: {
            if (d.targetCity == NO_CITY || !cities_.count(d.targetCity)) break;

            // Soldiers scale with kingdom size
            uint32_t soldiers = 500u +
                static_cast<uint32_t>(actor.cities.size()) * 300u;
            soldiers = std::min(soldiers, 4000u);
            // Diplomatic kingdoms field smaller armies (focus on trade, not war)
            if (actor.personality == KingdomPersonality::Diplomatic)
                soldiers = static_cast<uint32_t>(soldiers * 0.82f);
            float s = static_cast<float>(soldiers);

            // ── Cost model ──────────────────────────────────────────────────
            // Reinforce: cheap gold (resupply)
            // New army:  gold only — iron removed to prevent late-game deadlock
            ResourceLedger reinforceCost;
            reinforceCost.gold = s * 0.03f;   // ~24g to top up 800 soldiers

            ResourceLedger spawnCost;
            spawnCost.gold = s * 0.08f;       // ~64g for 800 soldiers

            // Count only armies that still exist AND have soldiers
            int livingArmies = 0;
            bool hasSiegeArmy = false;
            for (ArmyID eid : actor.armies) {
                if (armies_.count(eid) && !armies_.at(eid).isEmpty()) {
                    ++livingArmies;
                    const Army& army = armies_.at(eid);
                    for (const auto& unit : army.units) {
                        if (unit.type == UnitType::SiegeUnit && unit.soldiers > 0) {
                            hasSiegeArmy = true;
                            break;
                        }
                    }
                }
            }

            bool atWar = false;
            auto relIt = relations_.find(d.actor);
            if (relIt != relations_.end()) {
                for (const auto& [other, rel] : relIt->second) {
                    if (rel.state == RelationState::War &&
                        kingdoms_.count(other) &&
                        kingdoms_.at(other).isAlive) {
                        atWar = true;
                        break;
                    }
                }
            }
            if (!atWar) {
                for (const auto& [other, rels] : relations_) {
                    auto it = rels.find(d.actor);
                    if (it != rels.end() &&
                        it->second.state == RelationState::War &&
                        kingdoms_.count(other) &&
                        kingdoms_.at(other).isAlive) {
                        atWar = true;
                        break;
                    }
                }
            }

            // Cap: at least 3 armies so one-city kingdoms can field an attacker,
            // a siege force, and a replacement instead of freezing after losses.
            int bonusArmies =
                (actor.strategyPlan == StrategyPlan::OpportunisticRaid ||
                 actor.strategyPlan == StrategyPlan::RevengeWar ||
                 actor.strategyPlan == StrategyPlan::TotalConquest) ? 1 : 0;
            // Diplomatic kingdoms are limited to 2 armies (no military culture)
            const int hardCap = (actor.personality == KingdomPersonality::Diplomatic) ? 2 : 8;
            int maxArmies = std::clamp(static_cast<int>(actor.cities.size()) + 1 + bonusArmies, 3, hardCap);
            if (livingArmies >= maxArmies) {
                // At cap — reinforce cheaply if we can afford it
                if (!actor.treasury.canAfford(reinforceCost)) break;
                for (ArmyID eid : actor.armies) {
                    if (!armies_.count(eid) || armies_.at(eid).isEmpty()) continue;
                    auto& ea = armies_.at(eid);
                    if (!ea.units.empty()) {
                        actor.treasury -= reinforceCost;
                        ea.units.front().soldiers += soldiers / 2;
                    }
                    break;
                }
                break;
            }

            UnitType recruitType = UnitType::Infantry;
            uint32_t recruitSoldiers = soldiers;

            // If every army was wiped out, raise emergency militia even when broke.
            // This prevents permanent no-army states and keeps wars resolvable.
            bool emergencyLevy = livingArmies == 0;
            if (emergencyLevy) {
                recruitType = UnitType::Militia;
                recruitSoldiers = std::max(400u, soldiers / 2);
            } else if (atWar && !hasSiegeArmy) {
                recruitType = UnitType::SiegeUnit;
                recruitSoldiers = std::max(1000u, soldiers);
            }

            // Spawn new army. Normal armies need gold; emergency militia are free.
            if (!emergencyLevy) {
                if (!actor.treasury.canAfford(spawnCost)) break;
                actor.treasury -= spawnCost;
            }

            // Personality modifies recruit pool size
            if (actor.personality == KingdomPersonality::Diplomatic) {
                // Diplomatic: small armies, no military culture
                recruitSoldiers = static_cast<uint32_t>(recruitSoldiers * 0.50f);
                recruitSoldiers = std::max(recruitSoldiers, 300u);
            } else if (actor.personality == KingdomPersonality::Aggressive) {
                // Warrior culture: larger levies
                recruitSoldiers = static_cast<uint32_t>(recruitSoldiers * 1.40f);
            } else if (actor.personality == KingdomPersonality::Expansionist) {
                // Large population from food surplus → bigger armies
                recruitSoldiers = static_cast<uint32_t>(recruitSoldiers * 1.30f);
            } else if (actor.personality == KingdomPersonality::Opportunistic) {
                // Good at mobilizing resources when needed
                recruitSoldiers = static_cast<uint32_t>(recruitSoldiers * 1.15f);
            }

            RecruitOrder order;
            order.kingdom  = d.actor;
            order.city     = d.targetCity;
            order.unitType = recruitType;
            order.soldiers = recruitSoldiers;

            militaryEngine_.spawnArmy(kingdoms_, armies_, worldMap_,
                                      nextArmyID_, nextUnitID_, order, cities_);
            break;
        }
        case AIDecisionType::ResearchTech: {
            if (actor.currentResearch != NO_TECH) break;
            // Pick first available tech that matches specialization preference
            auto available = techTree_.available(actor.researchedTechs);
            if (available.empty()) break;

            // Personality overrides specialization when at war or in crisis
            bool atWar = false;
            for (const auto& [oid, rel] : relations_) {
                if (oid != actor.id) continue;
                for (const auto& [rid, r] : rel)
                    if (r.state == RelationState::War) { atWar = true; break; }
                if (atWar) break;
            }

            TechCategory preferred;
            // Personality-driven override (situational)
            if (atWar && (actor.personality == KingdomPersonality::Aggressive ||
                           actor.personality == KingdomPersonality::Expansionist)) {
                preferred = TechCategory::Military;
            } else if (actor.personality == KingdomPersonality::Defensive) {
                preferred = TechCategory::Fortification;
            } else if (actor.personality == KingdomPersonality::Diplomatic) {
                preferred = TechCategory::Administration;
            } else {
                // Fallback to specialization
                switch (actor.specialization) {
                    case KingdomSpecialization::Military:    preferred = TechCategory::Military;       break;
                    case KingdomSpecialization::Economy:     preferred = TechCategory::Economy;        break;
                    case KingdomSpecialization::Agriculture: preferred = TechCategory::Agriculture;    break;
                    case KingdomSpecialization::Technology:  preferred = TechCategory::Administration; break;
                    case KingdomSpecialization::Trade:       preferred = TechCategory::Economy;        break;
                    case KingdomSpecialization::Defense:     preferred = TechCategory::Fortification;  break;
                }
            }

            TechID chosen = NO_TECH;
            for (TechID tid : available) {
                const Technology* t = techTree_.find(tid);
                if (t && t->category == preferred) {
                    if (actor.treasury.canAfford(t->researchCost)) {
                        chosen = tid;
                        break;
                    }
                }
            }
            // Fallback to any affordable tech
            if (chosen == NO_TECH) {
                for (TechID tid : available) {
                    const Technology* t = techTree_.find(tid);
                    if (t && actor.treasury.canAfford(t->researchCost)) {
                        chosen = tid;
                        break;
                    }
                }
            }
            if (chosen == NO_TECH) break;
            const Technology* t = techTree_.find(chosen);
            if (!t) break;
            actor.treasury -= t->researchCost;
            actor.currentResearch  = chosen;
            actor.researchProgress = 0.0f;
            break;
        }
        case AIDecisionType::ImproveEconomy: {
            // Build a market or farm in the capital if resources allow
            if (d.targetCity == NO_CITY || !cities_.count(d.targetCity)) {
                if (actor.capitalCity == NO_CITY) break;
            }
            CityID cid = (d.targetCity != NO_CITY) ? d.targetCity : actor.capitalCity;
            if (!cities_.count(cid)) break;
            City& city = cities_.at(cid);
            if (actor.treasury.gold > 40.0f && actor.treasury.wood > 20.0f) {
                actor.treasury.gold -= 40.0f;
                actor.treasury.wood -= 20.0f;
                city.buildings.push_back({BuildingType::Market, 1, 1.0f});
            } else if (actor.treasury.gold > 20.0f && actor.treasury.wood > 10.0f) {
                actor.treasury.gold -= 20.0f;
                actor.treasury.wood -= 10.0f;
                city.buildings.push_back({BuildingType::Farm, 1, 1.0f});
            }
            break;
        }
        case AIDecisionType::UpgradeCity: {
            CityID cid = (d.targetCity != NO_CITY) ? d.targetCity : actor.capitalCity;
            if (cid == NO_CITY || !cities_.count(cid)) break;
            City& city = cities_.at(cid);
            // Build walls if no fortification
            if (actor.treasury.stone > 30.0f && actor.treasury.gold > 20.0f) {
                bool hasWalls = false;
                for (auto& b : city.buildings)
                    if (b.type == BuildingType::Walls) { hasWalls = true; break; }
                if (!hasWalls) {
                    actor.treasury.stone -= 30.0f;
                    actor.treasury.gold  -= 20.0f;
                    city.buildings.push_back({BuildingType::Walls, 1, 1.0f});
                    city.fortification = std::min(1.0f, city.fortification + 0.2f);
                }
            }
            break;
        }
        case AIDecisionType::HireMercenary: {
            const float MERC_COST =
                actor.personality == KingdomPersonality::Opportunistic ? 255.0f : 300.0f;
            if (actor.treasury.gold < MERC_COST) break;
            if (actor.capitalCity == NO_CITY || !cities_.count(actor.capitalCity)) break;

            actor.treasury.gold -= MERC_COST;

            Army merc;
            merc.id           = nextArmyID_++;
            merc.owner        = d.actor;
            merc.isMercenary  = true;
            merc.contractUntil= currentTurn_ + 40;

            Unit infantry;
            infantry.id        = nextUnitID_++;
            infantry.type      = UnitType::Infantry;
            // Opportunistic hires elite mercenaries (they pay top coin for the best)
            const bool isOpp = (actor.personality == KingdomPersonality::Opportunistic);
            infantry.soldiers  = isOpp ? 1200u : 800u;
            infantry.training  = isOpp ? 0.78f : 0.70f;
            infantry.morale    = 0.85f;
            merc.units.push_back(infantry);

            const auto& cap = cities_.at(actor.capitalCity);
            merc.currentTile  = cap.tile;
            merc.position     = cap.position;
            merc.role         = ArmyRole::Attack;

            worldMap_.at(cap.tile).army = merc.id;
            actor.armies.push_back(merc.id);
            armies_[merc.id] = std::move(merc);

            HistoryEvent ev;
            ev.type           = EventType::WorldEventPositive;
            ev.turn           = currentTurn_;
            ev.primaryKingdom = d.actor;
            ev.description    = actor.name + " hired a mercenary company.";
            eventBus_.emit(std::move(ev));
            break;
        }
        default:
            break;
    }
}

void SimulationEngine::collectSnapshot() {
    SimulationSnapshot snap;
    snap.turn = currentTurn_;

    // Count living kingdoms and total cities
    for (const auto& [kid, k] : kingdoms_) {
        snap.kingdoms.push_back(k);
        if (k.isAlive) snap.livingKingdoms++;
    }
    for (const auto& [cid, c] : cities_) {
        snap.cities.push_back(c);
        snap.totalCities++;
    }
    for (const auto& [aid, a] : armies_) {
        snap.armies.push_back(a);
    }

    // Delta tiles
    const auto& current = worldMap_.tiles();
    for (size_t i = 0; i < current.size() && i < prevTileState_.size(); ++i) {
        if (current[i].owner   != prevTileState_[i].owner   ||
            current[i].terrain != prevTileState_[i].terrain ||
            current[i].city    != prevTileState_[i].city) {
            snap.changedTiles.push_back(current[i]);
        }
    }
    prevTileState_ = current;

    // Events this turn
    auto turnEvents = timeline_.eventsForTurn(currentTurn_);
    for (const HistoryEvent* ev : turnEvents) {
        snap.newEvents.push_back(*ev);
    }

    // Relations
    for (const auto& [ka, row] : relations_) {
        for (const auto& [kb, rel] : row) {
            snap.relations.push_back(rel);
        }
    }

    if (serializer_) serializer_->serialize(snap);
}

void SimulationEngine::printTurnSummary() const {
    int alive = 0;
    for (const auto& [kid, k] : kingdoms_) if (k.isAlive) alive++;

    std::cout << "[Turn " << std::setw(4) << currentTurn_ << "] "
              << "Kingdoms: " << alive << "  "
              << "Events this turn: "
              << timeline_.eventsForTurn(currentTurn_).size()
              << "\n";

    // Print any notable events
    for (const HistoryEvent* ev : timeline_.eventsForTurn(currentTurn_)) {
        if (ev->type == EventType::KingdomCollapsed ||
            ev->type == EventType::ContinentUnified ||
            ev->type == EventType::CivilWar         ||
            ev->type == EventType::CapitalCaptured) {
            std::cout << "  *** " << ev->description << "\n";
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  季節サイクル
// ─────────────────────────────────────────────────────────────────────────────
void SimulationEngine::updateSeasonEffects() {
    namespace C = constants;

    // Detect season change
    Season cur  = currentSeason();
    Season prev = static_cast<Season>(((currentTurn_ - 1) / C::TURNS_PER_SEASON) % 4);
    if (currentTurn_ > 0 && cur != prev) {
        HistoryEvent ev;
        ev.type        = EventType::SeasonChanged;
        ev.turn        = currentTurn_;
        switch (cur) {
            case Season::Spring:
                ev.description = "Spring campaign season has begun: armies prepare invasions.";
                break;
            case Season::Summer:
                ev.description = "Summer battle season has begun: field battles become decisive.";
                break;
            case Season::Autumn:
                ev.description = "Autumn siege season has begun: strongholds come under pressure.";
                break;
            case Season::Winter:
                ev.description = "Winter quarters have begun: armies rest, resupply, and defend.";
                break;
        }
        ev.context["season"] = static_cast<float>(static_cast<uint8_t>(cur));
        eventBus_.emit(std::move(ev));
        eventBus_.flush();
    }

    SeasonModifiers mods = seasonMods(cur);

    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        // Morale drift from season
        k.morale = std::clamp(k.morale + mods.moraleDelta, 0.0f, 1.0f);
        // Winter: apply food penalty now (EconomyEngine multiplied already by bonus;
        // we subtract a winter food tax per city to simulate poor harvests)
        if (cur == Season::Winter && !k.cities.empty()) {
            float coldTax = k.cities.size() * 3.0f;
            k.treasury.food = std::max(0.0f, k.treasury.food - coldTax);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  山賊・略奪者
// ─────────────────────────────────────────────────────────────────────────────
void SimulationEngine::updateBandits() {
    namespace C = constants;

    // Spawn new bandit group
    if (currentTurn_ % C::BANDIT_SPAWN_INTERVAL == 0 &&
        static_cast<int>(bandits_.size()) < C::MAX_BANDIT_GROUPS) {

        // Pick a random unclaimed, non-ocean tile
        std::vector<TileID> candidates;
        for (const auto& t : worldMap_.tiles()) {
            if (t.owner == NO_KINGDOM &&
                t.terrain != TerrainType::Ocean &&
                t.terrain != TerrainType::Lake  &&
                t.city == NO_CITY) {
                candidates.push_back(t.id);
            }
        }
        if (!candidates.empty()) {
            TileID spawnTile = candidates[rng_.nextInt(0, static_cast<int>(candidates.size()) - 1)];
            BanditGroup bg;
            bg.id       = nextBanditID_++;
            bg.tile     = spawnTile;
            bg.strength = rng_.nextInt(120, 350);
            bg.moveCd   = 0;
            bg.raidCd   = 0;
            bandits_.push_back(bg);
        }
    }

    // Update each bandit group
    std::vector<BanditGroup> survivors;
    for (auto& bg : bandits_) {
        if (bg.strength == 0) continue;

        // Check if a kingdom army is on the same tile — fight & destroy bandit
        if (worldMap_.at(bg.tile).army != NO_ARMY) {
            ArmyID aid = worldMap_.at(bg.tile).army;
            auto ait = armies_.find(aid);
            if (ait != armies_.end()) {
                Army& army = ait->second;
                // Bandits deal some casualties
                uint32_t dmg = std::min(static_cast<uint32_t>(bg.strength / 4), army.totalSoldiers() / 10);
                for (auto& u : army.units) {
                    uint32_t ud = std::min(u.soldiers, dmg / static_cast<uint32_t>(army.units.size() + 1));
                    u.soldiers -= ud;
                }
            }
            bg.strength = 0;  // bandits destroyed
            continue;
        }

        // Move toward nearest city
        if (bg.moveCd > 0) { --bg.moveCd; survivors.push_back(bg); continue; }

        CityID nearest = NO_CITY;
        float bestDist = 1e9f;
        auto bgPos = worldMap_.at(bg.tile).position;
        for (const auto& [cid, city] : cities_) {
            if (city.isRuined) continue;
            auto cp = city.position;
            float d = std::hypot(float(bgPos.x - cp.x), float(bgPos.y - cp.y));
            if (d < bestDist) { bestDist = d; nearest = cid; }
        }

        if (nearest == NO_CITY) { survivors.push_back(bg); continue; }

        const auto& target = cities_.at(nearest);
        if (bestDist <= 1.5f) {
            // Raid the city
            if (bg.raidCd == 0) {
                KingdomID owner = target.owner;
                if (owner != NO_KINGDOM && kingdoms_.count(owner) && kingdoms_.at(owner).isAlive) {
                    kingdoms_.at(owner).treasury.gold = std::max(0.0f,
                        kingdoms_.at(owner).treasury.gold - C::BANDIT_RAID_GOLD);
                    kingdoms_.at(owner).treasury.food = std::max(0.0f,
                        kingdoms_.at(owner).treasury.food - C::BANDIT_RAID_FOOD);
                    kingdoms_.at(owner).morale = std::max(0.0f,
                        kingdoms_.at(owner).morale - 0.02f);

                    HistoryEvent ev;
                    ev.type           = EventType::BanditRaid;
                    ev.turn           = currentTurn_;
                    ev.primaryKingdom = owner;
                    ev.relatedCity    = nearest;
                    ev.description    = "Bandits raided " + cities_.at(nearest).name +
                                        " in " + kingdoms_.at(owner).name + "!";
                    eventBus_.emit(std::move(ev));
                    eventBus_.flush();
                }
                bg.raidCd = 8;
                // After raiding, bandits lose some strength (garrison resistance)
                bg.strength = static_cast<uint32_t>(bg.strength * 0.85f);
            } else {
                --bg.raidCd;
            }
        } else {
            // Move one tile toward target
            TileID bnbs[4]; int bnCount = worldMap_.neighbors4(bg.tile, bnbs);
            TileID bestTile = bg.tile;
            float bestD2 = bestDist;
            for (int bni = 0; bni < bnCount; ++bni) {
                TileID nid = bnbs[bni];
                const auto& nt = worldMap_.at(nid);
                if (nt.terrain == TerrainType::Ocean || nt.terrain == TerrainType::Lake) continue;
                float dx = float(nt.position.x - target.position.x);
                float dy = float(nt.position.y - target.position.y);
                float d2 = dx*dx + dy*dy;
                if (d2 < bestD2) { bestD2 = d2; bestTile = nid; }
            }
            bg.tile   = bestTile;
            bg.moveCd = 3;  // move every 4 turns
        }

        if (bg.strength > 0) survivors.push_back(bg);
    }
    bandits_ = std::move(survivors);
}

// ─────────────────────────────────────────────────────────────────────────────
//  疫病の伝播
// ─────────────────────────────────────────────────────────────────────────────
void SimulationEngine::updatePlaguePropagation() {
    // Existing plague: apply effects and countdown
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive || k.plagueTurns <= 0) continue;

        // Plague kills population each turn
        for (CityID cid : k.cities) {
            if (!cities_.count(cid)) continue;
            City& c = cities_.at(cid);
            uint32_t loss = static_cast<uint32_t>(c.population * 0.012f);
            c.population  = c.population > loss ? c.population - loss : std::max(1u, c.population / 2);
            c.happiness   = std::max(0.0f, c.happiness - 0.02f);
        }
        k.morale    = std::max(0.0f, k.morale    - 0.008f);
        k.stability = std::max(0.0f, k.stability - 0.004f);
        k.plagueTurns--;

        if (k.plagueTurns == 0) {
            k.plagueImmune = true;  // survived — immune for a while
        }
    }

    // Reset immunity after 60 turns
    for (auto& [kid, k] : kingdoms_) {
        if (k.plagueImmune && k.plagueTurns <= -60) k.plagueImmune = false;
        if (k.plagueTurns < 0) k.plagueTurns--;
    }

    // Spread: plagued kingdoms infect neighbors through shared borders
    std::vector<KingdomID> toInfect;
    for (const auto& [kid, k] : kingdoms_) {
        if (!k.isAlive || k.plagueTurns <= 0) continue;

        // Check all neighbors of this kingdom's cities
        for (CityID cid : k.cities) {
            if (!cities_.count(cid)) continue;
            auto pos = cities_.at(cid).position;

            for (auto& [nkid, nk] : kingdoms_) {
                if (!nk.isAlive || nkid == kid || nk.plagueTurns > 0 || nk.plagueImmune) continue;
                // Check if any of nk's cities are close
                for (CityID ncid : nk.cities) {
                    if (!cities_.count(ncid)) continue;
                    auto np = cities_.at(ncid).position;
                    float d = std::hypot(float(pos.x - np.x), float(pos.y - np.y));
                    if (d < 22.0f && rng_.chance(0.06f)) {
                        toInfect.push_back(nkid);
                    }
                }
            }
        }
    }

    for (KingdomID kid : toInfect) {
        if (!kingdoms_.count(kid)) continue;
        auto& k = kingdoms_.at(kid);
        if (k.plagueTurns <= 0 && !k.plagueImmune) {
            k.plagueTurns = rng_.nextInt(15, 30);

            HistoryEvent ev;
            ev.type           = EventType::PlagueSpread;
            ev.turn           = currentTurn_;
            ev.primaryKingdom = kid;
            ev.description    = "Plague has spread to " + k.name + "!";
            eventBus_.emit(std::move(ev));
        }
    }
    eventBus_.flush();
}

// ─────────────────────────────────────────────────────────────────────────────
//  AIの都市建設
// ─────────────────────────────────────────────────────────────────────────────
bool SimulationEngine::canFoundCity(const Kingdom& k, TileID tile) const {
    if (tile == NO_TILE || tile >= static_cast<TileID>(worldMap_.tileCount())) return false;
    const auto& t = worldMap_.at(tile);

    // Expansionist/Opportunistic: settler culture — can found on unclaimed or own territory
    const bool isSettler = (k.personality == KingdomPersonality::Expansionist ||
                            k.personality == KingdomPersonality::Opportunistic);
    if (isSettler) {
        // Allow own territory OR unclaimed land (frontier expansion)
        if (t.owner != k.id && t.owner != NO_KINGDOM) return false;
    } else {
        if (t.owner != k.id) return false;
    }

    if (t.city  != NO_CITY) return false;
    if (t.terrain == TerrainType::Ocean    ||
        t.terrain == TerrainType::Lake     ||
        t.terrain == TerrainType::Mountain ||
        t.terrain == TerrainType::Coast)   return false;
    // Must be at least 16 tiles from any existing city
    for (const auto& [cid, city] : cities_) {
        float d = std::hypot(float(t.position.x - city.position.x),
                             float(t.position.y - city.position.y));
        if (d < 16.0f) return false;
    }
    return true;
}

void SimulationEngine::runAICityBuilding() {
    namespace C = constants;

    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        // Diplomatic kingdoms don't found cities — they grow through alliances and diplomacy
        // Their path to victory is CONQUEST and inheritance, not settler expansion
        if (k.personality == KingdomPersonality::Diplomatic) continue;
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

        // Find best buildable tile
        TileID best = NO_TILE;
        float  bestScore = 0.0f;
        for (const auto& t : worldMap_.tiles()) {
            if (!canFoundCity(k, t.id)) continue;
            float score = t.fertility * 1.5f + t.resourceRichness;
            if (t.hasRiver) score += 1.0f;
            if (t.terrain == TerrainType::Hill)   score += 0.5f;
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
        city.fortification = (isExpansionist || isOpportunistic) ? 0.22f : 0.08f;
        city.buildings.push_back({BuildingType::Farm, 1, 1.0f});

        CityID cid = nextCityID_++;
        city.id = cid;
        worldMap_.at(best).city = cid;
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
void SimulationEngine::recordHistorySnapshot() {
    for (const auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        KingdomSnapshot snap;
        snap.pop    = k.totalPopulation;
        snap.gold   = k.treasury.gold;
        snap.armies = static_cast<int>(k.armies.size());
        snap.cities = static_cast<int>(k.cities.size());
        history_[kid].emplace_back(currentTurn_, snap);

        // Keep only the last 300 snapshots per kingdom
        auto& vec = history_[kid];
        if (vec.size() > 300) vec.erase(vec.begin());
    }
}

// ── Ruler / Dynasty system ────────────────────────────────────────────────────

static const char* RULER_FIRST[] = {
    "Aldric","Bram","Cedric","Doran","Edric","Faeron","Gareth","Halvard",
    "Idris","Jareth","Kael","Lorcan","Maekar","Neron","Osric","Prynn",
    "Quillon","Rhaest","Soren","Taran","Ulric","Vael","Wulfric","Xander",
    "Ysera","Zoran","Aelindra","Brynn","Cassian","Dwyn",
};
static const char* RULER_DYNASTY[] = {
    "Ironwood","Greymantle","Thornwall","Dawnbreak","Emberveil","Stormcrest",
    "Coldwater","Blackthorn","Ashvale","Rivenmoor","Goldenspire","Silentkeep",
    "Wraithborn","Oakenshield","Frostmere","Scalden","Alderpeak","Veldthorn",
};
static const char* HEIR_FIRST[] = {
    "Aryn","Berit","Calan","Dael","Eiran","Fynn","Gwen","Hael",
    "Irys","Joren","Kira","Lysen","Mira","Nael","Owyn","Pyrra",
};

Ruler SimulationEngine::generateRuler(TurnNumber reignStart, bool isHeir) {
    Ruler r;
    int ni = static_cast<int>(rng_.nextInt(0, 29));
    int di = static_cast<int>(rng_.nextInt(0, 17));
    r.name        = std::string(RULER_FIRST[ni]) + " " + RULER_DYNASTY[di];
    r.dynastyName = RULER_DYNASTY[di];
    r.age         = isHeir ? static_cast<uint8_t>(rng_.nextInt(16, 28))
                           : static_cast<uint8_t>(rng_.nextInt(25, 50));
    r.reignStart  = reignStart;

    // Assign 1-3 random traits (no duplicates, no contradictory pairs)
    std::vector<RulerTrait> pool = {
        RulerTrait::Brave, RulerTrait::Cowardly, RulerTrait::Greedy,
        RulerTrait::Generous, RulerTrait::Wise, RulerTrait::Foolish,
        RulerTrait::Ambitious, RulerTrait::Diplomatic, RulerTrait::Cruel, RulerTrait::Pious
    };
    int numTraits = static_cast<int>(rng_.nextInt(1, 3));
    for (int i = 0; i < numTraits; ++i) {
        if (pool.empty()) break;
        int idx = static_cast<int>(rng_.nextInt(0, static_cast<uint32_t>(pool.size() - 1)));
        RulerTrait picked = pool[idx];
        pool.erase(pool.begin() + idx);
        // Remove contradictory trait
        RulerTrait opposite = picked;
        switch (picked) {
            case RulerTrait::Brave:      opposite = RulerTrait::Cowardly;   break;
            case RulerTrait::Cowardly:   opposite = RulerTrait::Brave;      break;
            case RulerTrait::Greedy:     opposite = RulerTrait::Generous;   break;
            case RulerTrait::Generous:   opposite = RulerTrait::Greedy;     break;
            case RulerTrait::Wise:       opposite = RulerTrait::Foolish;    break;
            case RulerTrait::Foolish:    opposite = RulerTrait::Wise;       break;
            case RulerTrait::Diplomatic: opposite = RulerTrait::Cruel;      break;
            case RulerTrait::Cruel:      opposite = RulerTrait::Diplomatic; break;
            default: break;
        }
        pool.erase(std::remove(pool.begin(), pool.end(), opposite), pool.end());
        r.traits.push_back(picked);
    }

    // 60% chance of having an heir
    // Precompute one-time multipliers from traits
    for (RulerTrait t : r.traits) {
        switch (t) {
            case RulerTrait::Brave:      r.combatMult *= 1.22f; break;  // +22% combat
            case RulerTrait::Cowardly:   r.combatMult *= 0.80f; break;  // -20% combat
            case RulerTrait::Greedy:     r.goldMult   *= 1.28f; break;  // +28% gold
            case RulerTrait::Generous:   r.goldMult   *= 0.88f; break;  // -12% gold
            case RulerTrait::Cruel:      r.combatMult *= 1.10f; break;  // +10% combat
            case RulerTrait::Diplomatic: r.goldMult   *= 1.12f; break;  // +12% gold (trade)
            default: break;
        }
    }

    r.hasHeir = rng_.chance(0.60f);
    if (r.hasHeir) {
        int hi = static_cast<int>(rng_.nextInt(0, 15));
        r.heirName = std::string(HEIR_FIRST[hi]) + " " + r.dynastyName;
        r.heirAge  = static_cast<uint8_t>(rng_.nextInt(8, 22));
    }
    return r;
}

void SimulationEngine::assignInitialRulers() {
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        k.ruler    = generateRuler(0);
        k.hasRuler = true;
    }
}

void SimulationEngine::updateRulerAging() {
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive || !k.hasRuler) continue;
        if (k.ruler.age < 255) k.ruler.age++;
        if (k.ruler.hasHeir && k.ruler.heirAge < 255) k.ruler.heirAge++;
        // Heir comes of age at 16 — publicly announced
        if (k.ruler.hasHeir && k.ruler.heirAge == 16) {
            HistoryEvent ev;
            ev.type           = EventType::WorldEventPositive;
            ev.turn           = currentTurn_;
            ev.primaryKingdom = kid;
            ev.description    = k.ruler.heirName + " of " + k.name +
                " has come of age and is declared heir to the throne.";
            eventBus_.emit(std::move(ev));
        }
    }
}

void SimulationEngine::applyRulerTraits() {
    // combatMult / goldMult are stored on Ruler and applied in BattleEngine /
    // EconomyEngine directly — NOT here — to avoid per-turn accumulation bug.
    // Only per-turn drift effects (stability, legitimacy, aggression) go here.
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive || !k.hasRuler) continue;
        for (RulerTrait t : k.ruler.traits) {
            switch (t) {
                case RulerTrait::Greedy:
                    k.stability  = std::max(0.0f, k.stability  - 0.002f); break;
                case RulerTrait::Generous:
                    k.stability  = std::min(1.0f, k.stability  + 0.002f); break;
                case RulerTrait::Wise:
                    k.stability  = std::min(1.0f, k.stability  + 0.003f);
                    k.researchProgress += 0.05f; break;
                case RulerTrait::Foolish:
                    k.stability  = std::max(0.0f, k.stability  - 0.003f); break;
                case RulerTrait::Ambitious:
                    k.aggression = std::min(1.0f, k.aggression + 0.003f); break;
                case RulerTrait::Pious:
                    k.legitimacy = std::min(1.0f, k.legitimacy + 0.003f); break;
                default: break;
            }
        }
    }
}

// ── Nomad Horde ───────────────────────────────────────────────────────────────

void SimulationEngine::updateNomadHorde() {
    // Spawn first time at turn 100-200; re-spawn 200 turns after dissolution
    const TurnNumber spawnWindow = (horde_.spawnTurn == 0) ? 100
                                  : horde_.spawnTurn + 200;
    if (!hordeSpawned_ && currentTurn_ >= spawnWindow) {
        TurnNumber spawnAt = spawnWindow + static_cast<TurnNumber>(rng_.nextInt(0, 50));
        if (currentTurn_ >= spawnAt) {
            hordeSpawned_ = true;
            horde_.active    = true;
            // Scale with the largest kingdom's army to always be a credible threat
            uint32_t maxArmy = 0;
            for (const auto& [aid, army] : armies_) maxArmy = std::max(maxArmy, army.totalSoldiers());
            uint32_t baseStr = std::max(6000u, maxArmy * 2u);
            horde_.strength  = baseStr + static_cast<uint32_t>(rng_.nextInt(0, 3000));
            horde_.spawnTurn = currentTurn_;
            horde_.moveCd    = 0;
            horde_.raidCd    = 0;
            horde_.turnsActive = 0;

            // Spawn at a random map edge
            const int W = static_cast<int>(worldMap_.width());
            const int H = static_cast<int>(worldMap_.height());
            int edge = static_cast<int>(rng_.nextInt(0, 3));
            int ex, ey;
            switch (edge) {
                case 0: ex = static_cast<int>(rng_.nextInt(0, W-1)); ey = 0;   break;
                case 1: ex = static_cast<int>(rng_.nextInt(0, W-1)); ey = H-1; break;
                case 2: ex = 0;   ey = static_cast<int>(rng_.nextInt(0, H-1)); break;
                default:ex = W-1; ey = static_cast<int>(rng_.nextInt(0, H-1)); break;
            }
            // Find a land tile near that edge point
            horde_.tile = NO_TILE;
            for (int r2 = 0; r2 < 10 && horde_.tile == NO_TILE; ++r2) {
                for (int dx = -r2; dx <= r2 && horde_.tile == NO_TILE; ++dx) {
                    int nx = std::clamp(ex + dx, 0, W-1);
                    int ny = std::clamp(ey + (r2 - std::abs(dx)), 0, H-1);
                    TileID t = static_cast<TileID>(ny * W + nx);
                    auto tt = worldMap_.at(t).terrain;
                    if (tt != TerrainType::Ocean && tt != TerrainType::Lake) horde_.tile = t;
                }
            }
            if (horde_.tile == NO_TILE) horde_.tile = 0;

            HistoryEvent ev;
            ev.type           = EventType::WorldEventNegative;
            ev.turn           = currentTurn_;
            ev.primaryKingdom = NO_KINGDOM;
            ev.description    = "A great nomadic horde has appeared on the horizon! "
                                "All kingdoms are threatened!";
            eventBus_.emit(std::move(ev));
            return;
        }
    }

    if (!horde_.active) return;

    horde_.turnsActive++;
    // Horde dissolves after 180 turns or when too weak; re-emerges after 200 turns
    if (horde_.turnsActive > 180 || horde_.strength < 300) {
        horde_.active = false;
        horde_.spawnTurn = currentTurn_;  // re-emerge after ~200 turns
        hordeSpawned_ = false;            // allow re-spawn
        HistoryEvent ev;
        ev.type        = EventType::WorldEventPositive;
        ev.turn        = currentTurn_;
        ev.primaryKingdom = NO_KINGDOM;
        ev.description = "The nomadic horde has been driven off. But they may return.";
        eventBus_.emit(std::move(ev));
        return;
    }

    const int W = static_cast<int>(worldMap_.width());

    // ── Movement: pick target (most populated city within range) ────────────
    if (horde_.moveCd > 0) { horde_.moveCd--; }
    else {
        // Find target city
        if (horde_.target == NO_CITY || !cities_.count(horde_.target) ||
            cities_.at(horde_.target).owner == NO_KINGDOM) {
            horde_.target = NO_CITY;
            uint32_t bestPop = 0;
            for (const auto& [cid, c] : cities_) {
                if (c.owner == NO_KINGDOM || !kingdoms_.count(c.owner)) continue;
                if (!kingdoms_.at(c.owner).isAlive) continue;
                if (c.population > bestPop) { bestPop = c.population; horde_.target = cid; }
            }
        }

        if (horde_.target != NO_CITY && cities_.count(horde_.target)) {
            const auto& tc = cities_.at(horde_.target);
            int tx = tc.position.x, ty = tc.position.y;
            int hx = static_cast<int>(horde_.tile) % W;
            int hy = static_cast<int>(horde_.tile) / W;

            // Move 3-5 tiles toward target
            int steps = 3 + static_cast<int>(rng_.nextInt(0, 2));
            for (int s = 0; s < steps; ++s) {
                int ndx = (tx > hx) ? 1 : (tx < hx) ? -1 : 0;
                int ndy = (ty > hy) ? 1 : (ty < hy) ? -1 : 0;
                int nx = hx + (ndx != 0 ? ndx : (rng_.chance(0.5f) ? 1 : -1));
                int ny = hy + (ndy != 0 ? ndy : (rng_.chance(0.5f) ? 1 : -1));
                nx = std::clamp(nx, 0, W - 1);
                ny = std::clamp(ny, 0, static_cast<int>(worldMap_.height()) - 1);
                TileID nt = static_cast<TileID>(ny * W + nx);
                auto tt = worldMap_.at(nt).terrain;
                if (tt != TerrainType::Ocean && tt != TerrainType::Lake) {
                    horde_.tile = nt; hx = nx; hy = ny;
                }
            }
        }
        horde_.moveCd = 2;
    }

    // ── Raid nearby cities ────────────────────────────────────────────────────
    if (horde_.raidCd > 0) { horde_.raidCd--; return; }

    int hx = static_cast<int>(horde_.tile) % W;
    int hy = static_cast<int>(horde_.tile) / W;

    for (auto& [cid, city] : cities_) {
        if (city.owner == NO_KINGDOM || !kingdoms_.count(city.owner)) continue;
        if (!kingdoms_.at(city.owner).isAlive) continue;

        int cx = city.position.x, cy = city.position.y;
        int dist = std::abs(cx - hx) + std::abs(cy - hy);
        if (dist > 6) continue;

        // Check if any army defends the city
        bool defended = false;
        if (city.tile != NO_TILE && city.tile < static_cast<TileID>(worldMap_.tileCount())) {
            ArmyID defender = worldMap_.at(city.tile).army;
            if (defender != NO_ARMY && armies_.count(defender)) {
                const Army& da = armies_.at(defender);
                if (da.owner == city.owner) {
                    // Horde vs army combat
                    float hordeStr  = static_cast<float>(horde_.strength) * 0.8f;
                    float armyStr   = da.combatStrength(worldMap_.at(city.tile).terrain, true);
                    if (hordeStr > armyStr) {
                        horde_.strength -= static_cast<uint32_t>(armyStr * 0.3f);
                        // Destroy defending army
                        for (auto& u : armies_.at(defender).units) u.soldiers = 0;
                    } else {
                        horde_.strength -= static_cast<uint32_t>(hordeStr * 0.5f);
                        defended = true;
                    }
                }
            }
        }
        if (defended) continue;

        // Raid! Loot gold, food, reduce population
        auto& k = kingdoms_.at(city.owner);
        float loot = std::min(k.treasury.gold, 200.0f + rng_.nextFloat(0.f, 200.f));
        k.treasury.gold  = std::max(0.0f, k.treasury.gold  - loot);
        k.treasury.food  = std::max(0.0f, k.treasury.food  - 100.0f);
        city.population  = static_cast<uint32_t>(city.population * 0.85f);
        city.happiness   = std::max(0.0f, city.happiness   - 0.25f);
        city.fortification = std::max(0.0f, city.fortification - 0.10f);
        horde_.strength  -= static_cast<uint32_t>(rng_.nextFloat(100.f, 300.f));

        // Clear target so horde picks a new one
        horde_.target = NO_CITY;

        HistoryEvent ev;
        ev.type           = EventType::WorldEventNegative;
        ev.turn           = currentTurn_;
        ev.primaryKingdom = city.owner;
        ev.description    = "The nomadic horde raided " + city.name + "! Gold stolen: " +
                            std::to_string(static_cast<int>(loot));
        eventBus_.emit(std::move(ev));
        horde_.raidCd = 4;
        break; // one raid per update
    }
}

// ── Culture system ────────────────────────────────────────────────────────────

void SimulationEngine::assignCultureGroups() {
    // Divide the map into 8 cultural regions based on capital position
    // Use quadrant + half to get 8 zones
    constexpr int GROUPS = 8;
    constexpr float MAP_SIZE = 257.0f;
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive || k.capitalCity == NO_CITY) continue;
        if (!cities_.count(k.capitalCity)) continue;
        const auto& cap = cities_.at(k.capitalCity);
        float nx = static_cast<float>(cap.position.x) / MAP_SIZE; // 0-1
        float ny = static_cast<float>(cap.position.y) / MAP_SIZE;
        int qx = (nx < 0.5f) ? 0 : 1;
        int qy = (ny < 0.5f) ? 0 : 1;
        int half = (nx < 0.25f || (nx >= 0.5f && nx < 0.75f)) ? 0 : 1;
        k.cultureGroup = static_cast<uint8_t>((qy * 2 + qx) * 2 + half) % GROUPS;
    }
    // Propagate culture group to each kingdom's owned cities
    for (auto& [cid, c] : cities_) {
        if (c.owner != NO_KINGDOM && kingdoms_.count(c.owner)) {
            c.cultureOwner        = c.owner;
            c.cultureAssimilation = 1.0f;
        }
    }
}

void SimulationEngine::updateCultureAssimilation() {
    for (auto& [cid, c] : cities_) {
        if (c.owner == NO_KINGDOM) continue;
        if (c.cultureOwner == c.owner) {
            // Native culture: maintain full assimilation
            c.cultureAssimilation = 1.0f;
            continue;
        }
        // Foreign culture: tick toward assimilation
        // Opportunistic kingdoms: fast-talking administrators integrate cities quickly
        float assimRate = 0.02f;
        auto kit = kingdoms_.find(c.owner);
        if (kit != kingdoms_.end() && kit->second.isAlive &&
            kit->second.personality == KingdomPersonality::Opportunistic)
            assimRate = 0.038f;  // ~2× faster integration
        c.cultureAssimilation = std::min(1.0f, c.cultureAssimilation + assimRate);
        if (c.cultureAssimilation >= 1.0f) {
            c.cultureOwner = c.owner; // fully assimilated
        }
    }
}

// ── Navy ─────────────────────────────────────────────────────────────────────

bool SimulationEngine::isCoastalTile(TileID tile) const {
    if (tile == NO_TILE || tile >= static_cast<TileID>(worldMap_.tileCount())) return false;
    auto t = worldMap_.at(tile).terrain;
    return t == TerrainType::Coast || t == TerrainType::Ocean;
}

TileID SimulationEngine::findAdjacentCoast(TileID tile) const {
    if (tile == NO_TILE || tile >= static_cast<TileID>(worldMap_.tileCount())) return NO_TILE;
    const int W = static_cast<int>(worldMap_.width());
    const int x = static_cast<int>(tile) % W;
    const int y = static_cast<int>(tile) / W;
    const int dx[] = {0, 0, -1, 1};
    const int dy[] = {-1, 1, 0, 0};
    for (int i = 0; i < 4; ++i) {
        int nx = x + dx[i], ny = y + dy[i];
        if (nx < 0 || ny < 0 || nx >= W || ny >= static_cast<int>(worldMap_.height())) continue;
        TileID adj = static_cast<TileID>(ny * W + nx);
        if (isCoastalTile(adj)) return adj;
    }
    return NO_TILE;
}

FleetID SimulationEngine::buildFleet(KingdomID owner, CityID portCity) {
    if (!cities_.count(portCity)) return NO_FLEET;
    const auto& city = cities_.at(portCity);
    TileID coastTile = findAdjacentCoast(city.tile);
    if (coastTile == NO_TILE) {
        coastTile = city.tile; // fallback: place at city tile
    }
    Fleet f;
    f.id       = nextFleetID_++;
    f.owner    = owner;
    f.tile     = coastTile;
    f.homePort = portCity;
    f.inPort   = true;
    f.hull     = 1000;
    fleets_.emplace(f.id, f);
    return f.id;
}

void SimulationEngine::updateNavy() {
    constexpr float FLEET_BUILD_GOLD = 200.0f;
    constexpr float FLEET_BUILD_WOOD = 150.0f;
    constexpr float FLEET_UPKEEP     = 4.0f;    // gold per turn
    constexpr int   FLEET_MOVE_CD    = 2;        // turns between moves
    constexpr int   FLEET_MOVE_RANGE = 4;        // coast tiles per move action

    // ── 1. Build fleets at Port cities (AI decision) ─────────────────────────
    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        // Count existing fleets
        int ownFleets = 0;
        for (const auto& [fid, fl] : fleets_) if (fl.owner == kid) ownFleets++;

        // Port city kingdoms can build 1 fleet per 2 Port cities
        int portCount = 0;
        for (CityID cid : k.cities) {
            if (cities_.count(cid) && cities_.at(cid).cityType == CityType::Port) portCount++;
        }
        int maxFleets = portCount / 2 + (portCount > 0 ? 1 : 0);
        if (portCount == 0 || ownFleets >= maxFleets) continue;
        if (k.treasury.gold < FLEET_BUILD_GOLD || k.treasury.wood < FLEET_BUILD_WOOD) continue;
        if (!rng_.chance(0.08f)) continue;

        // Pick a Port city without an assigned fleet
        for (CityID cid : k.cities) {
            if (!cities_.count(cid) || cities_.at(cid).cityType != CityType::Port) continue;
            bool hasFleet = false;
            for (const auto& [fid, fl] : fleets_)
                if (fl.homePort == cid) { hasFleet = true; break; }
            if (hasFleet) continue;

            k.treasury.gold -= FLEET_BUILD_GOLD;
            k.treasury.wood -= FLEET_BUILD_WOOD;
            FleetID fid = buildFleet(kid, cid);
            if (fid == NO_FLEET) continue;

            HistoryEvent ev;
            ev.type           = EventType::WorldEventPositive;
            ev.turn           = currentTurn_;
            ev.primaryKingdom = kid;
            ev.description    = k.name + " launched a fleet from " + cities_.at(cid).name + ".";
            eventBus_.emit(std::move(ev));
            break;
        }
    }

    // ── 2. Upkeep & sink fleets of dead kingdoms ────────────────────────────
    std::vector<FleetID> toSink;
    for (auto& [fid, fl] : fleets_) {
        if (!kingdoms_.count(fl.owner) || !kingdoms_.at(fl.owner).isAlive) {
            toSink.push_back(fid);
            continue;
        }
        kingdoms_.at(fl.owner).treasury.gold -= FLEET_UPKEEP;
    }
    for (FleetID fid : toSink) fleets_.erase(fid);

    // ── 3. Sink fleets whose home port was captured ─────────────────────────
    toSink.clear();
    for (auto& [fid, fl] : fleets_) {
        auto cit = cities_.find(fl.homePort);
        if (fl.homePort == NO_CITY || cit == cities_.end()) continue;
        if (cit->second.owner != fl.owner) toSink.push_back(fid);
    }
    for (FleetID fid : toSink) {
        auto fit = fleets_.find(fid);
        if (fit == fleets_.end()) continue;
        // Disembark cargo if any
        if (fit->second.cargo != NO_ARMY) {
            ArmyID aid = fit->second.cargo;
            auto ait = armies_.find(aid);
            if (ait != armies_.end()) {
                ait->second.currentTile = fit->second.tile;
                ait->second.isBesieging = false;
            }
        }
        fleets_.erase(fit);
    }

    // ── 4. Move fleets (basic AI: find enemy coast, transport army) ──────────
    const int W = static_cast<int>(worldMap_.width());
    for (auto& [fid, fl] : fleets_) {
        if (fl.moveCd > 0) { fl.moveCd--; continue; }
        Kingdom& k = kingdoms_.at(fl.owner);

        // Find the active war enemy
        KingdomID enemy = NO_KINGDOM;
        for (const auto& [eid, ek] : kingdoms_) {
            if (!ek.isAlive || eid == fl.owner) continue;
            KingdomID lo = std::min(fl.owner, eid), hi = std::max(fl.owner, eid);
            if (relations_.count(lo) && relations_.at(lo).count(hi) &&
                relations_.at(lo).at(hi).state == RelationState::War) {
                enemy = eid;
                break;
            }
        }
        if (enemy == NO_KINGDOM) continue;

        // If empty, try to embark a nearby army
        if (fl.cargo == NO_ARMY) {
            for (ArmyID aid : k.armies) {
                auto ait2 = armies_.find(aid);
                if (ait2 == armies_.end()) continue;
                auto& army = ait2->second;
                TileID at = army.currentTile;
                if (at == NO_TILE || at >= static_cast<TileID>(worldMap_.tileCount())) continue;
                // Army must be adjacent to fleet tile
                int ax = static_cast<int>(at) % W, ay = static_cast<int>(at) / W;
                int fx = static_cast<int>(fl.tile) % W, fy = static_cast<int>(fl.tile) / W;
                int dist = std::abs(ax - fx) + std::abs(ay - fy);
                if (dist <= 2 && army.role == ArmyRole::Attack) {
                    fl.cargo = aid;
                    army.currentTile = fl.tile; // army moves onto fleet
                    fl.inPort = false;
                    break;
                }
            }
            if (fl.cargo == NO_ARMY) continue;
        }

        // Navigate toward nearest enemy coast tile
        TileID bestTile = NO_TILE;
        int bestScore = INT_MAX;
        for (const auto& [ecid, ec] : cities_) {
            if (ec.owner != enemy) continue;
            TileID coast = findAdjacentCoast(ec.tile);
            if (coast == NO_TILE) continue;
            int cx = static_cast<int>(coast) % W, cy = static_cast<int>(coast) / W;
            int fx = static_cast<int>(fl.tile) % W, fy = static_cast<int>(fl.tile) / W;
            int d = std::abs(cx - fx) + std::abs(cy - fy);
            if (d < bestScore) { bestScore = d; bestTile = coast; }
        }
        if (bestTile == NO_TILE) continue;

        // Move toward target (up to FLEET_MOVE_RANGE steps along coast/ocean)
        int tx = static_cast<int>(bestTile) % W, ty = static_cast<int>(bestTile) / W;
        int fx = static_cast<int>(fl.tile) % W, fy = static_cast<int>(fl.tile) / W;
        int steps = 0;
        for (; steps < FLEET_MOVE_RANGE && (fx != tx || fy != ty); ++steps) {
            int ndx = (tx > fx) ? 1 : (tx < fx) ? -1 : 0;
            int ndy = (ty > fy) ? 1 : (ty < fy) ? -1 : 0;
            // Prefer coast movement, try horizontal then vertical
            auto tryStep = [&](int ddx, int ddy) -> bool {
                int nx = fx + ddx, ny = fy + ddy;
                if (nx < 0 || ny < 0 || nx >= W || ny >= static_cast<int>(worldMap_.height())) return false;
                TileID nt = static_cast<TileID>(ny * W + nx);
                if (!isCoastalTile(nt)) return false;
                fx = nx; fy = ny;
                fl.tile = nt;
                return true;
            };
            if (!tryStep(ndx, ndy)) {
                if (!tryStep(ndx, 0)) tryStep(0, ndy);
            }
        }

        // Disembark army if adjacent to enemy city
        if (fl.cargo != NO_ARMY) {
            for (const auto& [ecid, ec] : cities_) {
                if (ec.owner != enemy) continue;
                int cx = static_cast<int>(ec.tile) % W, cy = static_cast<int>(ec.tile) / W;
                int dist = std::abs(cx - fx) + std::abs(cy - fy);
                if (dist <= 3) {
                    // Disembark
                    if (armies_.count(fl.cargo)) {
                        armies_.at(fl.cargo).currentTile = fl.tile;
                        armies_.at(fl.cargo).targetTile  = ec.tile;
                        armies_.at(fl.cargo).role        = ArmyRole::Attack;
                    }
                    fl.cargo  = NO_ARMY;
                    fl.inPort = false;

                    HistoryEvent ev;
                    ev.type           = EventType::WorldEventNegative;
                    ev.turn           = currentTurn_;
                    ev.primaryKingdom = fl.owner;
                    ev.description    = k.name + "'s fleet landed troops near " + ec.name + "!";
                    eventBus_.emit(std::move(ev));
                    break;
                }
            }
        }
        fl.moveCd = FLEET_MOVE_CD;
    }
}

} // namespace jke
