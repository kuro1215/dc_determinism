#!/usr/bin/env bash
# Runs INSIDE the build container (started by `docker run`, as documented in README.md); not meant for the host.
# Everything that is downloaded (apt packages, pip packages, git sources) is downloaded here.
#
# Mounts:  /recording (ro)  this directory (patches/)
#          /out       (rw)  prebuilt/pybind
#          /official  (ro)  optional directory with <engine>/simulator-official.so to copy and verify
# Env:     ENGINES (space separated), JOBS, IMAGE_NAME, IMAGE_ID, IMAGE_PIN
set -Eeuo pipefail
export DEBIAN_FRONTEND=noninteractive

readonly FCV1_URL="https://github.com/kr-work/FCV1-Simulator.git"
readonly BOX2D_URL="https://github.com/erincatto/box2d.git"
readonly BOX2D_SHA="9ebbbcd960ad424e03e5de6e66a40764c16f51bc"
readonly JSON_URL="https://github.com/nlohmann/json.git"
readonly JSON_SHA="11a835df85677002a8aadc5b4e945684c5b7f68b"
readonly PIP_PACKAGES="cmake==4.4.4 pybind11==3.1.0 numpy==1.26.4"

fcv1_sha() {
  case "$1" in
    fcv1-py27) echo "dc1bfeecb41c7685f68f8b9d71bf7e7009362e5c" ;;  # v1.0.27 (GAT 2026)
    fcv1-py36) echo "4a567c2cd46bc1ff76ae98c6dff9b491b1dd3b98" ;;  # v1.0.36 (SIT 2026)
    *) echo "unknown engine: $1" >&2; exit 2 ;;
  esac
}

official_sha() {
  case "$1" in
    fcv1-py27) echo "e84c6ff60048c98436c57e2629fe3125bb8a84f14c0857c249ddba83de2b1bb9" ;;
    fcv1-py36) echo "3a94744a26cdbe48b5b1bf5abfde4651e959ee3be9dc021d7f926c377cca36fd" ;;
    *) echo "unknown engine: $1" >&2; exit 2 ;;
  esac
}

apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends git g++ make ca-certificates >/dev/null
python -m pip install --quiet --no-cache-dir $PIP_PACKAGES

fetch_pinned() {  # url sha dir
  git init -q "$3"
  git -C "$3" remote add origin "$1"
  git -C "$3" fetch -q --depth 1 origin "$2"
  git -C "$3" -c advice.detachedHead=false checkout -q FETCH_HEAD
  [ "$(git -C "$3" rev-parse HEAD)" = "$2" ] || { echo "SHA mismatch for $1" >&2; exit 1; }
}

mkdir -p /receipts
for NAME in $ENGINES; do
  echo "=== $NAME ==="
  FCV1_SHA="$(fcv1_sha "$NAME")"
  PATCH="/recording/patches/$NAME-record-v2.patch"
  [ -f "$PATCH" ] || { echo "patch not found: $PATCH" >&2; exit 1; }
  SRC="/work/$NAME"

  fetch_pinned "$FCV1_URL" "$FCV1_SHA" "$SRC"
  rm -rf "$SRC/extern/box2d" "$SRC/extern/json"
  fetch_pinned "$BOX2D_URL" "$BOX2D_SHA" "$SRC/extern/box2d"
  fetch_pinned "$JSON_URL" "$JSON_SHA" "$SRC/extern/json"

  git -C "$SRC" -c core.autocrlf=false apply --check "$PATCH"
  git -C "$SRC" -c core.autocrlf=false apply "$PATCH"
  echo "patch applied: $(basename "$PATCH")"

  ( cd "$SRC/extern/box2d" && mkdir build && cd build &&
    cmake -DBOX2D_BUILD_DOCS=OFF -DBOX2D_BUILD_UNIT_TESTS=OFF -DBOX2D_BUILD_TESTBED=OFF \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="./" .. >/dev/null &&
    cmake --build . --config Release -j"$JOBS" >/dev/null &&
    cmake --build . --target install --config Release >/dev/null )
  ( cd "$SRC/extern/json" && mkdir build && cd build &&
    cmake -DJSON_BuildTests=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DCMAKE_INSTALL_PREFIX="./" .. >/dev/null &&
    cmake --build . >/dev/null && cmake --build . --target install >/dev/null )
  ( cd "$SRC/src" && mkdir build && cd build &&
    cmake -Dpybind11_DIR="$(python -m pybind11 --cmakedir)" .. >/dev/null &&
    cmake --build . --config Release -j"$JOBS" 2>&1 | tail -3 )

  mkdir -p "/out/$NAME"
  cp "$SRC/src/build/simulator.so" "/out/$NAME/simulator-record-v2.so.building"
  mv -f "/out/$NAME/simulator-record-v2.so.building" "/out/$NAME/simulator-record-v2.so"

  if [ -f "/official/$NAME/simulator-official.so" ]; then
    ACTUAL="$(sha256sum "/official/$NAME/simulator-official.so" | awk '{print $1}')"
    [ "$ACTUAL" = "$(official_sha "$NAME")" ] || {
      echo "official $NAME binary has an unexpected SHA-256: $ACTUAL" >&2; exit 1;
    }
    cp "/official/$NAME/simulator-official.so" "/out/$NAME/simulator-official.so"
    echo "official binary copied: $NAME ($ACTUAL)"
  fi
done

python - <<'PY'
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import subprocess
import sys


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


official = {
    "fcv1-py27": "e84c6ff60048c98436c57e2629fe3125bb8a84f14c0857c249ddba83de2b1bb9",
    "fcv1-py36": "3a94744a26cdbe48b5b1bf5abfde4651e959ee3be9dc021d7f926c377cca36fd",
}
manifest_path = Path("/out/manifest.json")
manifest = {}
if manifest_path.is_file():
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
engines = manifest.get("engines", {})

for name in os.environ["ENGINES"].split():
    src = Path("/work") / name
    patch = Path("/recording/patches") / f"{name}-record-v2.patch"
    output = Path("/out") / name / "simulator-record-v2.so"
    entry = {
        "source": {
            "repository": "https://github.com/kr-work/FCV1-Simulator.git",
            "commit": command("git", "-C", str(src), "rev-parse", "HEAD"),
        },
        "dependencies": {
            "box2d": {"repository": "https://github.com/erincatto/box2d.git",
                      "commit": command("git", "-C", str(src / "extern/box2d"), "rev-parse", "HEAD")},
            "nlohmann_json": {"repository": "https://github.com/nlohmann/json.git",
                              "commit": command("git", "-C", str(src / "extern/json"), "rev-parse", "HEAD")},
        },
        "patch": {"path": f"src/worker/pybind/patches/{patch.name}", "sha256": sha256(patch)},
        "record": {"path": f"prebuilt/pybind/{name}/simulator-record-v2.so", "sha256": sha256(output)},
        "frame_layout": "float32 (F, 16, 6) = x, y, angle, vx, vy, w; absent stone = 6 zeros",
    }
    official_path = Path("/out") / name / "simulator-official.so"
    if official_path.is_file():
        entry["official"] = {"path": f"prebuilt/pybind/{name}/simulator-official.so",
                             "sha256": sha256(official_path),
                             "expected_sha256": official[name]}
    engines[name] = entry

manifest = {
    "schema": "dc-pybind-recording-v2-manifest-v1",
    "engines": dict(sorted(engines.items())),
    "build": {
        "image": {"name": os.environ["IMAGE_NAME"], "id": os.environ["IMAGE_ID"],
                  "pin": os.environ["IMAGE_PIN"]},
        "python": sys.version.split()[0],
        "platform": platform.platform(),
        "gxx": command("g++", "--version").splitlines()[0],
        "cmake": command("cmake", "--version").splitlines()[0],
        "git": command("git", "--version"),
        "pip_packages": {n: importlib.metadata.version(n) for n in ("cmake", "pybind11", "numpy")},
        "last_built_engines": os.environ["ENGINES"].split(),
        "note": "built in this Docker image, not in the toolchain of the official binaries "
                "(Ubuntu 22.04, GCC 11.4); same sources and flags, different compiler",
    },
}
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
print(f"wrote {manifest_path}")
PY
g++ --version | head -1
pip list 2>/dev/null | grep -iE '^(cmake|pybind11|numpy) '
