#include "jke/generators/ArmyGenerator.hpp"
#include <array>

namespace jke {
namespace {
std::string officerName(Random& rng) {
    constexpr std::array<const char*, 16> first = {
        "Alden", "Bran", "Cedric", "Darian", "Elric", "Gareth", "Hadrian", "Ilyas",
        "Kael", "Lucan", "Marek", "Osric", "Ronan", "Sylas", "Theron", "Varyn"
    };
    constexpr std::array<const char*, 16> title = {
        "Ironhand", "Stormwatch", "Greyford", "Dawnblade", "Redvale", "Highmarch",
        "Stonehelm", "Ashcourt", "Riverguard", "Wolfbanner", "Brightspear", "Blackfen",
        "Westfall", "Silverkeep", "Oakshield", "Emberlane"
    };
    return std::string(first[static_cast<size_t>(rng.nextInt(0, static_cast<int>(first.size()) - 1))]) +
           " " +
           title[static_cast<size_t>(rng.nextInt(0, static_cast<int>(title.size()) - 1))];
}

ArmyCommander makeCommander(Random& rng, const Kingdom& kingdom) {
    ArmyCommander c;
    c.name  = officerName(rng);
    c.alive = true;

    // Stat ranges reflect personality archetype
    switch (kingdom.personality) {
        case KingdomPersonality::Aggressive:
            // 猛攻型 — 攻撃と追撃が得意、守りは薄い
            c.attack  = rng.nextFloat(1.08f, 1.22f);
            c.defense = rng.nextFloat(0.96f, 1.08f);
            c.cavalry = rng.nextFloat(1.00f, 1.14f);
            c.morale  = rng.nextFloat(1.02f, 1.12f);
            c.pursuit = rng.nextFloat(1.08f, 1.20f);
            break;
        case KingdomPersonality::Defensive:
            // 守護型 — 防御と士気が高く、追撃は控えめ
            c.attack  = rng.nextFloat(1.00f, 1.10f);
            c.defense = rng.nextFloat(1.10f, 1.24f);
            c.cavalry = rng.nextFloat(0.96f, 1.08f);
            c.morale  = rng.nextFloat(1.06f, 1.16f);
            c.pursuit = rng.nextFloat(0.94f, 1.06f);
            break;
        case KingdomPersonality::Expansionist:
            // 遠征型 — 騎馬と追撃で素早く版図を広げる
            c.attack  = rng.nextFloat(1.04f, 1.16f);
            c.defense = rng.nextFloat(1.00f, 1.12f);
            c.cavalry = rng.nextFloat(1.08f, 1.22f);
            c.morale  = rng.nextFloat(1.02f, 1.12f);
            c.pursuit = rng.nextFloat(1.06f, 1.18f);
            break;
        case KingdomPersonality::Economic:
            // 管理型 — 兵士の士気を高く保つが攻勢は弱い
            c.attack  = rng.nextFloat(1.00f, 1.10f);
            c.defense = rng.nextFloat(1.02f, 1.14f);
            c.cavalry = rng.nextFloat(0.98f, 1.10f);
            c.morale  = rng.nextFloat(1.10f, 1.22f);
            c.pursuit = rng.nextFloat(0.96f, 1.08f);
            break;
        case KingdomPersonality::Diplomatic:
            // 慎重型 — 損害を抑えて士気を維持する、消耗戦向き
            c.attack  = rng.nextFloat(0.98f, 1.10f);
            c.defense = rng.nextFloat(1.06f, 1.18f);
            c.cavalry = rng.nextFloat(1.00f, 1.12f);
            c.morale  = rng.nextFloat(1.08f, 1.20f);
            c.pursuit = rng.nextFloat(0.94f, 1.06f);
            break;
        case KingdomPersonality::Opportunistic:
            // 奇襲型 — 攻撃と追撃で機を突く、ムラがある
            c.attack  = rng.nextFloat(1.04f, 1.18f);
            c.defense = rng.nextFloat(0.98f, 1.10f);
            c.cavalry = rng.nextFloat(1.04f, 1.16f);
            c.morale  = rng.nextFloat(1.00f, 1.12f);
            c.pursuit = rng.nextFloat(1.04f, 1.18f);
            break;
    }

    // Specialization bonuses
    switch (kingdom.specialization) {
        case KingdomSpecialization::Military:
            c.attack += 0.03f; c.morale += 0.03f; break;
        case KingdomSpecialization::Defense:
            c.defense += 0.05f; break;
        case KingdomSpecialization::Economy:
            c.morale += 0.03f; break;
        case KingdomSpecialization::Agriculture:
            c.morale += 0.02f; break;
        case KingdomSpecialization::Technology:
            c.attack += 0.02f; break;
        case KingdomSpecialization::Trade:
            c.cavalry += 0.03f; c.pursuit += 0.02f; break;
    }

    c.fame = rng.nextFloat(0.12f, 0.36f);
    return c;
}

ArmyStrategist makeStrategist(Random& rng, const Kingdom& kingdom) {
    ArmyStrategist s;
    s.name  = officerName(rng);
    s.alive = true;

    // Stat ranges reflect personality archetype
    switch (kingdom.personality) {
        case KingdomPersonality::Aggressive:
            // 強襲型 — 攻城と奇襲が得意、撤退は苦手
            s.siege     = rng.nextFloat(1.06f, 1.20f);
            s.ambush    = rng.nextFloat(1.04f, 1.16f);
            s.retreat   = rng.nextFloat(0.94f, 1.06f);
            s.logistics = rng.nextFloat(1.00f, 1.12f);
            s.lure      = rng.nextFloat(0.98f, 1.10f);
            break;
        case KingdomPersonality::Defensive:
            // 撤退術師 — 補給と撤退が得意、攻城は平凡
            s.siege     = rng.nextFloat(1.00f, 1.12f);
            s.ambush    = rng.nextFloat(0.96f, 1.08f);
            s.retreat   = rng.nextFloat(1.10f, 1.24f);
            s.logistics = rng.nextFloat(1.06f, 1.18f);
            s.lure      = rng.nextFloat(1.00f, 1.12f);
            break;
        case KingdomPersonality::Expansionist:
            // 遠征参謀 — 補給と誘引で長期戦を制する
            s.siege     = rng.nextFloat(1.02f, 1.14f);
            s.ambush    = rng.nextFloat(1.00f, 1.12f);
            s.retreat   = rng.nextFloat(1.00f, 1.12f);
            s.logistics = rng.nextFloat(1.08f, 1.22f);
            s.lure      = rng.nextFloat(1.06f, 1.18f);
            break;
        case KingdomPersonality::Economic:
            // 補給の鬼 — 兵站が突出、戦術的奇手は少ない
            s.siege     = rng.nextFloat(1.00f, 1.12f);
            s.ambush    = rng.nextFloat(0.96f, 1.08f);
            s.retreat   = rng.nextFloat(1.02f, 1.14f);
            s.logistics = rng.nextFloat(1.10f, 1.24f);
            s.lure      = rng.nextFloat(1.00f, 1.12f);
            break;
        case KingdomPersonality::Diplomatic:
            // 謀将型 — 誘引と撤退で消耗を避ける、攻城は不得手
            s.siege     = rng.nextFloat(0.96f, 1.08f);
            s.ambush    = rng.nextFloat(0.98f, 1.10f);
            s.retreat   = rng.nextFloat(1.06f, 1.18f);
            s.logistics = rng.nextFloat(1.04f, 1.16f);
            s.lure      = rng.nextFloat(1.08f, 1.22f);
            break;
        case KingdomPersonality::Opportunistic:
            // 奇計師 — 奇襲と誘引が突出、補給は二の次
            s.siege     = rng.nextFloat(1.00f, 1.12f);
            s.ambush    = rng.nextFloat(1.10f, 1.24f);
            s.retreat   = rng.nextFloat(1.00f, 1.12f);
            s.logistics = rng.nextFloat(0.98f, 1.10f);
            s.lure      = rng.nextFloat(1.08f, 1.22f);
            break;
    }

    // Specialization bonuses
    switch (kingdom.specialization) {
        case KingdomSpecialization::Military:
            s.ambush += 0.03f; break;
        case KingdomSpecialization::Defense:
            s.retreat += 0.05f; s.siege += 0.02f; break;
        case KingdomSpecialization::Economy:
            s.logistics += 0.04f; break;
        case KingdomSpecialization::Agriculture:
            s.logistics += 0.03f; break;
        case KingdomSpecialization::Technology:
            s.siege += 0.05f; s.logistics += 0.04f; break;
        case KingdomSpecialization::Trade:
            s.lure += 0.04f; s.logistics += 0.02f; break;
    }

    s.fame = rng.nextFloat(0.10f, 0.32f);
    return s;
}
}

ArmyGenerator::ArmyGenerator(Random& rng) : rng_(rng) {}

void ArmyGenerator::generate(GeneratedWorld& world) {
    for (auto& [kid, kingdom] : world.kingdoms) {
        // ── Main army at capital ───────────────────────────────────────────
        if (kingdom.capitalCity != NO_CITY) {
            Army army = buildStartingArmy(world, kingdom);
            ArmyID aid = world.nextArmyID++;
            army.id    = aid;
            army.owner = kid;
            const City& cap = world.cities.at(kingdom.capitalCity);
            army.currentTile = cap.tile;
            army.position    = cap.position;
            world.worldMap.at(cap.tile).army = aid;
            kingdom.armies.push_back(aid);
            world.armies[aid] = std::move(army);
        }

        // ── Garrison at each secondary city ───────────────────────────────
        for (CityID cid : kingdom.cities) {
            if (cid == kingdom.capitalCity) continue;
            auto cit = world.cities.find(cid);
            if (cit == world.cities.end()) continue;
            const City& city = cit->second;
            if (city.tile == NO_TILE) continue;

            // Skip if tile already has an army
            if (world.worldMap.at(city.tile).army != NO_ARMY) continue;

            Army garrison = buildGarrison(world, kingdom);
            ArmyID gid = world.nextArmyID++;
            garrison.id    = gid;
            garrison.owner = kid;
            garrison.currentTile = city.tile;
            garrison.position    = city.position;
            garrison.role        = ArmyRole::Defense;
            world.worldMap.at(city.tile).army = gid;
            kingdom.armies.push_back(gid);
            world.armies[gid] = std::move(garrison);
        }
    }
}

Army ArmyGenerator::buildStartingArmy(GeneratedWorld& world, const Kingdom& kingdom) const {
    Army army;
    army.supplyLevel = 1.0f;
    army.commander = makeCommander(rng_, kingdom);
    if (kingdom.specialization == KingdomSpecialization::Military ||
        kingdom.specialization == KingdomSpecialization::Technology ||
        rng_.chance(0.55f)) {
        army.strategist = makeStrategist(rng_, kingdom);
    }

    // Composition based on specialization
    switch (kingdom.specialization) {
        case KingdomSpecialization::Military:
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Infantry, 2000, 0.7f, 0.7f));
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Spearmen, 1000, 0.6f, 0.6f));
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Cavalry,   500, 0.65f, 0.65f));
            break;
        case KingdomSpecialization::Defense:
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Spearmen, 2000, 0.65f, 0.7f));
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Archers,   800, 0.6f, 0.6f));
            break;
        case KingdomSpecialization::Economy:
        case KingdomSpecialization::Trade:
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Infantry, 1000, 0.5f, 0.5f));
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Militia,  1000, 0.4f, 0.4f));
            break;
        case KingdomSpecialization::Agriculture:
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Militia,  1500, 0.45f, 0.4f));
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Infantry,  500, 0.5f, 0.5f));
            break;
        case KingdomSpecialization::Technology:
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Archers,   800, 0.6f, 0.65f));
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Infantry,  700, 0.55f, 0.55f));
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::SiegeUnit, 200, 0.5f, 0.6f));
            break;
    }
    return army;
}

Army ArmyGenerator::buildGarrison(GeneratedWorld& world, const Kingdom& kingdom) const {
    Army army;
    army.supplyLevel = 1.0f;
    // Garrisons are smaller — mostly militia/infantry
    uint32_t soldiers = 600u + static_cast<uint32_t>(kingdom.cities.size()) * 80u;
    soldiers = std::min(soldiers, 1200u);
    switch (kingdom.specialization) {
        case KingdomSpecialization::Defense:
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Spearmen, soldiers, 0.55f, 0.55f));
            break;
        case KingdomSpecialization::Military:
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Infantry, soldiers, 0.55f, 0.55f));
            break;
        default:
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Militia, soldiers * 3 / 4, 0.45f, 0.45f));
            army.units.push_back(buildUnit(world.nextUnitID++, UnitType::Infantry, soldiers / 4, 0.50f, 0.50f));
            break;
    }
    return army;
}

Unit ArmyGenerator::buildUnit(UnitID id, UnitType type, uint32_t soldiers,
                               float training, float equipment) const {
    Unit u;
    u.id         = id;
    u.type       = type;
    u.soldiers   = soldiers;
    u.training   = training;
    u.equipment  = equipment;
    u.experience = 0.0f;
    u.morale     = 1.0f;
    u.supply     = 1.0f;
    return u;
}

} // namespace jke
