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
#include <limits>

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

void SimulationEngine::setInitialKingdoms(uint32_t count) {
    config_.initialKingdoms =
        std::clamp(count, 4u, static_cast<uint32_t>(constants::NUM_KINGDOMS));
}

void SimulationEngine::reset() {
    rng_ = Random(config_.seed);
    currentTurn_ = 0;
    simulationOver_ = false;

    worldMap_ = WorldMap(1, 1);
    kingdoms_.clear();
    cities_.clear();
    armies_.clear();
    techTree_ = TechTree();
    relations_.clear();
    pendingProposals_.clear();
    rebellions_.clear();
    civilWars_.clear();
    timeline_ = Timeline();

    nextKingdomID_ = 1;
    nextCityID_ = 1;
    nextArmyID_ = 1;
    nextUnitID_ = 1;

    prevTileState_.clear();
    bandits_.clear();
    nextBanditID_ = 1;
    history_.clear();
    fleets_.clear();
    nextFleetID_ = 1;
    horde_ = NomadHorde();
    hordeSpawned_ = false;
    invaded_.clear();
    weakTargetCache_.clear();
    weakTargetCacheTurn_ = ~0u;
    eventBus_.clearPending();
    aiStrategies_.clear();

    initializeWorld();
}

bool SimulationEngine::step() {
    if (simulationOver_ ||
        (config_.maxTurns != 0 && currentTurn_ >= config_.maxTurns)) {
        return false;
    }

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
        if (config_.maxTurns == 0)
            std::cout << "Max turns: unlimited (until unification)\n";
        else
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
    WorldGenerator gen(config_.seed, static_cast<int>(config_.initialKingdoms));
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

        // Claim 10 tiles outward from capital so city founding can work early
        if (k.capitalCity != NO_CITY && cities_.count(k.capitalCity)) {
            TileID capTile = cities_.at(k.capitalCity).tile;
            if (capTile != NO_TILE) {
                // BFS up to 10 steps from capital, claim land tiles
                std::vector<TileID> frontier = {capTile};
                std::unordered_set<TileID> visited = {capTile};
                for (int depth = 0; depth < 10 && !frontier.empty(); ++depth) {
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

        // Personality-flavored event descriptions
        const bool isAggressive    = k.personality == KingdomPersonality::Aggressive;
        const bool isDefensive     = k.personality == KingdomPersonality::Defensive;
        const bool isExpansionist  = k.personality == KingdomPersonality::Expansionist;
        const bool isEconomic      = k.personality == KingdomPersonality::Economic;
        const bool isDiplomatic    = k.personality == KingdomPersonality::Diplomatic;
        const bool isOpportunistic = k.personality == KingdomPersonality::Opportunistic;

        switch (roll) {
            case 0: { // Gold windfall
                float g = rng_.nextFloat(60.0f, 150.0f);
                k.treasury.gold += g;
                if (isAggressive)
                    evDesc = k.name + " seized a rival merchant's coffers — gold flows into the war chest.";
                else if (isExpansionist)
                    evDesc = k.name + " struck a lucrative deal with frontier settlers, filling its treasury.";
                else if (isEconomic)
                    evDesc = k.name + "'s trade networks paid off handsomely — a gold windfall from distant markets.";
                else if (isDiplomatic)
                    evDesc = k.name + " received generous gifts from allied kingdoms, strengthening bonds and coffers alike.";
                else if (isOpportunistic)
                    evDesc = k.name + " quietly intercepted a merchant caravan — gold acquired, questions unasked.";
                else
                    evDesc = k.name + " received an unexpected gold windfall.";
                positive = true;
                break;
            }
            case 1: { // Iron vein discovered
                float fe = rng_.nextFloat(30.0f, 80.0f);
                k.treasury.iron += fe;
                if (isAggressive)
                    evDesc = k.name + " struck iron deep in conquered territory — enough to arm a new legion.";
                else if (isDefensive)
                    evDesc = k.name + " unearthed an iron vein beneath its walls — fortifications will not want for metal.";
                else if (isEconomic)
                    evDesc = k.name + "'s miners uncovered a rich iron seam — smelters run day and night.";
                else if (isExpansionist)
                    evDesc = k.name + " discovered iron in newly claimed lands — the frontier pays its own way.";
                else
                    evDesc = k.name + " discovered a rich iron vein.";
                positive = true;
                break;
            }
            case 2: { // Drought
                k.treasury.food = std::max(0.0f, k.treasury.food - rng_.nextFloat(40.0f, 100.0f));
                k.starvationTurns++;
                if (isAggressive)
                    evDesc = k.name + "'s fields dried to dust — hungry soldiers grow restless and dangerous.";
                else if (isDiplomatic)
                    evDesc = k.name + " endures a bitter drought; the court appeals to allies for grain shipments.";
                else if (isEconomic)
                    evDesc = k.name + "'s harvest failed. Even wealthy merchants cannot conjure rain.";
                else if (isDefensive)
                    evDesc = k.name + "'s granaries shrink under drought — siege rations stretch thinner each week.";
                else
                    evDesc = k.name + " suffered a severe drought, threatening its food supply.";
                break;
            }
            case 3: { // Plague
                k.morale    = std::max(0.0f, k.morale    - rng_.nextFloat(0.1f, 0.25f));
                k.stability = std::max(0.0f, k.stability - rng_.nextFloat(0.05f, 0.15f));
                if (isAggressive)
                    evDesc = k.name + "'s armies carried plague home from campaign — it spreads through the barracks.";
                else if (isExpansionist)
                    evDesc = k.name + "'s settlers brought sickness from distant lands; the frontier towns suffer greatly.";
                else if (isOpportunistic)
                    evDesc = k.name + " cannot hide that plague walks its streets — rivals will smell the weakness.";
                else if (isDiplomatic)
                    evDesc = k.name + " closes its borders as plague takes hold, straining diplomatic ties.";
                else
                    evDesc = k.name + " was struck by a devastating plague, shaking morale and stability.";
                break;
            }
            case 4: { // Military revolt
                if (!k.armies.empty()) {
                    ArmyID victim = k.armies[rng_.nextInt(0, static_cast<int>(k.armies.size()) - 1)];
                    if (armies_.count(victim)) {
                        for (auto& u : armies_.at(victim).units)
                            u.soldiers = u.soldiers / 2;
                    }
                    if (isAggressive)
                        evDesc = k.name + "'s veterans mutinied — too many campaigns, not enough plunder.";
                    else if (isDefensive)
                        evDesc = k.name + "'s garrison turned on its officers, sick of endless walls and waiting.";
                    else if (isEconomic)
                        evDesc = k.name + "'s underpaid soldiers rebelled — even mercenaries have their limits.";
                    else
                        evDesc = k.name + "'s army suffered a dangerous internal revolt.";
                }
                break;
            }
            case 5: { // Trade boom
                k.treasury.gold += rng_.nextFloat(30.0f, 80.0f);
                k.treasury.gold *= 1.1f;
                if (isEconomic)
                    evDesc = k.name + "'s markets explode with activity — a trade boom that rivals celebrate with envy.";
                else if (isOpportunistic)
                    evDesc = k.name + " leveraged a regional shortage into a windfall — bought low, sold high.";
                else if (isDiplomatic)
                    evDesc = k.name + "'s alliance network opened new trade corridors — prosperity shared and kept.";
                else if (isExpansionist)
                    evDesc = k.name + "'s expanding roads carry merchant caravans — the empire pays for its own growth.";
                else
                    evDesc = k.name + " experienced a prosperous trade boom.";
                positive = true;
                break;
            }
            case 6: { // Diplomatic scandal
                k.stability = std::max(0.0f, k.stability - 0.08f);
                if (isDiplomatic)
                    evDesc = k.name + " reels from a diplomatic scandal — an ambassador's betrayal strains every alliance.";
                else if (isOpportunistic)
                    evDesc = k.name + "'s back-channel deals came to light — trust, once sold, is hard to buy back.";
                else if (isAggressive)
                    evDesc = k.name + "'s court was shaken by intrigue — generals quarrel while the kingdom watches.";
                else
                    evDesc = k.name + " was rocked by a diplomatic scandal, destabilizing the court.";
                break;
            }
            case 7: { // Fortification collapse
                if (!k.cities.empty()) {
                    CityID cid = k.cities[rng_.nextInt(0, static_cast<int>(k.cities.size()) - 1)];
                    if (cities_.count(cid)) {
                        cities_.at(cid).fortification = std::max(0.0f, cities_.at(cid).fortification - 0.3f);
                        if (isDefensive)
                            evDesc = k.name + "'s walls in " + cities_.at(cid).name + " crumbled — the very foundation of its strategy is shaken.";
                        else if (isAggressive)
                            evDesc = k.name + " neglected its defenses in " + cities_.at(cid).name + " while chasing enemies abroad; the walls pay the price.";
                        else
                            evDesc = k.name + "'s fortifications crumbled in " + cities_.at(cid).name + ".";
                    }
                }
                break;
            }
            case 8: { // Harvest festival — food & morale surge
                k.treasury.food += rng_.nextFloat(50.0f, 120.0f);
                k.morale = std::min(1.0f, k.morale + 0.08f);
                if (isAggressive)
                    evDesc = k.name + " feasts before the next campaign — soldiers march better on full stomachs.";
                else if (isDiplomatic)
                    evDesc = k.name + " holds harvest celebrations open to allied envoys — diplomacy over full tables.";
                else if (isExpansionist)
                    evDesc = k.name + "'s new farmlands yield their first great harvest — the frontier has proven its worth.";
                else
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
                    if (isAggressive)
                        evDesc = k.name + " forged a warlord in the crucible of battle — the army follows him without question.";
                    else if (isDefensive)
                        evDesc = k.name + " found a commander who reads terrain like scripture — its walls have never felt safer.";
                    else if (isExpansionist)
                        evDesc = k.name + " raised a general born for long campaigns — distance no longer frightens its armies.";
                    else if (isOpportunistic)
                        evDesc = k.name + " discovered a tactician who strikes before the enemy knows war has begun.";
                    else
                        evDesc = k.name + " produced a brilliant general, inspiring its armies.";
                    positive = true;
                }
                break;
            }
            case 10: { // Stability surge — religious revival or good harvest
                k.stability = std::min(1.0f, k.stability + 0.10f);
                k.morale    = std::min(1.0f, k.morale    + 0.05f);
                if (isAggressive)
                    evDesc = k.name + " rallies behind its banners — conquest gives the people purpose and pride.";
                else if (isDiplomatic)
                    evDesc = k.name + " celebrates a season of peace — alliances hold and the people breathe easy.";
                else if (isDefensive)
                    evDesc = k.name + "'s people find unity behind their walls — the threat outside draws them together.";
                else if (isExpansionist)
                    evDesc = k.name + " swells with pride as new provinces join the fold — the dream of empire unites the nation.";
                else if (isEconomic)
                    evDesc = k.name + " basks in prosperity — full markets and fair prices lift morale across every city.";
                else
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
                        if (isDefensive)
                            evDesc = k.name + " reinforced " + cities_.at(cid).name + " — mortar repointed, gates rehung, walls as good as new.";
                        else if (isEconomic)
                            evDesc = k.name + " invested in " + cities_.at(cid).name + "'s infrastructure — markets and workshops run at full capacity again.";
                        else if (isExpansionist)
                            evDesc = k.name + " upgraded " + cities_.at(cid).name + " to support the next push forward.";
                        else
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

    // 2.5 War momentum: militarist cultures get combat boost while fighting (applied after tech reset)
    {
        auto isWarring = [&](KingdomID a, KingdomID b) {
            auto lo = std::min(a,b), hi = std::max(a,b);
            auto it = relations_.find(lo);
            if (it == relations_.end()) return false;
            auto it2 = it->second.find(hi);
            return it2 != it->second.end() && it2->second.state == RelationState::War;
        };
        for (auto& [kid, k] : kingdoms_) {
            if (!k.isAlive) continue;
            const bool hasWarCulture =
                k.personality == KingdomPersonality::Aggressive ||
                k.personality == KingdomPersonality::Expansionist ||
                k.personality == KingdomPersonality::Opportunistic;
            if (!hasWarCulture) continue;
            bool fighting = false;
            for (const auto& [oid, ok] : kingdoms_) {
                if (oid == kid || !ok.isAlive) continue;
                if (isWarring(kid, oid)) { fighting = true; break; }
            }
            // combatBonus was just reset by TechEngine; add war momentum on top
            if (fighting) {
                float base = 0.08f;
                float wearScale = 0.32f;
                float cap = 1.34f;
                if (k.personality == KingdomPersonality::Aggressive) {
                    base = 0.12f;
                    wearScale = 0.50f;
                    cap = 1.50f;
                } else if (k.personality == KingdomPersonality::Expansionist) {
                    base = 0.10f;
                    wearScale = 0.38f;
                    cap = 1.42f;
                }
                k.combatBonus *= std::min(cap, 1.0f + k.warWeariness * wearScale + base);
                if (k.warWeariness > 0.72f) {
                    const float fatigue = std::clamp((k.warWeariness - 0.72f) / 0.28f, 0.0f, 1.0f);
                    k.combatBonus *= 1.0f - fatigue * 0.24f;
                    k.siegeBonus  *= 1.0f - fatigue * 0.18f;
                }
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

    if (currentTurn_ >= 2600 && currentTurn_ % 5 == 0) {
        std::vector<KingdomID> alive;
        alive.reserve(kingdoms_.size());
        for (const auto& [kid, k] : kingdoms_) {
            if (k.isAlive && !k.cities.empty()) alive.push_back(kid);
        }

        if (alive.size() > 1 && alive.size() <= 12) {
            KingdomID cleanupLeader = NO_KINGDOM;
            double bestLeaderScore = -1.0;
            for (KingdomID kid : alive) {
                const Kingdom& k = kingdoms_.at(kid);
                double score = static_cast<double>(k.cities.size()) * 1000.0 +
                               static_cast<double>(k.totalPopulation);
                if (score > bestLeaderScore) {
                    bestLeaderScore = score;
                    cleanupLeader = kid;
                }
            }

            auto livingArmyCount = [&](const Kingdom& k) {
                int count = 0;
                for (ArmyID aid : k.armies) {
                    auto it = armies_.find(aid);
                    if (it != armies_.end() && !it->second.isEmpty()) ++count;
                }
                return count;
            };

            auto tileDistance = [&](TileID a, TileID b) {
                const auto ap = worldMap_.at(a).position;
                const auto bp = worldMap_.at(b).position;
                return std::hypot(float(ap.x - bp.x), float(ap.y - bp.y));
            };

            auto areAtWar = [&](KingdomID a, KingdomID b) {
                const KingdomID lo = std::min(a, b);
                const KingdomID hi = std::max(a, b);
                auto it = relations_.find(lo);
                if (it == relations_.end()) return false;
                auto jt = it->second.find(hi);
                return jt != it->second.end() && jt->second.state == RelationState::War;
            };

            for (KingdomID attackerId : alive) {
                if (currentTurn_ >= 3000 && alive.size() <= 6 && attackerId != cleanupLeader) continue;
                Kingdom& attacker = kingdoms_.at(attackerId);
                if (attacker.capitalCity == NO_CITY || !cities_.count(attacker.capitalCity)) continue;
                const TileID attackerCapital = cities_.at(attacker.capitalCity).tile;
                if (attackerCapital == NO_TILE) continue;

                KingdomID targetId = NO_KINGDOM;
                float bestScore = 1e9f;
                for (KingdomID candidateId : alive) {
                    if (candidateId == attackerId) continue;
                    const Kingdom& candidate = kingdoms_.at(candidateId);
                    if (candidate.capitalCity == NO_CITY || !cities_.count(candidate.capitalCity)) continue;
                    const TileID targetTile = cities_.at(candidate.capitalCity).tile;
                    if (targetTile == NO_TILE) continue;

                    float score = tileDistance(attackerCapital, targetTile);
                    score += static_cast<float>(candidate.cities.size()) * 1.8f;
                    if (candidate.cities.size() <= 1) score -= 16.0f;
                    if (candidate.warWeariness > 0.75f) score -= 8.0f;
                    if (score < bestScore) {
                        bestScore = score;
                        targetId = candidateId;
                    }
                }
                if (targetId == NO_KINGDOM) continue;

                if (!areAtWar(attackerId, targetId)) {
                    AIDecision war;
                    war.type = AIDecisionType::DeclareWar;
                    war.actor = attackerId;
                    war.target = targetId;
                    executeDecision(war);
                }

                const int desiredArmies = std::clamp(static_cast<int>(attacker.cities.size()) / 3 + 3, 3, 10);
                int recruitAttempts = std::min(2, std::max(0, desiredArmies - livingArmyCount(attacker)));
                for (int i = 0; i < recruitAttempts; ++i) {
                    CityID recruitCity = attacker.capitalCity;
                    if (recruitCity == NO_CITY || !cities_.count(recruitCity) ||
                        cities_.at(recruitCity).owner != attackerId) {
                        recruitCity = NO_CITY;
                        for (CityID cid : attacker.cities) {
                            if (cities_.count(cid) && cities_.at(cid).owner == attackerId) {
                                recruitCity = cid;
                                break;
                            }
                        }
                    }
                    if (recruitCity == NO_CITY) break;
                    AIDecision rec;
                    rec.type = AIDecisionType::Recruit;
                    rec.actor = attackerId;
                    rec.targetCity = recruitCity;
                    executeDecision(rec);
                }

                const Kingdom& target = kingdoms_.at(targetId);
                if (target.capitalCity == NO_CITY || !cities_.count(target.capitalCity)) continue;
                const TileID targetTile = cities_.at(target.capitalCity).tile;
                int assigned = 0;
                for (ArmyID aid : attacker.armies) {
                    auto it = armies_.find(aid);
                    if (it == armies_.end() || it->second.isEmpty()) continue;
                    Army& army = it->second;
                    army.targetTile = targetTile;
                    army.movementPath.clear();
                    army.pathCursor = 0;
                    army.role = (assigned % 3 == 0) ? ArmyRole::Siege : ArmyRole::Attack;
                    if (attackerId == cleanupLeader &&
                        currentTurn_ >= 3000 &&
                        assigned == 0 &&
                        army.currentTile != NO_TILE &&
                        army.currentTile < static_cast<TileID>(worldMap_.tileCount()) &&
                        tileDistance(army.currentTile, targetTile) > 24.0f) {
                        TileID oldTile = army.currentTile;
                        if (oldTile != NO_TILE &&
                            oldTile < static_cast<TileID>(worldMap_.tileCount()) &&
                            worldMap_.at(oldTile).army == aid) {
                            worldMap_.at(oldTile).army = NO_ARMY;
                        }
                        army.currentTile = targetTile;
                        army.position = worldMap_.at(targetTile).position;
                        if (worldMap_.at(targetTile).army == NO_ARMY) {
                            worldMap_.at(targetTile).army = aid;
                        }
                    }
                    ++assigned;
                    if (assigned >= desiredArmies) break;
                }
            }
        }
    }

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

    if (currentTurn_ >= 2400 && currentTurn_ % 20 == 0) {
        std::vector<KingdomID> alive;
        alive.reserve(kingdoms_.size());
        for (const auto& [kid, k] : kingdoms_) {
            if (k.isAlive && !k.cities.empty()) alive.push_back(kid);
        }

        if (alive.size() > 1) {
            auto militarySoldiers = [&](const Kingdom& k) {
                uint64_t total = 0;
                for (ArmyID aid : k.armies) {
                    auto ait = armies_.find(aid);
                    if (ait != armies_.end()) total += ait->second.totalSoldiers();
                }
                return total;
            };

            auto nationalPower = [&](const Kingdom& k) {
                return static_cast<double>(k.cities.size()) * 1800.0 +
                       static_cast<double>(k.totalPopulation) * 0.015 +
                       static_cast<double>(militarySoldiers(k)) * 0.65 +
                       static_cast<double>(k.stability + k.morale) * 600.0;
            };

            KingdomID receiver = NO_KINGDOM;
            for (KingdomID kid : alive) {
                if (receiver == NO_KINGDOM ||
                    nationalPower(kingdoms_.at(kid)) > nationalPower(kingdoms_.at(receiver))) {
                    receiver = kid;
                }
            }

            KingdomID exhausted = NO_KINGDOM;
            float worstPressure = -1.0f;
            for (KingdomID kid : alive) {
                if (kid == receiver) continue;
                const Kingdom& k = kingdoms_.at(kid);
                const float timePressure = std::min(1.0f, static_cast<float>(currentTurn_ - 2400) / 650.0f);
                const float popPerCity = static_cast<float>(k.totalPopulation) /
                    std::max(1.0f, static_cast<float>(k.cities.size()));
                const float demographicCollapse = popPerCity < 150.0f ? 0.45f : 0.0f;
                const float pressure =
                    k.warWeariness * 0.75f +
                    (1.0f - k.stability) * 0.55f +
                    (1.0f - k.morale) * 0.45f +
                    timePressure * 0.45f +
                    demographicCollapse;
                if (pressure > worstPressure) {
                    worstPressure = pressure;
                    exhausted = kid;
                }
            }

            const float threshold =
                currentTurn_ >= 3200 ? 0.75f :
                currentTurn_ >= 2800 ? 0.90f : 1.10f;
            if (receiver != NO_KINGDOM && exhausted != NO_KINGDOM && worstPressure >= threshold) {
                Kingdom& exhaustedKingdom = kingdoms_.at(exhausted);

                exhaustedKingdom.stability = std::max(0.0f, exhaustedKingdom.stability - 0.075f);
                exhaustedKingdom.morale = std::max(0.0f, exhaustedKingdom.morale - 0.060f);

                bool changed = false;

                if (!exhaustedKingdom.cities.empty() &&
                    (worstPressure >= threshold + 0.12f || currentTurn_ >= 2800)) {
                    int defections =
                        currentTurn_ >= 3400 ? 4 :
                        currentTurn_ >= 3100 ? 3 :
                        currentTurn_ >= 2800 ? 2 : 1;
                    defections = std::min<int>(defections, std::max<int>(1, exhaustedKingdom.cities.size() / 4));

                    for (int defection = 0; defection < defections && exhaustedKingdom.cities.size() > 1; ++defection) {
                        CityID breakaway = NO_CITY;
                        float worstCityScore = -1.0f;
                        for (CityID cid : exhaustedKingdom.cities) {
                            if (!cities_.count(cid)) continue;
                            const City& city = cities_.at(cid);
                            if (city.isCapital && exhaustedKingdom.cities.size() > 1) continue;
                            float cityScore =
                                (1.0f - city.happiness) * 1.4f +
                                (city.cultureOwner != exhausted && city.cultureOwner != NO_KINGDOM ? 0.35f : 0.0f) +
                                (city.underSiege ? 0.30f : 0.0f) +
                                rng_.nextFloat(0.0f, 0.20f);
                            if (cityScore > worstCityScore) {
                                worstCityScore = cityScore;
                                breakaway = cid;
                            }
                        }
                        if (breakaway == NO_CITY || !cities_.count(breakaway)) break;

                        City& city = cities_.at(breakaway);
                        auto& oldCities = exhaustedKingdom.cities;
                        oldCities.erase(std::remove(oldCities.begin(), oldCities.end(), breakaway), oldCities.end());

                        city.owner = NO_KINGDOM;
                        city.isCapital = false;
                        city.happiness = std::max(0.12f, city.happiness - 0.16f);
                        city.fortification = std::max(0.05f, city.fortification - 0.20f);
                        if (city.tile != NO_TILE && city.tile < static_cast<TileID>(worldMap_.tileCount())) {
                            worldMap_.at(city.tile).owner = NO_KINGDOM;
                        }

                        if (exhaustedKingdom.capitalCity == breakaway) {
                            exhaustedKingdom.capitalCity =
                                exhaustedKingdom.cities.empty() ? NO_CITY : exhaustedKingdom.cities.front();
                            if (exhaustedKingdom.capitalCity != NO_CITY && cities_.count(exhaustedKingdom.capitalCity)) {
                                cities_.at(exhaustedKingdom.capitalCity).isCapital = true;
                            }
                        }

                        HistoryEvent ev;
                        ev.id = timeline_.nextEventID();
                        ev.type = EventType::WorldEventNegative;
                        ev.turn = currentTurn_;
                        ev.primaryKingdom = exhausted;
                        ev.secondaryKingdom = NO_KINGDOM;
                        ev.relatedCity = breakaway;
                        ev.description = city.name + " broke away from exhausted " +
                                         exhaustedKingdom.name + " and became ungoverned.";
                        timeline_.record(std::move(ev));
                        changed = true;
                    }
                }

                if (!changed && !exhaustedKingdom.armies.empty()) {
                    ArmyID deserter = NO_ARMY;
                    uint32_t fewest = UINT32_MAX;
                    for (ArmyID aid : exhaustedKingdom.armies) {
                        auto ait = armies_.find(aid);
                        if (ait == armies_.end()) continue;
                        uint32_t soldiers = ait->second.totalSoldiers();
                        if (soldiers < fewest) {
                            fewest = soldiers;
                            deserter = aid;
                        }
                    }
                    if (deserter != NO_ARMY) {
                        auto ait = armies_.find(deserter);
                        if (ait != armies_.end()) {
                            for (auto& unit : ait->second.units) {
                                unit.soldiers = static_cast<uint32_t>(unit.soldiers * 0.45f);
                                unit.morale = std::max(0.05f, unit.morale - 0.20f);
                            }
                        }
                        HistoryEvent ev;
                        ev.id = timeline_.nextEventID();
                        ev.type = EventType::WorldEventNegative;
                        ev.turn = currentTurn_;
                        ev.primaryKingdom = exhausted;
                        ev.description = exhaustedKingdom.name + " suffered mass desertion as war exhaustion spread.";
                        timeline_.record(std::move(ev));
                        changed = true;
                    }
                }

                const bool terminalCollapse = exhaustedKingdom.cities.empty();

                if (terminalCollapse) {
                    for (ArmyID aid : exhaustedKingdom.armies) {
                        auto ait = armies_.find(aid);
                        if (ait == armies_.end()) continue;
                        TileID armyTile = ait->second.currentTile;
                        if (armyTile != NO_TILE &&
                            armyTile < static_cast<TileID>(worldMap_.tileCount()) &&
                            worldMap_.at(armyTile).army == aid) {
                            worldMap_.at(armyTile).army = NO_ARMY;
                        }
                        for (auto& unit : ait->second.units) unit.soldiers = 0;
                    }
                    exhaustedKingdom.cities.clear();
                    exhaustedKingdom.armies.clear();
                    exhaustedKingdom.capitalCity = NO_CITY;
                    exhaustedKingdom.isAlive = false;
                    exhaustedKingdom.annexedBy = receiver;
                    exhaustedKingdom.collapsedTurn = currentTurn_;

                    HistoryEvent ev;
                    ev.id = timeline_.nextEventID();
                    ev.type = EventType::KingdomCollapsed;
                    ev.turn = currentTurn_;
                    ev.primaryKingdom = exhausted;
                    ev.secondaryKingdom = receiver;
                    ev.description = exhaustedKingdom.name + " finally collapsed after losing its last governed city.";
                    timeline_.record(std::move(ev));
                }
            }
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

    if (alive.size() <= 1 || currentTurn_ < 250 || totalCities == 0) {
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

    // If the final powers keep trading cities for thousands of turns, settle the
    // continent under the strongest remaining bloc instead of letting FinalWar loop.
    const bool finalWarSettlement =
        currentTurn_ >= 4200 &&
        alive.size() <= 5 &&
        leaderScore >= std::max(0.01f, secondScore) * 1.08f &&
        (popShare >= 0.24f || cityShare >= 0.24f || militaryRatio >= 0.95f);

    const bool hardStalemateSettlement =
        currentTurn_ >= 5500 &&
        alive.size() <= 6 &&
        leaderScore >= std::max(0.01f, secondScore) * 1.02f;

    // grandAlliance (Diplomatic victory): a kingdom holding alliances with ≥55% of remaining
    // kingdoms demonstrates diplomatic supremacy — Pax Diplomatica
    KingdomID grandAllianceLeader = NO_KINGDOM;
    if (currentTurn_ >= 1500 && alive.size() >= 3) {
        for (KingdomID kid : alive) {
            const Kingdom& k = kingdoms_.at(kid);
            if (k.personality != KingdomPersonality::Diplomatic) continue;
            int allyCount = 0;
            for (KingdomID oid : alive) {
                if (oid == kid) continue;
                KingdomID lo = std::min(kid, oid), hi = std::max(kid, oid);
                auto it = relations_.find(lo);
                if (it == relations_.end()) continue;
                auto it2 = it->second.find(hi);
                if (it2 == it->second.end()) continue;
                if (it2->second.state == RelationState::Alliance) ++allyCount;
            }
            if (allyCount >= static_cast<int>(alive.size() - 1) * 55 / 100) {
                grandAllianceLeader = kid;
                break;
            }
        }
    }
    const bool grandAlliance = (grandAllianceLeader != NO_KINGDOM);

    if (!continentHegemon && !twoPowerSubmission && !exhaustedLateGame &&
        !imperialSettlement && !finalWarSettlement &&
        !hardStalemateSettlement && !grandAlliance) {
        return false;
    }

    // Grand Alliance: Diplomatic pax — override leader
    if (grandAlliance && !continentHegemon && !twoPowerSubmission &&
        !exhaustedLateGame && !imperialSettlement &&
        !finalWarSettlement && !hardStalemateSettlement) {
        leader = grandAllianceLeader;
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

    finalWinner.totalPopulation = static_cast<uint32_t>(
        std::min<uint64_t>(totalPop, std::numeric_limits<uint32_t>::max()));
    finalWinner.policy = NationalPolicy::FinalWar;

    HistoryEvent unified;
    unified.id = timeline_.nextEventID();
    unified.type = EventType::ContinentUnified;
    unified.turn = currentTurn_;
    unified.primaryKingdom = leader;
    unified.description = finalWinner.name + " has unified the continent!";
    timeline_.record(std::move(unified));

    simulationOver_ = true;
    return true;
}

void SimulationEngine::collectSnapshot() {
    if (!serializer_) return;

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
    serializer_->serialize(snap);
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

} // namespace jke
