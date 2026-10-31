#pragma once
#include "lob/engine.hpp"
#include <filesystem>
#include <fstream>
#include <string>

namespace lob {
std::string encode(const EventBatch &batch);
EventBatch decode(const std::string &record);
struct Recovery {
    std::vector<EventBatch> batches;
    std::uint64_t valid_bytes{};
    std::string error;
};
// Strict by default. Prefix recovery stops at the first bad record, never skips it.
Recovery read_events(const std::filesystem::path &path, bool recover_prefix = false);
class EventFile {
    std::ofstream stream_;
    std::uint64_t sequence_{};
    bool failed_{};

  public:
    enum class Mode { CreateNew, Append };
    explicit EventFile(const std::filesystem::path &path, Mode mode = Mode::CreateNew);
    void append(const EventBatch &batch);
    void flush(); // Checks stream flush, NOT fsync / power-loss durability.
};
} // namespace lob
