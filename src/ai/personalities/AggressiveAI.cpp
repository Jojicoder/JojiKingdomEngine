#include "jke/ai/personalities/AggressiveAI.hpp"
#include <algorithm>

namespace jke {

std::vector<AIDecision> AggressiveAI::evaluate(const AIContext& ctx) const {
    std::vector<AIDecision> decisions;
    const Kingdom& self = ctx.self;

    const bool finalWar    = (self.policy == NationalPolicy::FinalWar);
    const bool capitalRush = (self.strategyPlan == StrategyPlan::CapitalRush) || finalWar;

    // Priority 1: FinalWar — declare on ALL living kingdoms simultaneously
    if (finalWar) {
        for (const auto& [kid, k] : ctx.kingdoms) {
            if (kid == self.id || !k.isAlive) continue;
            if (alliedWith(ctx, kid)) continue;
            if (atWarWith(ctx, kid)) continue;
            AIDecision d;
            d.type      = AIDecisionType::DeclareWar;
            d.actor     = self.id;
            d.target    = kid;
            d.priority  = 0.98f;
            d.reasoning = "Aggressive/FinalWar: total war — declare on all";
            decisions.push_back(d);
        }
    } else if (enemies(ctx).empty()) {
        // Always be at war — declare on weakest neighbor
        KingdomID target = self.strategicTarget != NO_KINGDOM
            ? self.strategicTarget : findWeakestNeighbor(ctx);
        if (target != NO_KINGDOM && !alliedWith(ctx, target)) {
            AIDecision d;
            d.type      = AIDecisionType::DeclareWar;
            d.actor     = self.id;
            d.target    = target;
            d.priority  = 0.95f;
            d.reasoning = "Aggressive: always at war";
            decisions.push_back(d);
        }
    }

    // Priority 2: Attack — CapitalRush goes straight for capital; otherwise nearest city
    if (!enemies(ctx).empty() && !self.armies.empty()) {
        // Prefer strategicTarget if set, else first enemy
        KingdomID enemy = (self.strategicTarget != NO_KINGDOM &&
                           ctx.kingdoms.count(self.strategicTarget) &&
                           ctx.kingdoms.at(self.strategicTarget).isAlive &&
                           atWarWith(ctx, self.strategicTarget))
            ? self.strategicTarget : enemies(ctx).front();

        auto it = ctx.kingdoms.find(enemy);
        if (it != ctx.kingdoms.end()) {
            // CapitalRush: always aim at enemy capital
            CityID targetCity = capitalRush ? it->second.capitalCity
                                            : it->second.capitalCity; // fallback same for now
            if (targetCity != NO_CITY) {
                AIDecision d;
                d.type       = AIDecisionType::Attack;
                d.actor      = self.id;
                d.target     = enemy;
                d.targetCity = targetCity;
                d.targetArmy = self.armies.front();
                d.priority   = finalWar ? 0.96f : 0.85f;
                d.reasoning  = capitalRush ? "Aggressive/CapitalRush: strike enemy capital"
                                           : "Aggressive: attack enemy";
                decisions.push_back(d);
            }
        }
    }

    // Priority 3: Recruit whenever affordable — always build up forces
    if (!self.cities.empty()) {
        AIDecision d;
        d.type       = AIDecisionType::Recruit;
        d.actor      = self.id;
        d.targetCity = self.capitalCity;
        d.priority   = 0.80f;
        d.reasoning  = "Aggressive: build up military";
        decisions.push_back(d);
    }

    // Priority 4: Research military tech
    if (self.currentResearch == NO_TECH && self.treasury.gold > 40.0f) {
        AIDecision d;
        d.type      = AIDecisionType::ResearchTech;
        d.actor     = self.id;
        d.priority  = 0.4f;
        d.reasoning = "Aggressive: research military tech";
        decisions.push_back(d);
    }

    // Priority 5: Upgrade barracks if short on manpower
    if (self.totalPopulation < 3000) {
        AIDecision d;
        d.type       = AIDecisionType::UpgradeCity;
        d.actor      = self.id;
        d.targetCity = self.capitalCity;
        d.priority   = 0.3f;
        d.reasoning  = "Aggressive: boost population";
        decisions.push_back(d);
    }

    std::sort(decisions.begin(), decisions.end(),
              [](const AIDecision& a, const AIDecision& b){ return a.priority > b.priority; });
    return decisions;
}

} // namespace jke
