#include "jke/SimulationEngine.hpp"
#include <algorithm>
#include <cmath>

namespace jke {

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
            if (currentTurn_ >= 1800) wearRate *= 1.15f;
            if (currentTurn_ >= 3200) wearRate *= 1.25f;
            k.warWeariness = std::min(1.0f, k.warWeariness + wearRate);
            if (k.warWeariness > 0.6f) {
                k.morale    = std::max(0.05f, k.morale    - 0.004f);
                k.stability = std::max(0.05f, k.stability - 0.003f);
            }
            if (k.warWeariness > 0.82f) {
                k.morale    = std::max(0.03f, k.morale    - 0.006f);
                k.stability = std::max(0.03f, k.stability - 0.004f);
            }
            k.peaceTurns = 0;  // reset peace counter when at war
        } else {
            k.warWeariness = std::max(0.0f, k.warWeariness - 0.015f);
            k.peaceTurns++;    // accumulate peace dividend
        }

        if (hasWar && k.warWeariness > 0.55f && currentTurn_ % 5 == 0) {
            const float fatigue = std::clamp((k.warWeariness - 0.55f) / 0.45f, 0.0f, 1.0f);
            for (ArmyID aid : k.armies) {
                auto ait = armies_.find(aid);
                if (ait == armies_.end()) continue;
                Army& army = ait->second;
                army.supplyLevel = std::max(0.08f, army.supplyLevel - 0.010f * fatigue);
                for (Unit& unit : army.units) {
                    unit.morale = std::max(0.08f, unit.morale - 0.006f * fatigue);
                    unit.training = std::max(0.30f, unit.training - 0.0015f * fatigue);
                }
            }
        }

        if (hasWar && k.warWeariness > 0.78f && currentTurn_ % 80 == 0) {
            HistoryEvent ev;
            ev.type = EventType::WorldEventNegative;
            ev.turn = currentTurn_;
            ev.primaryKingdom = kid;
            ev.description = k.name + " is exhausted by years of war.";
            ev.context["war_weariness"] = k.warWeariness;
            eventBus_.emit(std::move(ev));
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
        // Earlier trigger (700 turns, ≤12 alive) — endgame should converge
        // without changing the actual victory condition.
        if (currentTurn_ >= 700 && alive.size() <= 12) {
            TileID ownCapitalTile = NO_TILE;
            if (k.capitalCity != NO_CITY && cities_.count(k.capitalCity)) {
                ownCapitalTile = cities_.at(k.capitalCity).tile;
            }
            float bestLateScore = 1e9f;
            for (KingdomID otherId : alive) {
                if (otherId == kid) continue;
                const Kingdom& other = kingdoms_.at(otherId);
                if (other.capitalCity == NO_CITY || !cities_.count(other.capitalCity)) continue;

                float score = static_cast<float>(other.totalPopulation) * 0.001f +
                              static_cast<float>(other.cities.size()) * 2.5f;
                if (ownCapitalTile != NO_TILE) {
                    const auto ownPos = worldMap_.at(ownCapitalTile).position;
                    const auto otherPos = worldMap_.at(cities_.at(other.capitalCity).tile).position;
                    score += std::hypot(float(ownPos.x - otherPos.x),
                                        float(ownPos.y - otherPos.y)) * 1.6f;
                }
                if (other.cities.size() <= 1) score -= 18.0f;
                if (other.warWeariness > 0.75f) score -= 10.0f;

                if (score < bestLateScore) {
                    bestLateScore = score;
                    lateWarTarget = otherId;
                }
            }
        }

        k.strategicTarget = NO_KINGDOM;

        // FinalWar when ≤8 alive — breaks late micro-state stalemates by forcing invasion.
        if (alive.size() <= 8 || (currentTurn_ >= 2600 && alive.size() <= 10)) {
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
    auto isDefenseRole = [](ArmyRole role) {
        return role == ArmyRole::Defense ||
               role == ArmyRole::Garrison ||
               role == ArmyRole::SupplyGuard;
    };
    auto vanguardScore = [](const Army& army) {
        float score = static_cast<float>(army.totalSoldiers()) * 0.00022f;
        if (army.hasCommander()) {
            score += army.commander.attack * 1.6f;
            score += army.commander.cavalry * 1.1f;
            score += army.commander.pursuit * 1.0f;
            score += army.commander.morale * 0.5f;
        }
        if (army.hasStrategist()) {
            score += army.strategist.ambush * 0.5f;
            score += army.strategist.logistics * 0.4f;
        }
        return score;
    };
    auto flankerScore = [](const Army& army) {
        float score = static_cast<float>(army.totalSoldiers()) * 0.00018f;
        if (army.hasCommander()) {
            score += army.commander.cavalry * 1.5f;
            score += army.commander.pursuit * 1.0f;
            score += army.commander.attack * 0.7f;
        }
        if (army.hasStrategist()) {
            score += army.strategist.ambush * 1.3f;
            score += army.strategist.lure * 0.8f;
            score += army.strategist.logistics * 0.4f;
        }
        return score;
    };
    auto defenseScore = [](const Army& army) {
        float score = static_cast<float>(army.totalSoldiers()) * 0.00025f;
        if (army.hasCommander()) {
            score += army.commander.defense * 1.8f;
            score += army.commander.morale * 0.9f;
        }
        if (army.hasStrategist()) {
            score += army.strategist.retreat * 0.7f;
            score += army.strategist.lure * 0.8f;
        }
        return score;
    };
    auto siegeScore = [](const Army& army) {
        float score = static_cast<float>(army.totalSoldiers()) * 0.00016f;
        for (const Unit& unit : army.units) {
            if (unit.type == UnitType::SiegeUnit && unit.soldiers > 0) score += 3.0f;
        }
        if (army.hasStrategist()) {
            score += army.strategist.siege * 2.2f;
            score += army.strategist.logistics * 0.6f;
        }
        if (army.hasCommander()) score += army.commander.morale * 0.3f;
        return score;
    };
    auto supplyScore = [](const Army& army) {
        float score = static_cast<float>(army.totalSoldiers()) * 0.00016f;
        if (army.hasStrategist()) {
            score += army.strategist.logistics * 2.0f;
            score += army.strategist.retreat * 0.8f;
            score += army.strategist.lure * 0.5f;
        }
        if (army.hasCommander()) {
            score += army.commander.defense * 0.7f;
            score += army.commander.morale * 0.5f;
        }
        return score;
    };
    auto bestByScore = [&](const std::vector<ArmyID>& ids, auto&& predicate, auto&& scoreFn) {
        ArmyID best = NO_ARMY;
        float bestScore = -1e9f;
        for (ArmyID aid : ids) {
            Army& army = armies_.at(aid);
            if (!predicate(army)) continue;
            const float score = scoreFn(army);
            if (score > bestScore) {
                bestScore = score;
                best = aid;
            }
        }
        return best;
    };
    auto tileDistance = [&](TileID a, TileID b) {
        if (a == NO_TILE || b == NO_TILE) return 1e9f;
        const auto ap = worldMap_.at(a).position;
        const auto bp = worldMap_.at(b).position;
        return std::hypot(float(ap.x - bp.x), float(ap.y - bp.y));
    };
    auto relationAtWar = [&](KingdomID a, KingdomID b) {
        if (a == b || a == NO_KINGDOM || b == NO_KINGDOM) return false;
        KingdomID lo = std::min(a, b);
        KingdomID hi = std::max(a, b);
        auto it = relations_.find(lo);
        if (it == relations_.end()) return false;
        auto it2 = it->second.find(hi);
        return it2 != it->second.end() && it2->second.state == RelationState::War;
    };
    auto nearestEnemyArmyDistance = [&](KingdomID owner, TileID tile) {
        float best = 1e9f;
        for (const auto& [enemyAid, enemyArmy] : armies_) {
            (void)enemyAid;
            if (enemyArmy.owner == owner || enemyArmy.isEmpty()) continue;
            if (!kingdoms_.count(enemyArmy.owner) || !kingdoms_.at(enemyArmy.owner).isAlive) continue;
            if (!relationAtWar(owner, enemyArmy.owner)) continue;
            best = std::min(best, tileDistance(tile, enemyArmy.currentTile));
        }
        return best;
    };
    auto assignArmyTarget = [&](Army& army, TileID target) {
        if (target == NO_TILE || army.targetTile == target) return;
        army.targetTile = target;
        army.movementPath.clear();
        army.pathCursor = 0;
    };

    for (auto& [aid, army] : armies_) {
        army.role = ArmyRole::Reserve;
    }

    for (auto& [kid, k] : kingdoms_) {
        if (!k.isAlive) continue;
        const int aliveRealms = static_cast<int>(std::count_if(
            kingdoms_.begin(), kingdoms_.end(),
            [](const auto& p) { return p.second.isAlive && !p.second.isRebel; }));
        const bool decisiveEndgame = currentTurn_ >= 2600 || aliveRealms <= 2;

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
        } else if (!decisiveEndgame &&
                   k.policy == NationalPolicy::Invading &&
                   k.cities.size() >= 2 &&
                   living.size() >= 3) {
            // Keep a home guard while campaigning so one front can advance
            // without leaving the opposite side of the realm empty.
            defenseSlots = 1;
        }
        defenseSlots = std::min(defenseSlots, static_cast<int>(living.size()));

        int reserveSlots = 0;
        if (k.campaignPlan.active()) {
            reserveSlots = static_cast<int>(
                std::round(static_cast<float>(living.size()) * k.campaignPlan.reserveRatio));
            if (k.campaignPlan.phase == CampaignPhase::Resupply) {
                reserveSlots = std::max(reserveSlots, static_cast<int>(living.size()) / 2);
            }
            if (k.campaignPlan.supportAlly && living.size() >= 4) {
                reserveSlots = std::max(1, reserveSlots - 1);
            }
        }
        reserveSlots = std::clamp(reserveSlots, 0, std::max(0, static_cast<int>(living.size()) - defenseSlots));

        std::vector<ArmyID> plannedReserves;
        for (int i = 0; i < defenseSlots; ++i) {
            armies_.at(living[i]).role = ArmyRole::Defense;
        }
        if (defenseSlots > 0) {
            const ArmyRole homeRole =
                (k.personality == KingdomPersonality::Defensive ||
                 k.policy == NationalPolicy::Defending ||
                 k.cities.size() <= 2)
                    ? ArmyRole::Garrison
                    : ArmyRole::SupplyGuard;
            std::vector<ArmyID> homeCandidates;
            homeCandidates.reserve(static_cast<size_t>(defenseSlots));
            for (int i = 0; i < defenseSlots; ++i) homeCandidates.push_back(living[i]);
            const ArmyID homeAid = bestByScore(
                homeCandidates,
                [](const Army&) { return true; },
                homeRole == ArmyRole::Garrison ? defenseScore : supplyScore);
            Army& homeArmy = armies_.at(homeAid == NO_ARMY ? living[0] : homeAid);
            homeArmy.role = homeRole;
            // Lock Garrison to capital so it never wanders to the frontline
            if (homeRole == ArmyRole::Garrison &&
                k.capitalCity != NO_CITY &&
                cities_.count(k.capitalCity) &&
                cities_.at(k.capitalCity).tile != NO_TILE) {
                TileID capitalTile = cities_.at(k.capitalCity).tile;
                if (homeArmy.targetTile != capitalTile) {
                    homeArmy.targetTile    = capitalTile;
                    homeArmy.movementPath.clear();
                    homeArmy.pathCursor    = 0;
                }
            }
        }
        for (int i = 0; i < reserveSlots; ++i) {
            const size_t idx = static_cast<size_t>(defenseSlots + i);
            if (idx < living.size()) {
                armies_.at(living[idx]).role = ArmyRole::Reserve;
                plannedReserves.push_back(living[idx]);
            }
        }

        ArmyID siegeArmy = bestByScore(
            living,
            [&](const Army& army) {
                if (isDefenseRole(army.role)) return false;
                for (const auto& unit : army.units) {
                    if (unit.type == UnitType::SiegeUnit && unit.soldiers > 0) return true;
                }
                return false;
            },
            siegeScore);

        if ((k.policy == NationalPolicy::Invading || k.policy == NationalPolicy::FinalWar) &&
            siegeArmy != NO_ARMY) {
            armies_.at(siegeArmy).role = ArmyRole::Siege;
        }

        // Late wars often hit the army cap before a dedicated siege army exists.
        // Refit one non-defense field army so campaigns do not stall outside cities.
        if ((k.policy == NationalPolicy::Invading || k.policy == NationalPolicy::FinalWar) &&
            siegeArmy == NO_ARMY &&
            living.size() >= 2) {
            siegeArmy = bestByScore(
                living,
                [&](const Army& candidate) {
                    if (isDefenseRole(candidate.role) || candidate.units.empty()) return false;
                    return candidate.totalSoldiers() >= 650 || k.policy == NationalPolicy::FinalWar;
                },
                siegeScore);
            if (siegeArmy != NO_ARMY) {
                Army& candidate = armies_.at(siegeArmy);
                candidate.units.front().type = UnitType::SiegeUnit;
                candidate.units.front().equipment = std::max(candidate.units.front().equipment, 0.62f);
                candidate.role = ArmyRole::Siege;
            }
        }

        for (ArmyID aid : living) {
            Army& army = armies_.at(aid);
            if (army.role != ArmyRole::Reserve) continue;

            const bool depleted = army.totalSoldiers() < 450 || army.supplyLevel < 0.25f;
            if (depleted && k.policy != NationalPolicy::FinalWar) {
                army.role = ArmyRole::Reserve;
            } else if (std::find(plannedReserves.begin(), plannedReserves.end(), aid) != plannedReserves.end()) {
                army.role = ArmyRole::Reserve;
            } else if (k.campaignPlan.active() &&
                       k.campaignPlan.phase == CampaignPhase::Resupply &&
                       k.policy != NationalPolicy::FinalWar) {
                army.role = ArmyRole::Reserve;
            } else if (k.policy == NationalPolicy::Invading ||
                       k.policy == NationalPolicy::FinalWar) {
                army.role = ArmyRole::Attack;
            }
        }

        if (k.policy == NationalPolicy::Invading || k.policy == NationalPolicy::FinalWar) {
            std::vector<ArmyID> attackers;
            attackers.reserve(living.size());
            for (ArmyID aid : living) {
                if (armies_.at(aid).role == ArmyRole::Attack) attackers.push_back(aid);
            }

            const ArmyID vanguard = bestByScore(
                attackers,
                [](const Army&) { return true; },
                vanguardScore);
            if (vanguard != NO_ARMY) {
                armies_.at(vanguard).role = ArmyRole::Vanguard;
                attackers.erase(std::remove(attackers.begin(), attackers.end(), vanguard), attackers.end());
            }

            int flankers = 0;
            const int desiredFlankers = living.size() >= 8 ? 2 :
                                        living.size() >= 4 ? 1 : 0;
            while (flankers < desiredFlankers && !attackers.empty()) {
                const ArmyID flanker = bestByScore(
                    attackers,
                    [](const Army&) { return true; },
                    flankerScore);
                if (flanker == NO_ARMY) break;
                armies_.at(flanker).role = ArmyRole::Flanker;
                attackers.erase(std::remove(attackers.begin(), attackers.end(), flanker), attackers.end());
                ++flankers;
            }
        } else if (k.policy == NationalPolicy::Rebuilding && living.size() >= 4) {
            const ArmyID supplyGuard = bestByScore(
                living,
                [](const Army& army) {
                    return army.role == ArmyRole::Reserve && army.supplyLevel >= 0.70f;
                },
                supplyScore);
            if (supplyGuard != NO_ARMY) {
                armies_.at(supplyGuard).role = ArmyRole::SupplyGuard;
            }
        }

        if (!decisiveEndgame &&
            (k.policy == NationalPolicy::Defending ||
             k.personality == KingdomPersonality::Defensive) &&
            living.size() >= 4 &&
            k.cities.size() >= 5) {
            const int desiredGarrisons = std::min<int>(
                3,
                std::max(1, static_cast<int>(living.size()) / 4));
            int currentGarrisons = 0;
            for (ArmyID aid : living) {
                if (armies_.at(aid).role == ArmyRole::Garrison) ++currentGarrisons;
            }
            while (currentGarrisons < desiredGarrisons) {
                const ArmyID guard = bestByScore(
                    living,
                    [&](const Army& army) {
                        return army.role == ArmyRole::Reserve ||
                               army.role == ArmyRole::Defense ||
                               army.role == ArmyRole::SupplyGuard;
                    },
                    defenseScore);
                if (guard == NO_ARMY || armies_.at(guard).role == ArmyRole::Garrison) break;
                armies_.at(guard).role = ArmyRole::Garrison;
                ++currentGarrisons;
            }
        }

        if (!decisiveEndgame &&
            (k.policy == NationalPolicy::Invading || k.policy == NationalPolicy::FinalWar) &&
            living.size() >= 7 &&
            k.cities.size() >= 8) {
            int currentSupplyGuards = 0;
            for (ArmyID aid : living) {
                if (armies_.at(aid).role == ArmyRole::SupplyGuard) ++currentSupplyGuards;
            }
            if (currentSupplyGuards < 2) {
                const ArmyID guard = bestByScore(
                    living,
                    [&](const Army& army) {
                        return army.role == ArmyRole::Reserve ||
                               army.role == ArmyRole::Defense;
                    },
                    supplyScore);
                if (guard != NO_ARMY) armies_.at(guard).role = ArmyRole::SupplyGuard;
            }
        }

        std::vector<TileID> assignedGuardTiles;
        auto chooseGarrisonTarget = [&](const Kingdom& kingdom) {
            TileID bestTile = NO_TILE;
            float bestScore = -1e9f;
            for (CityID cid : kingdom.cities) {
                if (!cities_.count(cid)) continue;
                const City& city = cities_.at(cid);
                if (city.owner != kingdom.id || city.tile == NO_TILE) continue;
                if (std::find(assignedGuardTiles.begin(), assignedGuardTiles.end(), city.tile) !=
                    assignedGuardTiles.end()) {
                    continue;
                }
                const float threatDist = nearestEnemyArmyDistance(kingdom.id, city.tile);
                float score = static_cast<float>(city.population) * 0.004f;
                if (city.isCapital || kingdom.capitalCity == cid) score += 70.0f;
                if (city.cityType == CityType::Fortress) score += 44.0f;
                if (city.cityType == CityType::TradeHub || city.cityType == CityType::Port) score += 18.0f;
                if (threatDist < 36.0f) score += (36.0f - threatDist) * 2.2f;
                score += city.fortification * 20.0f;
                if (score > bestScore) {
                    bestScore = score;
                    bestTile = city.tile;
                }
            }
            return bestTile;
        };
        auto chooseSupplyGuardTarget = [&](const Kingdom& kingdom) {
            TileID bestTile = NO_TILE;
            float bestScore = -1e9f;
            for (const Tile& tile : worldMap_.tiles()) {
                if (tile.owner != kingdom.id) continue;
                if (tile.strategicPoint != StrategicPointType::SupplyDepot &&
                    tile.strategicPoint != StrategicPointType::Bridge &&
                    tile.strategicPoint != StrategicPointType::HarborSite &&
                    tile.strategicPoint != StrategicPointType::RiverFord) {
                    continue;
                }
                if (std::find(assignedGuardTiles.begin(), assignedGuardTiles.end(), tile.id) !=
                    assignedGuardTiles.end()) {
                    continue;
                }
                const float threatDist = nearestEnemyArmyDistance(kingdom.id, tile.id);
                float score = tile.strategicValue;
                if (tile.strategicPoint == StrategicPointType::SupplyDepot) score += 55.0f;
                if (tile.strategicPoint == StrategicPointType::Bridge) score += 36.0f;
                if (tile.strategicPoint == StrategicPointType::HarborSite) score += 30.0f;
                if (tile.strategicPoint == StrategicPointType::RiverFord) score += 24.0f;
                if (threatDist < 42.0f) score += (42.0f - threatDist) * 1.6f;
                if (score > bestScore) {
                    bestScore = score;
                    bestTile = tile.id;
                }
            }
            if (bestTile != NO_TILE) return bestTile;
            return chooseGarrisonTarget(kingdom);
        };

        for (ArmyID aid : living) {
            Army& army = armies_.at(aid);
            TileID target = NO_TILE;
            if (army.role == ArmyRole::Garrison) {
                target = chooseGarrisonTarget(k);
            } else if (army.role == ArmyRole::SupplyGuard) {
                target = chooseSupplyGuardTarget(k);
            }
            if (target != NO_TILE) {
                assignedGuardTiles.push_back(target);
                assignArmyTarget(army, target);
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
            army.recoveringSupply = true;
            army.supplyRetreatUntil = std::max(army.supplyRetreatUntil, currentTurn_ + 24);
        }
    }
}


} // namespace jke
