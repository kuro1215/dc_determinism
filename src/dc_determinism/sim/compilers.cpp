#include "sim/compilers.hpp"

#include <algorithm>

#include "common/error.hpp"

namespace dc_determinism {

namespace {

struct Variant {
    const char* id;
    const char* base;
};

const Variant kVariants[] = {{"gcc-nofma", "gcc"}, {"clang-nofma", "clang"}};

const char* const kFpContractOff = "-ffp-contract=off";

const Variant* find_variant(const std::string& id) {
    for (const Variant& variant : kVariants) {
        if (id == variant.id) return &variant;
    }
    return nullptr;
}

}  // namespace

const std::vector<std::string>& native_compiler_ids() {
    static const std::vector<std::string> ids = {"msvc", "gcc", "clang", "gcc-nofma", "clang-nofma"};
    return ids;
}

const std::vector<std::string>& simulator_compiler_choices() {
    static const std::vector<std::string> choices = {"msvc", "gcc", "clang", "gcc-nofma", "clang-nofma", "wasm"};
    return choices;
}

const std::vector<std::string>& player_compiler_choices() {
    static const std::vector<std::string> choices = {"gcc", "clang", "gcc-nofma", "clang-nofma", "wasm", "msvc"};
    return choices;
}

bool is_native_compiler(const std::string& id) {
    const std::vector<std::string>& ids = native_compiler_ids();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

std::string base_compiler(const std::string& id) {
    const Variant* variant = find_variant(id);
    return variant != nullptr ? std::string(variant->base) : id;
}

bool is_variant_compiler(const std::string& id) { return find_variant(id) != nullptr; }

std::vector<std::string> extra_compiler_flags(const std::string& id) {
    if (find_variant(id) == nullptr) return {};
    return {kFpContractOff};
}

void require_compiler_platform(const std::string& id, const std::string& platform) {
    if (is_variant_compiler(id) && platform != "linux-arm64") {
        throw DcDeterminismError("--compiler " + id + " is available on linux-arm64 only");
    }
}

std::string local_compiler_text(bool with_wasm) {
#if defined(__linux__) && defined(__aarch64__)
    std::string text = "gcc, clang, gcc-nofma, clang-nofma";
#else
    std::string text = "msvc, gcc, clang";
#endif
    return with_wasm ? text + ", wasm" : text;
}

}  // namespace dc_determinism
