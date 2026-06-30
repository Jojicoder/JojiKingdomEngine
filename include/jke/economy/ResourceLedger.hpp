#pragma once

namespace jke {

struct ResourceLedger {
    float food  = 0.0f;
    float gold  = 0.0f;
    float wood  = 0.0f;
    float stone = 0.0f;
    float iron  = 0.0f;

    ResourceLedger operator+(const ResourceLedger& o) const noexcept {
        return {food+o.food, gold+o.gold, wood+o.wood, stone+o.stone, iron+o.iron};
    }
    ResourceLedger operator-(const ResourceLedger& o) const noexcept {
        return {food-o.food, gold-o.gold, wood-o.wood, stone-o.stone, iron-o.iron};
    }
    ResourceLedger& operator+=(const ResourceLedger& o) noexcept {
        food+=o.food; gold+=o.gold; wood+=o.wood; stone+=o.stone; iron+=o.iron;
        return *this;
    }
    ResourceLedger& operator-=(const ResourceLedger& o) noexcept {
        food-=o.food; gold-=o.gold; wood-=o.wood; stone-=o.stone; iron-=o.iron;
        return *this;
    }
    ResourceLedger operator*(float s) const noexcept {
        return {food*s, gold*s, wood*s, stone*s, iron*s};
    }

    bool canAfford(const ResourceLedger& cost) const noexcept {
        return food >= cost.food && gold >= cost.gold &&
               wood >= cost.wood && stone >= cost.stone && iron >= cost.iron;
    }

    void clampToZero() noexcept {
        if (food  < 0) food  = 0;
        if (gold  < 0) gold  = 0;
        if (wood  < 0) wood  = 0;
        if (stone < 0) stone = 0;
        if (iron  < 0) iron  = 0;
    }
};

} // namespace jke
