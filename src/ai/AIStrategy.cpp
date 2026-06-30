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

} // namespace jke
