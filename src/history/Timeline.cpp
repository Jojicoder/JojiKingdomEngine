#include "jke/history/Timeline.hpp"
#include <algorithm>

namespace jke {

void Timeline::record(HistoryEvent event) {
    EventID id = event.id;
    byTurn_[event.turn].push_back(id);
    if (event.primaryKingdom   != NO_KINGDOM) byKingdom_[event.primaryKingdom].push_back(id);
    if (event.secondaryKingdom != NO_KINGDOM) byKingdom_[event.secondaryKingdom].push_back(id);
    events_.push_back(std::move(event));
}

std::vector<const HistoryEvent*> Timeline::eventsForKingdom(KingdomID k) const {
    std::vector<const HistoryEvent*> result;
    auto it = byKingdom_.find(k);
    if (it == byKingdom_.end()) return result;
    for (EventID id : it->second)
        for (const auto& ev : events_)
            if (ev.id == id) { result.push_back(&ev); break; }
    return result;
}

std::vector<const HistoryEvent*> Timeline::eventsForTurn(TurnNumber t) const {
    std::vector<const HistoryEvent*> result;
    auto it = byTurn_.find(t);
    if (it == byTurn_.end()) return result;
    for (EventID id : it->second)
        for (const auto& ev : events_)
            if (ev.id == id) { result.push_back(&ev); break; }
    return result;
}

std::vector<const HistoryEvent*> Timeline::eventsOfType(EventType type) const {
    std::vector<const HistoryEvent*> result;
    for (const auto& ev : events_)
        if (ev.type == type) result.push_back(&ev);
    return result;
}

} // namespace jke
