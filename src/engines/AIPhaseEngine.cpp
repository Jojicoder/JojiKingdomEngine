#include "jke/SimulationEngine.hpp"
#include "jke/ai/personalities/AggressiveAI.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace jke {

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

        auto tileDistance = [&](TileID a, TileID b) {
            auto ap = worldMap_.at(a).position;
            auto bp = worldMap_.at(b).position;
            return std::hypot(float(ap.x - bp.x), float(ap.y - bp.y));
        };

        auto isWarGoalTargetValid = [&](const Kingdom& ownerKingdom) {
            if (!ownerKingdom.currentWarGoal.active()) return false;
            if (!kingdoms_.count(ownerKingdom.currentWarGoal.enemy) ||
                !kingdoms_.at(ownerKingdom.currentWarGoal.enemy).isAlive) return false;
            const DiplomaticRelation* rel =
                relationBetween(ownerKingdom.id, ownerKingdom.currentWarGoal.enemy);
            return rel && rel->state == RelationState::War;
        };

        auto chooseWarGoal = [&](Kingdom& actorKingdom, KingdomID enemyId) {
            if (enemyId == NO_KINGDOM || !kingdoms_.count(enemyId)) {
                actorKingdom.currentWarGoal = WarGoal{};
                return;
            }
            if (isWarGoalTargetValid(actorKingdom) &&
                actorKingdom.currentWarGoal.enemy == enemyId &&
                currentTurn_ < actorKingdom.currentWarGoal.startedTurn +
                    std::max<TurnNumber>(18, actorKingdom.currentWarGoal.maxTurns / 3)) {
                if (!actorKingdom.currentWarGoal.targetCities.empty()) {
                    auto nextTarget = std::find_if(
                        actorKingdom.currentWarGoal.targetCities.begin(),
                        actorKingdom.currentWarGoal.targetCities.end(),
                        [&](CityID cid) {
                            return cities_.count(cid) &&
                                   cities_.at(cid).owner == enemyId;
                        });
                    if (nextTarget != actorKingdom.currentWarGoal.targetCities.end() &&
                        nextTarget != actorKingdom.currentWarGoal.targetCities.begin()) {
                        std::rotate(actorKingdom.currentWarGoal.targetCities.begin(),
                                    nextTarget,
                                    std::next(nextTarget));
                    }
                }
                return;
            }

            const Kingdom& enemy = kingdoms_.at(enemyId);
            WarGoal goal;
            goal.enemy = enemyId;
            goal.startedTurn = currentTurn_;

            if (actorKingdom.policy == NationalPolicy::FinalWar ||
                actorKingdom.strategyPlan == StrategyPlan::TotalConquest) {
                goal.type = WarGoalType::TotalConquest;
                goal.maxTurns = 180;
                goal.desiredCaptures = std::max(2, static_cast<int>(enemy.cities.size()));
                goal.requiredForceRatio = 0.62f;
                goal.allowPeaceAfterGoal = false;
            } else if (actorKingdom.strategyPlan == StrategyPlan::TurtleDefense) {
                goal.type = WarGoalType::DefensiveHold;
                goal.maxTurns = 55;
                goal.desiredCaptures = 0;
                goal.requiredForceRatio = 0.95f;
            } else if (actorKingdom.strategyPlan == StrategyPlan::AntiHegemonWar) {
                goal.type = WarGoalType::AntiHegemon;
                goal.maxTurns = 120;
                goal.desiredCaptures = 2;
                goal.requiredForceRatio = 0.78f;
            } else if (actorKingdom.strategyPlan == StrategyPlan::CapitalRush) {
                goal.type = WarGoalType::Capital;
                goal.maxTurns = 110;
                goal.desiredCaptures = 2;
                goal.requiredForceRatio = 0.70f;
            } else if (actorKingdom.strategyPlan == StrategyPlan::RevengeWar) {
                goal.type = WarGoalType::Punitive;
                goal.maxTurns = 90;
                goal.desiredCaptures = 1;
                goal.requiredForceRatio = 0.74f;
            } else if (actorKingdom.strategyPlan == StrategyPlan::OpportunisticRaid) {
                goal.type = WarGoalType::SupplyHub;
                goal.maxTurns = 70;
                goal.desiredCaptures = 1;
                goal.requiredForceRatio = 0.66f;
            } else {
                goal.type = WarGoalType::BorderCities;
                goal.maxTurns = 85;
                goal.desiredCaptures = actorKingdom.personality == KingdomPersonality::Expansionist ? 2 : 1;
                goal.requiredForceRatio = 0.82f;
            }

            if (actorKingdom.personality == KingdomPersonality::Aggressive ||
                actorKingdom.personality == KingdomPersonality::Expansionist) {
                // 長期戦も辞さない、数の劣勢でも攻める
                goal.maxTurns += 25;
                goal.requiredForceRatio -= 0.06f;
            } else if (actorKingdom.personality == KingdomPersonality::Defensive) {
                // 守りに入ったら戦争を長引かせたくない、有利なうちに終わらせる
                goal.maxTurns = std::min<TurnNumber>(goal.maxTurns, 55);
                goal.requiredForceRatio += 0.12f;
            } else if (actorKingdom.personality == KingdomPersonality::Opportunistic) {
                // 戦況が不利になったらすぐ手を引く
                goal.maxTurns = std::min<TurnNumber>(goal.maxTurns, 60);
                goal.requiredForceRatio += 0.05f;
            } else if (actorKingdom.personality == KingdomPersonality::Economic ||
                       actorKingdom.personality == KingdomPersonality::Diplomatic) {
                // 消耗を嫌う、早期決着か撤退
                goal.maxTurns = std::min<TurnNumber>(goal.maxTurns, 65);
                goal.requiredForceRatio += 0.08f;
            }

            struct GoalCityCandidate {
                CityID city;
                float score;
            };
            auto nearestOwnedCityDistance = [&](TileID target) {
                float bestDist = 1e9f;
                for (CityID ownCid : actorKingdom.cities) {
                    if (!cities_.count(ownCid)) continue;
                    const City& ownCity = cities_.at(ownCid);
                    if (ownCity.owner != actorKingdom.id || ownCity.tile == NO_TILE) continue;
                    bestDist = std::min(bestDist, tileDistance(ownCity.tile, target));
                }
                return bestDist > 1e8f ? 0.0f : bestDist;
            };
            auto nearbyOwnedTerritory = [&](TileID center) {
                if (center == NO_TILE ||
                    center >= static_cast<TileID>(worldMap_.tileCount())) return 0;
                std::vector<TileID> frontier{center};
                std::unordered_set<TileID> visited{center};
                int ownedPressure = 0;
                for (int depth = 0; depth < 4 && !frontier.empty(); ++depth) {
                    std::vector<TileID> next;
                    for (TileID tile : frontier) {
                        for (TileID nid : worldMap_.neighbors8(tile)) {
                            if (!visited.insert(nid).second) continue;
                            if (worldMap_.at(nid).owner == actorKingdom.id) {
                                ownedPressure += 5 - depth;
                            }
                            next.push_back(nid);
                        }
                    }
                    frontier = std::move(next);
                }
                return ownedPressure;
            };

            std::vector<GoalCityCandidate> candidates;
            for (CityID cid : enemy.cities) {
                if (!cities_.count(cid)) continue;
                const City& city = cities_.at(cid);
                if (city.owner != enemyId || city.tile == NO_TILE) continue;
                if (city.isCapital && goal.type != WarGoalType::Capital &&
                    goal.type != WarGoalType::TotalConquest &&
                    enemy.cities.size() > 1) {
                    continue;
                }

                const float nearestBaseDist = nearestOwnedCityDistance(city.tile);
                const int ownedPressure = nearbyOwnedTerritory(city.tile);
                float score = nearestBaseDist;
                score += std::max(0.0f, nearestBaseDist - 28.0f) * 1.8f;
                score -= std::min(32.0f, static_cast<float>(ownedPressure) * 1.15f);
                if (nearestBaseDist <= 14.0f) score -= 16.0f;
                else if (nearestBaseDist <= 24.0f) score -= 8.0f;
                const bool hub = city.cityType == CityType::Fortress ||
                                 city.cityType == CityType::TradeHub ||
                                 city.cityType == CityType::Port;
                const bool localTown =
                    !city.isCapital &&
                    (city.cityType == CityType::Generic ||
                     city.cityType == CityType::Agricultural);
                if (localTown) {
                    float townValue = 10.0f;
                    townValue += std::min(10.0f, static_cast<float>(city.population) * 0.010f);
                    if (ownedPressure > 0) townValue += 10.0f;
                    if (nearestBaseDist <= 18.0f) townValue += 8.0f;
                    if (city.cityType == CityType::Agricultural) townValue += 5.0f;
                    score -= townValue;
                }
                if (goal.type == WarGoalType::SupplyHub ||
                    goal.type == WarGoalType::AntiHegemon) {
                    if (hub) score -= 32.0f;
                    if (city.cityType == CityType::Mining) score -= 10.0f;
                }
                if (goal.type == WarGoalType::Capital && city.isCapital) score -= 90.0f;
                if (goal.type == WarGoalType::Punitive && city.originalOwner == actorKingdom.id) score -= 70.0f;
                if (goal.type == WarGoalType::BorderCities && hub) score -= 8.0f;
                score += city.fortification * 8.0f;
                candidates.push_back({cid, score});
            }
            std::sort(candidates.begin(), candidates.end(),
                      [](const GoalCityCandidate& a, const GoalCityCandidate& b) {
                          return a.score < b.score;
                      });
            const size_t goalCityLimit =
                goal.type == WarGoalType::TotalConquest ? 5u :
                goal.type == WarGoalType::Capital ? 3u : 3u;
            for (const auto& c : candidates) {
                if (goal.targetCities.size() >= goalCityLimit) break;
                goal.targetCities.push_back(c.city);
            }
            if (goal.targetCities.empty() && enemy.capitalCity != NO_CITY) {
                goal.targetCities.push_back(enemy.capitalCity);
            }

            actorKingdom.currentWarGoal = std::move(goal);
        };

        if (warEnemy != NO_KINGDOM) {
            chooseWarGoal(k, warEnemy);
        } else {
            k.currentWarGoal = WarGoal{};
            k.campaignPlan = CampaignPlan{};
            k.aiReason = "At peace: rebuilding economy and reserves.";
        }

        auto chooseCampaignPlan = [&](Kingdom& actorKingdom) {
            CampaignPlan plan;
            if (!actorKingdom.currentWarGoal.active()) {
                actorKingdom.campaignPlan = plan;
                return;
            }

            plan.enemy = actorKingdom.currentWarGoal.enemy;
            plan.primaryObjective = actorKingdom.currentWarGoal.targetCities.empty()
                ? NO_CITY : actorKingdom.currentWarGoal.targetCities.front();
            if (actorKingdom.currentWarGoal.targetCities.size() >= 2) {
                plan.secondaryObjective = actorKingdom.currentWarGoal.targetCities[1];
            }

            if (season == Season::Spring) plan.phase = CampaignPhase::Invasion;
            else if (season == Season::Summer) plan.phase = CampaignPhase::Battle;
            else if (season == Season::Autumn) plan.phase = CampaignPhase::Siege;
            else plan.phase = CampaignPhase::Resupply;

            plan.reserveRatio = 0.22f;
            plan.retreatSupply = 0.36f;
            plan.commitThreshold = actorKingdom.currentWarGoal.requiredForceRatio;
            plan.primaryCommitShare = 0.58f;
            if (actorKingdom.personality == KingdomPersonality::Defensive ||
                actorKingdom.personality == KingdomPersonality::Economic) {
                plan.reserveRatio += 0.12f;
                plan.retreatSupply += 0.06f;
                plan.commitThreshold += 0.08f;
                plan.primaryCommitShare = 0.50f;
            }
            if (actorKingdom.personality == KingdomPersonality::Aggressive ||
                actorKingdom.personality == KingdomPersonality::Expansionist) {
                plan.reserveRatio -= 0.06f;
                plan.retreatSupply -= 0.04f;
                plan.commitThreshold -= 0.05f;
                plan.primaryCommitShare = 0.70f;
            }
            if (actorKingdom.personality == KingdomPersonality::Opportunistic) {
                plan.primaryCommitShare = 0.64f;
            }
            if (actorKingdom.recentDefeats > 0 &&
                currentTurn_ <= actorKingdom.lastDefeatTurn + 90) {
                plan.reserveRatio += std::min(0.22f, actorKingdom.recentDefeats * 0.045f);
                plan.retreatSupply += std::min(0.12f, actorKingdom.recentDefeats * 0.025f);
                plan.commitThreshold += std::min(0.16f, actorKingdom.recentDefeats * 0.035f);
                plan.primaryCommitShare -= std::min(0.12f, actorKingdom.recentDefeats * 0.025f);
            } else if (actorKingdom.recentDefeats > 0 &&
                       currentTurn_ > actorKingdom.lastDefeatTurn + 90) {
                actorKingdom.recentDefeats = std::max(0, actorKingdom.recentDefeats - 1);
            }
            if (plan.phase == CampaignPhase::Resupply) {
                plan.reserveRatio = std::max(plan.reserveRatio, 0.45f);
                plan.retreatSupply += 0.08f;
                plan.primaryCommitShare = std::min(plan.primaryCommitShare, 0.52f);
            } else if (plan.phase == CampaignPhase::Siege) {
                plan.reserveRatio += 0.05f;
                plan.primaryCommitShare += 0.06f;
            }
            plan.reserveRatio = std::clamp(plan.reserveRatio, 0.10f, 0.62f);
            plan.retreatSupply = std::clamp(plan.retreatSupply, 0.28f, 0.58f);
            plan.commitThreshold = std::clamp(plan.commitThreshold, 0.55f, 1.10f);
            plan.primaryCommitShare = std::clamp(plan.primaryCommitShare, 0.42f, 0.78f);

            const DiplomaticRelation* allyRel = nullptr;
            for (const auto& [allyId, ally] : kingdoms_) {
                if (allyId == actorKingdom.id || !ally.isAlive) continue;
                allyRel = relationBetween(actorKingdom.id, allyId);
                if (!allyRel || allyRel->state != RelationState::Alliance) continue;
                if (!ally.currentWarGoal.active() ||
                    ally.currentWarGoal.enemy != actorKingdom.currentWarGoal.enemy) continue;
                if (!ally.currentWarGoal.targetCities.empty() &&
                    actorKingdom.personality != KingdomPersonality::Aggressive &&
                    actorKingdom.personality != KingdomPersonality::Expansionist) {
                    plan.primaryObjective = ally.currentWarGoal.targetCities.front();
                    plan.supportAlly = true;
                    break;
                }
            }

            if (actorKingdom.armies.size() >= 5 &&
                plan.secondaryObjective != NO_CITY &&
                plan.phase != CampaignPhase::Resupply &&
                plan.primaryCommitShare < 0.68f) {
                plan.diversion = true;
            }
            plan.concentratedAssault =
                !plan.diversion &&
                plan.primaryObjective != NO_CITY &&
                plan.phase != CampaignPhase::Resupply;

            if (plan.primaryObjective != NO_CITY && cities_.count(plan.primaryObjective)) {
                CityID bestBase = NO_CITY;
                float bestDist = 1e9f;
                for (CityID cid : actorKingdom.cities) {
                    if (!cities_.count(cid) || cities_.at(cid).owner != actorKingdom.id) continue;
                    float dist = tileDistance(cities_.at(cid).tile, cities_.at(plan.primaryObjective).tile);
                    if (dist < bestDist) {
                        bestDist = dist;
                        bestBase = cid;
                    }
                }
                plan.stagingCity = bestBase;
            }

            auto nearestOwnedCity = [&](TileID tile) {
                CityID bestBase = NO_CITY;
                float bestDist = 1e9f;
                for (CityID cid : actorKingdom.cities) {
                    if (!cities_.count(cid) || cities_.at(cid).owner != actorKingdom.id) continue;
                    float dist = tileDistance(cities_.at(cid).tile, tile);
                    if (dist < bestDist) {
                        bestDist = dist;
                        bestBase = cid;
                    }
                }
                return bestBase;
            };

            auto nearestOwnedCityDistanceForPlan = [&](TileID tile) {
                float bestDist = 1e9f;
                for (CityID cid : actorKingdom.cities) {
                    if (!cities_.count(cid) || cities_.at(cid).owner != actorKingdom.id) continue;
                    bestDist = std::min(bestDist, tileDistance(cities_.at(cid).tile, tile));
                }
                return bestDist > 1e8f ? 0.0f : bestDist;
            };

            auto adjacentOwnedPressureForPlan = [&](TileID tile) {
                int pressure = 0;
                if (tile == NO_TILE || tile >= static_cast<TileID>(worldMap_.tileCount())) return pressure;
                for (TileID nid : worldMap_.neighbors8(tile)) {
                    const Tile& n = worldMap_.at(nid);
                    if (n.owner == actorKingdom.id) pressure += 4;
                    if (n.city != NO_CITY &&
                        cities_.count(n.city) &&
                        cities_.at(n.city).owner == actorKingdom.id) {
                        pressure += 8;
                    }
                }
                return pressure;
            };

            struct LocalFrontCandidate {
                CityID city = NO_CITY;
                float score = 0.0f;
                float distance = 0.0f;
                int pressure = 0;
            };
            std::vector<LocalFrontCandidate> localFronts;
            if (kingdoms_.count(plan.enemy)) {
                const Kingdom& enemy = kingdoms_.at(plan.enemy);
                for (CityID cid : enemy.cities) {
                    if (!cities_.count(cid)) continue;
                    const City& city = cities_.at(cid);
                    if (city.owner != plan.enemy || city.tile == NO_TILE) continue;
                    if (city.isCapital &&
                        enemy.cities.size() > 1 &&
                        actorKingdom.currentWarGoal.type != WarGoalType::Capital &&
                        actorKingdom.currentWarGoal.type != WarGoalType::TotalConquest) {
                        continue;
                    }

                    const float dist = nearestOwnedCityDistanceForPlan(city.tile);
                    const int pressure = adjacentOwnedPressureForPlan(city.tile);
                    float score = dist;
                    score += std::max(0.0f, dist - 20.0f) * 2.4f;
                    score -= std::min(36.0f, static_cast<float>(pressure) * 1.7f);
                    if (!city.isCapital &&
                        (city.cityType == CityType::Generic ||
                         city.cityType == CityType::Agricultural)) {
                        score -= 18.0f;
                    }
                    if (city.cityType == CityType::Fortress ||
                        city.cityType == CityType::Port ||
                        city.cityType == CityType::TradeHub) {
                        score -= dist <= 24.0f ? 16.0f : 4.0f;
                    }
                    if (city.isCapital && dist > 18.0f) score += 38.0f;
                    localFronts.push_back({cid, score, dist, pressure});
                }
                std::sort(localFronts.begin(), localFronts.end(),
                          [](const LocalFrontCandidate& a, const LocalFrontCandidate& b) {
                              return a.score < b.score;
                          });
            }

            bool frontlineAdjusted = false;
            const CampaignPlan& prevPlan = actorKingdom.campaignPlan;
            const bool prevValid = prevPlan.active() &&
                prevPlan.enemy == plan.enemy &&
                prevPlan.primaryObjective != NO_CITY &&
                cities_.count(prevPlan.primaryObjective) &&
                cities_.at(prevPlan.primaryObjective).owner == plan.enemy;

            // Fix 2: siege lock — never pull armies off a city they are actively besieging
            if (prevValid && cities_.at(prevPlan.primaryObjective).underSiege) {
                plan.primaryObjective = prevPlan.primaryObjective;
                if (prevPlan.stagingCity != NO_CITY &&
                    cities_.count(prevPlan.stagingCity) &&
                    cities_.at(prevPlan.stagingCity).owner == actorKingdom.id) {
                    plan.stagingCity = prevPlan.stagingCity;
                }
            } else if (!localFronts.empty()) {
                // Fix 1: compare against previous plan's primary (not the raw war-goal reset)
                // so the comparison baseline is stable across turns
                const CityID oldPrimary = prevValid ? prevPlan.primaryObjective
                                                    : plan.primaryObjective;
                const LocalFrontCandidate& bestLocal = localFronts.front();

                float oldPrimaryDist = 1e9f;
                if (oldPrimary != NO_CITY && cities_.count(oldPrimary)) {
                    oldPrimaryDist = nearestOwnedCityDistanceForPlan(
                        cities_.at(oldPrimary).tile);
                }

                // Widen the distance margin (16 vs old 8) to resist casual flipping
                const bool currentTooFar =
                    oldPrimary == NO_CITY ||
                    oldPrimaryDist > bestLocal.distance + 16.0f ||
                    (cities_.count(oldPrimary) && cities_.at(oldPrimary).isCapital &&
                     bestLocal.distance <= 18.0f);
                // Require meaningful own-territory pressure AND a real distance gain
                // (removed the old `|| pressure > 0` which fired on every border city)
                const bool clearlyBetter =
                    bestLocal.pressure >= 4 &&
                    bestLocal.distance < oldPrimaryDist - 6.0f;

                if (currentTooFar || clearlyBetter) {
                    const CityID warGoalPrimary = plan.primaryObjective;
                    plan.primaryObjective = bestLocal.city;
                    if (warGoalPrimary != NO_CITY && warGoalPrimary != plan.primaryObjective) {
                        plan.secondaryObjective = warGoalPrimary;
                    }
                    if (cities_.count(plan.primaryObjective)) {
                        plan.stagingCity = nearestOwnedCity(
                            cities_.at(plan.primaryObjective).tile);
                    }
                    frontlineAdjusted = true;
                } else if (prevValid) {
                    // Carry forward previous primary to prevent war-goal baseline churn
                    plan.primaryObjective = prevPlan.primaryObjective;
                    if (prevPlan.stagingCity != NO_CITY &&
                        cities_.count(prevPlan.stagingCity) &&
                        cities_.at(prevPlan.stagingCity).owner == actorKingdom.id) {
                        plan.stagingCity = prevPlan.stagingCity;
                    }
                }
            }

            auto addFront = [&](CampaignFront front) {
                if (front.objective == NO_CITY || !cities_.count(front.objective)) return;
                if (front.stagingCity == NO_CITY) {
                    front.stagingCity = front.defensive
                        ? front.objective
                        : nearestOwnedCity(cities_.at(front.objective).tile);
                }
                auto existing = std::find_if(plan.fronts.begin(), plan.fronts.end(),
                    [&](const CampaignFront& f) {
                        return f.objective == front.objective && f.defensive == front.defensive;
                    });
                if (existing != plan.fronts.end()) {
                    existing->priority = std::max(existing->priority, front.priority);
                    existing->threat = std::max(existing->threat, front.threat);
                    existing->opportunity = std::max(existing->opportunity, front.opportunity);
                    return;
                }
                plan.fronts.push_back(front);
            };

            auto opportunityFor = [&](CityID cid) {
                if (cid == NO_CITY || !cities_.count(cid)) return 0.0f;
                const City& city = cities_.at(cid);
                float score = 18.0f;
                if (city.isCapital) score += 30.0f;
                if (city.cityType == CityType::Fortress) score += 16.0f;
                if (city.cityType == CityType::TradeHub || city.cityType == CityType::Port) score += 12.0f;
                if (city.cityType == CityType::Mining) score += 10.0f;
                score += std::min(16.0f, static_cast<float>(city.population) * 0.012f);
                if (std::find(actorKingdom.currentWarGoal.targetCities.begin(),
                              actorKingdom.currentWarGoal.targetCities.end(),
                              cid) != actorKingdom.currentWarGoal.targetCities.end()) {
                    score += 22.0f;
                }
                if (plan.supportAlly) score += 8.0f;
                if (plan.stagingCity != NO_CITY && cities_.count(plan.stagingCity)) {
                    score -= std::min(20.0f,
                        tileDistance(cities_.at(plan.stagingCity).tile, city.tile) * 0.35f);
                }
                return score;
            };

            if (plan.primaryObjective != NO_CITY && cities_.count(plan.primaryObjective)) {
                const float opportunity = opportunityFor(plan.primaryObjective);
                addFront({plan.primaryObjective, plan.stagingCity, plan.enemy,
                          62.0f + opportunity, 0.0f, opportunity, 0.0f, false});
            }
            size_t localAdded = 0;
            for (const LocalFrontCandidate& local : localFronts) {
                if (local.city == plan.primaryObjective || local.city == plan.secondaryObjective) continue;
                if (local.distance > 28.0f && local.pressure <= 0) continue;
                const float opportunity = opportunityFor(local.city);
                addFront({local.city, NO_CITY, plan.enemy,
                          54.0f + opportunity * 0.65f - local.score * 0.25f,
                          0.0f, opportunity, 0.0f, false});
                if (++localAdded >= 2) break;
            }
            if (plan.secondaryObjective != NO_CITY &&
                plan.secondaryObjective != plan.primaryObjective &&
                cities_.count(plan.secondaryObjective)) {
                const float opportunity = opportunityFor(plan.secondaryObjective);
                addFront({plan.secondaryObjective, NO_CITY, plan.enemy,
                          34.0f + opportunity * 0.75f, 0.0f, opportunity, 0.0f, false});
            }

            CityID threatenedCity = NO_CITY;
            KingdomID threateningEnemy = plan.enemy;
            float bestThreat = 0.0f;
            for (CityID cid : actorKingdom.cities) {
                if (!cities_.count(cid)) continue;
                const City& city = cities_.at(cid);
                if (city.owner != actorKingdom.id || city.tile == NO_TILE) continue;
                const bool vitalCity = city.isCapital ||
                    city.cityType == CityType::Fortress ||
                    city.cityType == CityType::TradeHub ||
                    city.cityType == CityType::Port ||
                    city.cityType == CityType::Mining ||
                    city.population >= 900;
                if (!vitalCity) continue;

                float threat = 0.0f;
                KingdomID nearestEnemy = plan.enemy;
                for (const auto& [enemyAid, enemyArmy] : armies_) {
                    (void)enemyAid;
                    if (enemyArmy.owner == actorKingdom.id) continue;
                    if (!kingdoms_.count(enemyArmy.owner) || !kingdoms_.at(enemyArmy.owner).isAlive) continue;
                    const DiplomaticRelation* rel = relationBetween(actorKingdom.id, enemyArmy.owner);
                    if (!rel || rel->state != RelationState::War) continue;
                    if (enemyArmy.currentTile == NO_TILE ||
                        enemyArmy.currentTile >= static_cast<TileID>(worldMap_.tileCount())) continue;
                    float dist = tileDistance(city.tile, enemyArmy.currentTile);
                    if (dist > 30.0f) continue;
                    threat += (30.0f - dist) * 1.2f;
                    threat += static_cast<float>(enemyArmy.totalSoldiers()) * 0.003f;
                    if (worldMap_.at(enemyArmy.currentTile).owner == actorKingdom.id) threat += 16.0f;
                    nearestEnemy = enemyArmy.owner;
                }
                if (city.isCapital) threat += 10.0f;
                if (city.cityType == CityType::Fortress) threat += 6.0f;
                if (threat > bestThreat) {
                    bestThreat = threat;
                    threatenedCity = cid;
                    threateningEnemy = nearestEnemy;
                }
            }
            if (threatenedCity != NO_CITY && bestThreat >= 18.0f) {
                addFront({threatenedCity, threatenedCity, threateningEnemy,
                          46.0f + bestThreat, bestThreat, 0.0f, 0.0f, true});
            }

            std::sort(plan.fronts.begin(), plan.fronts.end(),
                      [](const CampaignFront& a, const CampaignFront& b) {
                          return a.priority > b.priority;
                      });
            if (plan.fronts.size() > 3) plan.fronts.resize(3);
            float totalFrontPriority = 0.0f;
            for (const CampaignFront& front : plan.fronts) {
                totalFrontPriority += std::max(1.0f, front.priority);
            }
            for (CampaignFront& front : plan.fronts) {
                front.desiredShare = totalFrontPriority > 0.0f
                    ? std::max(1.0f, front.priority) / totalFrontPriority
                    : 1.0f / static_cast<float>(std::max<size_t>(1, plan.fronts.size()));
                if (front.defensive) {
                    front.desiredShare = std::clamp(front.desiredShare, 0.25f, 0.48f);
                } else if (front.objective == plan.primaryObjective) {
                    front.desiredShare = std::max(front.desiredShare, plan.primaryCommitShare);
                }
            }

            std::string phase = std::string(campaignPhaseName(plan.phase));
            std::string goal = std::string(warGoalTypeName(actorKingdom.currentWarGoal.type));
            if (plan.primaryObjective != NO_CITY && cities_.count(plan.primaryObjective)) {
                plan.reason = phase + ": " + goal + " targeting " +
                    cities_.at(plan.primaryObjective).name;
            } else {
                plan.reason = phase + ": " + goal;
            }
            if (plan.supportAlly) plan.reason += " with ally support";
            if (actorKingdom.recentDefeats > 0 &&
                currentTurn_ <= actorKingdom.lastDefeatTurn + 90) {
                plan.reason += "; cautious after recent defeat";
            }
            if (frontlineAdjusted) plan.reason += "; frontline adjusted";
            if (plan.diversion) plan.reason += "; secondary diversion ready";
            if (plan.concentratedAssault) plan.reason += "; concentrating main force";
            if (plan.fronts.size() >= 2) {
                plan.reason += "; " + std::to_string(plan.fronts.size()) + " fronts";
            }
            if (std::any_of(plan.fronts.begin(), plan.fronts.end(),
                            [](const CampaignFront& f) { return f.defensive; })) {
                plan.reason += "; defensive front held";
            }

            actorKingdom.aiReason = plan.reason;
            actorKingdom.campaignPlan = std::move(plan);
        };

        if (warEnemy != NO_KINGDOM) {
            chooseCampaignPlan(k);
        }

        auto kingdomFieldPower = [&](KingdomID owner) {
            float power = 0.0f;
            if (!kingdoms_.count(owner)) return power;
            const Kingdom& ownerKingdom = kingdoms_.at(owner);
            for (ArmyID aid : ownerKingdom.armies) {
                auto ait = armies_.find(aid);
                if (ait == armies_.end() || ait->second.isEmpty()) continue;
                const Army& army = ait->second;
                float supply = std::clamp(army.supplyLevel, 0.25f, 1.15f);
                power += static_cast<float>(army.totalSoldiers()) * supply;
            }
            return power;
        };

        auto capturedCitiesFrom = [&](KingdomID owner, KingdomID formerOwner) {
            int count = 0;
            if (!kingdoms_.count(owner)) return count;
            for (CityID cid : kingdoms_.at(owner).cities) {
                if (!cities_.count(cid)) continue;
                const City& city = cities_.at(cid);
                if (city.owner == owner && city.originalOwner == formerOwner) ++count;
            }
            return count;
        };

        auto shouldOfferObjectivePeace = [&](const Kingdom& actorKingdom, KingdomID enemyId) {
            if (enemyId == NO_KINGDOM || !kingdoms_.count(enemyId)) return false;
            const Kingdom& enemy = kingdoms_.at(enemyId);
            if (!enemy.isAlive) return false;
            if (actorKingdom.policy == NationalPolicy::FinalWar ||
                actorKingdom.strategyPlan == StrategyPlan::TotalConquest ||
                enemy.policy == NationalPolicy::FinalWar) {
                return false;
            }

            const DiplomaticRelation* rel = relationBetween(actorKingdom.id, enemyId);
            if (!rel || rel->state != RelationState::War || rel->turnsAtWar <= 25) return false;

            const int captured = capturedCitiesFrom(actorKingdom.id, enemyId);
            const int lost = capturedCitiesFrom(enemyId, actorKingdom.id);
            const float ourPower = kingdomFieldPower(actorKingdom.id) +
                static_cast<float>(actorKingdom.cities.size()) * 220.0f;
            const float enemyPower = kingdomFieldPower(enemyId) +
                static_cast<float>(enemy.cities.size()) * 220.0f;
            const float powerRatio = ourPower / std::max(1.0f, enemyPower);

            float peaceNeed = 0.72f;
            if (actorKingdom.personality == KingdomPersonality::Economic ||
                actorKingdom.personality == KingdomPersonality::Diplomatic) {
                peaceNeed = 0.42f;
            } else if (actorKingdom.personality == KingdomPersonality::Defensive) {
                peaceNeed = 0.50f;
            } else if (actorKingdom.personality == KingdomPersonality::Opportunistic) {
                peaceNeed = 0.56f;
            } else if (actorKingdom.personality == KingdomPersonality::Aggressive ||
                       actorKingdom.personality == KingdomPersonality::Expansionist) {
                peaceNeed = 0.76f;
            }

            const bool limitedGainAchieved =
                captured >= 2 ||
                (captured >= 1 &&
                 (actorKingdom.strategyPlan == StrategyPlan::BorderExpansion ||
                  actorKingdom.strategyPlan == StrategyPlan::OpportunisticRaid));
            bool warGoalAchieved = false;
            if (actorKingdom.currentWarGoal.active() &&
                actorKingdom.currentWarGoal.enemy == enemyId &&
                actorKingdom.currentWarGoal.allowPeaceAfterGoal) {
                int heldTargets = 0;
                for (CityID cid : actorKingdom.currentWarGoal.targetCities) {
                    if (cities_.count(cid) && cities_.at(cid).owner == actorKingdom.id) {
                        ++heldTargets;
                    }
                }
                warGoalAchieved =
                    heldTargets >= actorKingdom.currentWarGoal.desiredCaptures ||
                    (actorKingdom.currentWarGoal.type == WarGoalType::DefensiveHold &&
                     rel->turnsAtWar >= actorKingdom.currentWarGoal.maxTurns &&
                     lost == 0);
            }
            const bool revengeSatisfied =
                actorKingdom.strategyPlan == StrategyPlan::RevengeWar &&
                captured > lost;
            const bool badWar =
                lost > captured &&
                (actorKingdom.warWeariness > 0.45f || powerRatio < 0.72f);
            const bool longExhaustingWar =
                rel->turnsAtWar > std::max<TurnNumber>(90, actorKingdom.currentWarGoal.maxTurns) &&
                (actorKingdom.warWeariness > 0.45f || powerRatio < 0.90f);

            if (warGoalAchieved &&
                (actorKingdom.warWeariness > 0.28f ||
                 actorKingdom.personality == KingdomPersonality::Economic ||
                 actorKingdom.personality == KingdomPersonality::Diplomatic)) {
                return true;
            }
            if ((limitedGainAchieved || revengeSatisfied) &&
                actorKingdom.warWeariness >= peaceNeed) {
                return true;
            }
            if (badWar || longExhaustingWar) return true;
            return false;
        };

        if (warEnemy != NO_KINGDOM && shouldOfferObjectivePeace(k, warEnemy)) {
            AIDecision peace;
            peace.type = AIDecisionType::NegotiatePeace;
            peace.actor = kid;
            peace.target = warEnemy;
            executeDecision(peace);
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
            CityID attackCity = NO_CITY;
            if (k.campaignPlan.active() &&
                k.campaignPlan.enemy == warEnemy &&
                k.campaignPlan.primaryObjective != NO_CITY &&
                cities_.count(k.campaignPlan.primaryObjective) &&
                cities_.at(k.campaignPlan.primaryObjective).owner == warEnemy) {
                attackCity = k.campaignPlan.primaryObjective;
            } else if (k.currentWarGoal.active() &&
                       k.currentWarGoal.enemy == warEnemy) {
                auto target = std::find_if(
                    k.currentWarGoal.targetCities.begin(),
                    k.currentWarGoal.targetCities.end(),
                    [&](CityID cid) {
                        return cities_.count(cid) &&
                               cities_.at(cid).owner == warEnemy;
                    });
                if (target != k.currentWarGoal.targetCities.end()) {
                    attackCity = *target;
                }
            }
            if (attackCity == NO_CITY) attackCity = enemy.capitalCity;
            if (attackCity != NO_CITY) {
                AIDecision attack;
                attack.type = AIDecisionType::Attack;
                attack.actor = kid;
                attack.target = warEnemy;
                attack.targetCity = attackCity;
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
                    bool finalWar = ak.policy == NationalPolicy::FinalWar ||
                                    kingdoms_.at(d.target).policy == NationalPolicy::FinalWar;
                    if (!hasRevenge &&
                        !finalWar &&
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
                if (army.recoveringSupply &&
                    army.supplyLevel >= 0.86f &&
                    currentTurn_ >= army.supplyRetreatUntil) {
                    army.recoveringSupply = false;
                }
                const float retreatSupply = actor.campaignPlan.active()
                    ? actor.campaignPlan.retreatSupply : 0.52f;
                if (army.supplyLevel >= retreatSupply) continue;
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
                    army.recoveringSupply = true;
                    army.supplyRetreatUntil = std::max(army.supplyRetreatUntil, currentTurn_ + 18);
                }
            }

            std::vector<ArmyID> attackArmies;
            for (ArmyID eid : ownArmies) {
                const Army& army = armies_.at(eid);
                if (army.recoveringSupply &&
                    (army.supplyLevel < 0.86f || currentTurn_ < army.supplyRetreatUntil) &&
                    actor.policy != NationalPolicy::FinalWar) {
                    continue;
                }
                const float minAttackSupply = actor.campaignPlan.active()
                    ? std::max(0.46f, actor.campaignPlan.retreatSupply + 0.10f) : 0.52f;
                if (army.supplyLevel < minAttackSupply && actor.policy != NationalPolicy::FinalWar) continue;
                ArmyRole role = armies_.at(eid).role;
                if (role == ArmyRole::Attack || role == ArmyRole::Siege ||
                    actor.policy == NationalPolicy::FinalWar) {
                    attackArmies.push_back(eid);
                }
            }
            if (attackArmies.empty()) {
                for (ArmyID eid : ownArmies) {
                    if (armies_.at(eid).role != ArmyRole::Defense &&
                        !armies_.at(eid).recoveringSupply &&
                        armies_.at(eid).supplyLevel >= 0.58f) attackArmies.push_back(eid);
                }
            }
            if (actor.campaignPlan.active() &&
                actor.campaignPlan.phase == CampaignPhase::Battle &&
                attackArmies.size() < std::max<size_t>(2, ownArmies.size() / 2)) {
                for (ArmyID eid : ownArmies) {
                    if (std::find(attackArmies.begin(), attackArmies.end(), eid) != attackArmies.end()) continue;
                    const Army& army = armies_.at(eid);
                    if (army.role != ArmyRole::Reserve || army.supplyLevel < 0.64f) continue;
                    attackArmies.push_back(eid);
                    if (attackArmies.size() >= std::max<size_t>(2, ownArmies.size() / 2)) break;
                }
            }
            if (attackArmies.empty()) break;

            KingdomID enemyFilter = d.target != NO_KINGDOM ? d.target : actor.strategicTarget;
            if (actor.currentWarGoal.active() &&
                kingdoms_.count(actor.currentWarGoal.enemy) &&
                kingdoms_.at(actor.currentWarGoal.enemy).isAlive &&
                atWar(actor.id, actor.currentWarGoal.enemy)) {
                enemyFilter = actor.currentWarGoal.enemy;
            }

            auto warGoalTargetRank = [&](CityID cid) {
                if (!actor.currentWarGoal.active()) return -1;
                for (size_t i = 0; i < actor.currentWarGoal.targetCities.size(); ++i) {
                    if (actor.currentWarGoal.targetCities[i] == cid) return static_cast<int>(i);
                }
                return -1;
            };
            auto isAlliedWith = [&](KingdomID other) {
                KingdomID lo = std::min(actor.id, other);
                KingdomID hi = std::max(actor.id, other);
                auto it = relations_.find(lo);
                if (it == relations_.end()) return false;
                auto it2 = it->second.find(hi);
                return it2 != it->second.end() && it2->second.state == RelationState::Alliance;
            };

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

            if (actor.campaignPlan.active() && !actor.campaignPlan.fronts.empty()) {
                std::vector<CampaignFront> defensiveFronts;
                for (const CampaignFront& front : actor.campaignPlan.fronts) {
                    if (front.defensive &&
                        front.objective != NO_CITY &&
                        cities_.count(front.objective) &&
                        cities_.at(front.objective).owner == actor.id) {
                        defensiveFronts.push_back(front);
                    }
                }
                std::sort(defensiveFronts.begin(), defensiveFronts.end(),
                          [](const CampaignFront& a, const CampaignFront& b) {
                              return a.priority > b.priority;
                          });

                std::vector<ArmyID> assignedFrontDefense;
                const size_t defenseLimit = std::min(
                    defensiveFronts.size(),
                    std::max<size_t>(1, ownArmies.size() / 4));
                for (size_t fi = 0; fi < defenseLimit; ++fi) {
                    const CampaignFront& front = defensiveFronts[fi];
                    const TileID defendTile = cities_.at(front.objective).tile;
                    std::vector<ArmyID> candidates;
                    for (ArmyID eid : ownArmies) {
                        if (std::find(assignedFrontDefense.begin(),
                                      assignedFrontDefense.end(),
                                      eid) != assignedFrontDefense.end()) {
                            continue;
                        }
                        const Army& army = armies_.at(eid);
                        if (army.role == ArmyRole::Siege) continue;
                        if (army.supplyLevel < 0.34f) continue;
                        if (army.role == ArmyRole::Defense ||
                            army.role == ArmyRole::Reserve ||
                            attackArmies.size() >= 4) {
                            candidates.push_back(eid);
                        }
                    }
                    if (candidates.empty()) continue;
                    auto best = std::min_element(
                        candidates.begin(), candidates.end(),
                        [&](ArmyID a, ArmyID b) {
                            return distanceBetween(armies_.at(a).currentTile, defendTile) <
                                   distanceBetween(armies_.at(b).currentTile, defendTile);
                        });
                    if (best == candidates.end()) continue;

                    ArmyID aid = *best;
                    Army& army = armies_.at(aid);
                    army.role = ArmyRole::Defense;
                    army.targetTile = defendTile;
                    army.movementPath.clear();
                    army.pathCursor = 0;
                    assignedFrontDefense.push_back(aid);
                }

                if (!assignedFrontDefense.empty()) {
                    attackArmies.erase(
                        std::remove_if(attackArmies.begin(), attackArmies.end(),
                            [&](ArmyID aid) {
                                return std::find(assignedFrontDefense.begin(),
                                                 assignedFrontDefense.end(),
                                                 aid) != assignedFrontDefense.end();
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
                float forceRatio;
            };
            std::vector<TargetCandidate> targets;

            auto armyTacticalPower = [&](const Army& army, TileID objective) {
                float roleFactor = 1.0f;
                if (army.role == ArmyRole::Attack) roleFactor = 1.12f;
                else if (army.role == ArmyRole::Siege) roleFactor = 0.76f;
                else if (army.role == ArmyRole::Reserve) roleFactor = 0.88f;
                else if (army.role == ArmyRole::Defense) roleFactor = 0.68f;

                const float dist = distanceBetween(army.currentTile, objective);
                const float arrivalFactor = dist <= 6.0f ? 1.0f :
                    dist <= 12.0f ? 0.82f :
                    dist <= 22.0f ? 0.58f : 0.30f;
                return static_cast<float>(army.totalSoldiers()) *
                    std::clamp(army.supplyLevel, 0.20f, 1.10f) *
                    roleFactor * arrivalFactor;
            };

            auto adjacentOwnedPressure = [&](TileID tileId) {
                int owned = 0;
                int friendly = 0;
                for (TileID nid : worldMap_.neighbors8(tileId)) {
                    const Tile& n = worldMap_.at(nid);
                    if (n.owner == actor.id) ++owned;
                    if (n.city != NO_CITY &&
                        cities_.count(n.city) &&
                        cities_.at(n.city).owner == actor.id) {
                        friendly += 2;
                    }
                }
                return owned + friendly;
            };

            auto frontierBonus = [&](TileID tileId, float supplyBaseDist) {
                float bonus = 0.0f;
                const int adjacent = adjacentOwnedPressure(tileId);
                if (adjacent > 0) bonus += std::min(24.0f, adjacent * 4.0f);
                if (supplyBaseDist <= 10.0f) bonus += 22.0f;
                else if (supplyBaseDist <= 16.0f) bonus += 14.0f;
                else if (supplyBaseDist <= 22.0f) bonus += 6.0f;
                return bonus;
            };

            for (const auto& [enemyAid, enemyArmy] : armies_) {
                if (enemyArmy.owner == actor.id || enemyArmy.isEmpty()) continue;
                if (!kingdoms_.count(enemyArmy.owner) || !kingdoms_.at(enemyArmy.owner).isAlive) continue;
                if (enemyFilter != NO_KINGDOM && enemyArmy.owner != enemyFilter) continue;
                if (!atWar(actor.id, enemyArmy.owner)) continue;
                if (enemyArmy.currentTile == NO_TILE ||
                    enemyArmy.currentTile >= static_cast<TileID>(worldMap_.tileCount())) continue;

                const float dist = distanceBetween(leadArmy.currentTile, enemyArmy.currentTile);
                const float supplyBaseDist = nearestOwnedCityDistanceTo(actor.id, enemyArmy.currentTile);
                const float supplyPenalty = std::max(0.0f, dist - safeRange) * 2.3f;
                const float basePenalty = std::max(0.0f, supplyBaseDist - 22.0f) * 2.2f;

                float localAttackPower = 0.0f;
                for (ArmyID aid : attackArmies) {
                    if (!armies_.count(aid)) continue;
                    localAttackPower += armyTacticalPower(armies_.at(aid), enemyArmy.currentTile);
                }

                float enemyPower = static_cast<float>(enemyArmy.totalSoldiers()) *
                    std::clamp(enemyArmy.supplyLevel, 0.18f, 1.10f);
                for (const auto& [supportAid, supportArmy] : armies_) {
                    if (supportAid == enemyAid || supportArmy.owner != enemyArmy.owner) continue;
                    if (supportArmy.currentTile == NO_TILE ||
                        supportArmy.currentTile >= static_cast<TileID>(worldMap_.tileCount())) continue;
                    const float supportDist = distanceBetween(supportArmy.currentTile, enemyArmy.currentTile);
                    if (supportDist > 10.0f) continue;
                    const float supportFactor = supportDist <= 4.0f ? 0.52f :
                        supportDist <= 7.0f ? 0.34f : 0.20f;
                    enemyPower += static_cast<float>(supportArmy.totalSoldiers()) *
                        std::clamp(supportArmy.supplyLevel, 0.20f, 1.05f) * supportFactor;
                }

                const float forceRatio = localAttackPower / std::max(180.0f, enemyPower);
                float score = dist + supplyPenalty + basePenalty;
                if (worldMap_.at(enemyArmy.currentTile).owner == actor.id) score -= 42.0f;
                if (enemyArmy.supplyLevel < 0.45f) score -= 30.0f;
                if (enemyArmy.supplyLevel < 0.25f) score -= 22.0f;
                if (enemyArmy.role == ArmyRole::Siege) score -= 22.0f;
                if (enemyArmy.role == ArmyRole::Attack) score -= 8.0f;
                if (enemyArmy.totalSoldiers() >= 1400) score -= 18.0f;
                if (actor.campaignPlan.active() &&
                    actor.campaignPlan.phase == CampaignPhase::Battle) {
                    score -= 14.0f;
                }
                if (actor.personality == KingdomPersonality::Opportunistic &&
                    enemyArmy.supplyLevel < 0.60f) {
                    score -= 20.0f;
                }
                if (actor.personality == KingdomPersonality::Aggressive ||
                    actor.personality == KingdomPersonality::Expansionist) {
                    score -= 8.0f;
                }

                float requiredFieldRatio = 0.74f;
                if (actor.personality == KingdomPersonality::Defensive ||
                    actor.personality == KingdomPersonality::Economic) {
                    requiredFieldRatio = 0.88f;
                } else if (actor.personality == KingdomPersonality::Opportunistic) {
                    requiredFieldRatio = 0.62f;
                } else if (actor.personality == KingdomPersonality::Aggressive ||
                           actor.personality == KingdomPersonality::Expansionist) {
                    requiredFieldRatio = 0.68f;
                }
                if (actor.policy == NationalPolicy::FinalWar) requiredFieldRatio *= 0.84f;

                if (forceRatio < requiredFieldRatio) {
                    score += (requiredFieldRatio - forceRatio) * 92.0f;
                } else if (forceRatio > requiredFieldRatio * 1.55f) {
                    score -= std::min(22.0f, (forceRatio - requiredFieldRatio) * 12.0f);
                }

                const bool tacticallyRelevant =
                    score < 52.0f ||
                    worldMap_.at(enemyArmy.currentTile).owner == actor.id ||
                    enemyArmy.supplyLevel < 0.35f;
                if (tacticallyRelevant) {
                    targets.push_back({NO_CITY, enemyArmy.currentTile,
                                       score, supplyBaseDist, forceRatio});
                }
            }

            for (const auto& [cid, city] : cities_) {
                if (city.owner == actor.id) continue;
                const bool neutralCity = city.owner == NO_KINGDOM;
                if (enemyFilter != NO_KINGDOM && city.owner != enemyFilter && !neutralCity) continue;
                if (!neutralCity) {
                    if (!kingdoms_.count(city.owner) || !kingdoms_.at(city.owner).isAlive) continue;
                    if (!atWar(actor.id, city.owner)) continue;
                    if (city.isCapital && kingdoms_.at(city.owner).cities.size() > 1) continue;
                }
                auto cp = worldMap_.at(city.tile).position;
                float dist = std::hypot(float(leadPos.x - cp.x), float(leadPos.y - cp.y));
                // Penalise targets beyond supply range — prefer stepping-stone cities first
                float supplyPenalty = std::max(0.0f, dist - safeRange) * 2.5f;
                float supplyBaseDist = nearestOwnedCityDistanceTo(actor.id, city.tile);
                float basePenalty = std::max(0.0f, supplyBaseDist - 18.0f) * 4.0f;
                basePenalty += std::pow(std::max(0.0f, supplyBaseDist - 26.0f), 2.0f) * 0.16f;
                float score = dist + supplyPenalty;
                score += basePenalty;
                score -= frontierBonus(city.tile, supplyBaseDist);
                const int goalRank = warGoalTargetRank(cid);
                if (goalRank >= 0) {
                    score -= 55.0f - static_cast<float>(goalRank) * 8.0f;
                } else if (actor.currentWarGoal.active() &&
                           actor.currentWarGoal.type != WarGoalType::TotalConquest) {
                    score += 18.0f;
                }
                if (actor.campaignPlan.active()) {
                    if (cid == actor.campaignPlan.primaryObjective) score -= 34.0f;
                    if (actor.campaignPlan.diversion &&
                        cid == actor.campaignPlan.secondaryObjective) score -= 20.0f;
                    if (actor.campaignPlan.phase == CampaignPhase::Siege &&
                        city.cityType == CityType::Fortress) score -= 10.0f;
                    for (const CampaignFront& front : actor.campaignPlan.fronts) {
                        if (front.defensive || front.objective != cid) continue;
                        score -= 28.0f + front.desiredShare * 35.0f;
                        if (front.stagingCity != NO_CITY && cities_.count(front.stagingCity)) {
                            const float frontDist =
                                distanceBetween(cities_.at(front.stagingCity).tile, city.tile);
                            score += std::max(0.0f, frontDist - 26.0f) * 0.8f;
                        }
                    }
                }

                const bool supplyHub =
                    city.cityType == CityType::Fortress ||
                    city.cityType == CityType::TradeHub ||
                    city.cityType == CityType::Port;
                const bool localTown =
                    !city.isCapital &&
                    (city.cityType == CityType::Generic ||
                     city.cityType == CityType::Agricultural);
                if (city.cityType == CityType::Fortress) score -= 22.0f;
                if (city.cityType == CityType::TradeHub) score -= 16.0f;
                if (city.cityType == CityType::Port) score -= 14.0f;
                if (localTown) {
                    float townValue = 10.0f;
                    townValue += std::min(12.0f, static_cast<float>(city.population) * 0.010f);
                    if (city.cityType == CityType::Agricultural) townValue += 6.0f;
                    if (supplyBaseDist <= 14.0f) townValue += 24.0f;
                    else if (supplyBaseDist <= 24.0f) townValue += 12.0f;
                    score -= townValue;
                }
                if (neutralCity) {
                    score -= 24.0f;
                    if (supplyBaseDist <= 20.0f) score -= 18.0f;
                }
                if (supplyHub && supplyBaseDist <= 26.0f) score -= 24.0f;
                if (!supplyHub && supplyBaseDist > 24.0f) score += 26.0f;
                if (city.isCapital && supplyBaseDist > 18.0f) score += 34.0f;
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

                auto armyAssaultPower = [&](ArmyID aid) {
                    const Army& army = armies_.at(aid);
                    float roleFactor = 1.0f;
                    if (army.role == ArmyRole::Siege) roleFactor = 1.18f;
                    else if (army.role == ArmyRole::Attack) roleFactor = 1.08f;
                    else if (army.role == ArmyRole::Reserve) roleFactor = 0.82f;
                    else if (army.role == ArmyRole::Defense) roleFactor = 0.60f;

                    const float supplyFactor = std::clamp(army.supplyLevel, 0.20f, 1.12f);
                    const float targetDist = distanceBetween(army.currentTile, city.tile);
                    const float arrivalFactor = targetDist <= 10.0f ? 1.0f :
                        targetDist <= 20.0f ? 0.82f :
                        targetDist <= 32.0f ? 0.58f : 0.32f;
                    return static_cast<float>(army.totalSoldiers()) *
                        supplyFactor * roleFactor * arrivalFactor;
                };

                float localAttackPower = 0.0f;
                for (ArmyID aid : attackArmies) {
                    if (!armies_.count(aid)) continue;
                    localAttackPower += armyAssaultPower(aid);
                }
                float alliedSupportPower = 0.0f;
                for (const auto& [allyAid, allyArmy] : armies_) {
                    if (allyArmy.owner == actor.id) continue;
                    if (!isAlliedWith(allyArmy.owner)) continue;
                    if (!kingdoms_.count(allyArmy.owner) || !kingdoms_.at(allyArmy.owner).isAlive) continue;
                    if (!atWar(allyArmy.owner, city.owner)) continue;
                    if (allyArmy.currentTile == NO_TILE ||
                        allyArmy.currentTile >= static_cast<TileID>(worldMap_.tileCount())) continue;
                    float allyDist = distanceBetween(allyArmy.currentTile, city.tile);
                    if (allyDist > 24.0f) continue;
                    const float supportFactor = allyDist <= 10.0f ? 0.62f :
                        allyDist <= 18.0f ? 0.42f : 0.24f;
                    alliedSupportPower += static_cast<float>(allyArmy.totalSoldiers()) *
                        std::clamp(allyArmy.supplyLevel, 0.25f, 1.05f) * supportFactor;
                }
                localAttackPower += alliedSupportPower;
                if (alliedSupportPower > 0.0f) score -= std::min(18.0f, alliedSupportPower * 0.006f);

                float localDefensePower = 80.0f +
                    static_cast<float>(city.population) * 0.18f +
                    city.fortification * 260.0f;
                if (neutralCity) localDefensePower *= 0.35f;
                if (city.isCapital) localDefensePower += 420.0f;
                if (city.cityType == CityType::Fortress) localDefensePower += 360.0f;
                if (city.cityType == CityType::TradeHub ||
                    city.cityType == CityType::Port) localDefensePower += 160.0f;

                for (const auto& [enemyAid, enemyArmy] : armies_) {
                    if (enemyArmy.owner != city.owner) continue;
                    if (enemyArmy.currentTile == NO_TILE ||
                        enemyArmy.currentTile >= static_cast<TileID>(worldMap_.tileCount())) continue;
                    const float armyDist = distanceBetween(enemyArmy.currentTile, city.tile);
                    if (armyDist > 18.0f) continue;
                    const float reactionFactor = armyDist <= 8.0f ? 1.0f :
                        armyDist <= 14.0f ? 0.70f : 0.45f;
                    localDefensePower += static_cast<float>(enemyArmy.totalSoldiers()) *
                        std::clamp(enemyArmy.supplyLevel, 0.25f, 1.10f) * reactionFactor;
                }

                const float forceRatio =
                    localAttackPower / std::max(120.0f, localDefensePower);
                if (localTown && forceRatio >= 1.0f) {
                    score -= 8.0f;
                }
                float requiredRatio = 0.80f;
                if (actor.personality == KingdomPersonality::Aggressive ||
                    actor.personality == KingdomPersonality::Expansionist) {
                    requiredRatio = 0.70f;
                } else if (actor.personality == KingdomPersonality::Defensive ||
                           actor.personality == KingdomPersonality::Economic) {
                    requiredRatio = 0.92f;
                } else if (actor.personality == KingdomPersonality::Opportunistic) {
                    requiredRatio = 0.66f;
                }
                if (actor.currentWarGoal.active()) {
                    requiredRatio = (requiredRatio + actor.currentWarGoal.requiredForceRatio) * 0.5f;
                }
                if (actor.campaignPlan.active()) {
                    requiredRatio = (requiredRatio + actor.campaignPlan.commitThreshold) * 0.5f;
                    if (actor.campaignPlan.supportAlly && alliedSupportPower > 0.0f) {
                        requiredRatio -= 0.06f;
                    }
                }
                if (actor.policy == NationalPolicy::FinalWar) requiredRatio *= 0.82f;

                if (forceRatio < requiredRatio) {
                    score += (requiredRatio - forceRatio) * 95.0f;
                    if (city.isCapital || city.cityType == CityType::Fortress) score += 26.0f;
                } else if (forceRatio > requiredRatio * 1.65f) {
                    score -= std::min(20.0f, (forceRatio - requiredRatio) * 10.0f);
                }

                targets.push_back({cid, city.tile, score, supplyBaseDist, forceRatio});
            }

            for (const Tile& pointTile : worldMap_.tiles()) {
                if (pointTile.strategicPoint == StrategicPointType::None) continue;
                if (pointTile.city != NO_CITY) continue;
                if (pointTile.terrain == TerrainType::Ocean ||
                    pointTile.terrain == TerrainType::Lake) continue;
                if (pointTile.owner == actor.id) continue;
                if (enemyFilter != NO_KINGDOM &&
                    pointTile.owner != enemyFilter &&
                    pointTile.owner != NO_KINGDOM) {
                    continue;
                }
                if (pointTile.owner != NO_KINGDOM &&
                    !atWar(actor.id, pointTile.owner)) {
                    continue;
                }

                const auto pointPos = pointTile.position;
                const float dist = std::hypot(float(leadPos.x - pointPos.x),
                                              float(leadPos.y - pointPos.y));
                const float supplyPenalty = std::max(0.0f, dist - safeRange) * 2.2f;
                const float supplyBaseDist = nearestOwnedCityDistanceTo(actor.id, pointTile.id);
                const float basePenalty = std::max(0.0f, supplyBaseDist - 18.0f) * 3.4f;
                float value = pointTile.strategicValue;
                switch (pointTile.strategicPoint) {
                    case StrategicPointType::MountainPass: value += 16.0f; break;
                    case StrategicPointType::Bridge:       value += 14.0f; break;
                    case StrategicPointType::RiverFord:    value += 10.0f; break;
                    case StrategicPointType::HarborSite:   value += 13.0f; break;
                    case StrategicPointType::SupplyDepot:  value += 18.0f; break;
                    case StrategicPointType::None: break;
                }
                if (supplyBaseDist <= 12.0f) value += 26.0f;
                else if (supplyBaseDist <= 22.0f) value += 14.0f;
                if (pointTile.owner == NO_KINGDOM) value += 7.0f;

                float enemyPressure = 0.0f;
                for (const auto& [enemyAid, enemyArmy] : armies_) {
                    (void)enemyAid;
                    if (enemyArmy.owner == actor.id) continue;
                    if (pointTile.owner != NO_KINGDOM && enemyArmy.owner != pointTile.owner) continue;
                    if (enemyArmy.currentTile == NO_TILE ||
                        enemyArmy.currentTile >= static_cast<TileID>(worldMap_.tileCount())) continue;
                    float armyDist = distanceBetween(enemyArmy.currentTile, pointTile.id);
                    if (armyDist > 12.0f) continue;
                    enemyPressure += static_cast<float>(enemyArmy.totalSoldiers()) *
                        std::clamp(enemyArmy.supplyLevel, 0.25f, 1.0f) *
                        (armyDist <= 5.0f ? 0.004f : 0.002f);
                }

                float score = dist + supplyPenalty + basePenalty - value + enemyPressure;
                score -= frontierBonus(pointTile.id, supplyBaseDist);
                score += std::pow(std::max(0.0f, supplyBaseDist - 26.0f), 2.0f) * 0.12f;
                const float forceRatio = enemyPressure > 0.0f
                    ? static_cast<float>(attackArmies.size()) * 650.0f / std::max(120.0f, enemyPressure * 110.0f)
                    : 1.25f;
                if (forceRatio < 0.75f) score += 32.0f;
                targets.push_back({NO_CITY, pointTile.id, score, supplyBaseDist, forceRatio});
            }
            std::sort(targets.begin(), targets.end(),
                      [](const TargetCandidate& a, const TargetCandidate& b) {
                          return a.score < b.score;
                      });

            float bestFrontierScore = 1e9f;
            for (const TargetCandidate& t : targets) {
                if (t.supplyDistance <= 22.0f) {
                    bestFrontierScore = std::min(bestFrontierScore, t.score);
                }
            }
            if (bestFrontierScore < 85.0f) {
                targets.erase(
                    std::remove_if(targets.begin(), targets.end(),
                        [&](const TargetCandidate& t) {
                            if (t.supplyDistance <= 30.0f) return false;
                            if (t.forceRatio >= 1.8f && t.score < bestFrontierScore - 18.0f) return false;
                            return t.score > bestFrontierScore - 6.0f;
                        }),
                    targets.end());
            }

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
                                       supplyBaseDist, supplyBaseDist, 1.0f});
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

            struct FrontTargetChoice {
                size_t targetIndex = 0;
                float desiredShare = 0.0f;
                bool primary = false;
            };
            std::vector<FrontTargetChoice> frontTargetChoices;
            if (actor.campaignPlan.active() && !actor.campaignPlan.fronts.empty()) {
                for (const CampaignFront& front : actor.campaignPlan.fronts) {
                    if (front.defensive || front.objective == NO_CITY) continue;
                    for (size_t ti = 0; ti < targets.size(); ++ti) {
                        if (targets[ti].city != front.objective) continue;
                        const bool primary = front.objective == actor.campaignPlan.primaryObjective;
                        frontTargetChoices.push_back({
                            ti,
                            std::max(0.08f, front.desiredShare),
                            primary
                        });
                        break;
                    }
                }
                std::sort(frontTargetChoices.begin(), frontTargetChoices.end(),
                          [](const FrontTargetChoice& a, const FrontTargetChoice& b) {
                              if (a.primary != b.primary) return a.primary;
                              return a.desiredShare > b.desiredShare;
                          });
                float shareTotal = 0.0f;
                for (const FrontTargetChoice& choice : frontTargetChoices) {
                    shareTotal += choice.desiredShare;
                }
                if (shareTotal > 0.0f) {
                    for (FrontTargetChoice& choice : frontTargetChoices) {
                        choice.desiredShare /= shareTotal;
                    }
                }
                if (actor.campaignPlan.primaryObjective != NO_CITY) {
                    auto primary = std::find_if(
                        frontTargetChoices.begin(), frontTargetChoices.end(),
                        [&](const FrontTargetChoice& choice) {
                            return choice.primary;
                        });
                    if (primary != frontTargetChoices.end()) {
                        primary->desiredShare = std::max(
                            primary->desiredShare,
                            actor.campaignPlan.primaryCommitShare);
                        float otherTotal = 0.0f;
                        for (const FrontTargetChoice& choice : frontTargetChoices) {
                            if (&choice != &(*primary)) otherTotal += choice.desiredShare;
                        }
                        const float remaining = std::max(0.0f, 1.0f - primary->desiredShare);
                        if (otherTotal > 0.0f) {
                            for (FrontTargetChoice& choice : frontTargetChoices) {
                                if (&choice == &(*primary)) continue;
                                choice.desiredShare = choice.desiredShare / otherTotal * remaining;
                            }
                        }
                    }
                }
            }

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
            auto choosePlannedFrontTarget = [&](size_t slot, size_t total, bool siege) {
                if (frontTargetChoices.empty()) return size_t{0};
                if (siege) {
                    auto primary = std::find_if(
                        frontTargetChoices.begin(), frontTargetChoices.end(),
                        [](const FrontTargetChoice& choice) { return choice.primary; });
                    return primary != frontTargetChoices.end()
                        ? primary->targetIndex
                        : frontTargetChoices.front().targetIndex;
                }
                const float position = (static_cast<float>(slot) + 0.5f) /
                    static_cast<float>(std::max<size_t>(1, total));
                float cursor = 0.0f;
                for (const FrontTargetChoice& choice : frontTargetChoices) {
                    cursor += choice.desiredShare;
                    if (position <= cursor) return choice.targetIndex;
                }
                return frontTargetChoices.back().targetIndex;
            };

            for (size_t i = 0; i < attackArmies.size(); ++i) {
                ArmyID eid = attackArmies[i];
                const bool siege = armies_.at(eid).role == ArmyRole::Siege ||
                    (!assignedCityAssault && i == 0);
                size_t targetIndex = siege ? 0u : mobileIndex++ % frontCount;
                if (!frontTargetChoices.empty() &&
                    (attackArmies.size() >= 3 || siege)) {
                    targetIndex = choosePlannedFrontTarget(i, attackArmies.size(), siege);
                    if (actor.campaignPlan.concentratedAssault &&
                        !siege &&
                        attackArmies.size() >= 2) {
                        auto primary = std::find_if(
                            frontTargetChoices.begin(), frontTargetChoices.end(),
                            [](const FrontTargetChoice& choice) { return choice.primary; });
                        const size_t primarySlots = std::max<size_t>(
                            1,
                            static_cast<size_t>(std::ceil(
                                static_cast<float>(attackArmies.size()) *
                                actor.campaignPlan.primaryCommitShare)));
                        if (primary != frontTargetChoices.end() && i < primarySlots) {
                            targetIndex = primary->targetIndex;
                        }
                    }
                } else if (!siege &&
                    actor.campaignPlan.active() &&
                    actor.campaignPlan.diversion &&
                    actor.campaignPlan.secondaryObjective != NO_CITY &&
                    attackArmies.size() >= 4 &&
                    i >= attackArmies.size() / 2) {
                    for (size_t ti = 0; ti < targets.size(); ++ti) {
                        if (targets[ti].city == actor.campaignPlan.secondaryObjective) {
                            targetIndex = ti;
                            break;
                        }
                    }
                }
                const bool fieldBattleOpportunity =
                    !targets.empty() &&
                    targets.front().city == NO_CITY &&
                    targets.front().forceRatio >= 0.62f &&
                    targets.front().score < 48.0f;
                if (!siege && fieldBattleOpportunity) {
                    const bool defensiveInterception =
                        worldMap_.at(targets.front().tile).owner == actor.id;
                    const size_t fieldSlots = defensiveInterception
                        ? std::max<size_t>(1, attackArmies.size() / 2)
                        : std::max<size_t>(1, attackArmies.size() / 3);
                    if (i < fieldSlots) targetIndex = 0u;
                }
                if (siege &&
                    actor.campaignPlan.active() &&
                    actor.campaignPlan.primaryObjective != NO_CITY) {
                    for (size_t ti = 0; ti < targets.size(); ++ti) {
                        if (targets[ti].city == actor.campaignPlan.primaryObjective) {
                            targetIndex = ti;
                            break;
                        }
                    }
                }
                const TargetCandidate& target = targets[targetIndex];
                const bool cityAssault = siege && target.city != NO_CITY;

                std::vector<TileID> stagingTargets{target.tile};
                for (TileID nid : worldMap_.neighbors8(target.tile)) {
                    if (!passable(nid)) continue;
                    stagingTargets.push_back(nid);
                }

                auto tacticalStagingScore = [&](TileID tileId, const Army& army, bool siegeArmy) {
                    const Tile& tile = worldMap_.at(tileId);
                    float score = distanceBetween(army.currentTile, tileId);

                    if (!siegeArmy && target.city != NO_CITY && tileId == target.tile) score += 8.0f;
                    if (tile.army != NO_ARMY &&
                        armies_.count(tile.army) &&
                        armies_.at(tile.army).owner != actor.id) {
                        score += 25.0f;
                    }

                    switch (tile.terrain) {
                        case TerrainType::Plain:
                            score += siegeArmy ? 0.0f : -1.5f;
                            break;
                        case TerrainType::River:
                            score += siegeArmy ? -1.0f : -2.5f;
                            break;
                        case TerrainType::Forest:
                            score += siegeArmy ? 2.0f : -3.5f;
                            break;
                        case TerrainType::Hill:
                            score += siegeArmy ? 1.0f : -4.5f;
                            break;
                        case TerrainType::Mountain:
                            score += siegeArmy ? 6.0f : -2.0f;
                            break;
                        case TerrainType::Coast:
                            score += 1.5f;
                            break;
                        case TerrainType::Ocean:
                        case TerrainType::Lake:
                            score += 1000.0f;
                            break;
                    }

                    if (tile.owner == actor.id) score -= 4.0f;
                    else if (tile.owner == NO_KINGDOM) score -= 1.0f;
                    else score += siegeArmy ? 0.0f : 2.0f;

                    if (target.supplyDistance > 20.0f &&
                        (tile.strategicPoint == StrategicPointType::SupplyDepot ||
                         tile.strategicPoint == StrategicPointType::Bridge ||
                         tile.strategicPoint == StrategicPointType::RiverFord)) {
                        score -= 6.0f;
                    }
                    if (tile.hasRiver) score -= 1.0f;
                    return score;
                };

                auto chooseTacticalStagingTarget = [&](const Army& army, bool siegeArmy) {
                    TileID best = target.tile;
                    float bestScore = 1e9f;
                    for (TileID candidate : stagingTargets) {
                        float score = tacticalStagingScore(candidate, army, siegeArmy);
                        if (score < bestScore) {
                            bestScore = score;
                            best = candidate;
                        }
                    }
                    return best;
                };

                TileID assignedTarget = cityAssault
                    ? target.tile
                    : chooseTacticalStagingTarget(armies_.at(eid), cityAssault);

                float assaultThreshold = 0.72f;
                if (actor.personality == KingdomPersonality::Defensive ||
                    actor.personality == KingdomPersonality::Economic) {
                    assaultThreshold = 0.88f;
                } else if (actor.personality == KingdomPersonality::Opportunistic) {
                    assaultThreshold = 0.64f;
                }
                if (actor.currentWarGoal.active()) {
                    assaultThreshold =
                        (assaultThreshold + actor.currentWarGoal.requiredForceRatio) * 0.5f;
                }
                if (actor.campaignPlan.active()) {
                    assaultThreshold =
                        (assaultThreshold + actor.campaignPlan.commitThreshold) * 0.5f;
                }
                if (actor.policy == NationalPolicy::FinalWar) assaultThreshold *= 0.82f;
                if (!frontlineBases.empty() &&
                    target.forceRatio < assaultThreshold &&
                    distanceBetween(armies_.at(eid).currentTile, target.tile) > 5.0f) {
                    TileID frontline = bestFrontlineFor(target.tile);
                    if (frontline != NO_TILE) assignedTarget = frontline;
                }

                if (cityAssault && !frontlineBases.empty() && attackArmies.size() >= 2) {
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
                if (cityAssault) assignedCityAssault = true;
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
            // Diplomatic kingdoms: limited military culture but can defend themselves (cap 5)
            const int hardCap = (actor.personality == KingdomPersonality::Diplomatic) ? 5 : 8;
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
            if (actor.personality == KingdomPersonality::Aggressive) {
                // 戦士文化 — 大規模な徴兵が根付いている
                recruitSoldiers = static_cast<uint32_t>(recruitSoldiers * 1.40f);
            } else if (actor.personality == KingdomPersonality::Expansionist) {
                // 農業余剰による人口増 → 大軍を養える
                recruitSoldiers = static_cast<uint32_t>(recruitSoldiers * 1.30f);
            } else if (actor.personality == KingdomPersonality::Opportunistic) {
                // 必要なときだけ素早く動員できる
                recruitSoldiers = static_cast<uint32_t>(recruitSoldiers * 1.15f);
            } else if (actor.personality == KingdomPersonality::Defensive) {
                // 守備隊優先 — 野戦軍より城壁に人を割く
                recruitSoldiers = static_cast<uint32_t>(recruitSoldiers * 1.10f);
            } else if (actor.personality == KingdomPersonality::Economic) {
                // 兵より商人 — 傭兵で補う前提で自前徴兵は少なめ
                recruitSoldiers = static_cast<uint32_t>(recruitSoldiers * 0.80f);
            } else {
                // Diplomatic: 非軍事文化、最小限の常備軍
                recruitSoldiers = static_cast<uint32_t>(recruitSoldiers * 0.50f);
                recruitSoldiers = std::max(recruitSoldiers, 300u);
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


} // namespace jke
