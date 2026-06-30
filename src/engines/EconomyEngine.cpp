#include "jke/engines/EconomyEngine.hpp"
#include "jke/core/Constants.hpp"
#include "jke/world/Season.hpp"
#include <algorithm>
#include <cmath>

namespace jke {
namespace {
    namespace C = jke::constants;
}

void EconomyEngine::update(
    std::unordered_map<KingdomID, Kingdom>& kingdoms,
    std::unordered_map<CityID, City>& cities,
    const std::unordered_map<ArmyID, Army>& armies,
    EventBus& bus,
    TurnNumber turn,
    Season season,
    const RelationMap* relations)
{
    SeasonModifiers smods = seasonMods(season);

    for (auto& [kid, k] : kingdoms) {
        if (!k.isAlive) continue;

        computeIncome(k, cities, kingdoms, relations);
        computeExpenses(k, armies);

        // Apply season multipliers
        k.perTurnIncome.food *= smods.foodMult;
        k.perTurnIncome.gold *= smods.goldMult;

        ResourceLedger net = k.perTurnIncome - k.perTurnExpense;

        // Apply net to treasury
        k.treasury += net;

        // Handle food surplus / deficit
        applyStarvation(k, cities, bus, turn);

        // Population growth (if food positive)
        if (k.treasury.food > 0) {
            growPopulation(k, cities);
        }

        // Track debt before clamping so SimulationEngine can disband armies
        if (k.treasury.gold < 0) {
            k.debtTurns++;
        } else {
            k.debtTurns = 0;
        }

        k.treasury.clampToZero();

        // Update city happiness
        for (CityID cid : k.cities) {
            auto it = cities.find(cid);
            if (it != cities.end()) updateCityHappiness(k, it->second);
        }

        // Stability decay from economic trouble
        if (k.debtTurns > 0 || k.starvationTurns > 0) {
            k.stability -= 0.02f;
        } else {
            k.stability = std::min(1.0f, k.stability + 0.01f);
        }
        k.stability = std::clamp(k.stability, 0.0f, 1.0f);
    }
}

void EconomyEngine::computeIncome(
    Kingdom& k,
    const std::unordered_map<CityID, City>& cities,
    const std::unordered_map<KingdomID, Kingdom>& kingdoms,
    const RelationMap* relations) const
{
    k.perTurnIncome = {};
    for (CityID cid : k.cities) {
        auto it = cities.find(cid);
        if (it == cities.end() || it->second.isRuined) continue;
        const City& c = it->second;
        ResourceLedger prod = c.effectiveProduction();

        k.perTurnIncome.food  += prod.food;
        k.perTurnIncome.wood  += prod.wood;
        k.perTurnIncome.stone += prod.stone;
        k.perTurnIncome.iron  += prod.iron;

        // Tax income in gold
        float taxable = c.population * 0.001f * C::BASE_GOLD_PER_CITY;
        k.perTurnIncome.gold += taxable * c.taxRate * k.goldBonus;
        k.perTurnIncome.gold += prod.gold;
    }
    k.perTurnIncome.food *= k.foodBonus;
    k.perTurnIncome.gold *= k.goldBonus;
    if (k.hasRuler) k.perTurnIncome.gold *= k.ruler.goldMult;

    // Culture penalty: foreign cities produce less until assimilated
    // Cap total kingdom-level penalty at -25%
    {
        float totalPenalty = 0.0f;
        for (CityID cid : k.cities) {
            auto it = cities.find(cid);
            if (it == cities.end() || it->second.isRuined) continue;
            const City& c = it->second;
            if (c.cultureOwner != k.id && c.cultureAssimilation < 1.0f) {
                // Opportunistic: pragmatic rule — foreign cities resent them less
                float penaltyRate = (k.personality == KingdomPersonality::Opportunistic) ? 0.07f : 0.15f;
                totalPenalty += (1.0f - c.cultureAssimilation) * penaltyRate;
            }
        }
        totalPenalty = std::min(totalPenalty, 0.25f);
        k.perTurnIncome.gold *= (1.0f - totalPenalty);
        k.perTurnIncome.food *= (1.0f - totalPenalty * 0.5f);
    }

    // Trade route income: flat per partner, but large Diplomatic empires earn less per route
    // (diplomatic overstretch: managing many partners and cities reduces per-link efficiency)
    if (relations) {
        KingdomID kid = k.id;
        float baseTradePerPartner;
        if (k.personality == KingdomPersonality::Diplomatic) {
            // Diminishing returns: starts at 12g but halves every 8 cities above 4
            const float dipScale = 4.0f / std::max(4.0f, static_cast<float>(k.cities.size()));
            baseTradePerPartner = 12.0f * dipScale;   // 4 cities=12g, 8=6g, 12=4g, 16=3g
        } else if (k.personality == KingdomPersonality::Economic) {
            baseTradePerPartner = 9.0f;
        } else {
            baseTradePerPartner = 6.0f;
        }
        for (const auto& [okid, ok] : kingdoms) {
            if (okid == kid || !ok.isAlive) continue;
            KingdomID lo = std::min(kid, okid), hi = std::max(kid, okid);
            auto rit = relations->find(lo);
            if (rit == relations->end()) continue;
            auto rit2 = rit->second.find(hi);
            if (rit2 == rit->second.end()) continue;
            const auto& rel = rit2->second;
            if (rel.state != RelationState::Alliance &&
                rel.state != RelationState::TradeAgreement &&
                rel.state != RelationState::Peace) continue;
            float tradeBonus = baseTradePerPartner * (1.0f + rel.trust * 0.5f);
            if (rel.state == RelationState::Alliance)       tradeBonus *= 1.20f;
            if (rel.state == RelationState::TradeAgreement) tradeBonus *= 1.10f;
            k.perTurnIncome.gold += tradeBonus;
        }
    }

    // No peace dividend — peace is the default state, not a bonus to exploit

    // Personality income bonuses — each personality has a unique economic path
    {
        // Aggressive: tribute from conquered cities (war is profitable)
        if (k.personality == KingdomPersonality::Aggressive) {
            int conqueredCities = 0;
            for (CityID cid : k.cities) {
                auto it = cities.find(cid);
                if (it == cities.end()) continue;
                if (it->second.originalOwner != k.id) ++conqueredCities;
            }
            k.perTurnIncome.gold += conqueredCities * 9.0f;
        }

        // Expansionist: empire administration income — all cities pay per turn
        // (settler culture: every colony contributes taxes to the crown)
        if (k.personality == KingdomPersonality::Expansionist) {
            k.perTurnIncome.gold += static_cast<float>(k.cities.size()) * 6.0f;
        }

        // Opportunistic: profits from other kingdoms' wars AND extracts maximum from their own cities
        if (k.personality == KingdomPersonality::Opportunistic) {
            int warringKingdoms = 0;
            for (const auto& [okid, ok] : kingdoms) {
                if (okid == k.id || !ok.isAlive) continue;
                if (ok.warWeariness > 0.25f) ++warringKingdoms;
            }
            k.perTurnIncome.gold += warringKingdoms * 6.0f;
            // Short-term extraction: squeeze every city for maximum immediate revenue
            k.perTurnIncome.gold += static_cast<float>(k.cities.size()) * 5.0f;
        }

        // Economic: production multiplier (their cities produce more)
        if (k.personality == KingdomPersonality::Economic) {
            k.perTurnIncome.gold  *= 1.22f;
            k.perTurnIncome.food  *= 1.15f;
            k.perTurnIncome.stone *= 1.15f;
        }

        // Expansionist: fast-growing cities produce more food (settler culture)
        if (k.personality == KingdomPersonality::Expansionist) {
            k.perTurnIncome.food *= 1.18f;
        }
    }
}

void EconomyEngine::computeExpenses(
    Kingdom& k,
    const std::unordered_map<ArmyID, Army>& armies) const
{
    k.perTurnExpense = {};
    // Aggressive kingdoms run an efficient military machine (-20% upkeep)
    const float personalityUpkeep = (k.personality == KingdomPersonality::Aggressive) ? 0.80f : 1.0f;
    for (ArmyID aid : k.armies) {
        auto it = armies.find(aid);
        if (it == armies.end()) continue;
        float upkeepMult = (it->second.isMercenary ? 2.5f : 1.0f) * personalityUpkeep;
        for (const auto& u : it->second.units) {
            k.perTurnExpense.gold += u.soldiers * 0.001f * unitUpkeepPerK(u.type) * upkeepMult;
        }
    }

    // City maintenance
    k.perTurnExpense.gold += k.cities.size() * 2.0f;

}

void EconomyEngine::applyStarvation(
    Kingdom& k,
    std::unordered_map<CityID, City>& cities,
    EventBus& bus,
    TurnNumber turn) const
{
    float foodNeeded = k.totalPopulation * 0.001f * C::FOOD_PER_1K_POP;

    if (k.treasury.food < foodNeeded) {
        k.starvationTurns++;
        float deficit   = foodNeeded - k.treasury.food;
        float lossFrac  = std::min(0.05f, (deficit / foodNeeded) * C::STARVATION_POP_LOSS);

        k.morale  -= C::STARVATION_MORALE;
        k.morale   = std::max(0.0f, k.morale);

        // Reduce population in each city proportionally
        for (CityID cid : k.cities) {
            auto it = cities.find(cid);
            if (it == cities.end()) continue;
            City& c = it->second;
            uint32_t loss = static_cast<uint32_t>(c.population * lossFrac);
            c.population  = (c.population > loss) ? c.population - loss : 1;
            c.happiness   = std::max(0.0f, c.happiness - 0.05f);
        }

        // Emit starvation event if severe
        if (k.starvationTurns == 3) {
            HistoryEvent ev;
            ev.type            = EventType::RebellionStarted;
            ev.turn            = turn;
            ev.primaryKingdom  = k.id;
            ev.description     = k.name + " faces severe famine.";
            ev.context["starvation_turns"] = static_cast<float>(k.starvationTurns);
            bus.emit(std::move(ev));
        }
        k.treasury.food = 0;
    } else {
        k.starvationTurns = std::max(0, k.starvationTurns - 1);
        k.treasury.food  -= foodNeeded;
        k.morale          = std::min(1.0f, k.morale + 0.01f);
    }
}

void EconomyEngine::growPopulation(
    Kingdom& k,
    std::unordered_map<CityID, City>& cities) const
{
    k.totalPopulation = 0;
    for (CityID cid : k.cities) {
        auto it = cities.find(cid);
        if (it == cities.end()) continue;
        City& c = it->second;
        if (!c.isRuined) {
            float growthRate = 0.005f * c.happiness;
            c.population = static_cast<uint32_t>(c.population * (1.0f + growthRate));
        }
        k.totalPopulation += c.population;
    }
}

void EconomyEngine::updateCityHappiness(const Kingdom& k, City& c) const {
    float target = 0.7f;
    // Better stability → better happiness
    target += (k.stability - 0.5f) * 0.2f;
    // Low tax rate → better happiness
    target -= (c.taxRate - 0.15f) * 1.5f;
    // Under siege → unhappy
    if (c.underSiege) target -= 0.3f;

    // Drift toward target
    c.happiness += (target - c.happiness) * 0.1f;
    c.happiness  = std::clamp(c.happiness, 0.0f, 1.0f);
}

} // namespace jke
