// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <cstddef>
#include "utf8_path.hpp"

namespace digitalcurling::plugins::detail {

bool IsValidUtf8(const std::string& value) {
    for (size_t i = 0; i < value.size();) {
        const auto lead = static_cast<unsigned char>(value[i]);
        size_t count = 0;
        unsigned int code = 0;
        if (lead < 0x80) { count = 1; code = lead; }
        else if (lead >= 0xC2 && lead <= 0xDF) { count = 2; code = lead & 0x1F; }
        else if (lead >= 0xE0 && lead <= 0xEF) { count = 3; code = lead & 0x0F; }
        else if (lead >= 0xF0 && lead <= 0xF4) { count = 4; code = lead & 0x07; }
        else return false;
        if (count > value.size() - i) return false;
        for (size_t j = 1; j < count; ++j) {
            const auto byte = static_cast<unsigned char>(value[i + j]);
            if ((byte & 0xC0) != 0x80) return false;
            code = (code << 6) | (byte & 0x3F);
        }
        if ((count == 3 && code < 0x800) || (count == 4 && code < 0x10000) ||
            (code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF) return false;
        i += count;
    }
    return true;
}

std::string PathToUtf8(const std::filesystem::path& path) {
    try {
        const auto u8 = path.u8string();
        const std::string result(u8.begin(), u8.end());
        return IsValidUtf8(result) ? result : "<unrepresentable path>";
    } catch (...) {
        return "<unrepresentable path>";
    }
}

} // namespace digitalcurling::plugins::detail
