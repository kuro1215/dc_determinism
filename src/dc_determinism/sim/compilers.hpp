// The native compiler ids and their variants: one table for the spec choices, the loader and player
// resolution, and the local builds.
//
// `gcc-nofma` / `clang-nofma` are the base compilers `gcc` / `clang` with `-ffp-contract=off` added to the
// C and C++ flags. They exist for linux-arm64, where the compilers may contract a*b+c into a fused
// multiply-add by default (aarch64 has it in its base ISA); the x86-64 builds never contract. The ids are
// accepted by the parser everywhere and refused at run time outside linux-arm64.
#pragma once

#include <string>
#include <vector>

namespace dc_determinism {

// Every native id the parser accepts: msvc, gcc, clang, gcc-nofma, clang-nofma.
const std::vector<std::string>& native_compiler_ids();

// The `--compiler` choices of the simulator commands (the native ids and wasm) and of the player commands.
const std::vector<std::string>& simulator_compiler_choices();
const std::vector<std::string>& player_compiler_choices();

bool is_native_compiler(const std::string& id);

// The compiler a variant is made of (`gcc-nofma` -> `gcc`; any other id is returned as it is).
std::string base_compiler(const std::string& id);

// True for `gcc-nofma` and `clang-nofma`.
bool is_variant_compiler(const std::string& id);

// The flags a variant adds to the C and C++ flags of its base compiler (empty for the other ids).
std::vector<std::string> extra_compiler_flags(const std::string& id);

// DcDeterminismError "--compiler <id> is available on linux-arm64 only" for a variant on any other platform.
void require_compiler_platform(const std::string& id, const std::string& platform);

// The ids a local build accepts on this host, as `a, b, c` (the text of the "unknown compiler" errors).
std::string local_compiler_text(bool with_wasm);

}  // namespace dc_determinism
