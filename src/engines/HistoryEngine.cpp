#include "jke/engines/HistoryEngine.hpp"
#include <algorithm>

namespace jke {

HistoryEngine::HistoryEngine(Timeline& timeline) : timeline_(timeline) {}

void HistoryEngine::registerHandlers(EventBus& bus) {
    bus.subscribeAll([this](const HistoryEvent& ev) {
        onEvent(ev);
    });
}

void HistoryEngine::onEvent(const HistoryEvent& ev) {
    HistoryEvent copy = ev;
    if (copy.id == 0) copy.id = nextID_++;
    timeline_.record(std::move(copy));
}

bool HistoryEngine::checkContinentUnified(
    const std::unordered_map<KingdomID, Kingdom>& kingdoms,
    EventBus& bus,
    TurnNumber turn)
{
    int alive = 0;
    KingdomID last = NO_KINGDOM;
    for (const auto& [kid, k] : kingdoms) {
        if (k.isAlive) { alive++; last = kid; }
    }

    if (alive == 1 && last != NO_KINGDOM) {
        HistoryEvent ev;
        ev.id              = nextID_++;
        ev.type            = EventType::ContinentUnified;
        ev.turn            = turn;
        ev.primaryKingdom  = last;
        ev.description     = kingdoms.at(last).name +
                             " has unified the continent!";
        bus.emit(std::move(ev));
        return true;
    }
    return false;
}

} // namespace jke
