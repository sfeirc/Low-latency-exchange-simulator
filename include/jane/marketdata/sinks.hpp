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
