#pragma once
#include "lob/domain.hpp"
#include <list>
#include <map>
#include <optional>
#include <set>
#include <variant>

namespace lob {
struct Accepted {
    IncomingOrder order;
    bool operator==(const Accepted &) const = default;
};
struct Traded {
    Fill fill;
    bool operator==(const Traded &) const = default;
};
struct Cancelled {
    OrderId id;
    bool operator==(const Cancelled &) const = default;
};
struct Rejected {
    OrderId id;
    Status reason;
    bool operator==(const Rejected &) const = default;
};
using Event = std::variant<Accepted, Traded, Cancelled, Rejected>;
struct EventBatch {
    std::uint32_t version{1};
    std::uint64_t sequence{};
    std::vector<Event> events;
    bool operator==(const EventBatch &) const = default;
};
class Engine {
    using Queue = std::list<RestingOrder>;
    using Levels = std::map<Price, Queue>;
    struct Location {
        Side side;
        Price price;
        Queue::iterator order;
    };
    Levels bids_, asks_;
    std::map<OrderId, Location> active_;
    std::set<OrderId> used_;
    std::uint64_t sequence_{};
    bool retain_log_;
    std::vector<EventBatch> log_;
    Levels &levels(Side side);
    EventBatch plan(const IncomingOrder &order) const;
    EventBatch plan_cancel(OrderId id) const;
    void erase(OrderId id);
    void consume(OrderId id, Quantity quantity);
    void apply(const EventBatch &batch);
    void commit(const EventBatch &batch);

  public:
    explicit Engine(bool retain_log = true) : retain_log_(retain_log) {}
    // Iterators in the ID index make accidental copying unsafe.
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;
    Engine(Engine &&) = delete;
    Engine &operator=(Engine &&) = delete;
    SubmissionResult submit(const IncomingOrder &order);
    CancelResult cancel(OrderId id);
    Snapshot snapshot() const;
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    const std::vector<EventBatch> &events() const noexcept { return log_; }
    void replay(const std::vector<EventBatch> &batches);
};
} // namespace lob
