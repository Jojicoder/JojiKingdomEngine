#include "jke/ai/personalities/DiplomaticAI.hpp"
#include <algorithm>

namespace jke {

std::vector<AIDecision> DiplomaticAI::evaluate(const AIContext& ctx) const {
    std::vector<AIDecision> decisions;
    const Kingdom& self = ctx.self;

    // Detect dominant kingdom (hegemon)
    KingdomID hegemon = NO_KINGDOM;
    size_t    hegemonCities = 0;
    for (const auto& [kid, k] : ctx.kingdoms) {
        if (kid == self.id || !k.isAlive) continue;
        if (k.cities.size() > hegemonCities) {
            hegemonCities = k.cities.size();
            hegemon = kid;
        }
    }
    bool threatened = (hegemon != NO_KINGDOM &&
                       hegemonCities >= self.cities.size() * 2 + 2);

    // Priority 1: If threatened by hegemon, seek peace with everyone else first
    auto enemyList = enemies(ctx);
    for (KingdomID enemy : enemyList) {
        if (enemy == hegemon && threatened) continue; // fight the hegemon
        AIDecision d;
        d.type      = AIDecisionType::NegotiatePeace;
        d.actor     = self.id;
        d.target    = enemy;
        d.priority  = 0.95f;
        d.reasoning = "Diplomatic: free up allies against hegemon";
        decisions.push_back(d);
    }

    // Priority 2: Coalition — ally with other small kingdoms against hegemon
    if (threatened) {
        for (const auto& [kid, k] : ctx.kingdoms) {
            if (kid == self.id || kid == hegemon || !k.isAlive) continue;
            if (k.cities.size() * 2 < hegemonCities && !alliedWith(ctx, kid)) {
                AIDecision d;
                d.type      = AIDecisionType::FormAlliance;
                d.actor     = self.id;
                d.target    = kid;
                d.priority  = 0.92f;
                d.reasoning = "Diplomatic: coalition against hegemon";
                decisions.push_back(d);
                break;
            }
        }
        // Declare war on hegemon if we have allies
        if (!atWarWith(ctx, hegemon) && !allies(ctx).empty() && !self.armies.empty()) {
            AIDecision d;
            d.type      = AIDecisionType::DeclareWar;
            d.actor     = self.id;
            d.target    = hegemon;
            d.priority  = 0.88f;
            d.reasoning = "Diplomatic: coalition war on hegemon";
            decisions.push_back(d);
        }
        // Attack hegemon if already at war
        if (atWarWith(ctx, hegemon) && !self.armies.empty()) {
            auto it = ctx.kingdoms.find(hegemon);
            if (it != ctx.kingdoms.end() && it->second.capitalCity != NO_CITY) {
                AIDecision d;
                d.type       = AIDecisionType::Attack;
                d.actor      = self.id;
                d.target     = hegemon;
                d.targetCity = it->second.capitalCity;
                d.priority   = 0.86f;
                d.reasoning  = "Diplomatic: press coalition attack on hegemon";
                decisions.push_back(d);
            }
        }
    } else {
        // No hegemon threat — normal alliance-building
        KingdomID allyTarget = findBestAllyTarget(ctx);
        if (allyTarget != NO_KINGDOM) {
            AIDecision d;
            d.type      = AIDecisionType::FormAlliance;
            d.actor     = self.id;
            d.target    = allyTarget;
            d.priority  = 0.85f;
            d.reasoning = "Diplomatic: expand alliance network";
            decisions.push_back(d);
        }
    }

    // Priority 3: Research administration tech
    if (self.currentResearch == NO_TECH) {
        AIDecision d;
        d.type      = AIDecisionType::ResearchTech;
        d.actor     = self.id;
        d.priority  = 0.70f;
        d.reasoning = "Diplomatic: research administration and trade";
        decisions.push_back(d);
    }

    // Priority 4: Improve economy to support alliance costs
    if (self.treasury.gold < 80.0f) {
        AIDecision d;
        d.type      = AIDecisionType::ImproveEconomy;
        d.actor     = self.id;
        d.priority  = 0.60f;
        d.reasoning = "Diplomatic: maintain economic base";
        decisions.push_back(d);
    }

    // Priority 5: Recruit defensive armies (not just when empty)
    if (!self.cities.empty()) {
        AIDecision d;
        d.type       = AIDecisionType::Recruit;
        d.actor      = self.id;
        d.targetCity = self.capitalCity;
        d.priority   = 0.65f;
        d.reasoning  = "Diplomatic: maintain defensive forces";
        decisions.push_back(d);
    }

    std::sort(decisions.begin(), decisions.end(),
              [](const AIDecision& a, const AIDecision& b){ return a.priority > b.priority; });
    return decisions;
}

} // namespace jke
