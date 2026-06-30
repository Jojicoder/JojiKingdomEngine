#pragma once
#include <unordered_map>
#include "jke/nation/Kingdom.hpp"
#include "jke/technology/TechTree.hpp"
#include "jke/core/EventBus.hpp"

namespace jke {

class TechnologyEngine {
public:
    void update(
        std::unordered_map<KingdomID, Kingdom>& kingdoms,
        const TechTree& techTree,
        EventBus& bus,
        TurnNumber turn
    );

private:
    void applyTechModifiers(Kingdom& k, const Technology& tech) const;
    void recomputeAllModifiers(Kingdom& k, const TechTree& tree) const;
};

} // namespace jke
