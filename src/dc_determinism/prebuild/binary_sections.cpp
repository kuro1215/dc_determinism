#include "prebuild/binary_sections.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "common/sha256.hpp"

namespace dc_determinism {

namespace {

// Little-endian reads; false when the range is outside the file.
bool in_range(const std::string& bytes, std::uint64_t offset, std::uint64_t size) {
    return offset <= bytes.size() && size <= bytes.size() - offset;
}

std::uint64_t read_le(const std::string& bytes, std::uint64_t offset, int size) {
    std::uint64_t value = 0;
    for (int i = size - 1; i >= 0; --i) value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
    return value;
}

std::optional<ExecutableSections> pe_sections(const std::string& bytes) {
    if (!in_range(bytes, 0, 0x40) || bytes[0] != 'M' || bytes[1] != 'Z') return std::nullopt;
    std::uint64_t pe = read_le(bytes, 0x3C, 4);
    if (!in_range(bytes, pe, 24) || std::memcmp(bytes.data() + pe, "PE\0\0", 4) != 0) return std::nullopt;
    std::uint64_t count = read_le(bytes, pe + 6, 2);
    std::uint64_t optional = read_le(bytes, pe + 20, 2);
    std::uint64_t table = pe + 24 + optional;
    if (!in_range(bytes, table, count * 40)) return std::nullopt;
    ExecutableSections result;
    result.format = "PE";
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t entry = table + i * 40;
        std::string name(bytes.data() + entry, 8);
        name.erase(std::find(name.begin(), name.end(), '\0'), name.end());
        std::uint64_t size = read_le(bytes, entry + 16, 4);
        std::uint64_t pointer = read_le(bytes, entry + 20, 4);
        std::uint64_t flags = read_le(bytes, entry + 36, 4);
        if ((flags & 0x20000020u) == 0 || size == 0) continue;  // IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE
        if (!in_range(bytes, pointer, size)) return std::nullopt;
        result.sections.emplace_back(name, sha256_hex(bytes.substr(pointer, size)));
    }
    return result;
}

std::optional<ExecutableSections> elf_sections(const std::string& bytes) {
    if (!in_range(bytes, 0, 0x40) || std::memcmp(bytes.data(), "\x7f" "ELF", 4) != 0) return std::nullopt;
    if (bytes[4] != 2 || bytes[5] != 1) return std::nullopt;  // ELF64, little endian
    std::uint64_t headers = read_le(bytes, 0x28, 8);
    std::uint64_t entry_size = read_le(bytes, 0x3A, 2);
    std::uint64_t count = read_le(bytes, 0x3C, 2);
    std::uint64_t names = read_le(bytes, 0x3E, 2);
    if (entry_size < 64 || names >= count || !in_range(bytes, headers, count * entry_size)) return std::nullopt;
    std::uint64_t strings = read_le(bytes, headers + names * entry_size + 24, 8);
    std::uint64_t strings_size = read_le(bytes, headers + names * entry_size + 32, 8);
    if (!in_range(bytes, strings, strings_size)) return std::nullopt;
    ExecutableSections result;
    result.format = "ELF";
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t entry = headers + i * entry_size;
        std::uint64_t type = read_le(bytes, entry + 4, 4);
        std::uint64_t flags = read_le(bytes, entry + 8, 8);
        std::uint64_t offset = read_le(bytes, entry + 24, 8);
        std::uint64_t size = read_le(bytes, entry + 32, 8);
        if ((flags & 4) == 0 || type == 8 || size == 0) continue;  // SHF_EXECINSTR, not SHT_NOBITS
        if (!in_range(bytes, offset, size)) return std::nullopt;
        std::uint64_t name_offset = read_le(bytes, entry, 4);
        std::string name;
        for (std::uint64_t at = name_offset; at < strings_size && bytes[strings + at] != '\0'; ++at) name.push_back(bytes[strings + at]);
        result.sections.emplace_back(name, sha256_hex(bytes.substr(offset, size)));
    }
    return result;
}

}  // namespace

std::optional<ExecutableSections> executable_sections(const std::string& bytes) {
    if (std::optional<ExecutableSections> pe = pe_sections(bytes)) return pe;
    return elf_sections(bytes);
}

CodeComparison compare_code_sections(const std::string& first, const std::string& second) {
    CodeComparison result;
    std::optional<ExecutableSections> a = executable_sections(first);
    std::optional<ExecutableSections> b = executable_sections(second);
    if (!a || !b || a->format != b->format) return result;
    result.comparable = true;
    auto find = [](const ExecutableSections& sections, const std::string& name) -> const std::string* {
        for (const auto& item : sections.sections) {
            if (item.first == name) return &item.second;
        }
        return nullptr;
    };
    auto note = [&](const std::string& name) {
        if (std::find(result.names.begin(), result.names.end(), name) == result.names.end()) result.names.push_back(name);
    };
    for (const auto& item : a->sections) note(item.first);
    for (const auto& item : b->sections) note(item.first);
    for (const std::string& name : result.names) {
        const std::string* left = find(*a, name);
        const std::string* right = find(*b, name);
        if (left == nullptr || right == nullptr || *left != *right) result.different.push_back(name);
    }
    result.identical = result.different.empty();
    return result;
}

std::string code_comparison_text(const CodeComparison& comparison) {
    auto join = [](const std::vector<std::string>& items) {
        std::string out;
        for (const std::string& item : items) out += (out.empty() ? "" : ", ") + item;
        return out;
    };
    if (!comparison.comparable) return "code sections not compared (not two PE or two ELF64 files)";
    if (comparison.identical) return "code sections identical (" + join(comparison.names) + ")";
    return "code sections differ (" + join(comparison.different) + ")";
}

}  // namespace dc_determinism
