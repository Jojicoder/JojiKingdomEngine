#include "jke/economy/ResourceLedger.hpp"
#include "jke/engines/EconomyEngine.hpp"
#include "jke/core/EventBus.hpp"
#include <cassert>
#include <iostream>

int main() {
    // ResourceLedger arithmetic
    jke::ResourceLedger a{100, 50, 30, 20, 10};
    jke::ResourceLedger b{10,  20,  5,  5,  5};
    auto c = a + b;
    assert(c.food == 110.0f);
    assert(c.gold == 70.0f);

    auto d = a - b;
    assert(d.food == 90.0f);

    assert(a.canAfford(b) && "Should afford b");
    jke::ResourceLedger expensive{200, 0, 0, 0, 0};
    assert(!a.canAfford(expensive) && "Should not afford expensive");

    // clampToZero
    jke::ResourceLedger neg{-10, -5, 0, 3, 0};
    neg.clampToZero();
    assert(neg.food == 0.0f);
    assert(neg.gold == 0.0f);
    assert(neg.stone == 3.0f);

    std::cout << "Economy tests PASSED\n";
    return 0;
}
