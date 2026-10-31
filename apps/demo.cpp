#include "lob/persistence.hpp"
#include <iostream>

using namespace lob;
void show(const Engine &engine) {
    auto s = engine.snapshot();
    std::cout << "Book at sequence " << s.sequence << '\n';
    for (const auto *side : {&s.bids, &s.asks})
        for (const auto &o : *side)
            std::cout << (o.side == Side::Buy ? " BID " : " ASK ") << o.price.value()
                      << " id=" << o.id.value() << " remaining=" << o.remaining.value()
                      << " original=" << o.original.value() << '\n';
}
int main(int argc, char **argv) {
    try {
        Engine engine;
        auto submit = [&](std::uint64_t id, Side side, std::uint64_t price, std::uint64_t qty) {
            auto result = engine.submit({OrderId(id), side, Price(price), Quantity(qty)});
            std::cout << "Submit " << id << " sequence=" << result.sequence
                      << " remaining=" << result.remaining.value() << '\n';
            for (const auto &fill : result.fills)
                std::cout << " Fill maker=" << fill.maker.value() << " taker=" << fill.taker.value()
                          << " ticks=" << fill.price.value()
                          << " quantity=" << fill.quantity.value() << '\n';
        };
        submit(1, Side::Sell, 101, 10);
        submit(2, Side::Sell, 102, 5);
        submit(3, Side::Buy, 102, 12);
        submit(4, Side::Buy, 99, 7);
        show(engine);
        auto cancelled = engine.cancel(OrderId(2));
        std::cout << "Cancel 2 sequence=" << cancelled.sequence
                  << " cancelled=" << (cancelled.status == Status::Cancelled) << '\n';
        show(engine);
        Engine restored;
        if (argc == 2) {
            {
                EventFile file(argv[1]);
                for (const auto &batch : engine.events())
                    file.append(batch);
                file.flush();
            }
            restored.replay(read_events(argv[1]).batches);
            std::cout << "Replayed file " << argv[1] << '\n';
        } else if (argc == 1)
            restored.replay(engine.events());
        else
            throw std::invalid_argument("usage: lob_demo [new-event-file]");
        if (restored.snapshot() != engine.snapshot())
            throw std::runtime_error("replay mismatch");
        std::cout << "Replay matches book, reserved IDs, and processing sequence.\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
