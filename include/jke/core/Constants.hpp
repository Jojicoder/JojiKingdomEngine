#pragma once
#include <cstdint>

namespace jke::constants {

// World
constexpr int   MAP_SIZE            = 257;  // 2^8 + 1 for diamond-square
constexpr int   NUM_KINGDOMS        = 20;
constexpr float OCEAN_THRESHOLD     = 0.20f;
constexpr float COAST_THRESHOLD     = 0.25f;
constexpr float PLAIN_THRESHOLD     = 0.45f;
constexpr float FOREST_THRESHOLD    = 0.58f;
constexpr float HILL_THRESHOLD      = 0.70f;
// Above HILL_THRESHOLD = Mountain

constexpr float RIVER_SOURCE_ELEV   = 0.72f;
constexpr int   NUM_RIVERS          = 12;
constexpr float LAKE_PROB           = 0.04f;

// Economy
constexpr float BASE_FOOD_PER_CITY   = 50.0f;
constexpr float BASE_GOLD_PER_CITY   = 30.0f;
constexpr float ARMY_UPKEEP_PER_1K   = 5.0f;   // gold per 1000 soldiers
constexpr float FOOD_PER_1K_POP      = 1.0f;
constexpr float STARVATION_POP_LOSS  = 0.02f;   // 2% per turn when starving
constexpr float STARVATION_MORALE    = 0.05f;

// Stability / rebellion
constexpr float REBELLION_THRESHOLD  = 0.30f;   // stability below this → risk
constexpr float CIVIL_WAR_THRESHOLD  = 0.15f;

// Combat
constexpr float RANDOM_COMBAT_FACTOR = 0.10f;   // ±10%
constexpr float RETREAT_MORALE_THRESHOLD = 0.25f;

// Seasons
constexpr int   TURNS_PER_SEASON     = 20;   // 80 turns per year

// Bandits
constexpr int   BANDIT_SPAWN_INTERVAL = 35;   // turns between new bandit groups
constexpr int   MAX_BANDIT_GROUPS     = 8;
constexpr float BANDIT_RAID_GOLD      = 30.0f;
constexpr float BANDIT_RAID_FOOD      = 20.0f;

// Technology
constexpr int   MAX_TECH_LEVEL       = 5;

// Kingdom starting resources
constexpr float START_FOOD   = 200.0f;
constexpr float START_GOLD   = 150.0f;
constexpr float START_WOOD   = 100.0f;
constexpr float START_STONE  = 80.0f;
constexpr float START_IRON   = 60.0f;

} // namespace jke::constants
