#include "jke/ai/personalities/DefensiveAI.hpp"
#include <algorithm>
#include <cmath>

namespace jke {

std::vector<AIDecision> DefensiveAI::evaluate(const AIContext& ctx) const {
    std::vector<AIDecision> decisions;
    const Kingdom& self = ctx.self;
    auto enemyList = enemies(ctx);

    // Priority 0: Counter-attack supply-starved invaders on our territory
    // An enemy army deep in our land with low supply is extremely vulnerable
    {
        KingdomID weakInvader = NO_KINGDOM;
        float     lowestSupply = 0.45f;

        for (const auto& [aid, army] : ctx.armies) {
            if (army.owner == self.id) continue;
            if (!ctx.kingdoms.count(army.owner) || !ctx.kingdoms.at(army.owner).isAlive) continue;
            if (!atWarWith(ctx, army.owner)) continue;
            if (army.supplyLevel >= lowestSupply) continue;
            if (army.currentTile == NO_TILE) continue;

            // Only react if they're on our territory
            if (ctx.worldMap.at(army.currentTile).owner != self.id) continue;

            lowestSupply  = army.supplyLevel;
            weakInvader   = army.owner;
        }

        if (weakInvader != NO_KINGDOM && !self.armies.empty()) {
            // Find their nearest supply city and attack it — cuts their lifeline
            CityID supplyBase = NO_CITY;
            float  bestDist   = 1e9f;
            auto& selfArmy = ctx.armies.count(self.armies.front())
                ? ctx.armies.at(self.armies.front()) : ctx.armies.begin()->second;
            auto selfPos = ctx.worldMap.at(selfArmy.currentTile).position;

            for (const auto& [cid, city] : ctx.cities) {
                if (city.owner != weakInvader || city.tile == NO_TILE) continue;
                auto cp = ctx.worldMap.at(city.tile).position;
                float d = std::hypot(float(selfPos.x - cp.x), float(selfPos.y - cp.y));
                if (d < bestDist) { bestDist = d; supplyBase = cid; }
            }

            if (supplyBase != NO_CITY) {
                AIDecision d;
                d.type       = AIDecisionType::Attack;
                d.actor      = self.id;
                d.target     = weakInvader;
                d.targetCity = supplyBase;
                d.priority   = 0.92f;
                d.reasoning  = "Defensive: counter-attack — enemy invader is supply-starved";
                decisions.push_back(d);
            }
        }
    }

    const bool turtle    = (self.strategyPlan == StrategyPlan::TurtleDefense);
    const bool finalWar  = (self.policy == NationalPolicy::FinalWar);

    // TurtleDefense: aggressively recruit and fortify every city.
    // In FinalWar, Defensive kingdoms should still defend well, but not turn
    // every city into a runaway recruitment engine.
    if (turtle) {
        // Fortify ALL cities, not just capital
        for (CityID cid : self.cities) {
            AIDecision d;
            d.type       = AIDecisionType::UpgradeCity;
            d.actor      = self.id;
            d.targetCity = cid;
            d.priority   = 0.94f;
            d.reasoning  = "Defensive/Turtle: fortify all cities";
            decisions.push_back(d);
        }
        // Recruit at every owned city
        for (CityID cid : self.cities) {
            AIDecision d;
            d.type       = AIDecisionType::Recruit;
            d.actor      = self.id;
            d.targetCity = cid;
            d.priority   = 0.92f;
            d.reasoning  = "Defensive/Turtle: raise every garrison";
            decisions.push_back(d);
        }
    }

    // Priority 1: Negotiate peace if at war and struggling / exhausted (skip in FinalWar)
    if (!enemyList.empty() && !finalWar &&
        (self.stability < 0.5f || self.warWeariness > 0.65f)) {
        float priority = 0.95f + self.warWeariness * 0.04f;
        AIDecision d;
        d.type      = AIDecisionType::NegotiatePeace;
        d.actor     = self.id;
        d.target    = enemyList.front();
        d.priority  = std::min(0.99f, priority);
        d.reasoning = "Defensive: seek peace while unstable or exhausted";
        decisions.push_back(d);
    }

    // Priority 2: Upgrade city fortifications
    if (!self.cities.empty() && self.treasury.stone > 30.0f && !turtle) {
        AIDecision d;
        d.type       = AIDecisionType::UpgradeCity;
        d.actor      = self.id;
        d.targetCity = self.capitalCity;
        d.priority   = 0.8f;
        d.reasoning  = "Defensive: fortify capital";
        decisions.push_back(d);
    }

    // Priority 3: Recruit defensive units when threatened
    if (self.treasury.gold > 60.0f && !enemyList.empty() && !turtle) {
        AIDecision d;
        d.type       = AIDecisionType::Recruit;
        d.actor      = self.id;
        d.targetCity = self.capitalCity;
        d.priority   = 0.75f;
        d.reasoning  = "Defensive: recruit defenders";
        decisions.push_back(d);
    }

    // Priority 4: Form alliance for protection
    if (enemyList.size() > 1 && allies(ctx).empty()) {
        KingdomID allyTarget = findBestAllyTarget(ctx);
        if (allyTarget != NO_KINGDOM) {
            AIDecision d;
            d.type      = AIDecisionType::FormAlliance;
            d.actor     = self.id;
            d.target    = allyTarget;
            d.priority  = 0.7f;
            d.reasoning = "Defensive: form alliance against multiple enemies";
            decisions.push_back(d);
        }
    }

    // Priority 5: Research fortification tech
    if (self.currentResearch == NO_TECH) {
        AIDecision d;
        d.type      = AIDecisionType::ResearchTech;
        d.actor     = self.id;
        d.priority  = 0.6f;
        d.reasoning = "Defensive: research fortification";
        decisions.push_back(d);
    }

    // Priority 6: Improve economy to sustain defense
    if (self.treasury.gold < 50.0f) {
        AIDecision d;
        d.type      = AIDecisionType::ImproveEconomy;
        d.actor     = self.id;
        d.priority  = 0.5f;
        d.reasoning = "Defensive: improve economy for upkeep";
        decisions.push_back(d);
    }

    std::sort(decisions.begin(), decisions.end(),
              [](const AIDecision& a, const AIDecision& b){ return a.priority > b.priority; });
    return decisions;
}

} // namespace jke
