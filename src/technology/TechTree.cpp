#include "jke/technology/TechTree.hpp"
#include <algorithm>

namespace jke {

TechTree::TechTree() {
    buildTree();
}

void TechTree::buildTree() {
    // ── Agriculture ───────────────────────────────────────────────────────────
    addTech({1, "Crop Rotation", TechCategory::Agriculture,
        {30,20,0,0,0}, 4, {}, {{"food_prod", 0.15f}},
        "Rotating crops improves soil fertility."});

    addTech({2, "Irrigation", TechCategory::Agriculture,
        {20,30,10,0,0}, 5, {1}, {{"food_prod", 0.20f}, {"river_bonus", 0.10f}},
        "Channels water to dry fields."});

    addTech({3, "Selective Breeding", TechCategory::Agriculture,
        {40,20,0,0,0}, 6, {1}, {{"food_prod", 0.10f}, {"cavalry_training", 0.10f}},
        "Breeding stronger horses and livestock."});

    // ── Infrastructure ────────────────────────────────────────────────────────
    addTech({4, "Roads", TechCategory::Infrastructure,
        {20,40,20,10,0}, 6, {}, {{"move_speed", 0.25f}},
        "Paved roads speed up army movement."});

    addTech({5, "Bridges", TechCategory::Infrastructure,
        {20,30,15,20,0}, 7, {4}, {{"river_penalty_reduce", 0.5f}},
        "Stone bridges allow crossing rivers swiftly."});

    addTech({6, "Aqueducts", TechCategory::Infrastructure,
        {30,50,10,30,0}, 8, {2, 4}, {{"pop_growth", 0.15f}, {"happiness", 0.10f}},
        "Clean water supply boosts city growth."});

    // ── Military ──────────────────────────────────────────────────────────────
    addTech({7, "Iron Working", TechCategory::Military,
        {20,30,0,0,30}, 5, {}, {{"infantry_combat", 0.15f}, {"spear_combat", 0.15f}},
        "Iron weapons give a decisive edge."});

    addTech({8, "Cavalry Training", TechCategory::Military,
        {30,40,0,0,10}, 6, {3}, {{"cavalry_combat", 0.20f}},
        "Disciplined cavalry charges break enemy lines."});

    addTech({9, "Composite Bows", TechCategory::Military,
        {20,30,0,0,15}, 5, {}, {{"archer_combat", 0.20f}, {"archer_range", 0.15f}},
        "Recurve bows made from horn and sinew."});

    addTech({10, "Shield Wall", TechCategory::Military,
        {15,20,0,0,20}, 4, {7}, {{"infantry_defense", 0.20f}},
        "Interlocking shields form an impenetrable wall."});

    // ── Administration ────────────────────────────────────────────────────────
    addTech({11, "Writing", TechCategory::Administration,
        {20,30,0,0,0}, 5, {}, {{"research_speed", 0.10f}, {"admin_efficiency", 0.10f}},
        "Written records enable better governance."});

    addTech({12, "Administration", TechCategory::Administration,
        {30,40,0,0,0}, 7, {11}, {{"stability", 0.10f}, {"tax_efficiency", 0.10f}},
        "Organized bureaucracy increases state efficiency."});

    addTech({13, "Taxation", TechCategory::Administration,
        {20,30,0,0,0}, 5, {11}, {{"gold_income", 0.15f}},
        "Systematic taxation raises more revenue."});

    // ── Economy ───────────────────────────────────────────────────────────────
    addTech({14, "Trade Routes", TechCategory::Economy,
        {20,50,0,0,0}, 6, {11}, {{"gold_income", 0.20f}, {"trade_bonus", 0.15f}},
        "Long-distance trade brings wealth."});

    addTech({15, "Currency", TechCategory::Economy,
        {10,60,0,0,10}, 5, {14}, {{"gold_income", 0.10f}, {"market_efficiency", 0.15f}},
        "Standardized coins simplify commerce."});

    addTech({16, "Mining", TechCategory::Economy,
        {20,30,10,0,10}, 5, {}, {{"stone_prod", 0.20f}, {"iron_prod", 0.20f}},
        "Improved mining techniques extract more ore."});

    // ── Medicine ──────────────────────────────────────────────────────────────
    addTech({17, "Herbalism", TechCategory::Medicine,
        {30,20,0,0,0}, 4, {}, {{"pop_growth", 0.10f}, {"morale_recovery", 0.05f}},
        "Knowledge of healing plants reduces casualties."});

    addTech({18, "Surgery", TechCategory::Medicine,
        {40,30,0,0,0}, 6, {17}, {{"battle_casualty_reduce", 0.15f}, {"pop_growth", 0.10f}},
        "Field surgeons save lives after battle."});

    // ── Fortification ─────────────────────────────────────────────────────────
    addTech({19, "Fortification", TechCategory::Fortification,
        {20,30,10,40,0}, 6, {}, {{"city_defense", 0.25f}, {"fortification_build_speed", 0.15f}},
        "Thick stone walls resist enemy assaults."});

    addTech({20, "Tower Defense", TechCategory::Fortification,
        {20,30,0,30,10}, 7, {19}, {{"city_defense", 0.15f}, {"archer_city_bonus", 0.20f}},
        "Towers give archers superior firing positions."});

    // ── Siege ─────────────────────────────────────────────────────────────────
    addTech({21, "Battering Ram", TechCategory::Siege,
        {20,20,0,10,15}, 5, {}, {{"siege_power", 0.20f}},
        "Heavy rams break down wooden gates."});

    addTech({22, "Siege Tower", TechCategory::Siege,
        {30,30,0,20,10}, 6, {21}, {{"siege_power", 0.20f}, {"siege_casualty_reduce", 0.10f}},
        "Towers allow troops to scale enemy walls."});

    addTech({23, "Catapult", TechCategory::Siege,
        {40,30,0,15,20}, 8, {21, 16}, {{"siege_power", 0.30f}, {"city_wall_damage", 0.20f}},
        "Stone-hurling engines batter enemy fortifications."});
}

const Technology* TechTree::find(TechID id) const {
    for (const auto& t : techs_)
        if (t.id == id) return &t;
    return nullptr;
}

std::vector<TechID> TechTree::available(const std::unordered_set<TechID>& researched) const {
    std::vector<TechID> result;
    for (const auto& t : techs_) {
        if (researched.count(t.id)) continue;
        bool prereqsMet = std::all_of(t.prerequisites.begin(), t.prerequisites.end(),
                                       [&](TechID pid){ return researched.count(pid) > 0; });
        if (prereqsMet) result.push_back(t.id);
    }
    return result;
}

} // namespace jke
