#include "jke/city/City.hpp"
#include <algorithm>

namespace jke {

ResourceLedger City::effectiveProduction() const {
    ResourceLedger prod = baseProduction;
    for (const auto& b : buildings) {
        float mult = 1.0f + (b.level - 1) * 0.25f;
        mult *= b.condition;
        switch (b.type) {
            case BuildingType::Farm:
                prod.food += 20.0f * mult;
                break;
            case BuildingType::LumberMill:
                prod.wood += 15.0f * mult;
                break;
            case BuildingType::Quarry:
                prod.stone += 12.0f * mult;
                break;
            case BuildingType::IronMine:
                prod.iron += 10.0f * mult;
                break;
            case BuildingType::Market:
                prod.gold += 18.0f * mult;
                break;
            case BuildingType::Granary:
                prod.food += 10.0f * mult;
                break;
            case BuildingType::Workshop:
                prod.wood += 8.0f * mult;
                prod.iron += 6.0f * mult;
                break;
            default:
                break;
        }
    }
    return prod;
}

float City::rebellionRisk() const {
    // Risk rises when happiness is low and fortification is low
    float risk = (1.0f - happiness) * 0.6f;
    if (fortification < 0.2f) risk += 0.2f;
    return std::clamp(risk, 0.0f, 1.0f);
}

} // namespace jke
