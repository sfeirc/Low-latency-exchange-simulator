#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <vector>

// Two sinks satisfying the same informal contract MarketDataPublisher
// depends on — `void write(std::span<const std::byte>)` — chosen as a
// duck-typed template parameter rather than a virtual interface so
// publishing never pays for indirection on what is, per the architecture
// diagram, a hot pipeline stage.
namespace jane::marketdata {

// Retains every published byte in memory, in order — used by tests and by
// the live book viewer / animation recorder (jane::book_viewer), which
// need to read back exactly what was published rather than just confirm
// it was.
class InMemorySink {
public:
    void write(std::span<const std::byte> bytes) {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    }
    [[nodiscard]] std::span<const std::byte> data() const noexcept { return buffer_; }
    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
    void clear() noexcept { buffer_.clear(); }

    // Pre-size the backing buffer. Not required for correctness — write()
    // grows it automatically — but a long-running session that never
    // calls this pays for std::vector's doubling reallocation strategy
    // exactly like any other unreserved vector would: each doubling
    // copies the *entire* buffer so far, so the cost of each one grows
    // geometrically with how much has accumulated. This is not
    // hypothetical: bench_end_to_end.cpp's first run showed a handful of
    // multi-millisecond latency spikes at order indices that themselves
    // doubled each time (655, 2576, 5133, ... 328802) — the signature of
    // exactly this pattern — before this method existed and the
    // benchmark started calling it. See docs/tradeoffs.md.
    void reserve(std::size_t bytes) { buffer_.reserve(bytes); }

private:
    std::vector<std::byte> buffer_;
};

// Appends published bytes to a binary file — the recording jane::replay
// can later play back for deterministic reproduction.
class FileSink {
public:
    explicit FileSink(const std::filesystem::path& path)
        : file_(path, std::ios::binary | std::ios::out | std::ios::trunc) {
        if (!file_) {
            throw std::runtime_error("FileSink: failed to open " + path.string());
        }
    }

    void write(std::span<const std::byte> bytes) {
        file_.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }

private:
    std::ofstream file_;
};

}  // namespace jane::marketdata
