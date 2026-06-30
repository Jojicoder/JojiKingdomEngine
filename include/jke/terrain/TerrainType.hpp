#pragma once
#include <string_view>

namespace jke {

enum class TerrainType : uint8_t {
    Ocean   = 0,
    Coast   = 1,
    Plain   = 2,
    Forest  = 3,
    Hill    = 4,
    Mountain= 5,
    River   = 6,
    Lake    = 7,
};

constexpr std::string_view terrainName(TerrainType t) noexcept {
    switch (t) {
        case TerrainType::Ocean:    return "Ocean";
        case TerrainType::Coast:    return "Coast";
        case TerrainType::Plain:    return "Plain";
        case TerrainType::Forest:   return "Forest";
        case TerrainType::Hill:     return "Hill";
        case TerrainType::Mountain: return "Mountain";
        case TerrainType::River:    return "River";
        case TerrainType::Lake:     return "Lake";
    }
    return "Unknown";
}

// Movement cost multiplier (1.0 = normal)
constexpr float terrainMoveCost(TerrainType t) noexcept {
    switch (t) {
        case TerrainType::Ocean:    return 999.0f;
        case TerrainType::Coast:    return 1.5f;
        case TerrainType::Plain:    return 1.0f;
        case TerrainType::Forest:   return 1.8f;
        case TerrainType::Hill:     return 2.0f;
        case TerrainType::Mountain: return 4.0f;
        case TerrainType::River:    return 1.3f;
        case TerrainType::Lake:     return 999.0f;
    }
    return 1.0f;
}

// Base fertility (food production modifier)
constexpr float terrainFertility(TerrainType t) noexcept {
    switch (t) {
        case TerrainType::Plain:    return 1.0f;
        case TerrainType::Forest:   return 0.6f;
        case TerrainType::Hill:     return 0.4f;
        case TerrainType::River:    return 1.3f;
        case TerrainType::Coast:    return 0.7f;
        default:                    return 0.0f;
    }
}

// Border strength (higher = natural border)
constexpr float terrainBorderStrength(TerrainType t) noexcept {
    switch (t) {
        case TerrainType::Mountain: return 5.0f;
        case TerrainType::River:    return 3.0f;
        case TerrainType::Lake:     return 4.0f;
        case TerrainType::Hill:     return 2.0f;
        case TerrainType::Forest:   return 1.5f;
        default:                    return 1.0f;
    }
}

} // namespace jke
