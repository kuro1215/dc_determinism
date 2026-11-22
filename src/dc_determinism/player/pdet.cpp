#include "player/pdet.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>

#include "common/sha256.hpp"

namespace dc_determinism {

namespace {

std::uint32_t load_u32(const std::string& data, std::size_t offset) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data()) + offset;
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// `struct.unpack_from(fmt, data, offset)` of `size` bytes: struct.error text when the buffer is
// too short.
void require_bytes(const std::string& data, std::size_t offset, std::size_t size) {
    if (offset > data.size() || data.size() - offset < size) {
        throw PdetError("unpack_from requires a buffer of at least " + std::to_string(offset + size) +
                        " bytes for unpacking " + std::to_string(size) + " bytes at offset " +
                        std::to_string(offset) + " (actual buffer size is " + std::to_string(data.size()) + ")");
    }
}

bool is_finite_bits(std::uint32_t bits) { return (bits & 0x7f800000u) != 0x7f800000u; }

// `as_float(bits)`: the float32 value as a double; a non-finite value is an error.
double as_float(std::uint32_t bits) {
    if (!is_finite_bits(bits)) throw PdetError("non-finite output");
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return static_cast<double>(value);
}

std::string hex8(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08x", static_cast<unsigned>(value));
    return text;
}

}  // namespace

PdetRecord decode_pdet(std::string data) {
    PdetRecord record;
    if (data.compare(0, 8, "PDET0001") != 0) throw PdetError("invalid output magic");
    require_bytes(data, 8, 8);
    std::uint32_t player = load_u32(data, 8);
    std::uint32_t count = load_u32(data, 12);
    if ((player != 1 && player != 2) || count != kPdetBlocks) throw PdetError("invalid player or block count");

    std::size_t offset = 16;
    std::set<std::pair<std::uint32_t, std::uint32_t>> keys;
    std::int64_t failures = 0;
    std::int64_t samples = 0;
    std::vector<PdetBlock> blocks;
    blocks.reserve(count);
    std::size_t per_profile[4] = {0, 0, 0, 0};
    for (std::uint32_t i = 0; i < count; ++i) {
        require_bytes(data, offset, 12);
        PdetBlock block;
        block.profile = load_u32(data, offset);
        block.seed = load_u32(data, offset + 4);
        block.n = load_u32(data, offset + 8);
        offset += 12;
        if (block.profile > 3 || block.n < 640 || keys.count({block.profile, block.seed}) != 0) {
            throw PdetError("invalid block identity");
        }
        keys.insert({block.profile, block.seed});
        ++per_profile[block.profile];
        block.config_offset = offset;
        block.controls_offset = offset + kPdetConfigSize;
        offset += kPdetConfigSize + kPdetControlsSize;
        block.calls_offset = offset;
        std::uint64_t call_bytes = static_cast<std::uint64_t>(block.n) * kPdetCallSize;
        if (offset > data.size() || data.size() - offset < call_bytes) throw PdetError("truncated sample records");
        offset += static_cast<std::size_t>(call_bytes);
        static const std::uint32_t kCheckpoints[6] = {1, 2, 3, 17, 624, 625};
        for (std::uint32_t expected : kCheckpoints) {
            require_bytes(data, offset, 16);
            std::uint32_t checkpoint = load_u32(data, offset);
            bool valid = checkpoint == expected;
            for (std::size_t flag = 0; flag < 3; ++flag) {
                std::uint32_t value = load_u32(data, offset + 4 + 4 * flag);
                if (value != 0 && value != 1) valid = false;
                if (value == 0) ++failures;
            }
            offset += 16;
            if (!valid) throw PdetError("invalid restore probe");
        }
        samples += block.n;
        blocks.push_back(block);
    }
    for (std::size_t profile = 0; profile < 4; ++profile) {
        if (per_profile[profile] != kPdetBlocksPerProfile) {
            throw PdetError("trailing bytes or incomplete seed/profile coverage");
        }
    }
    if (offset != data.size()) throw PdetError("trailing bytes or incomplete seed/profile coverage");

    record.player = player;
    record.blocks = std::move(blocks);
    record.samples = samples;
    record.restore_probes = 256 * 6 * 3;
    record.restore_failures = failures;
    record.sha256 = sha256_hex(data);
    record.data = std::move(data);
    return record;
}

Json compare_pdet(const PdetRecord& left, const PdetRecord& right) {
    if (left.player != right.player) throw PdetError("cross-player sequence equality is not a determinism test");
    if (left.samples != right.samples) throw PdetError("sample coverage conflict");

    std::int64_t different_samples = 0;
    std::int64_t different_fields[3] = {0, 0, 0};
    double max_difference[3] = {0.0, 0.0, 0.0};
    Json first_difference = nullptr;
    struct Profile {
        std::int64_t samples = 0;
        std::int64_t different_samples = 0;
        double max_difference[3] = {0.0, 0.0, 0.0};
    } profiles[4];

    for (std::size_t block_index = 0; block_index < left.blocks.size() && block_index < right.blocks.size();
         ++block_index) {
        const PdetBlock& a = left.blocks[block_index];
        const PdetBlock& b = right.blocks[block_index];
        if (a.profile != b.profile || a.seed != b.seed || a.n != b.n ||
            std::memcmp(left.data.data() + a.config_offset, right.data.data() + b.config_offset,
                        kPdetConfigSize) != 0 ||
            std::memcmp(left.data.data() + a.controls_offset, right.data.data() + b.controls_offset,
                        kPdetControlsSize) != 0) {
            throw PdetError("input configuration or raw PRNG control conflict");
        }
        Profile& profile = profiles[a.profile];
        profile.samples += a.n;
        for (std::uint32_t index = 0; index < a.n; ++index) {
            std::size_t pa = a.calls_offset + static_cast<std::size_t>(index) * kPdetCallSize;
            std::size_t pb = b.calls_offset + static_cast<std::size_t>(index) * kPdetCallSize;
            std::uint32_t x[6], y[6];
            for (std::size_t k = 0; k < 6; ++k) {
                x[k] = load_u32(left.data, pa + 4 * k);
                y[k] = load_u32(right.data, pb + 4 * k);
            }
            if (x[0] != y[0] || x[1] != y[1] || x[2] != y[2]) throw PdetError("input shot bits conflict");
            for (std::size_t k = 3; k < 6; ++k) as_float(x[k]);
            for (std::size_t k = 3; k < 6; ++k) as_float(y[k]);
            bool differs[3];
            bool any = false;
            for (std::size_t k = 0; k < 3; ++k) {
                differs[k] = x[k + 3] != y[k + 3];
                any = any || differs[k];
            }
            if (!any) continue;
            ++different_samples;
            ++profile.different_samples;
            if (first_difference.is_null()) {
                Json left_bits = Json::array(), right_bits = Json::array();
                for (std::size_t k = 3; k < 6; ++k) {
                    left_bits.push_back(hex8(x[k]));
                    right_bits.push_back(hex8(y[k]));
                }
                first_difference = Json::object();
                first_difference["profile"] = static_cast<std::int64_t>(a.profile);
                first_difference["seed"] = static_cast<std::int64_t>(a.seed);
                first_difference["index"] = static_cast<std::int64_t>(index);
                first_difference["left_bits"] = left_bits;
                first_difference["right_bits"] = right_bits;
            }
            for (std::size_t k = 0; k < 3; ++k) {
                if (!differs[k]) continue;
                double delta = std::fabs(as_float(x[k + 3]) - as_float(y[k + 3]));
                ++different_fields[k];
                max_difference[k] = std::max(max_difference[k], delta);
                profile.max_difference[k] = std::max(profile.max_difference[k], delta);
            }
        }
    }

    auto floats = [](const double* values) {
        Json out = Json::array();
        for (std::size_t k = 0; k < 3; ++k) out.push_back(values[k]);
        return out;
    };
    Json result = Json::object();
    result["samples"] = left.samples;
    result["different_samples"] = different_samples;
    Json fields = Json::array();
    for (std::size_t k = 0; k < 3; ++k) fields.push_back(different_fields[k]);
    result["different_fields"] = fields;
    result["max_absolute_difference"] = floats(max_difference);
    result["first_difference"] = first_difference;
    Json profile_list = Json::array();
    for (std::size_t p = 0; p < 4; ++p) {
        Json entry = Json::object();
        entry["profile"] = static_cast<std::int64_t>(p);
        entry["samples"] = profiles[p].samples;
        entry["different_samples"] = profiles[p].different_samples;
        entry["max_absolute_difference"] = floats(profiles[p].max_difference);
        profile_list.push_back(entry);
    }
    result["profiles"] = profile_list;
    // `100 * different / samples`: an exact integer product, one correctly rounded division.
    result["different_percent"] = static_cast<double>(100 * different_samples) / static_cast<double>(left.samples);
    return result;
}

}  // namespace dc_determinism
