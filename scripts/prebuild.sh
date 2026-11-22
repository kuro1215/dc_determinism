#!/usr/bin/env bash
# Build the Linux binary of the C++ dc_determinism tool for prebuilt/dc_determinism/linux-<arch>/dc_determinism, where <arch> is the
# architecture of the host (amd64 on x86-64, arm64 on aarch64 and on an Apple-silicon Mac), and record it in the
# `dc_determinism` section of prebuilt/manifest.json (or, with --check, compare it with what is installed). Nothing is emulated:
# the Docker image must have the architecture of the host.
#
# Usage (on a Linux or macOS host with Docker; the default mode):
#   scripts/prebuild.sh [--image NAME] [--test] [--check] [--no-clean]
# Usage (inside the image; the host mode runs it, scripts/prebuild.ps1 runs it for linux-amd64):
#   scripts/prebuild.sh --in-image [--image NAME --image-id ID] [--merge] [--test] [--check] [--no-clean] [--out-dir DIR]
#
# Host mode: starts `docker run --platform linux/<arch>` on NAME (default dc_determinism-cpu:local; an image of another
# architecture is refused) with this checkout mounted at /work, as the calling user, and runs this script in it with
# `--in-image --merge`. The `dc_determinism` section is written (or checked) by the binary that was just built: the last step of
# --in-image --merge is `dc_determinism prebuild --tool-fragment <fragment> [--check]`. On a Linux or macOS host the section is
# only updated while src/dc_determinism and its dependencies equal the recorded ones; a changed source needs the Windows host
# (scripts/prebuild.ps1), which rebuilds every platform.
#
# --in-image: runs scripts/build.sh (g++, Release, statically linked) into a fresh build directory (default
# build/dc_determinism/pb/linux-<arch>, ignored by Git), strips a copy of the binary as <out-dir>/dc_determinism.stripped, checks
# its ELF machine type and --version, and writes the fragment <out-dir>/dc_determinism-linux-<arch>.json (platform, path, sha256,
# compiler, flags, link_flags, image, image_id, toolchain, built). Without --merge nothing else happens (the PowerShell
# script merges the fragments of its platforms); with --merge the binary then installs itself and writes the manifest (--check:
# nothing is installed, the report lines and the exit code 0/1/2 of `prebuild --check` follow). --image and --image-id name the
# image the build runs in (they are recorded); the host mode passes them.
#   docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD:/work" -w /work dc_determinism-cpu:local \
#       scripts/prebuild.sh --in-image --image dc_determinism-cpu:local --image-id <id> --merge
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The architecture of this host: amd64 or arm64 (`sysctl hw.optional.arm64` is 1 on Apple silicon even in a Rosetta shell).
host_arch() {
    local machine system
    machine="$(uname -m)"
    system="$(uname -s)"
    if [ "$system" = Darwin ] && [ "$(sysctl -n hw.optional.arm64 2>/dev/null || true)" = 1 ]; then
        echo arm64
        return
    fi
    case "$machine" in
        x86_64 | amd64) echo amd64 ;;
        aarch64 | arm64) echo arm64 ;;
        *) echo "$machine" ;;
    esac
}

arch="$(host_arch)"
case "$arch" in
    amd64) elf_machine="Advanced Micro Devices X86-64" ;;
    arm64) elf_machine="AArch64" ;;
    *) echo "prebuild.sh: unsupported host architecture: $arch (x86_64 and aarch64 are supported)" >&2; exit 2 ;;
esac
platform="linux-$arch"
out_dir="$root/build/dc_determinism/pb/$platform"
in_image=0
merge=0
run_tests=0
clean=1
check=0
image="${DC_DETERMINISM_IMAGE:-dc_determinism-cpu:local}"
image_id=""
while [ $# -gt 0 ]; do
    case "$1" in
        --in-image) in_image=1; shift ;;
        --merge) merge=1; shift ;;
        --image) image="${2:?--image needs a value}"; shift 2 ;;
        --image-id) image_id="${2:?--image-id needs a value}"; shift 2 ;;
        --out-dir) out_dir="${2:?--out-dir needs a value}"; shift 2 ;;
        --test) run_tests=1; shift ;;
        --no-clean) clean=0; shift ;;
        --check) check=1; shift ;;
        -h|--help) sed -n '2,28p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
if [ "$merge" = 1 ] && [ "$in_image" = 0 ]; then echo "prebuild.sh: --merge belongs to --in-image" >&2; exit 2; fi
if [ "$check" = 1 ] && [ "$in_image" = 1 ] && [ "$merge" = 0 ]; then
    echo "prebuild.sh: --check with --in-image needs --merge (the check is made by the freshly built binary)" >&2
    exit 2
fi

if [ "$in_image" = 1 ]; then
    case "$out_dir" in
        "$root"/*) built_rel="${out_dir#"$root"/}/dc_determinism.stripped" ;;
        *) echo "prebuild.sh: --out-dir must be below the repository ($root)" >&2; exit 2 ;;
    esac
    if [ "$merge" = 1 ] && { [ -z "$image" ] || [ -z "$image_id" ]; }; then
        echo "prebuild.sh: --merge needs --image and --image-id (the host mode passes them)" >&2
        exit 2
    fi
fi

# ---------------------------------------------------------------- host mode: the Docker run
if [ "$in_image" = 0 ]; then
    command -v docker >/dev/null 2>&1 || { echo "prebuild.sh: docker was not found on PATH (inside a container pass --in-image)" >&2; exit 2; }
    if ! docker image inspect "$image" >/dev/null 2>&1; then
        echo "prebuild.sh: Docker image '$image' not found; build it with ./dc_determinism.sh image build (--tag $image)" >&2
        exit 2
    fi
    image_arch="$(docker image inspect --format '{{.Architecture}}' "$image")"
    if [ "$image_arch" != "$arch" ]; then
        echo "prebuild.sh: Docker image '$image' is $image_arch, but this host builds $platform and needs an $arch image (nothing is emulated); build one with ./dc_determinism.sh image build" >&2
        exit 2
    fi
    image_id="$(docker image inspect --format '{{.Id}}' "$image")"
    docker_args=(run --rm --platform "linux/$arch" -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$root:/work" -w /work)
    # The tests read the corpus; Docker cannot follow a symbolic link inside a bind mount.
    if [ -L "$root/logs" ]; then
        docker_args+=(-v "$(cd -P -- "$root/logs" && pwd -P):/work/logs:ro")
    fi
    inner=(bash scripts/prebuild.sh --in-image --merge --image "$image" --image-id "$image_id")
    if [ "$run_tests" = 1 ]; then inner+=(--test); fi
    if [ "$check" = 1 ]; then inner+=(--check); fi
    if [ "$clean" = 0 ]; then inner+=(--no-clean); fi
    exec docker "${docker_args[@]}" "$image" "${inner[@]}"
fi

# ---------------------------------------------------------------- in the image
build_args=(--build-dir "$out_dir")
if [ "$clean" = 1 ]; then build_args+=(--clean); fi
if [ "$run_tests" = 1 ]; then build_args+=(--test); fi
bash "$root/scripts/build.sh" "${build_args[@]}"

built="$out_dir/dc_determinism"
rel_target="prebuilt/dc_determinism/$platform/dc_determinism"
# Strip and check a copy in the build directory; prebuilt/ receives the binary only through the fragment, after the checks.
stripped="$out_dir/dc_determinism.stripped"
cp "$built" "$stripped"
strip -s "$stripped"
chmod 755 "$stripped"
# An x86-64 PC runs x86-64 ELF files natively even inside an arm64 container: the machine type must be the host's.
actual_machine="$(readelf -h "$stripped" | sed -n 's/^ *Machine: *//p')"
if [ "$actual_machine" != "$elf_machine" ]; then
    echo "prebuild.sh: $stripped is '$actual_machine', expected '$elf_machine'" >&2
    exit 1
fi
"$stripped" --version >/dev/null
sha="$(sha256sum "$stripped" | cut -d' ' -f1)"
# Compile and link flags of the real build (ninja -t commands), without paths.
commands="$(ninja -C "$out_dir" -t commands dc_determinism)"
compile_line="$(printf '%s\n' "$commands" | grep -F 'main.cpp.o' | grep -F -- ' -c ' | head -n 1)"
link_line="$(printf '%s\n' "$commands" | grep -F -- '-o dc_determinism' | tail -n 1)"
compile_flags=()
skip=0
first=1
for token in $compile_line; do
    if [ "$first" = 1 ]; then first=0; continue; fi
    if [ "$skip" = 1 ]; then skip=0; continue; fi
    case "$token" in
        -o|-MT|-MF|-isystem|-c) skip=1 ;;
        -MD|-MQ) ;;
        -I*|-D*) case "$token" in -D*) compile_flags+=("$token") ;; esac ;;
        -*) compile_flags+=("$token") ;;
    esac
done
link_flags=()
skip=0
first=1
for token in $link_line; do
    if [ "$first" = 1 ]; then first=0; continue; fi
    if [ "$skip" = 1 ]; then skip=0; continue; fi
    case "$token" in
        -o) skip=1 ;;
        -l*|-static|-pthread|-s) link_flags+=("$token") ;;
    esac
done
link_flags+=("strip -s")

compiler_id="$(g++ --version | head -n 1)"
json_list() {
    local first=1 item
    printf '['
    for item in "$@"; do
        if [ "$first" = 0 ]; then printf ', '; fi
        first=0
        printf '"%s"' "$item"
    done
    printf ']'
}
# `built` is relative to the repository root: the tool resolves it there, wherever the repository is mounted.
fragment="$out_dir/dc_determinism-$platform.json"
{
    printf '{\n'
    printf '  "platform": "%s",\n' "$platform"
    printf '  "path": "%s",\n' "$rel_target"
    printf '  "sha256": "%s",\n' "$sha"
    printf '  "compiler": "%s",\n' "$compiler_id"
    printf '  "flags": %s,\n' "$(json_list "${compile_flags[@]}")"
    printf '  "link_flags": %s,\n' "$(json_list "${link_flags[@]}")"
    printf '  "image": "%s",\n' "$image"
    printf '  "image_id": "%s",\n' "$image_id"
    printf '  "toolchain": {"image": "%s", "image_id": "%s"},\n' "$image" "$image_id"
    printf '  "built": "%s"\n' "$built_rel"
    printf '}\n'
} > "$fragment"

echo "binary:   $stripped"
echo "sha256:   $sha"
echo "compiler: $compiler_id"
echo "fragment: $fragment"
if [ "$merge" = 1 ]; then
    tool_args=(prebuild --tool-fragment "$fragment")
    if [ "$check" = 1 ]; then tool_args+=(--check); fi
    DC_DETERMINISM_REPO="$root" exec "$stripped" "${tool_args[@]}"
fi
