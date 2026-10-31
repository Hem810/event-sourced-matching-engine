#pragma once
#include "lob/engine.hpp"
#include <mutex>
namespace lob {
class ConcurrentEngine {
    mutable std::mutex mutex_;
    Engine engine_;

  public:
    explicit ConcurrentEngine(bool retain_log = true) : engine_(retain_log) {}
    SubmissionResult submit(const IncomingOrder &order) {
        std::lock_guard lock(mutex_);
        return engine_.submit(order);
    }
    CancelResult cancel(OrderId id) {
        std::lock_guard lock(mutex_);
        return engine_.cancel(id);
    }
    Snapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return engine_.snapshot();
    }
    std::vector<EventBatch> events() const {
        std::lock_guard lock(mutex_);
        return engine_.events(); // Copy before unlocking; no mutable reference escapes.
    }
    void replay(const std::vector<EventBatch> &events) {
        std::lock_guard lock(mutex_);
        engine_.replay(events);
    }
};
} // namespace lob
