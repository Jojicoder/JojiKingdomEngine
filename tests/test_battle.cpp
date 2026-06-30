#include "jke/army/Army.hpp"
#include "jke/army/Unit.hpp"
#define private public
#include "jke/engines/BattleEngine.hpp"
#undef private
#include "jke/terrain/TerrainType.hpp"
#include <cassert>
#include <iostream>
#include <unordered_map>

int main() {
    // Build two armies and compare combat strength
    jke::Army attacker;
    attacker.id = 1;
    {
        jke::Unit u;
        u.id       = 1;
        u.type     = jke::UnitType::Cavalry;
        u.soldiers = 1000;
        u.training = 0.8f;
        u.equipment= 0.8f;
        u.experience=0.5f;
        u.morale   = 1.0f;
        u.supply   = 1.0f;
        attacker.units.push_back(u);
    }

    jke::Army defender;
    defender.id = 2;
    {
        jke::Unit u;
        u.id       = 2;
        u.type     = jke::UnitType::Spearmen;
        u.soldiers = 1000;
        u.training = 0.6f;
        u.equipment= 0.6f;
        u.experience=0.0f;
        u.morale   = 1.0f;
        u.supply   = 1.0f;
        defender.units.push_back(u);
    }

    // On plains, cavalry should have advantage
    float atkStr = attacker.combatStrength(jke::TerrainType::Plain, false);
    float defStr = defender.combatStrength(jke::TerrainType::Plain, true);
    assert(atkStr > 0.0f && "Attacker strength must be positive");
    assert(defStr > 0.0f && "Defender strength must be positive");

    // On mountains, cavalry should be weakened significantly
    float atkMtn = attacker.combatStrength(jke::TerrainType::Mountain, false);
    assert(atkMtn < atkStr && "Cavalry should be weaker in mountains");

    // Test unit base power
    float power = attacker.units[0].baseCombatPower();
    assert(power > 0.0f && "Base combat power must be positive");

    // A defending city owner winning a field battle on its own city must not
    // count as conquering its own city.
    jke::Random rng(7);
    jke::BattleEngine battleEngine(rng);

    jke::WorldMap map(1, 1);
    map.at(0).id = 0;
    map.at(0).city = 1;
    map.at(0).owner = 1;

    std::unordered_map<jke::KingdomID, jke::Kingdom> kingdoms;
    kingdoms[1].id = 1;
    kingdoms[1].name = "Owner";
    kingdoms[1].cities.push_back(1);
    kingdoms[1].capitalCity = 1;
    kingdoms[2].id = 2;
    kingdoms[2].name = "Invader";

    std::unordered_map<jke::CityID, jke::City> cities;
    cities[1].id = 1;
    cities[1].name = "Capital";
    cities[1].owner = 1;
    cities[1].tile = 0;
    cities[1].isCapital = true;

    std::unordered_map<jke::ArmyID, jke::Army> armies;
    armies[10].id = 10;
    armies[10].owner = 1;
    armies[10].currentTile = 0;
    armies[10].units.push_back(attacker.units.front());
    armies[10].units.front().type = jke::UnitType::Infantry;
    armies[10].units.front().soldiers = 5000;

    armies[20].id = 20;
    armies[20].owner = 2;
    armies[20].currentTile = 0;
    armies[20].units.push_back(defender.units.front());
    armies[20].units.front().soldiers = 100;

    jke::BattleContext ctx;
    ctx.attackerArmy = 10;
    ctx.defenderArmy = 20;
    ctx.attackerKingdom = 1;
    ctx.defenderKingdom = 2;
    ctx.battleTile = 0;
    ctx.terrain = jke::TerrainType::Plain;
    ctx.contestedCity = 1;

    auto result = battleEngine.resolveBattle(ctx, armies, kingdoms, cities);
    assert(result.victor == 1);
    assert(!result.cityConquered && "Owner victory must not reconquer its own city");

    std::cout << "Battle tests PASSED\n";
    std::cout << "  Cavalry (plain) attack: " << atkStr << "\n";
    std::cout << "  Spearmen (plain) defense: " << defStr << "\n";
    std::cout << "  Cavalry (mountain) attack: " << atkMtn << "\n";
    return 0;
}
