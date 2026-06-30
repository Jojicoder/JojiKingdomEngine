#pragma once
#include "jke/ai/AIStrategy.hpp"

namespace jke {

class DefensiveAI : public AIStrategy {
public:
    std::vector<AIDecision> evaluate(const AIContext& ctx) const override;
};

} // namespace jke
