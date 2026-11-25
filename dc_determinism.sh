#!/usr/bin/env bash
# Thin wrapper: runs the prebuilt Linux binary prebuilt/dc_determinism/linux-<arch>/dc_determinism inside a Linux
# Docker container (e.g. on the Linux server or an Apple Silicon Mac). <arch> is the architecture of the
# selected image (amd64 -> linux-amd64, arm64 -> linux-arm64; any other image architecture exits 2) and
# the container runs with `--platform linux/<arch>`. Every command, the pybind ones (`compare pybind`,
# `capture simulator --engine pybind-*`) included, runs the C++ dc_determinism in that image; the pybind commands start the
# Python worker src/worker/pybind/pybind_worker.py there and need an amd64 image (the official pybind extensions are
# x86-64 only).
#
# Usage: ./dc_determinism.sh [--image NAME] <command> [options]
#   Same commands, options, image selection and path translation as dc_determinism.ps1:
#   --image NAME, else $DC_DETERMINISM_IMAGE, else dc_determinism-cpu:local (pybind work included; there is no other image
#   and no fallback: a missing dc_determinism-cpu:local is an error that names `image build`). On an arm64 host
#   (Apple Silicon: `sysctl -n hw.optional.arm64` is 1 even in a Rosetta shell) the wrapper refuses every
#   image of another architecture unless --image / $DC_DETERMINISM_IMAGE names it explicitly (the same
#   refusal applies on an x86-64 host to a non-explicit arm64 image). The
#   repository is mounted at /w/dc_determinism and the current directory (if outside it) at /data;
#   the container runs as the calling user so outputs are owned by you.
#   `image build [--tag NAME] [--platform linux/amd64|linux/arm64]` builds the Docker image of this repository
#   from the Dockerfile on stdin, so no build context is sent to the daemon (-> dc_determinism-cpu:local, or
#   dc_determinism-cpu:local-arm64 for --platform linux/arm64). It needs no image, so --image / $DC_DETERMINISM_IMAGE do not
#   apply. The Docker build downloads packages (apt); nothing else does.
#   `prebuild` is the exception: it builds for the host it runs on and drives the Docker CLI, which a container cannot do
#   (except through the in-container executor below), so on a Linux host it runs the host binary
#   prebuilt/dc_determinism/linux-<arch of the host>/dc_determinism directly (amd64 or arm64; no path is translated); on
#   Windows use dc_determinism.ps1 / dc_determinism-native.ps1. A `--image NAME` given before `prebuild` is forwarded as
#   `prebuild --image NAME` ($DC_DETERMINISM_IMAGE is not forwarded; giving --image on both sides exits 2). On an
#   Apple-silicon Mac (no Linux host binary runs there) it starts the arm64 container as root:
#   `docker run --rm -i --platform linux/arm64 -v <repo>:/w/dc_determinism -e DC_DETERMINISM_REPO=/w/dc_determinism
#   --entrypoint /w/dc_determinism/prebuilt/dc_determinism/linux-arm64/dc_determinism <image> prebuild --image <image>
#   --image-id <id> --build-user <uid:gid> ...`, where <image> is --image, else $DC_DETERMINISM_IMAGE, else
#   dc_determinism-cpu:local (an image that is not arm64 is refused); inside the container the binary runs each build script
#   itself instead of `docker run` (it builds linux-arm64 only). Build the host binary first with
#   scripts/prebuild.sh.
#
# Runs with the bash 3.2 of macOS: no mapfile, no ${var,,}, no associative arrays, no GNU realpath.
set -euo pipefail
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
caller="$(pwd -P)"
repo_c=/w/dc_determinism
caller_c=/data
path_options=" --logs-dir --prepared --output --shots --expect --trajectory-shots --official-py27 --official-py36 --record-py27 --record-py36 "

# The architecture of this host: amd64, arm64 or the raw `uname -m`.
host_arch() {
    local machine system
    machine="$(uname -m)"
    system="$(uname -s)"
    if [[ "$system" == Darwin && "$(sysctl -n hw.optional.arm64 2>/dev/null || true)" == 1 ]]; then
        echo arm64
        return
    fi
    case "$machine" in
        x86_64 | amd64) echo amd64 ;;
        aarch64 | arm64) echo arm64 ;;
        *) echo "$machine" ;;
    esac
}

# `realpath -ms` without GNU realpath: an absolute path with `.`, `..` and repeated slashes resolved
# lexically (symbolic links are not followed, the path need not exist).
normalize_path() {
    local path="$1" part result="" old_ifs="$IFS"
    set -f
    IFS=/
    for part in $path; do
        case "$part" in
            "" | .) ;;
            ..) result="${result%/*}" ;;
            *) result="$result/$part" ;;
        esac
    done
    IFS="$old_ifs"
    set +f
    printf '%s\n' "${result:-/}"
}

image="${DC_DETERMINISM_IMAGE:-}"
explicit=0
if [[ -n "$image" ]]; then explicit=1; fi
cli_image=""
if [[ $# -ge 2 && "$1" == --image ]]; then image="$2"; cli_image="$2"; explicit=1; shift 2; fi
if [[ $# -eq 0 ]]; then
    echo "usage: dc_determinism.sh [--image NAME] <command> [options]  (see dc_determinism --help)" >&2
    exit 2
fi
if [[ "$1" == prebuild ]]; then
    system="$(uname -s)"
    arch="$(host_arch)"
    if [[ "$system" != Linux && "$system" != Darwin ]]; then
        echo "dc_determinism.sh: prebuild needs a Linux host or an Apple-silicon Mac (use dc_determinism.ps1 or dc_determinism-native.ps1 on Windows)" >&2
        exit 2
    fi
    if [[ "$arch" != amd64 && "$arch" != arm64 ]]; then
        echo "dc_determinism.sh: prebuild supports x86-64 and aarch64 hosts only (this host is $arch)" >&2
        exit 2
    fi
    if [[ "$system" == Linux ]]; then
        cpp_binary="prebuilt/dc_determinism/linux-$arch/dc_determinism"
        [[ -f "$repo/$cpp_binary" ]] || { echo "dc_determinism.sh: $cpp_binary not found (build it with scripts/prebuild.sh)" >&2; exit 2; }
        if [[ -n "$cli_image" ]]; then
            for a in "$@"; do
                if [[ "$a" == --image || "$a" == --image=* ]]; then
                    echo "dc_determinism.sh: --image was given both before prebuild and after it; give it once" >&2
                    exit 2
                fi
            done
            set -- "$1" --image "$cli_image" "${@:2}"
        fi
        DC_DETERMINISM_REPO="${DC_DETERMINISM_REPO:-$repo}" exec "$repo/$cpp_binary" "$@"
    fi
    # macOS: no Linux binary runs on the host, so the arm64 container runs it (the in-container executor of `prebuild`).
    if [[ "$arch" != arm64 ]]; then
        echo "dc_determinism.sh: prebuild on a Mac needs Apple silicon (it builds linux-arm64 in an arm64 container)" >&2
        exit 2
    fi
    cpp_binary=prebuilt/dc_determinism/linux-arm64/dc_determinism
    [[ -f "$repo/$cpp_binary" ]] || { echo "dc_determinism.sh: $cpp_binary not found (build it with scripts/prebuild.sh)" >&2; exit 2; }
    command -v docker >/dev/null 2>&1 || { echo "dc_determinism.sh: docker was not found on PATH" >&2; exit 2; }
    # The image is --image (before or after prebuild, once), else $DC_DETERMINISM_IMAGE, else dc_determinism-cpu:local;
    # the binary gets it as `--image` (the container does not know its own name).
    prebuild_args=()
    after_image=""
    shift
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --image)
                [[ $# -ge 2 ]] || { echo "dc_determinism.sh: --image needs a value" >&2; exit 2; }
                after_image="$2"; shift 2 ;;
            --image=*) after_image="${1#--image=}"; shift ;;
            *) prebuild_args+=("$1"); shift ;;
        esac
    done
    if [[ -n "$cli_image" && -n "$after_image" ]]; then
        echo "dc_determinism.sh: --image was given both before prebuild and after it; give it once" >&2
        exit 2
    fi
    if [[ -n "$after_image" ]]; then image="$after_image"; fi
    [[ -n "$image" ]] || image=dc_determinism-cpu:local
    docker image inspect "$image" >/dev/null 2>&1 || {
        echo "dc_determinism.sh: Docker image '$image' not found; build it with dc_determinism.sh image build (dc_determinism-cpu:local)" >&2
        exit 2
    }
    image_arch="$(docker image inspect --format '{{.Architecture}}' "$image")"
    if [[ "$image_arch" != arm64 ]]; then
        echo "dc_determinism.sh: prebuild on a Mac builds linux-arm64 and needs an arm64 image, but '$image' is $image_arch (nothing is emulated); build one with dc_determinism.sh image build" >&2
        exit 2
    fi
    image_id="$(docker image inspect --format '{{.Id}}' "$image")"
    docker_args=(run --rm -i --platform linux/arm64 -v "$repo:$repo_c")
    if [[ -L "$repo/logs" ]]; then
        docker_args+=(-v "$(cd -P -- "$repo/logs" && pwd -P):$repo_c/logs:ro")
    fi
    docker_args+=(-e "DC_DETERMINISM_REPO=$repo_c" --entrypoint "$repo_c/$cpp_binary" "$image"
                  prebuild --image "$image" --image-id "$image_id" --build-user "$(id -u):$(id -g)")
    exec docker "${docker_args[@]}" ${prebuild_args[@]+"${prebuild_args[@]}"}
fi
command -v docker >/dev/null 2>&1 || { echo "dc_determinism.sh: docker was not found on PATH" >&2; exit 2; }

present() { docker image inspect "$1" >/dev/null 2>&1; }

# `image build [--tag NAME] [--platform linux/amd64|linux/arm64]`: builds the Docker image of this repository from
# the Dockerfile on stdin (no build context). It needs no image, so --image / $DC_DETERMINISM_IMAGE do not apply (the image
# selection below is not reached).
if [[ "$1" == image ]]; then
    image_usage="usage: dc_determinism.sh image build [--tag NAME] [--platform linux/amd64|linux/arm64]"
    if [[ -n "$cli_image" ]]; then
        echo 'dc_determinism.sh: --image does not apply to `image build` (use --tag NAME)' >&2
        exit 2
    fi
    if [[ $# -lt 2 || "$2" != build ]]; then
        echo "$image_usage" >&2
        exit 2
    fi
    shift 2
    tag="" build_platform=""
    while [[ $# -gt 0 ]]; do
        option="$1"
        case "$option" in
            --tag=* | --platform=*) value="${option#*=}"; option="${option%%=*}"; shift ;;
            --tag | --platform)
                if [[ $# -lt 2 ]]; then echo "dc_determinism.sh: $option needs a value" >&2; exit 2; fi
                value="$2"
                shift 2
                ;;
            *) echo "dc_determinism.sh: unknown argument: $1" >&2; echo "$image_usage" >&2; exit 2 ;;
        esac
        if [[ -z "$value" ]]; then echo 'dc_determinism.sh: an option of `image build` has an empty value' >&2; exit 2; fi
        case "$option" in
            --tag) tag="$value" ;;
            --platform) build_platform="$value" ;;
        esac
    done
    if [[ -n "$build_platform" && "$build_platform" != linux/amd64 && "$build_platform" != linux/arm64 ]]; then
        echo "dc_determinism.sh: --platform must be linux/amd64 or linux/arm64, not $build_platform" >&2
        exit 2
    fi
    [[ -f "$repo/Dockerfile" ]] || { echo "dc_determinism.sh: Dockerfile not found in $repo" >&2; exit 2; }
    platform_args=()
    if [[ -n "$build_platform" ]]; then platform_args=(--platform "$build_platform"); fi
    # dc_determinism-cpu:local-arm64 is the default tag of an image built for linux/arm64.
    if [[ -z "$tag" ]]; then
        if [[ "$build_platform" == linux/arm64 ]]; then tag=dc_determinism-cpu:local-arm64; else tag=dc_determinism-cpu:local; fi
    fi
    docker build -t "$tag" ${platform_args[@]+"${platform_args[@]}"} - <"$repo/Dockerfile" ||
        { status=$?; echo "dc_determinism.sh: docker build failed (exit $status)" >&2; exit "$status"; }
    echo "image:    $tag"
    echo "image id: $(docker image inspect --format '{{.Id}}' "$tag")"
    exit 0
fi

needs_pybind=0
args=("$@")
if [[ "${args[0]}" == compare && "${args[1]:-}" == pybind ]]; then needs_pybind=1; fi
for ((i = 0; i < ${#args[@]}; i++)); do
    a="${args[$i]}"
    if [[ "${args[0]}" == capture && "${args[1]:-}" == simulator ]]; then
        if [[ "$a" == --engine=pybind-* ]]; then needs_pybind=1; fi
        if [[ "$a" == --engine && "${args[$((i + 1))]:-}" == pybind-* ]]; then needs_pybind=1; fi
    fi
done
host="$(host_arch)"
if [[ -z "$image" ]]; then
    image=dc_determinism-cpu:local
fi
present "$image" || {
    echo "dc_determinism.sh: Docker image '$image' not found; build it with dc_determinism.sh image build (dc_determinism-cpu:local)" >&2
    exit 2
}

# The image decides the binary: its architecture is the Docker platform and the prebuilt directory.
image_arch="$(docker image inspect -f '{{.Architecture}}' "$image")"
case "$image_arch" in
    amd64) platform=linux-amd64 ;;
    arm64) platform=linux-arm64 ;;
    *)
        echo "dc_determinism.sh: Docker image '$image' has the architecture '$image_arch'; only amd64 and arm64 images are supported" >&2
        exit 2
        ;;
esac
if (( needs_pybind )) && [[ "$image_arch" != amd64 ]]; then
    echo "dc_determinism.sh: the pybind commands need an amd64 image (the official pybind extensions are x86-64 only), but '$image' is $image_arch" >&2
    exit 2
fi
if (( ! explicit )) && [[ "$host" != "$image_arch" ]] && [[ "$host" == amd64 || "$host" == arm64 ]]; then
    echo "dc_determinism.sh: Docker image '$image' is $image_arch but this host is $host; pass --image $image (or set DC_DETERMINISM_IMAGE) to run it anyway (under emulation)" >&2
    exit 2
fi

convert() {
    local value="$1" full
    if [[ "$value" == /* ]]; then full="$value"; else full="$caller/$value"; fi
    full="$(normalize_path "$full")"
    if [[ "$full" == "$repo" || "$full" == "$repo"/* ]]; then echo "$repo_c${full#"$repo"}"; return; fi
    if [[ "$full" == "$caller" || "$full" == "$caller"/* ]]; then echo "$caller_c${full#"$caller"}"; return; fi
    echo "dc_determinism.sh: path is outside the repository and the current directory: $value" >&2
    return 1
}

positional=0
if [[ "${args[0]}" == compare && ( "${args[1]:-}" == simulator || "${args[1]:-}" == player ) ]]; then positional=1; fi
if [[ "${args[0]}" == render && "${args[1]:-}" == trajectory ]]; then positional=1; fi
converted=()
for ((i = 0; i < ${#args[@]}; i++)); do
    a="${args[$i]}"
    if [[ "$a" == --*=* && "$path_options" == *" ${a%%=*} "* ]]; then
        converted+=("${a%%=*}=$(convert "${a#*=}")") || exit 2; continue
    fi
    if [[ "$path_options" == *" $a "* ]]; then
        converted+=("$a")
        if (( i + 1 < ${#args[@]} )); then i=$((i + 1)); converted+=("$(convert "${args[$i]}")") || exit 2; fi
        continue
    fi
    if (( positional && i >= 2 )) && [[ "$a" != -* ]]; then
        prev="${args[$((i - 1))]}"
        if [[ "$prev" != --workers && "$prev" != --contact-margin ]]; then
            converted+=("$(convert "$a")") || exit 2; continue
        fi
    fi
    converted+=("$a")
done

cpp_binary="prebuilt/dc_determinism/$platform/dc_determinism"
if [[ ! -f "$repo/$cpp_binary" ]]; then
    echo "dc_determinism.sh: $cpp_binary not found (build it with scripts/prebuild.ps1)" >&2
    exit 2
fi
docker_args=(run --rm -i --platform "linux/$image_arch" -u "$(id -u):$(id -g)" -e PYTHONDONTWRITEBYTECODE=1 -e HOME=/tmp
             -v "$repo:$repo_c")
if [[ -L "$repo/logs" ]]; then
    # Docker cannot follow a symbolic link inside a bind mount: mount its target (`cd -P` resolves it).
    docker_args+=(-v "$(cd -P -- "$repo/logs" && pwd -P):$repo_c/logs:ro")
fi
if [[ "$caller" == "$repo" || "$caller" == "$repo"/* ]]; then
    workdir="$repo_c${caller#"$repo"}"
else
    docker_args+=(-v "$caller:$caller_c")
    workdir="$caller_c"
fi
docker_args+=(-w "$workdir" -e "DC_DETERMINISM_REPO=$repo_c" --entrypoint "$repo_c/$cpp_binary" "$image" "${converted[@]}")
exec docker "${docker_args[@]}"
