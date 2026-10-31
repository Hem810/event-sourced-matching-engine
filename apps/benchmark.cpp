#include "lob/concurrent.hpp"
#include "lob/persistence.hpp"
#include <algorithm>
#include <barrier>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <thread>

using namespace lob;
using Clock = std::chrono::steady_clock;
using Command = std::variant<IncomingOrder, OrderId>;
struct Mode {
    std::string name;
    int threads;
    bool log;
    bool facade;
    int persistence;
};
struct Counters {
    std::uint64_t fills{}, cancels{}, rejected{};
};
std::vector<Command> workload(const std::string &scenario, std::size_t cycles, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<Command> commands;
    for (std::size_t i = 0; i < cycles; ++i) {
        auto id = static_cast<std::uint64_t>(i * 5 + 1);
        auto price = 100 + rng() % 7;
        auto submit = [&](std::uint64_t offset, Side side, std::uint64_t p, std::uint64_t qty) {
            commands.emplace_back(
                IncomingOrder{OrderId(id + offset), side, Price(p), Quantity(qty)});
        };
        if (scenario == "resting") {
            submit(0, Side::Buy, price - 10, 10);
            submit(1, Side::Sell, price + 10, 10);
            commands.emplace_back(OrderId(id));
            commands.emplace_back(OrderId(id + 1));
        } else if (scenario == "crossing") {
            submit(0, Side::Sell, price, 10);
            submit(1, Side::Buy, price, 10);
        } else {
            submit(0, Side::Buy, price - 10, 10);
            submit(1, Side::Sell, price + 10, 10);
            submit(2, Side::Buy, price + 10, 4);
            submit(3, Side::Buy, price + 10, 8);
            submit(4, Side::Sell, price + 10, 2);
            commands.emplace_back(OrderId(id));
            commands.emplace_back(OrderId(id + 1));
        }
    }
    return commands;
}
void run(const std::string &scenario, const Mode &mode, std::size_t cycles, unsigned seed,
         int repeat, bool report) {
    const auto commands = workload(scenario, cycles, seed);
    Engine core(mode.log);
    ConcurrentEngine shared(mode.log);
    auto path = std::filesystem::temp_directory_path() /
                ("lob-bench-" + std::to_string(std::random_device{}()) + ".events");
    struct Cleanup {
        std::filesystem::path p;
        ~Cleanup() {
            std::error_code e;
            std::filesystem::remove(p, e);
        }
    } cleanup{path};
    std::unique_ptr<EventFile> file;
    if (mode.persistence)
        file = std::make_unique<EventFile>(path);
    std::vector<double> latency(commands.size());
    std::vector<Counters> counts(static_cast<std::size_t>(mode.threads));
    auto invoke = [&](auto &engine, std::size_t index, Counters &count) {
        if (auto *order = std::get_if<IncomingOrder>(&commands[index])) {
            auto result = engine.submit(*order);
            count.fills += result.fills.size();
            count.rejected += result.status == Status::DuplicateId;
        } else {
            auto result = engine.cancel(std::get<OrderId>(commands[index]));
            count.cancels += result.status == Status::Cancelled;
            count.rejected += result.status == Status::UnknownOrder;
        }
    };
    auto worker = [&](int t) {
        // Partition complete cycles so each producer preserves its local causal order.
        auto per_cycle = commands.size() / cycles;
        auto begin = cycles * static_cast<std::size_t>(t) / static_cast<std::size_t>(mode.threads) *
                     per_cycle;
        auto end = cycles * static_cast<std::size_t>(t + 1) /
                   static_cast<std::size_t>(mode.threads) * per_cycle;
        for (auto i = begin; i < end; ++i) {
            auto start = Clock::now();
            if (mode.facade)
                invoke(shared, i, counts[static_cast<std::size_t>(t)]);
            else
                invoke(core, i, counts[static_cast<std::size_t>(t)]);
            if (file) {
                file->append(core.events().back());
                if (mode.persistence == 2)
                    file->flush();
            }
            latency[i] = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
        }
    };
    std::vector<std::thread> threads;
    std::barrier gate(mode.threads + 1);
    if (mode.facade)
        for (int t = 0; t < mode.threads; ++t)
            threads.emplace_back([&, t] {
                gate.arrive_and_wait();
                worker(t);
            });
    const auto start = Clock::now();
    if (mode.facade) {
        gate.arrive_and_wait();
        for (auto &thread : threads)
            thread.join();
    } else
        worker(0);
    if (file)
        file->flush();
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    // Validate outside the measured interval; results are consumed, never optimized away.
    auto snapshot = mode.facade ? shared.snapshot() : core.snapshot();
    if (snapshot.sequence != commands.size())
        throw std::runtime_error("lost commands");
    if (!snapshot.bids.empty() && !snapshot.asks.empty() &&
        snapshot.bids[0].price >= snapshot.asks[0].price)
        throw std::runtime_error("crossed book");
    if (mode.log) {
        Engine replay(false);
        replay.replay(mode.facade ? shared.events() : core.events());
        if (snapshot != replay.snapshot())
            throw std::runtime_error("benchmark replay mismatch");
    }
    if (file) {
        Engine replay(false);
        replay.replay(read_events(path).batches);
        if (snapshot != replay.snapshot())
            throw std::runtime_error("persisted replay mismatch");
    }
    std::sort(latency.begin(), latency.end());
    auto percentile = [&](std::size_t p) { return latency[(latency.size() * p + 99) / 100 - 1]; };
    Counters total;
    for (const auto &count : counts) {
        total.fills += count.fills;
        total.cancels += count.cancels;
        total.rejected += count.rejected;
    }
    if (report)
        std::cout << scenario << ',' << mode.name << ',' << mode.threads << ',' << repeat << ','
                  << seed << ',' << cycles << ',' << commands.size() << ',' << std::fixed
                  << std::setprecision(3) << seconds << ',' << std::setprecision(1)
                  << static_cast<double>(commands.size()) / seconds << ',' << percentile(50) << ','
                  << percentile(95) << ',' << percentile(99) << ',' << total.fills << ','
                  << total.cancels << ',' << total.rejected << '\n';
}
int main(int argc, char **argv) {
    try {
        std::size_t cycles = argc > 1 ? std::stoull(argv[1]) : 2000;
        int repeats = argc > 2 ? std::stoi(argv[2]) : 3;
        if (argc > 3 || cycles < 4 || cycles > 1000000 || repeats < 1 || repeats > 100)
            throw std::invalid_argument("usage: lob_bench [cycles:4..1000000] [repeats:1..100]");
        std::vector<Mode> modes{
            {"core_no_log", 1, false, false, 0},  {"core_log", 1, true, false, 0},
            {"mutex_no_log", 1, false, true, 0},  {"mutex_no_log", 2, false, true, 0},
            {"mutex_no_log", 4, false, true, 0},  {"mutex_log", 4, true, true, 0},
            {"file_buffered", 1, true, false, 1}, {"file_flush", 1, true, false, 2}};
        std::cout << "scenario,mode,threads,repeat,seed,cycles,operations,seconds,ops_per_second,"
                     "p50_ns,p95_ns,p99_ns,fills,cancels,rejected\n";
        for (const auto &scenario : {"resting", "crossing", "partial_cancel"}) {
            for (const auto &mode : modes)
                run(scenario, mode, cycles, 424242, 0, false);
            for (int repeat = 1; repeat <= repeats; ++repeat) {
                // Rotate mode order to reduce fixed-order thermal/cache bias.
                for (std::size_t i = 0; i < modes.size(); ++i)
                    run(scenario, modes[(i + static_cast<std::size_t>(repeat)) % modes.size()],
                        cycles, 424242, repeat, true);
            }
        }
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
