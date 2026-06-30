#pragma once
#include <vector>
#include <unordered_set>
#include "jke/technology/Technology.hpp"

namespace jke {

class TechTree {
public:
    TechTree();

    const Technology* find(TechID id) const;
    std::vector<TechID> available(const std::unordered_set<TechID>& researched) const;
    const std::vector<Technology>& all() const { return techs_; }

private:
    std::vector<Technology> techs_;

    void addTech(Technology t) { techs_.push_back(std::move(t)); }
    void buildTree();
};

} // namespace jke
