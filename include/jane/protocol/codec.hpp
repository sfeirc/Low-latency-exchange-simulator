#pragma once

#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "jane/protocol/messages.hpp"

// Every function here is memcpy-based, not a reinterpret_cast view: `in`
// may come from a socket recv() buffer, a mmap'd recording file, or a
// ring-buffer slot, and this codec makes no alignment assumption about
// any of them. All of it is bounds-checked against the buffer length
// before any read, so feeding it truncated or garbage bytes is defined
// behavior (a DecodeStatus other than Ok), never UB — exercised directly
// by a fuzz-style test in tests/test_protocol.cpp.
namespace jane::protocol {

enum class DecodeStatus {
    Ok,
    Incomplete,      // fewer bytes available than the message needs so far
    UnexpectedType,  // header.type didn't match the Payload requested
    Malformed,       // header.length is inconsistent with Payload's fixed size
};

template <typename Payload>
struct DecodeResult {
    DecodeStatus status;
    Payload payload{};
    std::size_t consumed = 0;  // valid iff status == Ok
};

// Returns 0 (and writes nothing) if `out` is too small to hold the framed
// message — never partially writes.
template <typename Payload>
[[nodiscard]] std::size_t encode(std::span<std::byte> out, const Payload& payload) noexcept {
    static_assert(std::is_trivially_copyable_v<Payload>);
    constexpr std::size_t total = sizeof(MessageHeader) + sizeof(Payload);
    if (out.size() < total) {
        return 0;
    }
    const MessageHeader header{
        .length = static_cast<std::uint16_t>(sizeof(Payload)),
        .type = MessageTraits<Payload>::type,
        .reserved = 0,
    };
    std::memcpy(out.data(), &header, sizeof(header));
    std::memcpy(out.data() + sizeof(header), &payload, sizeof(payload));
    return total;
}

// Peek at just the header without committing to a Payload type — the
// intended use is a stream parser that reads `.type` here first, then
// dispatches to decode<T>() for the matching T.
[[nodiscard]] inline std::optional<MessageHeader> peek_header(
    std::span<const std::byte> in) noexcept {
    if (in.size() < sizeof(MessageHeader)) {
        return std::nullopt;
    }
    MessageHeader header;
    std::memcpy(&header, in.data(), sizeof(header));
    return header;
}

template <typename Payload>
[[nodiscard]] DecodeResult<Payload> decode(std::span<const std::byte> in) noexcept {
    static_assert(std::is_trivially_copyable_v<Payload>);
    const auto header = peek_header(in);
    if (!header) {
        return {.status = DecodeStatus::Incomplete};
    }
    if (header->type != MessageTraits<Payload>::type) {
        return {.status = DecodeStatus::UnexpectedType};
    }
    if (header->length != sizeof(Payload)) {
        return {.status = DecodeStatus::Malformed};
    }
    const std::size_t total = sizeof(MessageHeader) + header->length;
    if (in.size() < total) {
        return {.status = DecodeStatus::Incomplete};
    }
    Payload payload;
    std::memcpy(&payload, in.data() + sizeof(MessageHeader), sizeof(payload));
    return {.status = DecodeStatus::Ok, .payload = payload, .consumed = total};
}

// --- BookSnapshot: header + variable-length level records ---------------
//
// Not a hot-path message (snapshots are periodic, not per-order), so
// decode_snapshot copies levels into caller-provided vectors rather than
// offering a zero-copy view — simplicity over an optimization nothing on
// the hot path needs. Revisit only if profiling ever says otherwise.

[[nodiscard]] inline std::size_t encode_snapshot(std::span<std::byte> out, std::uint64_t sequence,
                                                  std::uint32_t symbol_id,
                                                  std::span<const PriceLevelRecord> bids,
                                                  std::span<const PriceLevelRecord> asks) noexcept {
    const std::size_t payload_bytes =
        sizeof(BookSnapshotHeader) + (bids.size() + asks.size()) * sizeof(PriceLevelRecord);
    const std::size_t total = sizeof(MessageHeader) + payload_bytes;
    if (out.size() < total || bids.size() > 0xFFFF || asks.size() > 0xFFFF ||
        payload_bytes > 0xFFFF) {
        return 0;
    }

    const MessageHeader header{
        .length = static_cast<std::uint16_t>(payload_bytes),
        .type = MessageType::BookSnapshot,
        .reserved = 0,
    };
    const BookSnapshotHeader snap_header{
        .sequence = sequence,
        .symbol_id = symbol_id,
        .bid_level_count = static_cast<std::uint16_t>(bids.size()),
        .ask_level_count = static_cast<std::uint16_t>(asks.size()),
    };

    std::byte* cursor = out.data();
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    std::memcpy(cursor, &snap_header, sizeof(snap_header));
    cursor += sizeof(snap_header);
    if (!bids.empty()) {
        std::memcpy(cursor, bids.data(), bids.size() * sizeof(PriceLevelRecord));
        cursor += bids.size() * sizeof(PriceLevelRecord);
    }
    if (!asks.empty()) {
        std::memcpy(cursor, asks.data(), asks.size() * sizeof(PriceLevelRecord));
        cursor += asks.size() * sizeof(PriceLevelRecord);
    }
    return total;
}

struct SnapshotDecodeResult {
    DecodeStatus status;
    BookSnapshotHeader header{};
    std::vector<PriceLevelRecord> bids;
    std::vector<PriceLevelRecord> asks;
    std::size_t consumed = 0;
};

[[nodiscard]] inline SnapshotDecodeResult decode_snapshot(std::span<const std::byte> in) {
    const auto header = peek_header(in);
    if (!header) {
        return {.status = DecodeStatus::Incomplete, .bids = {}, .asks = {}};
    }
    if (header->type != MessageType::BookSnapshot) {
        return {.status = DecodeStatus::UnexpectedType, .bids = {}, .asks = {}};
    }
    const std::size_t total = sizeof(MessageHeader) + header->length;
    if (in.size() < total) {
        return {.status = DecodeStatus::Incomplete, .bids = {}, .asks = {}};
    }
    if (header->length < sizeof(BookSnapshotHeader)) {
        return {.status = DecodeStatus::Malformed, .bids = {}, .asks = {}};
    }

    BookSnapshotHeader snap_header;
    std::memcpy(&snap_header, in.data() + sizeof(MessageHeader), sizeof(snap_header));

    const std::size_t level_bytes =
        static_cast<std::size_t>(snap_header.bid_level_count + snap_header.ask_level_count) *
        sizeof(PriceLevelRecord);
    if (sizeof(BookSnapshotHeader) + level_bytes != header->length) {
        return {.status = DecodeStatus::Malformed, .bids = {}, .asks = {}};
    }

    SnapshotDecodeResult result{.status = DecodeStatus::Ok, .header = snap_header, .bids = {}, .asks = {}};
    const std::byte* cursor = in.data() + sizeof(MessageHeader) + sizeof(BookSnapshotHeader);

    result.bids.resize(snap_header.bid_level_count);
    if (snap_header.bid_level_count > 0) {
        std::memcpy(result.bids.data(), cursor,
                    static_cast<std::size_t>(snap_header.bid_level_count) * sizeof(PriceLevelRecord));
        cursor += static_cast<std::size_t>(snap_header.bid_level_count) * sizeof(PriceLevelRecord);
    }
    result.asks.resize(snap_header.ask_level_count);
    if (snap_header.ask_level_count > 0) {
        std::memcpy(result.asks.data(), cursor,
                    static_cast<std::size_t>(snap_header.ask_level_count) * sizeof(PriceLevelRecord));
    }
    result.consumed = total;
    return result;
}

}  // namespace jane::protocol
