#pragma once
#include <compare>
#include <concepts>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace lob {
template <class Tag> class Positive {
    std::uint64_t value_;

  public:
    template <std::integral Integer>
    explicit Positive(Integer value) : value_(static_cast<std::uint64_t>(value)) {
        if (value <= 0)
            throw std::invalid_argument("value must be positive");
    }
    std::uint64_t value() const noexcept { return value_; }
    auto operator<=>(const Positive &) const = default;
};
using Price = Positive<struct PriceTag>;
using Quantity = Positive<struct QuantityTag>;
using OrderId = Positive<struct OrderIdTag>;
enum class Side { Buy, Sell };
inline void validate(Side side) {
    if (side != Side::Buy && side != Side::Sell)
        throw std::invalid_argument("invalid side");
}
inline std::uint64_t next(std::uint64_t value) {
    if (value == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("sequence exhausted");
    return value + 1;
}
// Zero is meaningful for a completed order, unlike a submitted Quantity.
class Remaining {
    std::uint64_t value_;

  public:
    explicit Remaining(std::uint64_t value) : value_(value) {}
    std::uint64_t value() const noexcept { return value_; }
    void consume(Quantity quantity) {
        if (quantity.value() > value_)
            throw std::underflow_error("overfill");
        value_ -= quantity.value();
    }
    auto operator<=>(const Remaining &) const = default;
};
struct IncomingOrder {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;
    IncomingOrder(OrderId i, Side s, Price p, Quantity q) : id(i), side(s), price(p), quantity(q) {
        validate(s);
    }
    bool operator==(const IncomingOrder &) const = default;
};
struct RestingOrder {
    OrderId id;
    Side side;
    Price price;
    Quantity original;
    Remaining remaining;
    std::uint64_t arrival_sequence;
    bool operator==(const RestingOrder &) const = default;
};
struct Fill {
    OrderId maker;
    OrderId taker;
    Price price;
    Quantity quantity;
    bool operator==(const Fill &) const = default;
};
enum class Status { Accepted, DuplicateId, Cancelled, UnknownOrder };
struct SubmissionResult {
    std::uint64_t sequence;
    Status status;
    Remaining remaining;
    std::vector<Fill> fills;
    bool operator==(const SubmissionResult &) const = default;
};
struct CancelResult {
    std::uint64_t sequence;
    Status status;
    bool operator==(const CancelResult &) const = default;
};
struct Snapshot {
    std::vector<RestingOrder> bids; // Best price first, then FIFO.
    std::vector<RestingOrder> asks;
    std::vector<OrderId> used_ids; // Sorted, including completed/cancelled IDs.
    std::uint64_t sequence{};
    bool operator==(const Snapshot &) const = default;
};
} // namespace lob
