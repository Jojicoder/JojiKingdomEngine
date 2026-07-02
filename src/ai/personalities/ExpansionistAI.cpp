// ExpansionistAI — 版図拡張型
// 該当王国: Ironspire（鉄の帝国を広げる野心家）, Dawnreach（肥沃な土地を求め続ける）, Highcrest（誇り高い高台の覇者）, Crownsreach（経済力で覇権を狙う）
//
// 戦略方針:
//   - 序盤は同盟を結んで基盤を固め、中盤から積極的に版図を広げる
//   - 弱小国から順番に吸収し、都市数で他国を圧倒する
//   - TotalConquest 戦略では最終的に全王国の制圧を目指す
//   - 外交と戦争を状況に応じて使い分ける戦略的な拡張主義

#include "jke/ai/personalities/ExpansionistAI.hpp"
#include <algorithm>

namespace jke {

std::vector<AIDecision> ExpansionistAI::evaluate(const AIContext& ctx) const {
    std::vector<AIDecision> decisions;
    const Kingdom& self = ctx.self;

    const bool finalWar      = (self.policy == NationalPolicy::FinalWar);
    const bool totalConquest  = (self.strategyPlan == StrategyPlan::TotalConquest) || finalWar;
    const bool earlyGame      = (self.cities.size() < 3 && ctx.turn < 120);

    // Early game: form alliances first, don't charge into wars with 1-2 cities
    if (earlyGame) {
        KingdomID allyTarget = findBestAllyTarget(ctx);
        if (allyTarget != NO_KINGDOM && !atWarWith(ctx, allyTarget)) {
            AIDecision d;
            d.type      = AIDecisionType::FormAlliance;
            d.actor     = self.id;
            d.target    = allyTarget;
            d.priority  = 0.90f;
            d.reasoning = "Expansionist: secure flanks before expanding";
            decisions.push_back(d);
        }
    }

    // Priority 1: TotalConquest — declare war on every non-allied kingdom
    if (totalConquest) {
        for (const auto& [kid, k] : ctx.kingdoms) {
            if (kid == self.id || !k.isAlive) continue;
            if (alliedWith(ctx, kid)) continue;
            if (atWarWith(ctx, kid)) continue;
            AIDecision d;
            d.type      = AIDecisionType::DeclareWar;
            d.actor     = self.id;
            d.target    = kid;
            d.priority  = 0.96f;
            d.reasoning = "Expansionist/TotalConquest: war on all";
            decisions.push_back(d);
        }
    } else if (!earlyGame) {
        // Normal expansion: target strategicTarget or weakest neighbor
        // Wait until we have 3+ cities before picking fights
        KingdomID target = (self.strategicTarget != NO_KINGDOM &&
                            ctx.kingdoms.count(self.strategicTarget) &&
                            ctx.kingdoms.at(self.strategicTarget).isAlive)
            ? self.strategicTarget : findBestWarTarget(ctx);

        if (target != NO_KINGDOM && !alliedWith(ctx, target) &&
            !atWarWith(ctx, target) && self.treasury.gold > 80.0f &&
            self.armies.size() >= 2) {
            AIDecision d;
            d.type      = AIDecisionType::DeclareWar;
            d.actor     = self.id;
            d.target    = target;
            d.priority  = 0.92f;
            d.reasoning = "Expansionist: push the frontier";
            decisions.push_back(d);
        }
    }

    // Priority 2: Attack all active enemies
    auto enemyList = enemies(ctx);
    if (!enemyList.empty() && !self.armies.empty()) {
        // In TotalConquest, issue Attack for every enemy
        int attackSlot = 0;
        for (KingdomID enemy : enemyList) {
            auto it = ctx.kingdoms.find(enemy);
            if (it == ctx.kingdoms.end() || it->second.capitalCity == NO_CITY) continue;
            CityID targetCity = findBestAttackCity(ctx, enemy);
            if (targetCity == NO_CITY) continue;
            AIDecision d;
            d.type       = AIDecisionType::Attack;
            d.actor      = self.id;
            d.target     = enemy;
            d.targetCity = targetCity;
            d.targetArmy = self.armies.front();
            d.priority   = totalConquest ? 0.94f - attackSlot * 0.02f : 0.88f;
            d.reasoning  = "Expansionist: march on enemy";
            decisions.push_back(d);
            ++attackSlot;
            if (!totalConquest) break; // normal mode: one enemy at a time
        }
    }

    // Priority 3: Recruit constantly
    if (self.treasury.gold > 50.0f) {
        AIDecision d;
        d.type       = AIDecisionType::Recruit;
        d.actor      = self.id;
        d.targetCity = self.capitalCity;
        d.priority   = 0.75f;
        d.reasoning  = "Expansionist: armies enable expansion";
        decisions.push_back(d);
    }

    // Priority 4: Expand infrastructure in conquered lands
    if (self.cities.size() > 2) {
        AIDecision d;
        d.type      = AIDecisionType::ImproveEconomy;
        d.actor     = self.id;
        d.priority  = 0.55f;
        d.reasoning = "Expansionist: consolidate conquered territory";
        decisions.push_back(d);
    }

    // Priority 5: Research military/road tech
    if (self.currentResearch == NO_TECH) {
        AIDecision d;
        d.type      = AIDecisionType::ResearchTech;
        d.actor     = self.id;
        d.priority  = 0.45f;
        d.reasoning = "Expansionist: research to enable faster expansion";
        decisions.push_back(d);
    }

    std::sort(decisions.begin(), decisions.end(),
              [](const AIDecision& a, const AIDecision& b){ return a.priority > b.priority; });
    return decisions;
}

} // namespace jke
