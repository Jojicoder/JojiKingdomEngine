#include "jke/ai/AIStrategy.hpp"
#include <algorithm>
#include <cmath>

namespace jke {

KingdomID AIStrategy::findWeakestNeighbor(const AIContext& ctx) {
    KingdomID weakest = NO_KINGDOM;
    float     minPop  = 1e9f;

    for (const auto& [kid, k] : ctx.kingdoms) {
        if (kid == ctx.self.id || !k.isAlive) continue;
        if (k.totalPopulation < minPop) {
            minPop  = static_cast<float>(k.totalPopulation);
            weakest = kid;
        }
    }
    return weakest;
}

bool AIStrategy::atWarWith(const AIContext& ctx, KingdomID other) {
    KingdomID a = ctx.self.id, b = other;
    if (a > b) std::swap(a, b);
    auto it = ctx.relations.find(a);
    if (it == ctx.relations.end()) return false;
    auto it2 = it->second.find(b);
    if (it2 == it->second.end()) return false;
    return it2->second.state == RelationState::War;
}

bool AIStrategy::alliedWith(const AIContext& ctx, KingdomID other) {
    KingdomID a = ctx.self.id, b = other;
    if (a > b) std::swap(a, b);
    auto it = ctx.relations.find(a);
    if (it == ctx.relations.end()) return false;
    auto it2 = it->second.find(b);
    if (it2 == it->second.end()) return false;
    return it2->second.state == RelationState::Alliance;
}

float AIStrategy::threatLevel(const AIContext& ctx, KingdomID from) {
    auto it = ctx.kingdoms.find(from);
    if (it == ctx.kingdoms.end()) return 0.0f;
    const Kingdom& them = it->second;

    float myPop   = static_cast<float>(ctx.self.totalPopulation);
    float theirPop= static_cast<float>(them.totalPopulation);
    float threat  = (myPop > 0) ? theirPop / myPop : 1.0f;

    if (atWarWith(ctx, from)) threat *= 2.0f;
    return threat;
}

std::vector<KingdomID> AIStrategy::enemies(const AIContext& ctx) {
    std::vector<KingdomID> result;
    for (const auto& [kid, k] : ctx.kingdoms) {
        if (kid != ctx.self.id && k.isAlive && atWarWith(ctx, kid))
            result.push_back(kid);
    }
    return result;
}

std::vector<KingdomID> AIStrategy::allies(const AIContext& ctx) {
    std::vector<KingdomID> result;
    for (const auto& [kid, k] : ctx.kingdoms) {
        if (kid != ctx.self.id && k.isAlive && alliedWith(ctx, kid))
            result.push_back(kid);
    }
    return result;
}

KingdomID AIStrategy::findBestAllyTarget(const AIContext& ctx) {
    KingdomID best  = NO_KINGDOM;
    float     bestT = -1.0f;

    for (const auto& [kid, k] : ctx.kingdoms) {
        if (kid == ctx.self.id || !k.isAlive) continue;
        if (atWarWith(ctx, kid) || alliedWith(ctx, kid)) continue;

        KingdomID a = ctx.self.id, b = kid;
        if (a > b) std::swap(a, b);
        float trust = 0.0f;
        auto it = ctx.relations.find(a);
        if (it != ctx.relations.end()) {
            auto it2 = it->second.find(b);
            if (it2 != it->second.end()) trust = it2->second.trust;
        }

        float score = trust + static_cast<float>(k.totalPopulation) * 0.00001f;
        if (score > bestT) {
            bestT = score;
            best  = kid;
        }
    }
    return best;
}

TechID AIStrategy::bestAvailableTech(const AIContext& ctx) {
    // Just return the first available technology that matches the kingdom's needs
    // (TechTree lookup is done in SimulationEngine with full TechTree reference)
    // Return NO_TECH here; SimulationEngine will resolve using available list
    (void)ctx;
    return NO_TECH;
}

float AIStrategy::militaryStrength(const AIContext& ctx, KingdomID kingdom) {
    auto kit = ctx.kingdoms.find(kingdom);
    if (kit == ctx.kingdoms.end()) return 0.0f;

    float strength = 0.0f;
    for (ArmyID aid : kit->second.armies) {
        auto ait = ctx.armies.find(aid);
        if (ait == ctx.armies.end() || ait->second.isEmpty()) continue;

        const Army& army = ait->second;
        strength += army.combatStrength(TerrainType::Plain, false) *
                    std::clamp(army.supplyLevel, 0.35f, 1.0f);
    }
    return strength;
}

KingdomID AIStrategy::findBestWarTarget(const AIContext& ctx) {
    const float selfStrength = std::max(1.0f, militaryStrength(ctx, ctx.self.id));

    auto distanceBetween = [&](TileID a, TileID b) {
        auto ap = ctx.worldMap.at(a).position;
        auto bp = ctx.worldMap.at(b).position;
        return std::hypot(float(ap.x - bp.x), float(ap.y - bp.y));
    };

    auto nearestOwnCityDistance = [&](const Kingdom& enemy) {
        float best = 1e9f;
        for (CityID ownCid : ctx.self.cities) {
            auto ownIt = ctx.cities.find(ownCid);
            if (ownIt == ctx.cities.end() || ownIt->second.tile == NO_TILE) continue;
            for (CityID enemyCid : enemy.cities) {
                auto enemyIt = ctx.cities.find(enemyCid);
                if (enemyIt == ctx.cities.end() || enemyIt->second.tile == NO_TILE) continue;
                best = std::min(best, distanceBetween(ownIt->second.tile, enemyIt->second.tile));
            }
        }
        return best > 1e8f ? 32.0f : best;
    };

    KingdomID best = NO_KINGDOM;
    float bestScore = -1e9f;

    for (const auto& [kid, k] : ctx.kingdoms) {
        if (kid == ctx.self.id || !k.isAlive || k.isRebel) continue;
        if (k.cities.empty()) continue;
        if (alliedWith(ctx, kid) || atWarWith(ctx, kid)) continue;

        const float enemyStrength = std::max(1.0f, militaryStrength(ctx, kid));
        const float strengthRatio = selfStrength / enemyStrength;
        const float distance = nearestOwnCityDistance(k);

        float score = 0.0f;
        score += std::min(2.5f, strengthRatio) * 36.0f;
        score += static_cast<float>(k.cities.size()) * 5.0f;
        score += (1.0f - k.stability) * 28.0f;
        score += (1.0f - k.morale) * 16.0f;
        score -= distance * 0.75f;
        score -= k.warWeariness * 6.0f; // exhausted enemies may already accept peace soon

        if (ctx.self.strategyPlan == StrategyPlan::OpportunisticRaid) {
            bool distracted = false;
            for (const auto& [oid, other] : ctx.kingdoms) {
                if (oid == kid || !other.isAlive) continue;
                KingdomID a = kid, b = oid;
                if (a > b) std::swap(a, b);
                auto it = ctx.relations.find(a);
                if (it == ctx.relations.end()) continue;
                auto it2 = it->second.find(b);
                if (it2 != it->second.end() && it2->second.state == RelationState::War) {
                    distracted = true;
                    break;
                }
            }
            if (distracted) score += 42.0f;
        }

        if (k.personality == KingdomPersonality::Aggressive ||
            k.personality == KingdomPersonality::Expansionist) {
            score -= 12.0f;
        }
        if (strengthRatio < 0.75f &&
            ctx.self.policy != NationalPolicy::FinalWar &&
            ctx.self.strategyPlan != StrategyPlan::AntiHegemonWar) {
            score -= 45.0f;
        }

        if (score > bestScore) {
            bestScore = score;
            best = kid;
        }
    }

    return best;
}

CityID AIStrategy::findBestAttackCity(const AIContext& ctx, KingdomID enemy) {
    auto kit = ctx.kingdoms.find(enemy);
    if (kit == ctx.kingdoms.end() || !kit->second.isAlive) return NO_CITY;

    const Kingdom& target = kit->second;
    if (target.capitalCity != NO_CITY &&
        (ctx.self.strategyPlan == StrategyPlan::CapitalRush ||
         ctx.self.strategyPlan == StrategyPlan::TotalConquest ||
         ctx.self.policy == NationalPolicy::FinalWar)) {
        return target.capitalCity;
    }

    auto distanceBetween = [&](TileID a, TileID b) {
        auto ap = ctx.worldMap.at(a).position;
        auto bp = ctx.worldMap.at(b).position;
        return std::hypot(float(ap.x - bp.x), float(ap.y - bp.y));
    };

    auto nearestOwnCityDistance = [&](TileID tile) {
        float best = 1e9f;
        for (CityID ownCid : ctx.self.cities) {
            auto it = ctx.cities.find(ownCid);
            if (it == ctx.cities.end() || it->second.tile == NO_TILE) continue;
            best = std::min(best, distanceBetween(it->second.tile, tile));
        }
        return best > 1e8f ? 32.0f : best;
    };

    CityID bestCity = NO_CITY;
    float bestScore = 1e9f;
    for (CityID cid : target.cities) {
        auto cit = ctx.cities.find(cid);
        if (cit == ctx.cities.end()) continue;
        const City& city = cit->second;
        if (city.owner != enemy || city.tile == NO_TILE) continue;

        float score = nearestOwnCityDistance(city.tile);
        score += city.fortification * 35.0f;
        score += static_cast<float>(city.population) * 0.002f;

        if (city.isCapital && target.cities.size() > 1) score += 28.0f;
        if (ctx.self.strategyPlan == StrategyPlan::RevengeWar &&
            city.originalOwner == ctx.self.id) score -= 65.0f;
        if (ctx.self.strategyPlan == StrategyPlan::OpportunisticRaid) {
            if (city.cityType == CityType::Port ||
                city.cityType == CityType::TradeHub ||
                city.cityType == CityType::Fortress) {
                score -= 18.0f;
            }
            if (!city.isCapital) score -= 10.0f;
        }
        if (ctx.self.strategyPlan == StrategyPlan::BorderExpansion && !city.isCapital) {
            score -= 14.0f;
        }
        if (city.underSiege) score -= 22.0f;

        if (score < bestScore) {
            bestScore = score;
            bestCity = cid;
        }
    }

    return bestCity != NO_CITY ? bestCity : target.capitalCity;
}

} // namespace jke
