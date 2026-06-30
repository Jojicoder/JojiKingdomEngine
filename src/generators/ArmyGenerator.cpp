#include "jke/generators/ArmyGenerator.hpp"

namespace jke {

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
