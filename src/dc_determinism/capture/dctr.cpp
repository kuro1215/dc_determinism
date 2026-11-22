#include "capture/dctr.hpp"

#include <cstring>

#include "common/error.hpp"
#include "corpus/pytext.hpp"
#include "json/pyparse.hpp"
#include "json/writer.hpp"

namespace dc_determinism {

const char* const kDctrFormat = "dctr-v1";
const char* const kDctrFields[kDctrFieldCount] = {"x", "y", "angle", "vx", "vy", "w"};

namespace {

const char kMagic[] = {'D', 'C', 'T', 'R', '0', '0', '0', '1'};
constexpr std::size_t kMagicSize = sizeof(kMagic);
constexpr std::size_t kRawHeadSize = 8;
constexpr std::size_t kValueSize = 4;

std::uint32_t read_u32(const std::string& data, std::size_t offset) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data()) + offset;
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

void put_u32(std::string& out, std::size_t offset, std::uint32_t value) {
    for (std::size_t b = 0; b < 4; ++b) out[offset + b] = static_cast<char>((value >> (8 * b)) & 0xFF);
}

}  // namespace

std::vector<int> active_stones(std::uint32_t mask) {
    std::vector<int> stones;
    for (int index = 0; index < 32; ++index) {
        if (((mask >> index) & 1u) != 0) stones.push_back(index);
    }
    return stones;
}

RawCapture parse_raw(const std::string& raw) {
    if (raw.size() < kRawHeadSize) throw DcDeterminismError("capture file is shorter than its 8-byte header");
    RawCapture out;
    out.mask = read_u32(raw, 0);
    out.frames = read_u32(raw, 4);
    out.stones = active_stones(out.mask);
    std::uint64_t expected = kRawHeadSize + static_cast<std::uint64_t>(out.frames) * out.stones.size() *
                                                kDctrFieldCount * kValueSize;
    if (raw.size() != expected) {
        throw DcDeterminismError("capture file has " + std::to_string(raw.size()) + " bytes, expected " +
                         std::to_string(expected) + " for " + std::to_string(out.frames) + " frames and " +
                         std::to_string(out.stones.size()) + " active stones");
    }
    return out;
}

std::string raw_to_payload(const std::string& raw, const std::string& shot_id, std::int64_t index,
                           const std::string& engine, std::int64_t* frames_out) {
    RawCapture capture = parse_raw(raw);
    Json header = Json::object();
    header["format"] = kDctrFormat;
    header["shot_id"] = shot_id;
    header["index"] = index;
    header["engine"] = engine;
    header["active_mask"] = static_cast<std::int64_t>(capture.mask);
    header["frames"] = static_cast<std::int64_t>(capture.frames);
    Json stones = Json::array();
    for (int stone : capture.stones) stones.push_back(stone);
    header["stones"] = stones;
    Json fields = Json::array();
    for (const char* field : kDctrFields) fields.push_back(field);
    header["fields"] = fields;
    std::string head = json_dumps(header, DumpOptions::compact(true, false));

    std::string out(kMagic, kMagicSize);
    std::string length(4, '\0');
    put_u32(length, 0, static_cast<std::uint32_t>(head.size()));
    out += length;
    out += head;

    std::size_t columns = capture.stones.size() * kDctrFieldCount;
    std::size_t frames = capture.frames;
    if (frames != 0 && columns != 0) {
        std::size_t base = out.size();
        out.resize(base + columns * frames * kValueSize);
        std::vector<std::uint32_t> deltas(frames);
        for (std::size_t column = 0; column < columns; ++column) {
            std::uint32_t previous = 0;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                std::uint32_t value = read_u32(raw, kRawHeadSize + (frame * columns + column) * kValueSize);
                deltas[frame] = value ^ previous;
                previous = value;
            }
            // The 4 byte planes of the column, concatenated.
            std::size_t offset = base + column * frames * kValueSize;
            for (std::size_t plane = 0; plane < kValueSize; ++plane) {
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    out[offset + plane * frames + frame] = static_cast<char>((deltas[frame] >> (8 * plane)) & 0xFF);
                }
            }
        }
    }
    if (frames_out != nullptr) *frames_out = static_cast<std::int64_t>(capture.frames);
    return out;
}

CapturedTrajectory::CapturedTrajectory(std::string payload) : payload_(std::move(payload)) {
    if (payload_.size() < kMagicSize + 4 || std::memcmp(payload_.data(), kMagic, kMagicSize) != 0) {
        throw DcDeterminismError("not a dctr-v1 trajectory (bad magic)");
    }
    std::uint64_t length = read_u32(payload_, kMagicSize);
    std::size_t start = kMagicSize + 4;
    if (start + length > payload_.size()) throw DcDeterminismError("truncated dctr-v1 header");
    std::string text = payload_.substr(start, static_cast<std::size_t>(length));
    std::string error;
    if (!is_valid_utf8(text)) throw DcDeterminismError("malformed dctr-v1 header: invalid UTF-8");
    if (!py_json_loads(text, header_, error)) throw DcDeterminismError("malformed dctr-v1 header: " + error);

    bool known = header_.is_object() && header_.contains("format") && header_["format"] == kDctrFormat &&
                 header_.contains("fields") && header_["fields"].is_array() &&
                 header_["fields"].size() == kDctrFieldCount;
    for (std::size_t i = 0; known && i < kDctrFieldCount; ++i) known = header_["fields"][i] == kDctrFields[i];
    if (!known) throw DcDeterminismError("unsupported dctr-v1 header");

    bool consistent = header_.contains("stones") && header_["stones"].is_array() && header_.contains("frames") &&
                      header_["frames"].is_number_integer() && header_["frames"].get<std::int64_t>() >= 0 &&
                      header_.contains("active_mask") && header_["active_mask"].is_number_integer();
    std::int64_t mask = 0;
    if (consistent) {
        mask = header_["active_mask"].get<std::int64_t>();
        consistent = mask >= 0 && mask < (static_cast<std::int64_t>(1) << 32);
    }
    if (consistent) {
        std::vector<int> expected = active_stones(static_cast<std::uint32_t>(mask));
        const Json& listed = header_["stones"];
        consistent = listed.size() == expected.size();
        for (std::size_t i = 0; consistent && i < expected.size(); ++i) {
            consistent = listed[i].is_number_integer() && listed[i].get<std::int64_t>() == expected[i];
        }
        stones_ = expected;
    }
    if (!consistent) throw DcDeterminismError("inconsistent dctr-v1 header");
    mask_ = static_cast<std::uint32_t>(mask);
    frames_ = header_["frames"].get<std::int64_t>();
    body_offset_ = start + static_cast<std::size_t>(length);
    std::uint64_t body = payload_.size() - body_offset_;
    std::uint64_t expected_body = static_cast<std::uint64_t>(stones_.size()) * kDctrFieldCount *
                                  static_cast<std::uint64_t>(frames_) * kValueSize;
    if (body != expected_body) {
        throw DcDeterminismError("dctr-v1 body has " + std::to_string(body) + " bytes, expected " +
                         std::to_string(expected_body));
    }
}

bool CapturedTrajectory::has_stone(int stone) const {
    for (int candidate : stones_) {
        if (candidate == stone) return true;
    }
    return false;
}

std::vector<std::uint32_t> CapturedTrajectory::column_bits(int stone, int field) const {
    std::size_t position = 0;
    while (position < stones_.size() && stones_[position] != stone) ++position;
    if (position == stones_.size()) throw DcDeterminismError("stone " + std::to_string(stone) + " is not in the trajectory");
    std::size_t frames = static_cast<std::size_t>(frames_);
    std::size_t column = position * kDctrFieldCount + static_cast<std::size_t>(field);
    const unsigned char* planes =
        reinterpret_cast<const unsigned char*>(payload_.data()) + body_offset_ + column * frames * kValueSize;
    std::vector<std::uint32_t> values(frames);
    std::uint32_t running = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        std::uint32_t delta = 0;
        for (std::size_t plane = 0; plane < kValueSize; ++plane) {
            delta |= static_cast<std::uint32_t>(planes[plane * frames + frame]) << (8 * plane);
        }
        running ^= delta;  // prefix XOR
        values[frame] = running;
    }
    return values;
}

std::string CapturedTrajectory::to_raw() const {
    std::size_t columns = stones_.size() * kDctrFieldCount;
    std::size_t frames = static_cast<std::size_t>(frames_);
    std::string out(kRawHeadSize + frames * columns * kValueSize, '\0');
    put_u32(out, 0, mask_);
    put_u32(out, 4, static_cast<std::uint32_t>(frames_));
    for (std::size_t position = 0; position < stones_.size(); ++position) {
        for (std::size_t field = 0; field < kDctrFieldCount; ++field) {
            std::vector<std::uint32_t> bits = column_bits(stones_[position], static_cast<int>(field));
            std::size_t column = position * kDctrFieldCount + field;
            for (std::size_t frame = 0; frame < frames; ++frame) {
                put_u32(out, kRawHeadSize + (frame * columns + column) * kValueSize, bits[frame]);
            }
        }
    }
    return out;
}

}  // namespace dc_determinism
