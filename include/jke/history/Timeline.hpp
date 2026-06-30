#pragma once
#include <vector>
#include <unordered_map>
#include "jke/history/HistoryEvent.hpp"

namespace jke {

class Timeline {
public:
    void record(HistoryEvent event);

    const std::vector<HistoryEvent>& all() const { return events_; }

    std::vector<const HistoryEvent*> eventsForKingdom(KingdomID k) const;
    std::vector<const HistoryEvent*> eventsForTurn(TurnNumber t) const;
    std::vector<const HistoryEvent*> eventsOfType(EventType type) const;

    EventID nextEventID() { return ++nextID_; }

private:
    std::vector<HistoryEvent>                          events_;
    std::unordered_map<TurnNumber, std::vector<EventID>> byTurn_;
    std::unordered_map<KingdomID, std::vector<EventID>>  byKingdom_;
    EventID                                            nextID_ = 0;
};

} // namespace jke
