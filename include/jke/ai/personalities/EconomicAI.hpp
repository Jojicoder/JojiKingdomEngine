#pragma once
#include "jke/ai/AIStrategy.hpp"

namespace jke {

class EconomicAI : public AIStrategy {
public:
    std::vector<AIDecision> evaluate(const AIContext& ctx) const override;
};

} // namespace jke
