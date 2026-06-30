#pragma once
#include <unordered_map>
#include "jke/nation/Kingdom.hpp"
#include "jke/history/Timeline.hpp"
#include "jke/core/EventBus.hpp"

namespace jke {

class HistoryEngine {
public:
    explicit HistoryEngine(Timeline& timeline);

    // Register all subscriptions on the EventBus
    void registerHandlers(EventBus& bus);

    // Check win condition
    bool checkContinentUnified(
        const std::unordered_map<KingdomID, Kingdom>& kingdoms,
        EventBus& bus,
        TurnNumber turn
    );

private:
    Timeline& timeline_;
    EventID   nextID_ = 1;

    void onEvent(const HistoryEvent& ev);
};

} // namespace jke
