#include "jke/engines/TechnologyEngine.hpp"
#include <algorithm>

namespace jke {

void TechnologyEngine::update(
    std::unordered_map<KingdomID, Kingdom>& kingdoms,
    const TechTree& techTree,
    EventBus& bus,
    TurnNumber turn)
{
    for (auto& [kid, k] : kingdoms) {
        if (!k.isAlive || k.currentResearch == NO_TECH) continue;

        const Technology* tech = techTree.find(k.currentResearch);
        if (!tech) { k.currentResearch = NO_TECH; continue; }

        // Speed bonus from Library building (not computed here, use modifier)
        float speed = 1.0f;
        auto it = k.researchedTechs.find(11); // Writing
        if (it != k.researchedTechs.end()) speed += 0.10f;

        k.researchProgress += speed;

        if (k.researchProgress >= static_cast<float>(tech->turnsToComplete)) {
            k.researchedTechs.insert(k.currentResearch);

            HistoryEvent ev;
            ev.type            = EventType::TechResearched;
            ev.turn            = turn;
            ev.primaryKingdom  = kid;
            ev.description     = k.name + " researched " + tech->name + ".";
            bus.emit(std::move(ev));

            k.currentResearch  = NO_TECH;
            k.researchProgress = 0.0f;

            recomputeAllModifiers(k, techTree);
        }
    }
}

void TechnologyEngine::applyTechModifiers(Kingdom& k, const Technology& tech) const {
    for (const auto& [key, val] : tech.modifiers) {
        if (key == "food_prod")             k.foodBonus        += val;
        else if (key == "gold_income")      k.goldBonus        += val;
        else if (key == "infantry_combat")  k.combatBonus      += val;
        else if (key == "cavalry_combat")   k.combatBonus      += val * 0.5f;
        else if (key == "archer_combat")    k.combatBonus      += val * 0.5f;
        else if (key == "spear_combat")     k.combatBonus      += val * 0.5f;
        else if (key == "infantry_defense") k.defenseBonus     += val;
        else if (key == "city_defense")     k.defenseBonus     += val;
        else if (key == "siege_power")      k.siegeBonus       += val;
    }
}

void TechnologyEngine::recomputeAllModifiers(Kingdom& k, const TechTree& tree) const {
    // Reset to base
    k.combatBonus  = 1.0f;
    k.foodBonus    = 1.0f;
    k.goldBonus    = 1.0f;
    k.siegeBonus   = 1.0f;
    k.defenseBonus = 1.0f;

    for (TechID tid : k.researchedTechs) {
        const Technology* t = tree.find(tid);
        if (t) applyTechModifiers(k, *t);
    }
}

} // namespace jke
