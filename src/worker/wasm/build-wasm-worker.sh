#!/usr/bin/env bash
# Builds the WebAssembly dc_worker (dc_worker.js + dc_worker.wasm) from src/worker/worker.cpp and the
# vendored DigitalCurling simulator. Runs inside the emscripten/emsdk image (no network):
#
#   docker run --rm --entrypoint bash -v <repo>:/repo:ro -v <out dir>:/out emscripten/emsdk:6.0.9 \
#       /repo/src/worker/wasm/build-wasm-worker.sh
#
# `dc_determinism prebuild` (src/dc_determinism/prebuild) is the driver (it verifies the vendored trees first and installs the
# result). Outputs in /out: dc_worker.js, dc_worker.wasm, build-info.json.
#
# One statically linked module: the plugin loader and both simulator plugins (fcv1, fcv1plus) are built as
# static libraries and linked with the unchanged worker. Two adaptations keep the vendored sources
# unmodified:
#  * simulator_plugin_export.hpp switches to the embind bridge under __EMSCRIPTEN__, which would not
#    provide the C plugin API; a generated copy with only its native branch is found first (-iquote).
#  * dlopen/dlsym/dlerror of worker.cpp resolve to the statically linked dc_loader_* functions
#    (dcw_wasm_shim.h, forced include, dcw_wasm_shim.cpp).
set -euo pipefail

repo=/repo
out=/out
work=$out/work
build=$work/cmake
override=$work/override
box2d=$work/box2d-patched
shim_dir=$repo/src/worker/wasm
worker_cpp=$repo/src/worker/worker.cpp
deps=$repo/src/vendor
simulator=$repo/src/vendor/digitalcurling

rm -rf "$work" "$out/dc_worker.js" "$out/dc_worker.wasm" "$out/build-info.json"
mkdir -p "$work" "$override/digitalcurling/plugins"

em++ --version > "$work/em-version.txt"
node --version > "$work/node-version.txt"
cmake --version | head -n 1 > "$work/cmake-version.txt"
head -n 1 "$work/em-version.txt"

lf_sha256() { tr -d '\r' < "$1" | sha256sum | cut -c1-64; }

# Box2D: the same CMake-minimum compatibility patch as the local builds (on a copy).
cp -R "$deps/box2d" "$box2d"
chmod -R u+w "$box2d"
box2d_cmake=$box2d/CMakeLists.txt
cmake_minimum='cmake_minimum_required[[:blank:]]*\(VERSION[[:blank:]]+3\.[0-9]+\)'
# Universal newlines: CRLF and lone CR become LF; the patched file is written with LF.
sed -e 's/\r$//' -e 's/\r/\n/g' "$box2d_cmake" > "$work/box2d-cmake.txt"
[ "$(grep -o -E "$cmake_minimum" "$work/box2d-cmake.txt" | wc -l)" -eq 1 ] ||
    { echo "the Box2D CMake compatibility patch did not match exactly once" >&2; exit 1; }
sed -E "s/$cmake_minimum/cmake_minimum_required(VERSION 3.15)/" "$work/box2d-cmake.txt" > "$box2d_cmake"

# The override header: the vendored one with only its native (C plugin API) branch.
export_header=$simulator/src/plugin-api/include/digitalcurling/plugins/simulator_plugin_export.hpp
override_header=$override/digitalcurling/plugins/simulator_plugin_export.hpp
sed -e 's/\r$//' -e 's/\r/\n/g' "$export_header" > "$work/simulator_plugin_export.hpp"
LC_ALL=C awk '
function strip(s) { sub(/^[[:space:]]+/, "", s); sub(/[[:space:]]+$/, "", s); return s }
function fail(message) { print message > "/dev/stderr"; exit 1 }
function only(text,    index_, found, count) {
    count = 0
    for (index_ = 1; index_ <= total; index_++) {
        if (strip(lines[index_]) == text) { count++; found = index_ }
    }
    if (count != 1) {
        fail("simulator_plugin_export.hpp: expected exactly one \047" text "\047 line, found " count)
    }
    return found
}
{ lines[NR] = $0 }
END {
    total = NR
    start = only("#ifndef __EMSCRIPTEN__")
    middle = only("#else")
    finish = only("#endif // __EMSCRIPTEN__")
    ok = (start < middle && middle < finish)
    for (index_ = finish + 1; ok && index_ <= total; index_++) {
        if (strip(lines[index_]) != "") ok = 0
    }
    if (!ok) fail("simulator_plugin_export.hpp: the #ifndef __EMSCRIPTEN__ / #else / #endif structure changed")
    kept = 0
    for (index_ = 1; index_ < start; index_++) out[++kept] = lines[index_]
    out[++kept] = "// dc_worker WebAssembly build: generated copy of the vendored header with only its native branch, so"
    out[++kept] = "// that the plugins export the C plugin API instead of the embind bridge."
    out[++kept] = "#define DCW_NATIVE_PLUGIN_EXPORT_OVERRIDE 1"
    for (index_ = start + 1; index_ < middle; index_++) out[++kept] = lines[index_]
    while (kept > 0 && out[kept] == "") kept--
    for (index_ = 1; index_ <= kept; index_++) print out[index_]
}' "$work/simulator_plugin_export.hpp" > "$override_header"

cxx_flags="-Wno-deprecated-declarations -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1 -iquote $override"
configure=(
    -S "$simulator" -B "$build" -G "Unix Makefiles"
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 "-DCMAKE_CXX_FLAGS=$cxx_flags"
    -DBOX2D_BUILD_TESTBED=OFF -DBOX2D_BUILD_UNIT_TESTS=OFF -DDIGITALCURLING_BUILD_PLAYERS=OFF
    -DDIGITALCURLING_BUILD_SIMULATORS=ON -DDIGITALCURLING_BUILD_SIMULATOR_FCV1=ON
    -DDIGITALCURLING_BUILD_SIMULATOR_FCV1PLUS=ON -DDIGITALCURLING_BUILD_PLUGIN_LOADER=ON
    -DDIGITALCURLING_PLUGIN_LOADER_SHARED=OFF -DDIGITALCURLING_BUNDLE_PLUGINS=ON
    -DDIGITALCURLING_BUILD_TEST=OFF -DDIGITALCURLING_BUILD_DOCS=OFF
    "-DFETCHCONTENT_SOURCE_DIR_FPM=$deps/fpm"
    "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=$deps/nlohmann-json"
    "-DFETCHCONTENT_SOURCE_DIR_BOX2D=$box2d"
    "-DFETCHCONTENT_SOURCE_DIR_XXHASH=$deps/xxhash"
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON
)
emcmake cmake "${configure[@]}" > "$work/configure.log" 2>&1 || { tail -n 40 "$work/configure.log"; exit 1; }
cmake --build "$build" --target digitalcurling_plugin_loader --parallel "$(nproc)" > "$work/build.log" 2>&1 ||
    { tail -n 40 "$work/build.log"; exit 1; }

archives=(
    "$build/src/plugin-loader/libdigitalcurling_plugin_loader.a"
    "$build/src/simulator/src/fcv1/libdigitalcurling_simulator_fcv1.a"
    "$build/src/simulator/src/fcv1plus/libdigitalcurling_simulator_fcv1plus.a"
    "$build/_deps/box2d-build/bin/libbox2d.a"
)
for archive in "${archives[@]}"; do
    [ -f "$archive" ] || { echo "missing build output: $archive" >&2; exit 1; }
done

worker_sha=$(lf_sha256 "$worker_cpp")
link=(
    -std=c++17 -O3 -DNDEBUG -fwasm-exceptions -sWASM_LEGACY_EXCEPTIONS=1
    -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node -sEXIT_RUNTIME=1
    "-DDC_WORKER_SOURCE_SHA256=\"$worker_sha\"" -DDIGITALCURLING_PLUGIN_LOADER_STATIC_DEFINE
    -I "$simulator/src/plugin-loader/include" -I "$build/src/plugin-loader/include"
    -I "$simulator/src/plugin-api/include" -I "$simulator/src/core/include"
    -include "$shim_dir/dcw_wasm_shim.h"
    "$worker_cpp" "$shim_dir/dcw_wasm_shim.cpp"
    "${archives[0]}"
    -Wl,--whole-archive "${archives[1]}" "${archives[2]}" -Wl,--no-whole-archive
    "${archives[3]}"
    -o "$out/dc_worker.js"
)
em++ "${link[@]}" > "$work/link.log" 2>&1 || { grep -v INFO "$work/link.log" | tail -n 30; exit 1; }
[ -f "$out/dc_worker.wasm" ] || { echo "the link did not produce dc_worker.wasm" >&2; exit 1; }

# The module must not carry the embind bridge: that would mean the override header was not used.
if grep -a -q -E '_embind_|_emval_' "$out/dc_worker.js" "$out/dc_worker.wasm"; then
    echo "the module contains embind symbols: the C plugin API override was not applied" >&2
    exit 1
fi

# Container smoke test: version, self-test, simulator step of both engines, three shots per engine
# (twice: the second run must reproduce the first).
js=$out/dc_worker.js
wasm=$out/dc_worker.wasm
input=$shim_dir/smoke-input.bin
node "$js" --version > "$work/smoke-version.txt"
node "$js" --self-test > "$work/smoke-self-test.txt"
for engine in fcv1 fcv1plus; do
    node "$js" --loader "$wasm" --engine "$engine" --seconds-per-frame > "$work/smoke-spf-$engine.txt"
    for round in 1 2; do
        # The last field of a result line is the wall time of the simulation: not part of the check.
        printf '0\n1\n2\n' |
            node "$js" --loader "$wasm" --engine "$engine" --input "$input" --max-steps 60000 |
            awk '{ $NF = ""; sub(/ $/, ""); print }' > "$work/smoke-shots-$engine-$round.txt"
    done
    cmp "$work/smoke-shots-$engine-1.txt" "$work/smoke-shots-$engine-2.txt" ||
        { echo "the smoke shots of $engine are not reproducible" >&2; exit 1; }
done
printf '%s\n' "${configure[@]}" > "$work/configure-args.txt"
printf '%s\n' "${link[@]}" > "$work/link-args.txt"
grep -E '^(CMAKE_BUILD_TYPE|CMAKE_CXX_FLAGS|CMAKE_CXX_FLAGS_RELEASE|CMAKE_CXX_STANDARD|CMAKE_CXX_COMPILER|CMAKE_TOOLCHAIN_FILE|CMAKE_GENERATOR)[:=]' \
    "$build/CMakeCache.txt" | sort > "$work/cmake-cache.txt"

# build-info.json: the same text as json.dump(info, indent=2) plus a newline (ASCII only; any other byte
# in the collected texts is an error).
override_sha=$(lf_sha256 "$override/digitalcurling/plugins/simulator_plugin_export.hpp")
shim_cpp_sha=$(lf_sha256 "$shim_dir/dcw_wasm_shim.cpp")
shim_h_sha=$(lf_sha256 "$shim_dir/dcw_wasm_shim.h")
input_sha=$(sha256sum "$input" | cut -c1-64)
LC_ALL=C awk -v work="$work" -v worker_sha="$worker_sha" -v override_sha="$override_sha" \
    -v shim_cpp_sha="$shim_cpp_sha" -v shim_h_sha="$shim_h_sha" -v input_sha="$input_sha" '
function fail(message) { print message > "/dev/stderr"; exit 1 }
function strip(s) { sub(/^[[:space:]]+/, "", s); sub(/[[:space:]]+$/, "", s); return s }
# The lines of a text file with universal newlines (CRLF and lone CR end a line).
function load(path, arr,    count, line, parts, pieces, k, status) {
    count = 0
    while ((status = (getline line < path)) > 0) {
        sub(/\r$/, "", line)
        pieces = split(line, parts, "\r")
        if (pieces == 0) arr[++count] = ""
        for (k = 1; k <= pieces; k++) arr[++count] = parts[k]
    }
    if (status < 0) fail("cannot read " path)
    close(path)
    return count
}
function text(path,    count, arr, k, result) {
    count = load(path, arr)
    result = ""
    for (k = 1; k <= count; k++) result = result (k > 1 ? "\n" : "") arr[k]
    return strip(result)
}
function replace_all(s, from, to,    result, position) {
    result = ""
    while ((position = index(s, from)) > 0) {
        result = result substr(s, 1, position - 1) to
        s = substr(s, position + length(from))
    }
    return result s
}
function normalized(s) {
    s = replace_all(s, "/out/work", "<build>")
    s = replace_all(s, "/repo", "<repo>")
    return replace_all(s, "/out", "<out>")
}
function json(s,    k, n, c, result) {
    result = "\""
    n = length(s)
    for (k = 1; k <= n; k++) {
        c = substr(s, k, 1)
        if (c == "\"") result = result "\\\""
        else if (c == "\\") result = result "\\\\"
        else if (c in plain) result = result c
        else if (c in control) result = result control[c]
        else fail("build-info.json: unsupported byte in \047" s "\047")
    }
    return result "\""
}
# A JSON array of the lines of a file, indented below a key at indentation pad.
function array_of(path, pad, normalize,    count, arr, k, result) {
    count = load(path, arr)
    if (count == 0) return "[]"
    result = "[\n"
    for (k = 1; k <= count; k++) {
        result = result pad "  " json(normalize ? normalized(arr[k]) : arr[k]) (k < count ? ",\n" : "\n")
    }
    return result pad "]"
}
BEGIN {
    for (k = 32; k <= 126; k++) plain[sprintf("%c", k)] = 1
    for (k = 1; k < 32; k++) control[sprintf("%c", k)] = sprintf("\\u%04x", k)
    control[sprintf("%c", 127)] = "\\u007f"
    control["\n"] = "\\n"; control["\r"] = "\\r"; control["\t"] = "\\t"
    control[sprintf("%c", 8)] = "\\b"; control[sprintf("%c", 12)] = "\\f"
    delete plain["\""]; delete plain["\\"]

    em_text = text(work "/em-version.txt")
    newline = index(em_text, "\n")
    emxx = newline ? substr(em_text, 1, newline - 1) : em_text
    print "{"
    print "  \"schema\": \"dc-determinism-wasm-build-info-v1\","
    print "  \"toolchain\": {"
    print "    \"emxx\": " json(emxx) ","
    print "    \"emxx_version_text\": " json(em_text) ","
    print "    \"node\": " json(text(work "/node-version.txt")) ","
    print "    \"cmake\": " json(text(work "/cmake-version.txt"))
    print "  },"
    print "  \"worker_source_sha256_lf\": " json(worker_sha) ","
    print "  \"override_sha256\": " json(override_sha) ","
    print "  \"shim_sha256\": {"
    print "    \"dcw_wasm_shim.cpp\": " json(shim_cpp_sha) ","
    print "    \"dcw_wasm_shim.h\": " json(shim_h_sha)
    print "  },"
    print "  \"smoke_input_sha256\": " json(input_sha) ","
    print "  \"flags\": {"
    print "    \"configure\": " array_of(work "/configure-args.txt", "    ", 1) ","
    print "    \"link\": " array_of(work "/link-args.txt", "    ", 1) ","
    print "    \"cmake_cache\": " array_of(work "/cmake-cache.txt", "    ", 1) ","
    print "    \"fp_contract\": " json("compiler default; the wasm target has no FMA instruction (no -msimd128, no relaxed-simd)")
    print "  },"
    print "  \"smoke\": {"
    print "    \"version\": " json(text(work "/smoke-version.txt")) ","
    print "    \"self_test\": " json(text(work "/smoke-self-test.txt")) ","
    print "    \"seconds_per_frame\": {"
    print "      \"fcv1\": " json(text(work "/smoke-spf-fcv1.txt")) ","
    print "      \"fcv1plus\": " json(text(work "/smoke-spf-fcv1plus.txt"))
    print "    },"
    print "    \"shots\": {"
    print "      \"fcv1\": " array_of(work "/smoke-shots-fcv1-1.txt", "      ", 0) ","
    print "      \"fcv1plus\": " array_of(work "/smoke-shots-fcv1plus-1.txt", "      ", 0)
    print "    }"
    print "  }"
    print "}"
}' > "$out/build-info.json"
echo "built $js ($(stat -c %s "$wasm") bytes of wasm)"
