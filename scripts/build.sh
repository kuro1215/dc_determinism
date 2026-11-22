#!/usr/bin/env bash
# Build (and optionally test) the C++ implementation of dc_determinism on Linux with g++ (or clang++).
#
# Usage: scripts/build.sh [--config Release] [--build-dir DIR] [--cxx g++] [--test] [--clean]
#                               [--timeout-scale N] [--ctest-exclude REGEX] [--ctest-verbose]
#
# Runs cmake configure/build (and ctest with --test) for src/dc_determinism. Output: build/dc_determinism/linux-amd64 on an
# x86-64 host, build/dc_determinism/linux-arm64 on an aarch64 host (ignored by Git); the binary is <build dir>/dc_determinism,
# statically linked. No network access. --timeout-scale N multiplies the CTest timeouts (e.g. 20 on a slow or emulated
# host); --ctest-exclude REGEX passes -E REGEX to ctest; --ctest-verbose passes -V (the output of passing tests too). With --test
# on an aarch64 host the ELF machine of dc_determinism and of every test executable is checked first (an x86-64 PC runs
# x86-64 ELF files natively even inside an arm64 container). Inside the dc_determinism-cpu:local image (cmake, ninja, g++ and Python 3 are installed there):
#   docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/work" -w /work dc_determinism-cpu:local scripts/build.sh --test
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
machine="$(uname -m)"
case "$machine" in
    x86_64 | amd64) platform=linux-amd64; elf_machine="Advanced Micro Devices X86-64" ;;
    aarch64 | arm64) platform=linux-arm64; elf_machine="AArch64" ;;
    *) echo "build.sh: unsupported host architecture: $machine (x86_64 and aarch64 are supported)" >&2; exit 2 ;;
esac
config="Release"
build_dir="$root/build/dc_determinism/$platform"
cxx=""
run_tests=0
clean=0
timeout_scale=""
ctest_exclude=""
ctest_verbose=0
while [ $# -gt 0 ]; do
    case "$1" in
        --config) config="${2:?--config needs a value}"; shift 2 ;;
        --build-dir) build_dir="${2:?--build-dir needs a value}"; shift 2 ;;
        --cxx) cxx="${2:?--cxx needs a value}"; shift 2 ;;
        --test) run_tests=1; shift ;;
        --clean) clean=1; shift ;;
        --timeout-scale) timeout_scale="${2:?--timeout-scale needs a value}"; shift 2 ;;
        --ctest-exclude) ctest_exclude="${2:?--ctest-exclude needs a value}"; shift 2 ;;
        --ctest-verbose) ctest_verbose=1; shift ;;
        -h|--help) sed -n '2,14p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ "$clean" = 1 ] && [ -d "$build_dir" ]; then
    rm -rf "$build_dir"
fi

generator="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then generator="Ninja"; fi
args=(-S "$root/src/dc_determinism" -B "$build_dir" -G "$generator" "-DCMAKE_BUILD_TYPE=$config")
if [ -n "$cxx" ]; then args+=("-DCMAKE_CXX_COMPILER=$cxx"); fi
if [ -n "$timeout_scale" ]; then args+=("-DDC_DETERMINISM_TEST_TIMEOUT_SCALE=$timeout_scale"); fi

cmake "${args[@]}"
cmake --build "$build_dir" --config "$config" -j "$(nproc)"

# The ELF machine of dc_determinism and of every executable of the test tree must be the host's.
check_elf_machine() {
    local file machine_line checked=0
    command -v readelf >/dev/null 2>&1 || { echo "build.sh: readelf was not found (binutils)" >&2; exit 1; }
    while IFS= read -r file; do
        if [ "$(head -c 4 "$file" 2>/dev/null | od -An -c | tr -d ' ')" != '177ELF' ]; then continue; fi
        machine_line="$(readelf -h "$file" | sed -n 's/^ *Machine: *//p')"
        if [ "$machine_line" != "$elf_machine" ]; then
            echo "build.sh: $file is '$machine_line', expected '$elf_machine'" >&2
            exit 1
        fi
        checked=$((checked + 1))
    done < <(find "$build_dir" -type f -perm -u+x \( -name 'dc_determinism*' -o -path '*/tests/*' \) -not -name '*.so*')
    echo "ELF machine '$elf_machine': $checked executables checked"
}

if [ "$run_tests" = 1 ]; then
    if [ "$platform" = linux-arm64 ]; then check_elf_machine; fi
    ctest_args=(--test-dir "$build_dir" --build-config "$config" --output-on-failure)
    if [ -n "$ctest_exclude" ]; then ctest_args+=(-E "$ctest_exclude"); fi
    if [ "$ctest_verbose" = 1 ]; then ctest_args+=(-V); fi
    ctest "${ctest_args[@]}"
fi
echo "binary: $build_dir/dc_determinism"
