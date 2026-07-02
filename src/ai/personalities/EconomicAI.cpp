// EconomicAI — 経済覇権型
// 該当王国: Aethermoor（技術を売る研究国家）, Crystalholm（知識を独占する水晶の島国）, Silvershard（銀の商才で富む国）
//
// 戦略方針:
//   - 内政・交易・技術開発を最優先。軍事は最低限に抑える
//   - 金と資源が豊富になったら外交で他国を従属・同盟化する
//   - FinalWar になれば経済力で蓄えた大軍を一気に投入
//   - 弱体化した相手への限定侵攻で領土と資源を静かに広げる

#include "jke/ai/personalities/EconomicAI.hpp"
#include <algorithm>

namespace jke {

std::vector<AIDecision> EconomicAI::evaluate(const AIContext& ctx) const {
    std::vector<AIDecision> decisions;
    const Kingdom& self = ctx.self;

    const bool finalWar = (self.policy == NationalPolicy::FinalWar);
    const bool invading  = (self.policy == NationalPolicy::Invading);

    // Priority 1: FinalWar override — even merchants must fight
    if (finalWar && !self.armies.empty()) {
        auto enemyList = enemies(ctx);
        if (!enemyList.empty()) {
            KingdomID enemy = self.strategicTarget != NO_KINGDOM &&
                              ctx.kingdoms.count(self.strategicTarget) &&
                              atWarWith(ctx, self.strategicTarget)
                ? self.strategicTarget : enemyList.front();
            auto it = ctx.kingdoms.find(enemy);
            if (it != ctx.kingdoms.end() && it->second.capitalCity != NO_CITY) {
                AIDecision d;
                d.type       = AIDecisionType::Attack;
                d.actor      = self.id;
                d.target     = enemy;
                d.targetCity = it->second.capitalCity;
                d.priority   = 0.96f;
                d.reasoning  = "Economic/FinalWar: survival demands war";
                decisions.push_back(d);
            }
        }
    }

    // Priority 1 (normal): Negotiate peace — avoid wars / high weariness
    auto enemyList = enemies(ctx);
    if (!enemyList.empty() && !finalWar && !invading) {
        // Urgency rises with war weariness
        float peacePriority = 0.95f + self.warWeariness * 0.04f;
        AIDecision d;
        d.type      = AIDecisionType::NegotiatePeace;
        d.actor     = self.id;
        d.target    = enemyList.front();
        d.priority  = std::min(0.99f, peacePriority);
        d.reasoning = "Economic: wars are bad for business";
        decisions.push_back(d);
    }

    // Priority 2: Trade agreement with neighbors
    if (self.treasury.gold > 40.0f) {
        KingdomID tradeTarget = findBestAllyTarget(ctx);
        if (tradeTarget != NO_KINGDOM && !atWarWith(ctx, tradeTarget)) {
            AIDecision d;
            d.type      = AIDecisionType::FormAlliance; // trade pact via alliance
            d.actor     = self.id;
            d.target    = tradeTarget;
            d.priority  = 0.85f;
            d.reasoning = "Economic: establish trade relations";
            decisions.push_back(d);
        }
    }

    // Priority 3: Upgrade city (market/farm)
    if (self.treasury.gold > 60.0f && !self.cities.empty()) {
        AIDecision d;
        d.type       = AIDecisionType::ImproveEconomy;
        d.actor      = self.id;
        d.targetCity = self.capitalCity;
        d.priority   = 0.75f;
        d.reasoning  = "Economic: improve city production";
        decisions.push_back(d);
    }

    // Priority 4: Research economy/trade tech
    if (self.currentResearch == NO_TECH) {
        AIDecision d;
        d.type      = AIDecisionType::ResearchTech;
        d.actor     = self.id;
        d.priority  = 0.65f;
        d.reasoning = "Economic: research trade technologies";
        decisions.push_back(d);
    }

    // Priority 5: Recruit small army for defense
    if (self.armies.empty() && self.treasury.gold > 80.0f) {
        AIDecision d;
        d.type       = AIDecisionType::Recruit;
        d.actor      = self.id;
        d.targetCity = self.capitalCity;
        d.priority   = 0.5f;
        d.reasoning  = "Economic: maintain minimal defense";
        decisions.push_back(d);
    }

    std::sort(decisions.begin(), decisions.end(),
              [](const AIDecision& a, const AIDecision& b){ return a.priority > b.priority; });
    return decisions;
}

} // namespace jke
