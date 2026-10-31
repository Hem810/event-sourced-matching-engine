#pragma once
#include "lob/domain.hpp"
#include <algorithm>
#include <set>

// Deliberately slow oracle: a flat array, full scans for every maker, no price maps or event code.
struct Reference {
    std::vector<lob::RestingOrder> live;
    std::set<lob::OrderId> used;
    std::uint64_t sequence{};
    lob::SubmissionResult submit(const lob::IncomingOrder &incoming) {
        using namespace lob;
        SubmissionResult result{
            ++sequence, Status::Accepted, Remaining(incoming.quantity.value()), {}};
        if (!used.insert(incoming.id).second) {
            result.status = Status::DuplicateId;
            result.remaining = Remaining(0);
            return result;
        }
        while (result.remaining.value()) {
            auto best = live.end();
            for (auto it = live.begin(); it != live.end(); ++it) {
                if (it->side == incoming.side)
                    continue;
                if (incoming.side == Side::Buy ? it->price > incoming.price
                                               : it->price < incoming.price)
                    continue;
                bool better = best == live.end();
                if (!better && it->price != best->price)
                    better = incoming.side == Side::Buy ? it->price < best->price
                                                        : it->price > best->price;
                if (!better && it->price == best->price)
                    better = it->arrival_sequence < best->arrival_sequence;
                if (better)
                    best = it;
            }
            if (best == live.end())
                break;
            Quantity amount(std::min(result.remaining.value(), best->remaining.value()));
            result.fills.push_back({best->id, incoming.id, best->price, amount});
            result.remaining.consume(amount);
            best->remaining.consume(amount);
            if (!best->remaining.value())
                live.erase(best);
        }
        if (result.remaining.value())
            live.push_back({incoming.id, incoming.side, incoming.price, incoming.quantity,
                            result.remaining, sequence});
        return result;
    }
    lob::CancelResult cancel(lob::OrderId id) {
        auto it = std::find_if(live.begin(), live.end(), [&](const auto &o) { return o.id == id; });
        auto status = lob::Status::UnknownOrder;
        if (it != live.end()) {
            live.erase(it);
            status = lob::Status::Cancelled;
        }
        return {++sequence, status};
    }
    lob::Snapshot snapshot() const {
        lob::Snapshot result;
        result.sequence = sequence;
        result.used_ids.assign(used.begin(), used.end());
        for (const auto &o : live)
            (o.side == lob::Side::Buy ? result.bids : result.asks).push_back(o);
        auto sort = [](auto &orders, bool buy) {
            std::sort(orders.begin(), orders.end(), [=](const auto &a, const auto &b) {
                if (a.price != b.price)
                    return buy ? a.price > b.price : a.price < b.price;
                return a.arrival_sequence < b.arrival_sequence;
            });
        };
        sort(result.bids, true);
        sort(result.asks, false);
        return result;
    }
};
