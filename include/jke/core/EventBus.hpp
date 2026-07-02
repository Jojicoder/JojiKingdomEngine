#pragma once
#include <functional>
#include <unordered_map>
#include <vector>
#include "jke/history/HistoryEvent.hpp"

namespace jke {

// Synchronous publish-subscribe. Engines emit events; HistoryEngine subscribes.
// Events are queued and flushed once per turn (after all engines run).
class EventBus {
public:
    using Handler = std::function<void(const HistoryEvent&)>;

    void subscribe(EventType type, Handler handler) {
        handlers_[static_cast<int>(type)].push_back(std::move(handler));
    }

    void subscribeAll(Handler handler) {
        catchAll_.push_back(std::move(handler));
    }

    void emit(HistoryEvent event) {
        queue_.push_back(std::move(event));
    }

    // Drain queue and dispatch to all matching handlers
    void flush() {
        for (auto& ev : queue_) {
            auto it = handlers_.find(static_cast<int>(ev.type));
            if (it != handlers_.end()) {
                for (auto& h : it->second) h(ev);
            }
            for (auto& h : catchAll_) h(ev);
        }
        queue_.clear();
    }

    const std::vector<HistoryEvent>& pendingEvents() const { return queue_; }

    void clearPending() { queue_.clear(); }

private:
    std::unordered_map<int, std::vector<Handler>> handlers_;
    std::vector<Handler>                          catchAll_;
    std::vector<HistoryEvent>                     queue_;
};

} // namespace jke
