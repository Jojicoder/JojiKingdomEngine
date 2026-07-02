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
        "By resting fields in cycles, the soil recovers its strength. Kingdoms that master this never face lean years from exhausted land."});

    addTech({2, "Irrigation", TechCategory::Agriculture,
        {20,30,10,0,0}, 5, {1}, {{"food_prod", 0.20f}, {"river_bonus", 0.10f}},
        "Stone channels carry river water deep into dry farmland. A kingdom that commands water commands its people's survival."});

    addTech({3, "Selective Breeding", TechCategory::Agriculture,
        {40,20,0,0,0}, 6, {1}, {{"food_prod", 0.10f}, {"cavalry_training", 0.10f}},
        "Generations of careful breeding produce mounts faster and livestock meatier. The war horse is born in the pasture, not the battlefield."});

    // ── Infrastructure ────────────────────────────────────────────────────────
    addTech({4, "Roads", TechCategory::Infrastructure,
        {20,40,20,10,0}, 6, {}, {{"move_speed", 0.25f}},
        "Armies that march on cobblestone arrive fresh; those on mud arrive late. Roads are the veins of an empire."});

    addTech({5, "Bridges", TechCategory::Infrastructure,
        {20,30,15,20,0}, 7, {4}, {{"river_penalty_reduce", 0.5f}},
        "Rivers that once stopped armies for weeks become mere crossings. He who builds bridges chooses where battles are fought."});

    addTech({6, "Aqueducts", TechCategory::Infrastructure,
        {30,50,10,30,0}, 8, {2, 4}, {{"pop_growth", 0.15f}, {"happiness", 0.10f}},
        "Clean water flowing into cities cuts plague and fuels population growth. The greatest cities in history were built around their aqueducts."});

    // ── Military ──────────────────────────────────────────────────────────────
    addTech({7, "Iron Working", TechCategory::Military,
        {20,30,0,0,30}, 5, {}, {{"infantry_combat", 0.15f}, {"spear_combat", 0.15f}},
        "Iron swords shatter bronze. The kingdom that smelts iron first can arm a thousand men against foes still fighting with stone."});

    addTech({8, "Cavalry Training", TechCategory::Military,
        {30,40,0,0,10}, 6, {3}, {{"cavalry_combat", 0.20f}},
        "Riders who live in the saddle become extensions of their mounts. A disciplined cavalry charge has broken armies ten times its size."});

    addTech({9, "Composite Bows", TechCategory::Military,
        {20,30,0,0,15}, 5, {}, {{"archer_combat", 0.20f}, {"archer_range", 0.15f}},
        "Laminated from horn, sinew, and wood, composite bows punch through armor at ranges a longbow cannot reach."});

    addTech({10, "Shield Wall", TechCategory::Military,
        {15,20,0,0,20}, 4, {7}, {{"infantry_defense", 0.20f}},
        "Shields locked rim-to-rim, spears braced — a shield wall turns infantry into a moving fortress. Few cavalry charges survive contact."});

    // ── Administration ────────────────────────────────────────────────────────
    addTech({11, "Writing", TechCategory::Administration,
        {20,30,0,0,0}, 5, {}, {{"research_speed", 0.10f}, {"admin_efficiency", 0.10f}},
        "Laws scratched in clay outlast the rulers who wrote them. Written records allow kingdoms to govern lands their rulers will never see."});

    addTech({12, "Administration", TechCategory::Administration,
        {30,40,0,0,0}, 7, {11}, {{"stability", 0.10f}, {"tax_efficiency", 0.10f}},
        "Scribes, registrars, and appointed governors extend the ruler's will across provinces. An administered kingdom bends but does not break."});

    addTech({13, "Taxation", TechCategory::Administration,
        {20,30,0,0,0}, 5, {11}, {{"gold_income", 0.15f}},
        "Systematic census and levy replace ad-hoc tribute. Kingdoms that tax well fund armies that kingdoms taxing poorly cannot match."});

    // ── Economy ───────────────────────────────────────────────────────────────
    addTech({14, "Trade Routes", TechCategory::Economy,
        {20,50,0,0,0}, 6, {11}, {{"gold_income", 0.20f}, {"trade_bonus", 0.15f}},
        "Merchants who travel known safe roads bring back more than goods — they bring intelligence, alliances, and the wealth of distant lands."});

    addTech({15, "Currency", TechCategory::Economy,
        {10,60,0,0,10}, 5, {14}, {{"gold_income", 0.10f}, {"market_efficiency", 0.15f}},
        "Stamped coins replace barter. A kingdom with accepted currency can pay soldiers, fund allies, and corrupt enemies — all without drawing a sword."});

    addTech({16, "Mining", TechCategory::Economy,
        {20,30,10,0,10}, 5, {}, {{"stone_prod", 0.20f}, {"iron_prod", 0.20f}},
        "Shafts driven deeper, tunnels shored with timber — systematic mining turns a hillside into an endless supply of iron and stone."});

    // ── Medicine ──────────────────────────────────────────────────────────────
    addTech({17, "Herbalism", TechCategory::Medicine,
        {30,20,0,0,0}, 4, {}, {{"pop_growth", 0.10f}, {"morale_recovery", 0.05f}},
        "Healers who know which roots stanch wounds and which leaves fight fever keep armies marching longer than any commander's orders."});

    addTech({18, "Surgery", TechCategory::Medicine,
        {40,30,0,0,0}, 6, {17}, {{"battle_casualty_reduce", 0.15f}, {"pop_growth", 0.10f}},
        "Field surgeons who cut clean and bind fast return soldiers to the line that others would bury. Battles are often won by who heals faster."});

    // ── Fortification ─────────────────────────────────────────────────────────
    addTech({19, "Fortification", TechCategory::Fortification,
        {20,30,10,40,0}, 6, {}, {{"city_defense", 0.25f}, {"fortification_build_speed", 0.15f}},
        "Thick curtain walls, angled towers, deep ditches — a fortified city forces any attacker to pay in blood for every meter of ground."});

    addTech({20, "Tower Defense", TechCategory::Fortification,
        {20,30,0,30,10}, 7, {19}, {{"city_defense", 0.15f}, {"archer_city_bonus", 0.20f}},
        "Archers elevated in flanking towers can pour arrows into attackers who cannot effectively shoot back. Height is armor."});

    // ── Siege ─────────────────────────────────────────────────────────────────
    addTech({21, "Battering Ram", TechCategory::Siege,
        {20,20,0,10,15}, 5, {}, {{"siege_power", 0.20f}},
        "A great log swung by many hands hits harder than any single blow. Rams have cracked gates that armies could not otherwise breach for months."});

    addTech({22, "Siege Tower", TechCategory::Siege,
        {30,30,0,20,10}, 6, {21}, {{"siege_power", 0.20f}, {"siege_casualty_reduce", 0.10f}},
        "Rolling towers wheel to the wall and drop their bridges. Defenders who could hold a gate for weeks find the wall itself has become the enemy's road."});

    addTech({23, "Catapult", TechCategory::Siege,
        {40,30,0,15,20}, 8, {21, 16}, {{"siege_power", 0.30f}, {"city_wall_damage", 0.20f}},
        "Stone balls as heavy as a man, hurled at battlements — catapults make the walls themselves the enemy. What took months to build can crumble in days."});
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
