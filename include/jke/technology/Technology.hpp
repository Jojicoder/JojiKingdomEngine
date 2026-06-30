#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "jke/core/Types.hpp"
#include "jke/economy/ResourceLedger.hpp"

namespace jke {

enum class TechCategory : uint8_t {
    Agriculture,
    Infrastructure,
    Military,
    Administration,
    Economy,
    Medicine,
    Fortification,
    Siege,
};

struct Technology {
    TechID      id              = NO_TECH;
    std::string name;
    TechCategory category       = TechCategory::Agriculture;
    ResourceLedger researchCost;
    uint32_t    turnsToComplete = 5;
    std::vector<TechID> prerequisites;
    std::unordered_map<std::string, float> modifiers;
    std::string description;
};

} // namespace jke
