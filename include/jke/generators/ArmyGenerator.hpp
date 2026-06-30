#pragma once
#include "jke/generators/WorldGenerator.hpp"

namespace jke {

class ArmyGenerator {
public:
    explicit ArmyGenerator(Random& rng);
    void generate(GeneratedWorld& world);

private:
    Random& rng_;

    Army buildStartingArmy(GeneratedWorld& world, const Kingdom& kingdom) const;
    Army buildGarrison(GeneratedWorld& world, const Kingdom& kingdom) const;
    Unit buildUnit(UnitID id, UnitType type, uint32_t soldiers,
                   float training, float equipment) const;
};

} // namespace jke
