#!/usr/bin/env bash
# Checks of dc_determinism.sh without Docker: the path normalizer, the image/architecture selection, the path
# translation, `image build` (Dockerfile on stdin) and `prebuild` (the host binary on a Linux host of either architecture, the
# arm64 container on an Apple-silicon Mac), and the host mode of scripts/prebuild.sh, with fake `docker`, `uname`
# and `sysctl` commands in front of PATH.
#
#   bash scripts/check.sh [path/to/dc_determinism.sh]
#
# Written for bash 3.2 (macOS) as dc_determinism.sh is: run it also in the `bash:3.2` image:
#   docker run --rm -v "$PWD:/w:ro" -w /w bash:3.2 bash scripts/check.sh
set -u

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
script="${1:-$here/../dc_determinism.sh}"
[ -f "$script" ] || { echo "dc_determinism.sh not found: $script" >&2; exit 2; }
failures=0
checks=0

ok() { checks=$((checks + 1)); }
fail() { checks=$((checks + 1)); failures=$((failures + 1)); echo "FAIL: $1" >&2; }
expect_eq() {
    if [ "$2" = "$3" ]; then ok; else fail "$1: expected '$3', got '$2'"; fi
}
expect_contains() {
    case "$2" in *"$3"*) ok ;; *) fail "$1: '$3' not found in: $2" ;; esac
}
expect_not_contains() {
    case "$2" in *"$3"*) fail "$1: unexpected '$3' in: $2" ;; *) ok ;; esac
}

# --- normalize_path --------------------------------------------------------------------------------
eval "$(sed -n '/^normalize_path() {$/,/^}$/p' "$script")"
type normalize_path >/dev/null 2>&1 || { echo "normalize_path not found in $script" >&2; exit 2; }

check_path() {
    expect_eq "normalize_path '$1'" "$(normalize_path "$1")" "$2"
    # The same as GNU `realpath -ms` where that exists.
    if command -v realpath >/dev/null 2>&1 && realpath -ms -- / >/dev/null 2>&1; then
        expect_eq "realpath -ms '$1'" "$(realpath -ms -- "$1")" "$2"
    fi
}
check_path /a/b/c /a/b/c
check_path /a/b/../c /a/c
check_path /a//b/./c/ /a/b/c
check_path /a/b/.. /a
check_path /a/../.. /
check_path /.. /
check_path / /
expect_eq "normalize_path //" "$(normalize_path //)" /
check_path /a/./././b /a/b
check_path "/a b/c/../d e" "/a b/d e"
check_path "/a/*/b" "/a/*/b"
check_path "/a/b?/c" "/a/b?/c"
check_path /a/b/../../../c /c
check_path /a/..b/c /a/..b/c
check_path /a/b.. /a/b..
check_path "/a/..." "/a/..."

# --- the wrapper with fake docker / uname / sysctl -------------------------------------------------------
work="$(cd "$(mktemp -d "${TMPDIR:-/tmp}/dc_determinism-sh-check.XXXXXX")" && pwd -P)"
trap 'rm -rf "$work"' EXIT
repo="$work/repo"
bin="$work/bin"
mkdir -p "$repo/prebuilt/dc_determinism/linux-amd64" "$repo/prebuilt/dc_determinism/linux-arm64" "$bin" "$work/outside"
cp "$script" "$repo/dc_determinism.sh"
: >"$repo/prebuilt/dc_determinism/linux-amd64/dc_determinism"
: >"$repo/prebuilt/dc_determinism/linux-arm64/dc_determinism"

cat >"$bin/docker" <<'EOF'
#!/bin/sh
# Fake docker: FAKE_IMAGES="name=arch ..." lists the images (the value is the architecture, or an image id starting with
# sha256:); `image inspect --format` prints the architecture, or the id (the given one, else sha256:fake-id-of-<name>);
# `run` prints its arguments, one per line; `build` prints its arguments and the first line of its standard input.
if [ "$1" = image ] && [ "$2" = inspect ]; then
    shift 2
    format=""
    if [ "$1" = -f ] || [ "$1" = --format ]; then format="$2"; shift 2; fi
    for entry in $FAKE_IMAGES; do
        if [ "${entry%%=*}" = "$1" ]; then
            value="${entry#*=}"
            case "$format" in
                "") ;;
                *Id*) case "$value" in sha256:*) echo "$value" ;; *) echo "sha256:fake-id-of-$1" ;; esac ;;
                *) echo "$value" ;;
            esac
            exit 0
        fi
    done
    exit 1
fi
if [ "$1" = run ]; then
    shift
    for argument in "$@"; do printf '%s
' "$argument"; done
    exit 0
fi
if [ "$1" = build ]; then
    shift
    echo "BUILD $*"
    IFS= read -r first_line
    echo "STDIN: $first_line"
    exit "${FAKE_BUILD_STATUS:-0}"
fi
exit 99
EOF
cat >"$bin/uname" <<'EOF'
#!/bin/sh
case "$1" in
    -m) echo "$FAKE_UNAME_M" ;;
    -s) echo "$FAKE_UNAME_S" ;;
    *) echo "$FAKE_UNAME_S" ;;
esac
EOF
cat >"$bin/sysctl" <<'EOF'
#!/bin/sh
if [ "$2" = hw.optional.arm64 ] || [ "$1" = hw.optional.arm64 ]; then echo "${FAKE_SYSCTL_ARM64:-0}"; exit 0; fi
exit 1
EOF
chmod +x "$bin/docker" "$bin/uname" "$bin/sysctl"

# run_wrapper HOST_M HOST_S SYSCTL IMAGES [wrapper arguments...]: sets out and status.
run_wrapper() {
    local host_m="$1" host_s="$2" sysctl_arm64="$3" images="$4"
    shift 4
    out="$(cd "$repo" && PATH="$bin:$PATH" FAKE_UNAME_M="$host_m" FAKE_UNAME_S="$host_s" FAKE_SYSCTL_ARM64="$sysctl_arm64" \
        FAKE_IMAGES="$images" DC_DETERMINISM_IMAGE="${DC_DETERMINISM_IMAGE_FOR_TEST:-}" bash "$repo/dc_determinism.sh" "$@" 2>&1)"
    status=$?
}

# amd64 host, amd64 image
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local=amd64" verify
expect_eq "amd64/amd64 status" "$status" 0
expect_contains "amd64/amd64 platform" "$out" "linux/amd64"
expect_contains "amd64/amd64 binary" "$out" "/w/dc_determinism/prebuilt/dc_determinism/linux-amd64/dc_determinism"

# arm64 host (Linux), arm64 image
run_wrapper aarch64 Linux 0 "dc_determinism-cpu:local=arm64" verify
expect_eq "arm64/arm64 status" "$status" 0
expect_contains "arm64/arm64 platform" "$out" "linux/arm64"
expect_contains "arm64/arm64 binary" "$out" "/prebuilt/dc_determinism/linux-arm64/dc_determinism"

# macOS on Apple Silicon: uname -m arm64
run_wrapper arm64 Darwin 1 "dc_determinism-cpu:local=arm64" verify
expect_eq "mac/arm64 status" "$status" 0
expect_contains "mac/arm64 binary" "$out" "/prebuilt/dc_determinism/linux-arm64/dc_determinism"

# macOS in a Rosetta shell: uname -m says x86_64, sysctl says arm64 -> still an arm64 host
run_wrapper x86_64 Darwin 1 "dc_determinism-cpu:local=amd64" verify
expect_eq "rosetta shell with an amd64 image status" "$status" 2
expect_contains "rosetta shell message" "$out" "is amd64 but this host is arm64"

# arm64 host, only an amd64 dc_determinism-cpu:local: refused unless explicit
run_wrapper aarch64 Linux 0 "dc_determinism-cpu:local=amd64" verify
expect_eq "arm64 host/amd64 image status" "$status" 2
expect_contains "arm64 host/amd64 image message" "$out" "pass --image dc_determinism-cpu:local"
run_wrapper aarch64 Linux 0 "dc_determinism-cpu:local=amd64" --image dc_determinism-cpu:local verify
expect_eq "arm64 host/explicit amd64 image status" "$status" 0
expect_contains "arm64 host/explicit amd64 image" "$out" "linux/amd64"
expect_contains "arm64 host/explicit amd64 binary" "$out" "/prebuilt/dc_determinism/linux-amd64/dc_determinism"
DC_DETERMINISM_IMAGE_FOR_TEST=dc_determinism-cpu:local run_wrapper aarch64 Linux 0 "dc_determinism-cpu:local=amd64" verify
expect_eq "arm64 host/DC_DETERMINISM_IMAGE amd64 image status" "$status" 0

# no dc_determinism-cpu:local: there is no fallback to another image on any host, the message names `image build`
run_wrapper aarch64 Linux 0 "other-image:1=amd64" verify
expect_eq "arm64 host/no dc_determinism-cpu:local status" "$status" 2
expect_contains "arm64 host/no dc_determinism-cpu:local message" "$out" "'dc_determinism-cpu:local' not found"
expect_contains "arm64 host/no dc_determinism-cpu:local advice" "$out" "dc_determinism.sh image build"
run_wrapper x86_64 Linux 0 "other-image:1=amd64" verify
expect_eq "amd64 host/no dc_determinism-cpu:local status" "$status" 2
expect_contains "amd64 host/no dc_determinism-cpu:local message" "$out" "'dc_determinism-cpu:local' not found"
expect_not_contains "amd64 host/no fallback" "$out" "linux/amd64"
run_wrapper x86_64 Linux 0 "other-image:1=amd64" compare pybind --prepared p --logs-dir l --compiler gcc --output o.json
expect_eq "amd64 host/pybind without dc_determinism-cpu:local status" "$status" 2
expect_contains "amd64 host/pybind without dc_determinism-cpu:local message" "$out" "'dc_determinism-cpu:local' not found"

# amd64 host, explicit arm64 image: runs the arm64 binary under emulation
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local=amd64 dc_determinism-cpu:local-arm64=arm64" --image dc_determinism-cpu:local-arm64 verify
expect_eq "amd64 host/explicit arm64 image status" "$status" 0
expect_contains "amd64 host/explicit arm64 image platform" "$out" "linux/arm64"
expect_contains "amd64 host/explicit arm64 image binary" "$out" "/prebuilt/dc_determinism/linux-arm64/dc_determinism"
expect_not_contains "amd64 host/explicit arm64 image binary" "$out" "linux-amd64"
# ... but not by accident
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local=arm64" verify
expect_eq "amd64 host/default arm64 image status" "$status" 2

# an image of another architecture
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local=386" verify
expect_eq "386 image status" "$status" 2
expect_contains "386 image message" "$out" "only amd64 and arm64 images are supported"

# pybind runs the C++ dc_determinism in the same image (dc_determinism-cpu:local) like every command and needs an amd64 image
run_wrapper aarch64 Linux 0 "dc_determinism-cpu:local=arm64" compare pybind --prepared p --logs-dir l --compiler gcc --output o.json
expect_eq "pybind on arm64 status" "$status" 2
expect_contains "pybind on arm64 message" "$out" "the pybind commands need an amd64 image"
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local=amd64" compare pybind --prepared p --logs-dir l --compiler gcc --output o.json
expect_eq "pybind on amd64 status" "$status" 0
expect_contains "pybind on amd64 entrypoint" "$out" "/prebuilt/dc_determinism/linux-amd64/dc_determinism"
expect_contains "pybind on amd64 image" "$out" "dc_determinism-cpu:local"
expect_not_contains "pybind on amd64 does not use another image" "$out" "dc_determinism-pybind"
expect_not_contains "pybind on amd64 does not run python" "$out" "python3"
expect_not_contains "pybind on amd64 does not set PYTHONPATH" "$out" "PYTHONPATH"
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local=amd64" capture simulator --engine pybind-py27 --prepared p --shots s --output o.zip
expect_eq "pybind capture status" "$status" 0
expect_contains "pybind capture entrypoint" "$out" "/prebuilt/dc_determinism/linux-amd64/dc_determinism"
expect_contains "pybind capture keeps the command words" "$out" "simulator"
expect_not_contains "pybind capture does not run python" "$out" "python3"
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local=amd64" compare pybind --check-recording --prepared p --logs-dir l --output o.json
expect_eq "pybind check status" "$status" 0
expect_contains "pybind check entrypoint" "$out" "/prebuilt/dc_determinism/linux-amd64/dc_determinism"
expect_contains "pybind check keeps the flag" "$out" "--check-recording"
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local=amd64" compare pybind --prepared p --logs-dir l --compiler gcc --trajectory-shots t.txt --official-py27 o.so --record-py36 r.so --output o.json
expect_contains "pybind --trajectory-shots is translated" "$out" "/w/dc_determinism/t.txt"
expect_contains "pybind --official-py27 is translated" "$out" "/w/dc_determinism/o.so"
expect_contains "pybind --record-py36 is translated" "$out" "/w/dc_determinism/r.so"
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local=amd64 mine:1=arm64" --image mine:1 compare pybind --prepared p --logs-dir l --compiler gcc --output o.json
expect_eq "pybind with an explicit arm64 image status" "$status" 2
# benchmark never runs the Python worker: it runs the prebuilt binary
run_wrapper aarch64 Linux 0 "dc_determinism-cpu:local=arm64" benchmark --prepared p --compiler gcc --output b.json
expect_eq "benchmark on arm64 status" "$status" 0
expect_not_contains "benchmark on arm64 does not run python" "$out" "python3"
expect_contains "benchmark on arm64 runs the prebuilt binary" "$out" "/prebuilt/dc_determinism/linux-arm64/dc_determinism"

# a missing binary names the platform directory
rm "$repo/prebuilt/dc_determinism/linux-arm64/dc_determinism"
run_wrapper aarch64 Linux 0 "dc_determinism-cpu:local=arm64" verify
expect_eq "missing arm64 binary status" "$status" 2
expect_contains "missing arm64 binary message" "$out" "prebuilt/dc_determinism/linux-arm64/dc_determinism not found"
: >"$repo/prebuilt/dc_determinism/linux-arm64/dc_determinism"

# path translation (normalizer in use): inside the repository, in the current directory, outside both
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local=amd64" prepare --output ./x/../prep//small --logs-dir "$repo/logs/"
expect_eq "path translation status" "$status" 0
expect_contains "path translation --output" "$out" "/w/dc_determinism/prep/small"
expect_contains "path translation --logs-dir" "$out" "/w/dc_determinism/logs"
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local=amd64" prepare --output "$work/outside/o" --logs-dir logs
expect_eq "outside path status" "$status" 2
expect_contains "outside path message" "$out" "path is outside the repository and the current directory"

# prebuild builds for the host it runs on and drives the Docker CLI: a Linux host runs its own host binary (no container),
# for either architecture
printf '#!/bin/sh\necho "PREBUILD-BIN amd64 $*"\necho "REPO=$DC_DETERMINISM_REPO"\n' >"$repo/prebuilt/dc_determinism/linux-amd64/dc_determinism"
printf '#!/bin/sh\necho "PREBUILD-BIN arm64 $*"\necho "REPO=$DC_DETERMINISM_REPO"\n' >"$repo/prebuilt/dc_determinism/linux-arm64/dc_determinism"
chmod +x "$repo/prebuilt/dc_determinism/linux-amd64/dc_determinism" "$repo/prebuilt/dc_determinism/linux-arm64/dc_determinism"
run_wrapper x86_64 Linux 0 "" prebuild --worker-only
expect_eq "prebuild on x86 Linux status" "$status" 0
expect_contains "prebuild on x86 Linux runs the amd64 binary" "$out" "PREBUILD-BIN amd64 prebuild --worker-only"
expect_contains "prebuild on x86 Linux repository" "$out" "REPO=$repo"
run_wrapper aarch64 Linux 0 "" prebuild --check
expect_eq "prebuild on arm64 Linux status" "$status" 0
expect_contains "prebuild on arm64 Linux runs the arm64 binary" "$out" "PREBUILD-BIN arm64 prebuild --check"
expect_contains "prebuild on arm64 Linux repository" "$out" "REPO=$repo"
expect_not_contains "prebuild on arm64 Linux starts no container" "$out" "--platform"
run_wrapper armv7l Linux 0 "" prebuild
expect_eq "prebuild on another machine status" "$status" 2
expect_contains "prebuild on another machine message" "$out" "prebuild supports x86-64 and aarch64 hosts only"
mv "$repo/prebuilt/dc_determinism/linux-arm64/dc_determinism" "$work/arm64-binary"
run_wrapper aarch64 Linux 0 "" prebuild
expect_eq "prebuild on arm64 Linux without the binary status" "$status" 2
expect_contains "prebuild on arm64 Linux without the binary message" "$out" "prebuilt/dc_determinism/linux-arm64/dc_determinism not found"
expect_contains "prebuild on arm64 Linux without the binary advice" "$out" "scripts/prebuild.sh"
mv "$work/arm64-binary" "$repo/prebuilt/dc_determinism/linux-arm64/dc_determinism"
# an Apple-silicon Mac has no Linux host binary: the arm64 container runs it (root, the repository at /w/dc_determinism) and the
# in-container executor builds linux-arm64 with the image, its id and the user of the Mac
uid_gid="$(id -u):$(id -g)"
run_wrapper arm64 Darwin 1 "dc_determinism-cpu:local=arm64" prebuild --worker-only --check
expect_eq "prebuild on a Mac status" "$status" 0
joined="$(printf '%s ' $out)"
expect_contains "prebuild on a Mac docker line" "$joined" "--rm -i --platform linux/arm64 -v $repo:/w/dc_determinism -e DC_DETERMINISM_REPO=/w/dc_determinism --entrypoint /w/dc_determinism/prebuilt/dc_determinism/linux-arm64/dc_determinism dc_determinism-cpu:local prebuild --image dc_determinism-cpu:local --image-id sha256:fake-id-of-dc_determinism-cpu:local --build-user $uid_gid --worker-only --check"
if printf '%s\n' "$out" | grep -qx -e '-u'; then fail "prebuild on a Mac runs as the user instead of root"; else ok; fi
expect_not_contains "prebuild on a Mac does not run the host binary" "$out" "PREBUILD-BIN"
run_wrapper x86_64 Darwin 1 "dc_determinism-cpu:local=arm64" prebuild
expect_eq "prebuild in a Rosetta shell status (an arm64 host)" "$status" 0
expect_contains "prebuild in a Rosetta shell platform" "$out" "linux/arm64"
run_wrapper x86_64 Darwin 0 "dc_determinism-cpu:local=arm64" prebuild
expect_eq "prebuild on an Intel Mac status" "$status" 2
expect_contains "prebuild on an Intel Mac message" "$out" "needs Apple silicon"
run_wrapper arm64 Darwin 1 "dc_determinism-cpu:local=amd64" prebuild
expect_eq "prebuild on a Mac with an amd64 image status" "$status" 2
expect_contains "prebuild on a Mac with an amd64 image message" "$out" "needs an arm64 image, but 'dc_determinism-cpu:local' is amd64"
expect_not_contains "prebuild on a Mac with an amd64 image runs nothing" "$out" "--platform"
DC_DETERMINISM_IMAGE_FOR_TEST=dc_determinism-cpu:local run_wrapper arm64 Darwin 1 "dc_determinism-cpu:local=amd64" prebuild
expect_eq "prebuild on a Mac with an explicit amd64 image status" "$status" 2   # explicit does not make it a build image
run_wrapper arm64 Darwin 1 "" prebuild
expect_eq "prebuild on a Mac without the image status" "$status" 2
expect_contains "prebuild on a Mac without the image message" "$out" "'dc_determinism-cpu:local' not found"
# the image: --image before or after prebuild (once), else $DC_DETERMINISM_IMAGE, else dc_determinism-cpu:local
run_wrapper arm64 Darwin 1 "mine:1=arm64" --image mine:1 prebuild
expect_eq "prebuild on a Mac --image before status" "$status" 0
expect_contains "prebuild on a Mac --image before" "$(printf '%s ' $out)" "mine:1 prebuild --image mine:1 --image-id sha256:fake-id-of-mine:1 --build-user $uid_gid"
run_wrapper arm64 Darwin 1 "mine:2=arm64" prebuild --image mine:2 --worker-only
expect_eq "prebuild on a Mac --image after status" "$status" 0
expect_contains "prebuild on a Mac --image after" "$(printf '%s ' $out)" "mine:2 prebuild --image mine:2 --image-id sha256:fake-id-of-mine:2 --build-user $uid_gid --worker-only"
run_wrapper arm64 Darwin 1 "mine:2=arm64" prebuild --image=mine:2
expect_contains "prebuild on a Mac --image= after" "$(printf '%s ' $out)" "prebuild --image mine:2 --image-id"
DC_DETERMINISM_IMAGE_FOR_TEST=env:image run_wrapper arm64 Darwin 1 "env:image=arm64" prebuild
expect_eq "prebuild on a Mac with DC_DETERMINISM_IMAGE status" "$status" 0
expect_contains "prebuild on a Mac with DC_DETERMINISM_IMAGE" "$(printf '%s ' $out)" "env:image prebuild --image env:image --image-id"
run_wrapper arm64 Darwin 1 "mine:1=arm64 mine:2=arm64" --image mine:1 prebuild --image mine:2
expect_eq "prebuild on a Mac --image on both sides status" "$status" 2
expect_contains "prebuild on a Mac --image on both sides message" "$out" "both before prebuild and after it"
rm "$repo/prebuilt/dc_determinism/linux-arm64/dc_determinism"
run_wrapper arm64 Darwin 1 "dc_determinism-cpu:local=arm64" prebuild
expect_eq "prebuild on a Mac without the arm64 binary status" "$status" 2
expect_contains "prebuild on a Mac without the arm64 binary message" "$out" "prebuilt/dc_determinism/linux-arm64/dc_determinism not found (build it with scripts/prebuild.sh)"
printf '#!/bin/sh\necho "PREBUILD-BIN arm64 $*"\necho "REPO=$DC_DETERMINISM_REPO"\n' >"$repo/prebuilt/dc_determinism/linux-arm64/dc_determinism"
chmod +x "$repo/prebuilt/dc_determinism/linux-arm64/dc_determinism"
# a CLI --image before prebuild is forwarded to the command; $DC_DETERMINISM_IMAGE is not; both sides is a usage error
run_wrapper x86_64 Linux 0 "" --image my:image prebuild --worker-only
expect_eq "prebuild --image forwarding status" "$status" 0
expect_contains "prebuild --image forwarding" "$out" "PREBUILD-BIN amd64 prebuild --image my:image --worker-only"
DC_DETERMINISM_IMAGE_FOR_TEST=env:image run_wrapper x86_64 Linux 0 "" prebuild
expect_eq "prebuild with DC_DETERMINISM_IMAGE status" "$status" 0
expect_not_contains "prebuild does not forward DC_DETERMINISM_IMAGE" "$out" "--image"
run_wrapper x86_64 Linux 0 "" --image my:image prebuild --image other:image
expect_eq "prebuild --image on both sides status" "$status" 2
expect_contains "prebuild --image on both sides message" "$out" "both before prebuild and after it"
run_wrapper x86_64 Linux 0 "" --image my:image prebuild --image=other:image
expect_eq "prebuild --image= on both sides status" "$status" 2
expect_not_contains "prebuild --image on both sides does not run" "$out" "PREBUILD-BIN"

# image build: runs `docker build` with the Dockerfile on stdin (no context); no image is needed and no host or
# image architecture rule applies
printf 'FROM scratch\n# marker-line\n' >"$repo/Dockerfile"
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local=sha256:rt" image build
expect_eq "image build status" "$status" 0
expect_contains "image build command" "$out" "BUILD -t dc_determinism-cpu:local -"
expect_not_contains "image build sends no context directory" "$out" "$repo"
expect_contains "image build sends the Dockerfile on stdin" "$out" "STDIN: FROM scratch"
expect_contains "image build tag line" "$out" "image:    dc_determinism-cpu:local"
expect_contains "image build id line" "$out" "image id: sha256:rt"
expect_not_contains "image build without --platform" "$out" "--platform"
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local-arm64=sha256:a64" image build --platform linux/arm64
expect_eq "image build arm64 status" "$status" 0
expect_contains "image build arm64 command" "$out" "BUILD -t dc_determinism-cpu:local-arm64 --platform linux/arm64 -"
expect_contains "image build arm64 tag line" "$out" "image:    dc_determinism-cpu:local-arm64"
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local-arm64=sha256:a64" image build --platform=linux/arm64
expect_contains "image build --platform=" "$out" "BUILD -t dc_determinism-cpu:local-arm64 --platform linux/arm64 -"
run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local=sha256:rt" image build --platform linux/amd64
expect_contains "image build amd64 command" "$out" "BUILD -t dc_determinism-cpu:local --platform linux/amd64 -"
run_wrapper aarch64 Linux 0 "mine:1=sha256:mine" image build --tag mine:1
expect_eq "image build --tag status" "$status" 0
expect_contains "image build --tag command" "$out" "BUILD -t mine:1 -"
run_wrapper arm64 Darwin 1 "dc_determinism-cpu:local=sha256:rt" image build --tag=dc_determinism-cpu:local
expect_eq "image build on a Mac status" "$status" 0
run_wrapper x86_64 Linux 0 "" image build --platform linux/386
expect_eq "image build bad platform status" "$status" 2
run_wrapper x86_64 Linux 0 "" image build --bogus
expect_eq "image build unknown option status" "$status" 2
expect_contains "image build unknown option message" "$out" "unknown argument: --bogus"
run_wrapper x86_64 Linux 0 "" image build --tag
expect_eq "image build missing value status" "$status" 2
expect_contains "image build missing value message" "$out" "--tag needs a value"
run_wrapper x86_64 Linux 0 "" image build --tag=
expect_eq "image build empty value status" "$status" 2
run_wrapper x86_64 Linux 0 "" image build extra
expect_eq "image build with an extra argument status" "$status" 2
expect_contains "image build with an extra argument message" "$out" "unknown argument: extra"
expect_contains "image build with an extra argument usage" "$out" "usage: dc_determinism.sh image build [--tag NAME]"
expect_not_contains "image build with an extra argument does not build" "$out" "BUILD"
run_wrapper x86_64 Linux 0 "" image build --base x:1
expect_eq "image build --base status" "$status" 2
run_wrapper x86_64 Linux 0 "" image
expect_eq "image without build status" "$status" 2
run_wrapper x86_64 Linux 0 "" --image my:image image build
expect_eq "image build with --image status" "$status" 2
expect_contains "image build with --image message" "$out" "--image does not apply"
expect_not_contains "image build with --image does not build" "$out" "BUILD"
FAKE_BUILD_STATUS=3 run_wrapper x86_64 Linux 0 "dc_determinism-cpu:local=amd64" image build
expect_eq "image build failure status" "$status" 3
expect_contains "image build failure message" "$out" "docker build failed (exit 3)"
rm "$repo/Dockerfile"
run_wrapper x86_64 Linux 0 "" image build
expect_eq "image build without a Dockerfile status" "$status" 2
expect_contains "image build without a Dockerfile message" "$out" "Dockerfile not found"

# scripts/prebuild.sh, host mode (a Linux or macOS host with Docker): one `docker run` of its in-image part, in the
# image of the host's architecture, as the calling user; the binary built there writes the manifest (--in-image --merge)
prebuild_script="$here/prebuild.sh"
if [ -f "$prebuild_script" ]; then
    mkdir -p "$repo/scripts"
    cp "$prebuild_script" "$repo/scripts/prebuild.sh"
    # run_prebuild_script HOST_M HOST_S SYSCTL IMAGES [script arguments...]: sets out (one argument per line of the fake docker) and status.
    run_prebuild_script() {
        local host_m="$1" host_s="$2" sysctl_arm64="$3" images="$4"
        shift 4
        out="$(cd "$repo" && PATH="$bin:$PATH" FAKE_UNAME_M="$host_m" FAKE_UNAME_S="$host_s" FAKE_SYSCTL_ARM64="$sysctl_arm64" \
            FAKE_IMAGES="$images" DC_DETERMINISM_IMAGE="${DC_DETERMINISM_IMAGE_FOR_TEST:-}" bash "$repo/scripts/prebuild.sh" "$@" 2>&1)"
        status=$?
        joined="$(printf '%s ' $out)"
    }
    inner_line() {  # IMAGE: what follows the image in the docker line
        echo "$1 bash scripts/prebuild.sh --in-image --merge --image $1 --image-id sha256:fake-id-of-$1"
    }
    run_prebuild_script x86_64 Linux 0 "dc_determinism-cpu:local=amd64"
    expect_eq "prebuild script on x86 Linux status" "$status" 0
    expect_contains "prebuild script on x86 Linux docker line" "$joined" "--rm --platform linux/amd64 -u $uid_gid -e HOME=/tmp -v $repo:/work -w /work $(inner_line dc_determinism-cpu:local)"
    run_prebuild_script aarch64 Linux 0 "dc_determinism-cpu:local=arm64"
    expect_eq "prebuild script on arm64 Linux status" "$status" 0
    expect_contains "prebuild script on arm64 Linux docker line" "$joined" "--rm --platform linux/arm64 -u $uid_gid -e HOME=/tmp -v $repo:/work -w /work $(inner_line dc_determinism-cpu:local)"
    run_prebuild_script arm64 Darwin 1 "dc_determinism-cpu:local=arm64" --check --test
    expect_eq "prebuild script on a Mac status" "$status" 0
    expect_contains "prebuild script on a Mac docker line" "$joined" "--rm --platform linux/arm64 -u $uid_gid -e HOME=/tmp -v $repo:/work -w /work $(inner_line dc_determinism-cpu:local) --test --check"
    run_prebuild_script x86_64 Darwin 1 "dc_determinism-cpu:local=arm64" --no-clean
    expect_eq "prebuild script in a Rosetta shell status (an arm64 host)" "$status" 0
    expect_contains "prebuild script in a Rosetta shell docker line" "$joined" "--platform linux/arm64 -u $uid_gid -e HOME=/tmp -v $repo:/work -w /work $(inner_line dc_determinism-cpu:local) --no-clean"
    run_prebuild_script x86_64 Linux 0 "mine:1=amd64" --image mine:1
    expect_eq "prebuild script --image status" "$status" 0
    expect_contains "prebuild script --image docker line" "$joined" "-w /work $(inner_line mine:1)"
    DC_DETERMINISM_IMAGE_FOR_TEST=env:image run_prebuild_script x86_64 Linux 0 "env:image=amd64"
    expect_contains "prebuild script DC_DETERMINISM_IMAGE docker line" "$joined" "-w /work $(inner_line env:image)"
    # an image of another architecture than the host is refused (nothing is emulated), a missing one too: nothing runs
    run_prebuild_script x86_64 Linux 0 "dc_determinism-cpu:local=arm64"
    expect_eq "prebuild script with an arm64 image on x86 status" "$status" 2
    expect_contains "prebuild script with an arm64 image on x86 message" "$out" "'dc_determinism-cpu:local' is arm64, but this host builds linux-amd64 and needs an amd64 image (nothing is emulated)"
    expect_not_contains "prebuild script with an arm64 image on x86 runs nothing" "$out" "--platform linux"
    run_prebuild_script arm64 Darwin 1 "dc_determinism-cpu:local=amd64"
    expect_eq "prebuild script with an amd64 image on a Mac status" "$status" 2
    expect_contains "prebuild script with an amd64 image on a Mac message" "$out" "is amd64, but this host builds linux-arm64 and needs an arm64 image"
    run_prebuild_script x86_64 Linux 0 ""
    expect_eq "prebuild script without the image status" "$status" 2
    expect_contains "prebuild script without the image message" "$out" "'dc_determinism-cpu:local' not found"
    run_prebuild_script armv7l Linux 0 "dc_determinism-cpu:local=amd64"
    expect_eq "prebuild script on another machine status" "$status" 2
    expect_contains "prebuild script on another machine message" "$out" "unsupported host architecture: armv7l"
    # the in-image part: --merge belongs to it, and its --check needs --merge
    run_prebuild_script x86_64 Linux 0 "dc_determinism-cpu:local=amd64" --merge
    expect_eq "prebuild script --merge outside the image status" "$status" 2
    run_prebuild_script x86_64 Linux 0 "dc_determinism-cpu:local=amd64" --in-image --check
    expect_eq "prebuild script --in-image --check without --merge status" "$status" 2
    expect_contains "prebuild script --in-image --check without --merge message" "$out" "needs --merge"
    run_prebuild_script x86_64 Linux 0 "dc_determinism-cpu:local=amd64" --bogus
    expect_eq "prebuild script unknown argument status" "$status" 2
    # refused before anything is built
    run_prebuild_script x86_64 Linux 0 "dc_determinism-cpu:local=amd64" --in-image --merge --out-dir /elsewhere
    expect_eq "prebuild script --out-dir outside the repository status" "$status" 2
    expect_contains "prebuild script --out-dir outside the repository message" "$out" "--out-dir must be below the repository"
    run_prebuild_script x86_64 Linux 0 "dc_determinism-cpu:local=amd64" --in-image --merge
    expect_eq "prebuild script --merge without --image-id status" "$status" 2
    expect_contains "prebuild script --merge without --image-id message" "$out" "--merge needs --image and --image-id"
    expect_not_contains "prebuild script refuses before building" "$out" "binary:   "
fi

echo "$checks checks, $failures failures"
[ "$failures" -eq 0 ]
