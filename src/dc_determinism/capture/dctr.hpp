// Trajectory format `dctr-v1`: lossless storage of one shot's raw stone
// states.
//
// Input is the worker's `<pos>.raw` capture file (little-endian): u32 active_mask, u32 frame_count,
// then frame_count x popcount(mask) x 6 float32 (x, y, angle, vx, vy, w) in stone-index order.
//
// The payload stored in a capture archive is "DCTR0001", u32 header length, the UTF-8 JSON header,
// then for every active stone (ascending index) and each of the 6 fields the 4 byte planes of the
// field's per-frame XOR-delta stream (the first value is XORed with 0). Everything works on the
// 32-bit patterns, so -0.0 and NaN payloads round-trip exactly.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json/json.hpp"

namespace dc_determinism {

extern const char* const kDctrFormat;  // "dctr-v1"
constexpr std::size_t kDctrFieldCount = 6;
extern const char* const kDctrFields[kDctrFieldCount];  // x y angle vx vy w

// `active_stones(mask)`: the set bits of a 32-bit mask, ascending.
std::vector<int> active_stones(std::uint32_t mask);

// `parse_raw(raw)`: validates a worker capture file (DcDeterminismError); frames and stones come from it.
struct RawCapture {
    std::uint32_t mask = 0;
    std::uint32_t frames = 0;
    std::vector<int> stones;
};
RawCapture parse_raw(const std::string& raw);

// `raw_to_payload(raw, info)`: `shot_id`, `index` and `engine` fill the header. `frames` (optional)
// receives the frame count.
std::string raw_to_payload(const std::string& raw, const std::string& shot_id, std::int64_t index,
                           const std::string& engine, std::int64_t* frames = nullptr);

// A decoded `dctr-v1` payload; columns are produced on demand.
class CapturedTrajectory {
public:
    // DcDeterminismError: "not a dctr-v1 trajectory (bad magic)", "truncated dctr-v1 header",
    // "malformed dctr-v1 header: ...", "unsupported dctr-v1 header", "inconsistent dctr-v1 header",
    // "dctr-v1 body has N bytes, expected M".
    explicit CapturedTrajectory(std::string payload);

    const Json& header() const { return header_; }
    const std::vector<int>& stones() const { return stones_; }
    std::int64_t frames() const { return frames_; }
    bool has_stone(int stone) const;

    // The 32-bit patterns of one stone's field over all frames.
    std::vector<std::uint32_t> column_bits(int stone, int field) const;

    // Rebuilds the worker capture file (bit-exact inverse of raw_to_payload).
    std::string to_raw() const;

private:
    std::string payload_;
    std::size_t body_offset_ = 0;
    Json header_;
    std::vector<int> stones_;
    std::int64_t frames_ = 0;
    std::uint32_t mask_ = 0;
};

}  // namespace dc_determinism
