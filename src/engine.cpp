#include "lob/engine.hpp"
#include <algorithm>
#include <type_traits>

namespace lob {
Engine::Levels &Engine::levels(Side side) {
    return side == Side::Buy ? bids_ : asks_;
}
std::optional<Price> Engine::best_bid() const {
    if (bids_.empty())
        return std::nullopt;
    return bids_.rbegin()->first;
}
std::optional<Price> Engine::best_ask() const {
    if (asks_.empty())
        return std::nullopt;
    return asks_.begin()->first;
}
EventBatch Engine::plan(const IncomingOrder &order) const {
    validate(order.side); // IncomingOrder is a value object; callers can change its fields.
    EventBatch batch{1, next(sequence_), {}};
    if (used_.contains(order.id)) {
        batch.events.emplace_back(Rejected{order.id, Status::DuplicateId});
        return batch;
    }
    batch.events.emplace_back(Accepted{order});
    auto remaining = order.quantity.value();
    auto visit = [&](const auto &entry) {
        for (const auto &maker : entry.second) {
            auto amount = std::min(remaining, maker.remaining.value());
            batch.events.emplace_back(
                Traded{Fill{maker.id, order.id, maker.price, Quantity(amount)}});
            remaining -= amount;
            if (!remaining)
                break;
        }
    };
    if (order.side == Side::Buy) {
        for (auto it = asks_.begin(); it != asks_.end() && remaining && it->first <= order.price;
             ++it)
            visit(*it);
    } else {
        for (auto it = bids_.rbegin(); it != bids_.rend() && remaining && it->first >= order.price;
             ++it)
            visit(*it);
    }
    return batch;
}
EventBatch Engine::plan_cancel(OrderId id) const {
    EventBatch batch{1, next(sequence_), {}};
    if (active_.contains(id))
        batch.events.emplace_back(Cancelled{id});
    else
        batch.events.emplace_back(Rejected{id, Status::UnknownOrder});
    return batch;
}
void Engine::erase(OrderId id) {
    auto it = active_.find(id);
    const auto location = it->second;
    auto &book = levels(location.side);
    auto level = book.find(location.price);
    level->second.erase(location.order);
    if (level->second.empty())
        book.erase(level);
    active_.erase(it);
}
void Engine::consume(OrderId id, Quantity quantity) {
    auto &order = *active_.at(id).order;
    order.remaining.consume(quantity);
    if (!order.remaining.value())
        erase(id);
}
void Engine::apply(const EventBatch &batch) {
    for (const auto &event : batch.events) {
        std::visit(
            [&](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, Accepted>) {
                    const auto &o = value.order;
                    auto &queue = levels(o.side)[o.price];
                    queue.push_back({o.id, o.side, o.price, o.quantity,
                                     Remaining(o.quantity.value()), batch.sequence});
                    active_.emplace(o.id, Location{o.side, o.price, std::prev(queue.end())});
                    used_.insert(o.id);
                } else if constexpr (std::is_same_v<T, Traded>) {
                    consume(value.fill.maker, value.fill.quantity);
                    consume(value.fill.taker, value.fill.quantity);
                } else if constexpr (std::is_same_v<T, Cancelled>) {
                    erase(value.id);
                }
            },
            event);
    }
    sequence_ = batch.sequence;
}
void Engine::commit(const EventBatch &batch) {
    if (retain_log_)
        log_.push_back(batch);
    apply(batch);
}
SubmissionResult Engine::submit(const IncomingOrder &order) {
    const auto batch = plan(order);
    SubmissionResult result{
        batch.sequence, Status::Accepted, Remaining(order.quantity.value()), {}};
    if (std::holds_alternative<Rejected>(batch.events.front())) {
        result.status = Status::DuplicateId;
        result.remaining = Remaining(0); // Rejected quantity never enters the book.
    } else {
        for (const auto &event : batch.events) {
            if (const auto *trade = std::get_if<Traded>(&event)) {
                result.fills.push_back(trade->fill);
                result.remaining.consume(trade->fill.quantity);
            }
        }
    }
    commit(batch);
    return result;
}
CancelResult Engine::cancel(OrderId id) {
    auto batch = plan_cancel(id);
    const auto status = std::holds_alternative<Cancelled>(batch.events.front())
                            ? Status::Cancelled
                            : Status::UnknownOrder;
    commit(batch);
    return {batch.sequence, status};
}
Snapshot Engine::snapshot() const {
    Snapshot result;
    result.sequence = sequence_;
    result.used_ids.assign(used_.begin(), used_.end());
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it)
        result.bids.insert(result.bids.end(), it->second.begin(), it->second.end());
    for (const auto &[price, queue] : asks_)
        result.asks.insert(result.asks.end(), queue.begin(), queue.end());
    return result;
}
} // namespace lob

namespace lob {
void Engine::replay(const std::vector<EventBatch> &batches) {
    if (sequence_ != 0 || !active_.empty() || !log_.empty())
        throw std::logic_error("replay requires an empty engine");
    // Validate the entire stream using the same deterministic rules, without recording events.
    // Invalid input leaves the destination untouched. Allocation failure is a fatal resource error.
    Engine validator(false);
    for (const auto &batch : batches) {
        if (batch.version != 1 || batch.events.empty())
            throw std::invalid_argument("invalid event batch/version");
        EventBatch expected;
        const auto &first = batch.events.front();
        if (const auto *accepted = std::get_if<Accepted>(&first))
            expected = validator.plan(accepted->order);
        else if (const auto *cancelled = std::get_if<Cancelled>(&first))
            expected = validator.plan_cancel(cancelled->id);
        else if (const auto *rejected = std::get_if<Rejected>(&first)) {
            if (rejected->reason == Status::UnknownOrder)
                expected = validator.plan_cancel(rejected->id);
            else if (rejected->reason == Status::DuplicateId &&
                     validator.used_.contains(rejected->id))
                expected = {
                    1, next(validator.sequence_), {Rejected{rejected->id, Status::DuplicateId}}};
            else
                throw std::invalid_argument("invalid rejection");
        } else
            throw std::invalid_argument("batch must start with a command outcome");
        if (expected != batch)
            throw std::invalid_argument("event sequence or matching semantics mismatch");
        validator.apply(batch);
    }
    if (retain_log_)
        log_ = batches; // Retain originals once; never regenerate events during application.
    for (const auto &batch : batches)
        apply(batch);
}
} // namespace lob
