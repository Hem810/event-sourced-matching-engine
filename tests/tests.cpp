#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "lob/domain.hpp"
#include <type_traits>
using namespace lob;
static_assert(!std::is_convertible_v<Price, Quantity>);
static_assert(!std::is_convertible_v<OrderId, Price>);
static_assert(!std::is_convertible_v<std::uint64_t, Quantity>);
TEST_CASE("positive domain values and checked arithmetic") {
    CHECK_THROWS_AS(Price(0), std::invalid_argument);
    CHECK_THROWS_AS(Price(-1), std::invalid_argument);
    CHECK_THROWS_AS(Quantity(-1), std::invalid_argument);
    static_assert(!std::is_constructible_v<Price, double>);
    CHECK_THROWS_AS(Quantity(0), std::invalid_argument);
    CHECK_THROWS_AS(OrderId(0), std::invalid_argument);
    CHECK_THROWS_AS(IncomingOrder(OrderId(1), static_cast<Side>(2), Price(1), Quantity(1)),
                    std::invalid_argument);
    CHECK_THROWS_AS(next(UINT64_MAX), std::overflow_error);
    Remaining remaining(3);
    CHECK_THROWS_AS(remaining.consume(Quantity(4)), std::underflow_error);
    remaining.consume(Quantity(3));
    CHECK(remaining.value() == 0);
}
#include "lob/engine.hpp"
IncomingOrder order(std::uint64_t id, Side side, std::uint64_t price, std::uint64_t qty) {
    return {OrderId(id), side, Price(price), Quantity(qty)};
}
TEST_CASE("incoming intent, duplicates and cancellation lifecycle") {
    Engine e;
    CHECK(e.submit(order(1, Side::Buy, 100, 5)).status == Status::Accepted);
    CHECK(e.submit(order(1, Side::Sell, 100, 5)).status == Status::DuplicateId);
    CHECK(e.snapshot().bids.front().original == Quantity(5));
    CHECK(e.cancel(OrderId(1)).status == Status::Cancelled);
    CHECK(e.cancel(OrderId(1)).status == Status::UnknownOrder);
    CHECK(e.cancel(OrderId(99)).status == Status::UnknownOrder);
    CHECK(e.submit(order(1, Side::Buy, 100, 1)).status == Status::DuplicateId);
    CHECK(e.snapshot().sequence == 6);
}
TEST_CASE("ordered levels, FIFO removal and detached snapshots") {
    Engine e;
    CHECK_FALSE(e.best_bid());
    CHECK_FALSE(e.best_ask());
    for (auto id : {1, 2, 3})
        e.submit(order(id, Side::Buy, 100, 3));
    e.submit(order(4, Side::Buy, 99, 1));
    e.submit(order(5, Side::Sell, 102, 1));
    e.submit(order(6, Side::Sell, 101, 1));
    CHECK(e.best_bid() == Price(100));
    CHECK(e.best_ask() == Price(101));
    e.cancel(OrderId(2));
    auto snapshot = e.snapshot();
    REQUIRE(snapshot.bids.size() == 3);
    CHECK(snapshot.bids[0].id == OrderId(1));
    CHECK(snapshot.bids[1].id == OrderId(3));
    snapshot.bids.clear();
    CHECK(e.snapshot().bids.size() == 3);
}
TEST_CASE("exact fill and completed cancellation") {
    Engine e;
    e.submit(order(1, Side::Sell, 100, 5));
    auto result = e.submit(order(2, Side::Buy, 105, 5));
    REQUIRE(result.fills.size() == 1);
    CHECK(result.fills[0] == Fill{OrderId(1), OrderId(2), Price(100), Quantity(5)});
    CHECK(result.remaining.value() == 0);
    CHECK_FALSE(e.best_ask());
    CHECK_FALSE(e.best_bid());
    CHECK(e.cancel(OrderId(1)).status == Status::UnknownOrder);
    CHECK(e.submit(order(2, Side::Sell, 1, 1)).status == Status::DuplicateId);
}
TEST_CASE("partial, multilevel, price and time priority in both directions") {
    for (Side taker : {Side::Buy, Side::Sell}) {
        Engine e;
        Side maker = taker == Side::Buy ? Side::Sell : Side::Buy;
        auto better = taker == Side::Buy ? 99 : 101;
        e.submit(order(1, maker, 100, 4));
        e.submit(order(2, maker, better, 3));
        e.submit(order(3, maker, 100, 4));
        auto r = e.submit(order(4, taker, 100, 9));
        REQUIRE(r.fills.size() == 3);
        CHECK(r.fills[0].maker == OrderId(2));
        CHECK(r.fills[1].maker == OrderId(1));
        CHECK(r.fills[2].maker == OrderId(3));
        CHECK(r.fills[2].quantity == Quantity(2));
        auto s = e.snapshot();
        auto resting = maker == Side::Buy ? s.bids : s.asks;
        REQUIRE(resting.size() == 1);
        CHECK(resting[0].original == Quantity(4));
        CHECK(resting[0].remaining.value() == 2);
        r = e.submit(order(5, taker, 100, 7));
        CHECK(r.remaining.value() == 5);
        CHECK((taker == Side::Buy ? e.snapshot().bids : e.snapshot().asks)
                  .front()
                  .remaining.value() == 5);
    }
}
TEST_CASE("non-crossing and maximum values") {
    Engine e;
    e.submit(order(1, Side::Buy, 1, UINT64_MAX));
    CHECK(e.submit(order(2, Side::Sell, UINT64_MAX, UINT64_MAX)).fills.empty());
    CHECK(e.submit(order(3, Side::Buy, UINT64_MAX, UINT64_MAX)).remaining.value() == 0);
    CHECK(e.snapshot().bids.size() == 1);
}
#include "reference.hpp"
#include <random>
TEST_CASE("seeded independent oracle and quantity conservation after every command") {
    for (unsigned seed : {7U, 42U, 2026U, 19937U}) {
        CAPTURE(seed);
        std::mt19937 rng(seed);
        Engine engine;
        Reference reference;
        std::uint64_t accepted = 0, traded = 0, cancelled = 0;
        for (std::uint64_t step = 1; step <= 3000; ++step) {
            CAPTURE(step);
            if (rng() % 4 == 0) {
                OrderId id(1 + rng() % step);
                for (const auto &resting : reference.live)
                    if (resting.id == id)
                        cancelled += resting.remaining.value();
                CHECK(engine.cancel(id) == reference.cancel(id));
            } else {
                auto incoming =
                    order(rng() % 8 == 0 ? 1 + rng() % step : step,
                          rng() % 2 ? Side::Buy : Side::Sell, 90 + rng() % 21, 1 + rng() % 50);
                auto actual = engine.submit(incoming);
                CHECK(actual == reference.submit(incoming));
                if (actual.status == Status::Accepted) {
                    accepted += incoming.quantity.value();
                    std::uint64_t fills = 0;
                    for (const auto &fill : actual.fills)
                        fills += fill.quantity.value();
                    CHECK(fills + actual.remaining.value() == incoming.quantity.value());
                    traded += fills;
                }
            }
            auto s = engine.snapshot();
            CHECK(s == reference.snapshot());
            std::uint64_t resting = 0;
            for (const auto *side : {&s.bids, &s.asks})
                for (const auto &o : *side) {
                    CHECK(o.remaining.value() > 0);
                    CHECK(o.remaining.value() <= o.original.value());
                    resting += o.remaining.value();
                }
            CHECK(accepted == 2 * traded + cancelled + resting);
            if (!s.bids.empty() && !s.asks.empty())
                CHECK(s.bids.front().price < s.asks.front().price);
        }
        Engine replayed;
        replayed.replay(engine.events());
        CHECK(replayed.snapshot() == engine.snapshot());
        CHECK(replayed.events() == engine.events());
    }
}
#include "lob/persistence.hpp"
TEST_CASE("in memory replay preserves metadata, fills, rejects and continuation") {
    Engine original;
    original.submit(order(1, Side::Sell, 100, 10));
    original.submit(order(2, Side::Buy, 101, 4));
    original.submit(order(2, Side::Sell, 100, 1));
    original.cancel(OrderId(999));
    original.cancel(OrderId(1));
    Engine restored;
    restored.replay(original.events());
    CHECK(restored.snapshot() == original.snapshot());
    CHECK(restored.events() == original.events());
    CHECK(restored.submit(order(3, Side::Buy, 90, 5)) ==
          original.submit(order(3, Side::Buy, 90, 5)));
    CHECK_THROWS(restored.replay(original.events()));
    for (int mutation = 0; mutation < 6; ++mutation) {
        auto bad = original.events();
        if (mutation == 0)
            bad[0].version = 2;
        if (mutation == 1)
            bad[1].sequence = 1;
        if (mutation == 2)
            std::get<Traded>(bad[1].events[1]).fill.quantity = Quantity(3);
        if (mutation == 3)
            bad[1].events.pop_back();
        if (mutation == 4)
            bad[0].events.clear();
        if (mutation == 5)
            std::get<Traded>(bad[1].events[1]).fill.price = Price(99);
        Engine invalid;
        CHECK_THROWS(invalid.replay(bad));
        CHECK(invalid.snapshot().sequence == 0);
    }
}
struct TempFile {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("lob-test-" + std::to_string(std::random_device{}()) + ".events");
    ~TempFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};
TEST_CASE("file round trip, append and corruption recovery") {
    TempFile file;
    Engine engine;
    engine.submit(order(1, Side::Sell, 100, 10));
    engine.submit(order(2, Side::Buy, 101, 4));
    engine.cancel(OrderId(1));
    engine.cancel(OrderId(99));
    engine.submit(order(2, Side::Buy, 101, 4));
    {
        EventFile writer(file.path);
        for (const auto &batch : engine.events()) {
            CHECK(decode(encode(batch)) == batch);
            writer.append(batch);
        }
        writer.flush();
    }
    CHECK_THROWS(EventFile(file.path));
    auto recovered = read_events(file.path);
    Engine restored;
    restored.replay(recovered.batches);
    CHECK(restored.snapshot() == engine.snapshot());
    engine.submit(order(3, Side::Buy, 90, 2));
    {
        EventFile writer(file.path, EventFile::Mode::Append);
        writer.append(engine.events().back());
        writer.flush();
    }
    CHECK(read_events(file.path).batches == engine.events());
    auto good_bytes = std::filesystem::file_size(file.path);
    {
        std::ofstream out(file.path, std::ios::app);
        out << "LOB 1 7";
    }
    CHECK_THROWS(read_events(file.path));
    CHECK_THROWS(EventFile(file.path, EventFile::Mode::Append));
    recovered = read_events(file.path, true);
    CHECK(recovered.batches == engine.events());
    CHECK(recovered.valid_bytes == good_bytes);
    CHECK_FALSE(recovered.error.empty());
    auto encoded = encode(engine.events()[0]);
    encoded[5] = '9';
    CHECK_THROWS(decode(encoded));
    CHECK_THROWS(decode(encode(engine.events()[0]) + " trailing"));
    CHECK_THROWS(decode("LOB 1 1 0"));
}
TEST_CASE("every truncation of a two-record file recovers only complete commands") {
    Engine engine;
    engine.submit(order(1, Side::Sell, 100, 10));
    engine.submit(order(2, Side::Buy, 100, 2));
    auto first = encode(engine.events()[0]) + "\n";
    auto all = first + encode(engine.events()[1]) + "\n";
    TempFile file;
    for (std::size_t length = 0; length < all.size(); ++length) {
        {
            std::ofstream out(file.path, std::ios::binary | std::ios::trunc);
            out.write(all.data(), static_cast<std::streamsize>(length));
        }
        auto recovered = read_events(file.path, true);
        CHECK(recovered.batches.size() == (length >= first.size() ? 1 : 0));
        CHECK(recovered.error.empty() == (length == 0 || length == first.size()));
    }
}
#include "lob/concurrent.hpp"
#include <atomic>
#include <barrier>
#include <thread>
TEST_CASE("concurrent submit cancel and snapshots linearize by assigned sequence") {
    constexpr int thread_count = 4, per_thread = 300;
    using Command = std::variant<IncomingOrder, OrderId>;
    using Result = std::variant<SubmissionResult, CancelResult>;
    struct Observation {
        Command command;
        Result result;
    };
    ConcurrentEngine engine;
    std::vector<std::vector<Observation>> observations(thread_count);
    std::atomic<bool> done{false}, valid{true};
    std::barrier start(thread_count + 1);
    std::thread observer([&] {
        while (!done.load()) {
            auto s = engine.snapshot();
            if (!s.bids.empty() && !s.asks.empty() && s.bids.front().price >= s.asks.front().price)
                valid.store(false);
            for (const auto *side : {&s.bids, &s.asks})
                for (const auto &o : *side)
                    if (!o.remaining.value())
                        valid.store(false);
            std::this_thread::yield();
        }
    });
    std::vector<std::thread> threads;
    for (int t = 0; t < thread_count; ++t)
        threads.emplace_back([&, t] {
            std::mt19937 rng(42U + static_cast<unsigned>(t));
            start.arrive_and_wait();
            for (int i = 0; i < per_thread; ++i) {
                auto id = static_cast<std::uint64_t>(t * per_thread + i + 1);
                auto incoming =
                    order(id, rng() % 2 ? Side::Buy : Side::Sell, 98 + rng() % 5, 1 + rng() % 10);
                observations[t].push_back({incoming, engine.submit(incoming)});
                // Own and other producers' IDs: successful and unknown cancellation races.
                OrderId cancel_id(i % 2 ? id : 1 + rng() % (thread_count * per_thread));
                observations[t].push_back({cancel_id, engine.cancel(cancel_id)});
            }
        });
    start.arrive_and_wait();
    for (auto &thread : threads)
        thread.join();
    done.store(true);
    observer.join();
    CHECK(valid.load());
    std::vector<Observation> ordered;
    for (const auto &records : observations)
        ordered.insert(ordered.end(), records.begin(), records.end());
    auto sequence = [](const Observation &o) {
        return std::visit([](const auto &r) { return r.sequence; }, o.result);
    };
    std::sort(ordered.begin(), ordered.end(),
              [&](const auto &a, const auto &b) { return sequence(a) < sequence(b); });
    Reference oracle;
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        CHECK(sequence(ordered[i]) == i + 1);
        if (auto *incoming = std::get_if<IncomingOrder>(&ordered[i].command))
            CHECK(std::get<SubmissionResult>(ordered[i].result) == oracle.submit(*incoming));
        else
            CHECK(std::get<CancelResult>(ordered[i].result) ==
                  oracle.cancel(std::get<OrderId>(ordered[i].command)));
    }
    CHECK(engine.snapshot() == oracle.snapshot());
    Engine replay;
    replay.replay(engine.events());
    CHECK(replay.snapshot() == engine.snapshot());
    CHECK(replay.events() == engine.events());
}
TEST_CASE("canonical persistence ignores global numeric locale") {
    struct Grouped : std::numpunct<char> {
        char do_thousands_sep() const override { return ','; }
        std::string do_grouping() const override { return "\3"; }
    };
    Engine engine;
    engine.submit(order(1000, Side::Sell, 10000, 100000));
    auto expected = encode(engine.events().front());
    struct RestoreLocale {
        std::locale previous = std::locale();
        ~RestoreLocale() { std::locale::global(previous); }
    } guard;
    std::locale::global(std::locale(std::locale::classic(), new Grouped));
    CHECK(encode(engine.events().front()) == expected);
    CHECK(decode(expected) == engine.events().front());
}
