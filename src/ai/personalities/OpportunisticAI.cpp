// OpportunisticAI — 機会主義型
// 該当王国: Goldenveil（利があればどこにでも売る商人国家）, Ashford（廃墟から這い上がる抜け目ない民）,
//           Mistwood（霧の森で機を窺う）, Duskfell（滅びかけた国が生き残るために機を突く）
//
// 戦略方針:
//   - 弱った敵・補給切れの軍・戦争疲弊国を優先的に狙う
//   - 補給線を断って敵軍を孤立させてから攻撃する（supply interdiction）
//   - 自分から積極的に戦争を起こさず、漁夫の利を狙う
//   - 情勢が不利なら即座に撤退・講和し、体力を温存する

#include "jke/ai/personalities/OpportunisticAI.hpp"
#include <algorithm>
#include <cmath>

namespace jke {

std::vector<AIDecision> OpportunisticAI::evaluate(const AIContext& ctx) const {
    std::vector<AIDecision> decisions;
    const Kingdom& self = ctx.self;

    // Priority 0: Supply-line interdiction — attack the supply base of a starving enemy army
    {
        KingdomID supplyTarget = NO_KINGDOM;
        CityID    supplyBase   = NO_CITY;
        float     lowestSupply = 0.40f;

        for (const auto& [aid, army] : ctx.armies) {
            if (army.owner == self.id) continue;
            if (!ctx.kingdoms.count(army.owner) || !ctx.kingdoms.at(army.owner).isAlive) continue;
            if (alliedWith(ctx, army.owner)) continue;
            if (army.supplyLevel >= lowestSupply) continue;
            if (army.currentTile == NO_TILE) continue;

            auto armyPos = ctx.worldMap.at(army.currentTile).position;
            float nearestBaseDist = 1e9f;
            CityID candidateBase = NO_CITY;
            for (const auto& [cid, city] : ctx.cities) {
                if (city.owner != army.owner || city.tile == NO_TILE) continue;
                auto cp = ctx.worldMap.at(city.tile).position;
                float d = std::hypot(float(armyPos.x - cp.x), float(armyPos.y - cp.y));
                if (d < nearestBaseDist) { nearestBaseDist = d; candidateBase = cid; }
            }

            if (candidateBase != NO_CITY) {
                lowestSupply = army.supplyLevel;
                supplyTarget = army.owner;
                supplyBase   = candidateBase;
            }
        }

        if (supplyTarget != NO_KINGDOM && supplyBase != NO_CITY && !self.armies.empty()) {
            if (!atWarWith(ctx, supplyTarget)) {
                AIDecision d;
                d.type      = AIDecisionType::DeclareWar;
                d.actor     = self.id;
                d.target    = supplyTarget;
                d.priority  = 0.95f;
                d.reasoning = "Opportunistic: enemy army is supply-starved — seize the moment";
                decisions.push_back(d);
            } else {
                AIDecision d;
                d.type       = AIDecisionType::Attack;
                d.actor      = self.id;
                d.target     = supplyTarget;
                d.targetCity = supplyBase;
                d.priority   = 0.95f;
                d.reasoning  = "Opportunistic: cut enemy supply base to starve their army";
                decisions.push_back(d);
            }
        }
    }

    // Scan for vulnerable kingdoms (low stability, at war with others, no allies)
    KingdomID prey = NO_KINGDOM;
    float     bestVulnerability = 0.0f;

    for (const auto& [kid, k] : ctx.kingdoms) {
        if (kid == ctx.self.id || !k.isAlive) continue;
        if (alliedWith(ctx, kid)) continue;

        float vulnerability = (1.0f - k.stability) * 0.6f;
        auto countEnemies = [&]() {
            int n = 0;
            for (const auto& [oid, ok] : ctx.kingdoms) {
                if (oid == kid || !ok.isAlive) continue;
                KingdomID a = kid, b = oid;
                if (a > b) std::swap(a, b);
                auto it = ctx.relations.find(a);
                if (it != ctx.relations.end()) {
                    auto it2 = it->second.find(b);
                    if (it2 != it->second.end() &&
                        it2->second.state == RelationState::War) n++;
                }
            }
            return n;
        };
        const int activeEnemies = countEnemies();
        vulnerability += activeEnemies * 0.45f;
        if (k.armies.empty()) vulnerability += 0.5f;
        if (activeEnemies > 0 && !k.armies.empty()) vulnerability += 0.25f;

        for (const auto& [aid, army] : ctx.armies) {
            if (army.owner != kid) continue;
            if (army.supplyLevel < 0.35f) vulnerability += 0.55f;
            else if (army.supplyLevel < 0.55f) vulnerability += 0.25f;
        }

        // Big bonus if enemy army has strayed far from their capital — exposed!
        if (k.capitalCity != NO_CITY && ctx.cities.count(k.capitalCity)) {
            auto capPos = ctx.worldMap.at(ctx.cities.at(k.capitalCity).tile).position;
            for (const auto& [aid, army] : ctx.armies) {
                if (army.owner != kid) continue;
                auto ap = ctx.worldMap.at(army.currentTile).position;
                float dist = std::hypot(float(capPos.x - ap.x), float(capPos.y - ap.y));
                if (dist > 18.0f) vulnerability += 0.6f; // army left capital unguarded
            }
        }

        if (vulnerability > bestVulnerability) {
            bestVulnerability = vulnerability;
            prey = kid;
        }
    }

    // OpportunisticRaid: if a strategic target is set, prefer it over the scanned prey
    if (self.strategyPlan == StrategyPlan::OpportunisticRaid &&
        self.strategicTarget != NO_KINGDOM &&
        ctx.kingdoms.count(self.strategicTarget) &&
        ctx.kingdoms.at(self.strategicTarget).isAlive) {
        prey = self.strategicTarget;
        bestVulnerability = 1.0f; // override threshold
    }

    // Priority 1: Hire mercenaries when war chest is ready and we need more muscle
    if (self.treasury.gold > 350.0f && self.armies.size() < 3) {
        AIDecision d;
        d.type      = AIDecisionType::HireMercenary;
        d.actor     = self.id;
        d.priority  = 0.85f;
        d.reasoning = "Opportunistic: deploy war chest to buy elite mercenaries";
        decisions.push_back(d);
    }

    // Priority 2: Attack vulnerable target (raised threshold: only strike clearly distracted foes)
    if (prey != NO_KINGDOM && bestVulnerability > 0.45f &&
        self.treasury.gold > 50.0f && !self.armies.empty()) {
        if (!atWarWith(ctx, prey)) {
            AIDecision d;
            d.type      = AIDecisionType::DeclareWar;
            d.actor     = self.id;
            d.target    = prey;
            d.priority  = 0.90f;
            d.reasoning = "Opportunistic: attack vulnerable kingdom";
            decisions.push_back(d);
        } else {
            auto it = ctx.kingdoms.find(prey);
            if (it != ctx.kingdoms.end() && it->second.capitalCity != NO_CITY) {
                CityID raidTarget = findBestAttackCity(ctx, prey);
                if (raidTarget == NO_CITY) raidTarget = it->second.capitalCity;

                AIDecision d;
                d.type       = AIDecisionType::Attack;
                d.actor      = self.id;
                d.target     = prey;
                d.targetCity = raidTarget;
                d.targetArmy = self.armies.front();
                d.priority   = 0.92f;
                d.reasoning  = "Opportunistic: raid the weakest exposed city";
                decisions.push_back(d);
            }
        }
    }

    // Priority 2: Peace if overwhelmed
    auto enemyList = enemies(ctx);
    if (enemyList.size() > 1) {
        AIDecision d;
        d.type      = AIDecisionType::NegotiatePeace;
        d.actor     = self.id;
        d.target    = enemyList.front();
        d.priority  = 0.80f;
        d.reasoning = "Opportunistic: disengage when outnumbered";
        decisions.push_back(d);
    }

    // Priority 3: Also hire mercenaries at lower gold thresholds when in active war
    if (self.treasury.gold > 200.0f && self.armies.size() < 2 && !enemies(ctx).empty()) {
        AIDecision d;
        d.type      = AIDecisionType::HireMercenary;
        d.actor     = self.id;
        d.priority  = 0.72f;
        d.reasoning = "Opportunistic: buy hired swords for ongoing war";
        decisions.push_back(d);
    }

    // Priority 4: Recruit whenever possible — scale up forces
    if (!self.cities.empty()) {
        AIDecision d;
        d.type       = AIDecisionType::Recruit;
        d.actor      = self.id;
        d.targetCity = self.capitalCity;
        d.priority   = 0.68f;
        d.reasoning  = "Opportunistic: build forces";
        decisions.push_back(d);
    }

    // Priority 4: Research
    if (self.currentResearch == NO_TECH) {
        AIDecision d;
        d.type      = AIDecisionType::ResearchTech;
        d.actor     = self.id;
        d.priority  = 0.45f;
        d.reasoning = "Opportunistic: research when idle";
        decisions.push_back(d);
    }

    std::sort(decisions.begin(), decisions.end(),
              [](const AIDecision& a, const AIDecision& b){ return a.priority > b.priority; });
    return decisions;
}

} // namespace jke
