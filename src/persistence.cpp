#include "lob/persistence.hpp"
#include <charconv>
#include <sstream>
#include <locale>
#include <type_traits>

namespace lob {
namespace {
constexpr std::size_t max_record = 16 * 1024 * 1024;
std::uint64_t checksum(const std::string &bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    return hash; // FNV-1a; intentional modular arithmetic, not authentication.
}
std::uint64_t number(std::istream &input) {
    std::string token;
    if (!(input >> token))
        throw std::invalid_argument("missing integer");
    std::uint64_t value{};
    auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (error != std::errc{} || end != token.data() + token.size())
        throw std::invalid_argument("invalid integer");
    return value;
}
} // namespace
std::string encode(const EventBatch &batch) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "LOB " << batch.version << ' ' << batch.sequence << ' ' << batch.events.size();
    for (const auto &event : batch.events)
        std::visit(
            [&](const auto &value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, Accepted>) {
                    const auto &o = value.order;
                    validate(o.side);
                    out << " A " << o.id.value() << ' ' << (o.side == Side::Buy ? 'B' : 'S') << ' '
                        << o.price.value() << ' ' << o.quantity.value();
                } else if constexpr (std::is_same_v<T, Traded>) {
                    const auto &f = value.fill;
                    out << " T " << f.maker.value() << ' ' << f.taker.value() << ' '
                        << f.price.value() << ' ' << f.quantity.value();
                } else if constexpr (std::is_same_v<T, Cancelled>)
                    out << " C " << value.id.value();
                else {
                    if (value.reason != Status::DuplicateId && value.reason != Status::UnknownOrder)
                        throw std::invalid_argument("invalid rejection status");
                    out << " R " << value.id.value() << ' '
                        << (value.reason == Status::DuplicateId ? 'D' : 'U');
                }
            },
            event);
    auto payload = out.str();
    auto record = payload + " # " + std::to_string(checksum(payload));
    if (record.size() > max_record)
        throw std::length_error("event record exceeds 16 MiB");
    return record;
}
EventBatch decode(const std::string &record) {
    if (record.size() > max_record)
        throw std::invalid_argument("oversized record");
    auto split = record.rfind(" # ");
    if (split == std::string::npos)
        throw std::invalid_argument("missing checksum");
    auto payload = record.substr(0, split);
    std::istringstream hash_input(record.substr(split + 3));
    hash_input.imbue(std::locale::classic());
    if (number(hash_input) != checksum(payload))
        throw std::invalid_argument("checksum mismatch");
    std::istringstream input(payload);
    input.imbue(std::locale::classic());
    std::string tag;
    input >> tag;
    if (tag != "LOB" || number(input) != 1)
        throw std::invalid_argument("unknown format/version");
    auto sequence = number(input), count = number(input);
    if (!sequence || !count || count > max_record / 4)
        throw std::invalid_argument("invalid sequence/count");
    EventBatch batch{1, sequence, {}};
    for (std::uint64_t i = 0; i < count; ++i) {
        if (!(input >> tag))
            throw std::invalid_argument("missing event");
        OrderId id(number(input));
        if (tag == "A") {
            std::string side;
            input >> side;
            if (side != "B" && side != "S")
                throw std::invalid_argument("invalid side");
            Price price(number(input));
            Quantity qty(number(input));
            batch.events.emplace_back(
                Accepted{IncomingOrder{id, side == "B" ? Side::Buy : Side::Sell, price, qty}});
        } else if (tag == "T") {
            OrderId taker(number(input));
            Price price(number(input));
            Quantity qty(number(input));
            batch.events.emplace_back(Traded{Fill{id, taker, price, qty}});
        } else if (tag == "C")
            batch.events.emplace_back(Cancelled{id});
        else if (tag == "R") {
            std::string reason;
            input >> reason;
            if (reason != "D" && reason != "U")
                throw std::invalid_argument("invalid rejection");
            batch.events.emplace_back(
                Rejected{id, reason == "D" ? Status::DuplicateId : Status::UnknownOrder});
        } else
            throw std::invalid_argument("unknown event");
    }
    // Enforce canonical form, rejecting trailing data, signed integers, and ambiguous whitespace.
    if (encode(batch) != record)
        throw std::invalid_argument("noncanonical/trailing data");
    return batch;
}
Recovery read_events(const std::filesystem::path &path, bool recover_prefix) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open event file");
    Recovery result;
    std::string record;
    try {
        char ch;
        while (input.get(ch)) {
            if (ch != '\n') {
                if (record.size() == max_record)
                    throw std::invalid_argument("oversized record");
                record.push_back(ch);
                continue;
            }
            auto batch = decode(record);
            if (batch.sequence != result.batches.size() + 1)
                throw std::invalid_argument("noncontiguous sequence");
            result.batches.push_back(std::move(batch));
            result.valid_bytes += record.size() + 1;
            record.clear();
        }
        if (input.bad())
            throw std::runtime_error("event read failed");
        if (!record.empty())
            throw std::invalid_argument("truncated record (missing newline)");
    } catch (const std::exception &e) {
        if (!recover_prefix)
            throw;
        result.error = "at byte " + std::to_string(result.valid_bytes) + ": " + e.what();
    }
    return result;
}
EventFile::EventFile(const std::filesystem::path &path, Mode mode) {
    if (mode == Mode::CreateNew && std::filesystem::exists(path))
        throw std::runtime_error("event file already exists");
    if (mode == Mode::Append) {
        auto recovered = read_events(path);
        Engine validator(false);
        validator.replay(recovered.batches);
        sequence_ = recovered.batches.size();
    }
    stream_.open(path, std::ios::binary | std::ios::app);
    if (!stream_)
        throw std::runtime_error("cannot create/open event file");
}
void EventFile::append(const EventBatch &batch) {
    if (failed_)
        throw std::runtime_error("event writer failed; reopen after recovery");
    if (batch.version != 1 || batch.events.empty() || batch.sequence != next(sequence_))
        throw std::invalid_argument("invalid append batch/sequence");
    auto record = encode(batch);
    stream_ << record << '\n';
    if (!stream_) {
        failed_ = true;
        throw std::runtime_error("event write failed");
    }
    sequence_ = batch.sequence;
}
void EventFile::flush() {
    if (failed_)
        throw std::runtime_error("event writer failed");
    stream_.flush();
    if (!stream_) {
        failed_ = true;
        throw std::runtime_error("event flush failed");
    }
}
} // namespace lob
