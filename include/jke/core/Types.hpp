#pragma once
#include <cstdint>
#include <string>

namespace jke {

using KingdomID  = uint16_t;
using CityID     = uint32_t;
using ArmyID     = uint32_t;
using FleetID    = uint32_t;
using TileID     = uint32_t;
using UnitID     = uint32_t;
using TechID     = uint16_t;
using TurnNumber = uint32_t;
using EventID    = uint64_t;

constexpr KingdomID  NO_KINGDOM = 0;
constexpr CityID     NO_CITY    = 0;
constexpr ArmyID     NO_ARMY    = 0;
constexpr FleetID    NO_FLEET   = 0xFFFFFFFF;
constexpr TileID     NO_TILE    = 0xFFFFFFFF;
constexpr TechID     NO_TECH    = 0;

struct Coordinate {
    int32_t x = 0;
    int32_t y = 0;

    bool operator==(const Coordinate& o) const noexcept { return x == o.x && y == o.y; }
    bool operator!=(const Coordinate& o) const noexcept { return !(*this == o); }
};

} // namespace jke
