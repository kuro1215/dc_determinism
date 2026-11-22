# dc_determinism

An evaluator of the **cross-environment reproducibility** of the DigitalCurling physics simulators: does the same shot
produce the same result when the operating system, CPU, compiler or runtime library changes?
This repository publishes the evaluator used for the experiments of our paper.

It evaluates the two simulators of DigitalCurling.

| Engine | Description |
|---|---|
| FCV1 (`fcv1`) | Floating-point (float32) physics based on Box2D |
| FCV1Plus (`fcv1plus`) | Fixed-point (Q8.24) physics |

Every shot extracted from real game logs is run from the same initial state on configurations that differ in OS, CPU,
compiler and standard library, and the stone states are compared bit for bit.

## Features

- **Hashes the raw state**: after every step the evaluator takes `DigitalCurling_StoneCoordinate` from the
  DigitalCurling C API (position, angle, linear velocity and angular velocity of the 16 stones, 384 bytes) as raw bytes
  and hashes them with SHA-256 (observation contract `raw-stonecoordinate-v1`). Nothing is rounded or formatted, so a
  single-bit difference is detected.
- **Two digests per shot**: `trajectory_digest` over every frame (from the initial state until the stones stop) and
  `final_digest` over the final board only. Both are always recorded.
- **Many configurations**: native Windows (MSVC, MinGW gcc / clang), Linux (gcc / clang), arm64 Linux such as Apple
  silicon (four variants, with and without FMA) and WebAssembly (Node.js), all with the same procedure.
- **Reproducible prebuilt binaries**: the simulator loaders, the workers and the evaluator itself are built from the
  bundled sources with a fixed toolchain, and their SHA-256 are recorded in manifests. They are verified on every run,
  and `--check` confirms that the sources rebuild them byte for byte.
- **Detailed difference analysis**: full trajectories are captured only for the shots that differ, and the analysis
  reports the first differing step and field, the maximum differences, the final position differences, and whether
  the outcome (No. 1 stone, score) changes.

## Layout

| Path | Contents |
|---|---|
| `src/dc_determinism/` | The evaluator (C++17, CMake). Tests in `src/dc_determinism/tests/` (CTest) |
| `src/worker/` | The simulator worker `dc_worker` (`worker.cpp`), the player worker (`player.cpp`), the WebAssembly build (`wasm/`) and the pybind worker (`pybind/`) |
| `src/vendor/` | Bundled sources used by the builds (DigitalCurling, fpm, Box2D, nlohmann/json, xxHash, miniz) and `manifest.json` with their revisions and hashes |
| `prebuilt/` | Prebuilt binaries: the evaluator (`dc_determinism/`), the workers (`worker/`), the simulator loaders (`loaders/`), the players (`players/`), pybind (`pybind/`), and `manifest.json` files recording their SHA-256 |
| `dc_determinism.ps1`, `dc_determinism.sh`, `dc_determinism-native.ps1` | Entry scripts (see below) |
| `scripts/` | Build of the evaluator (`build.*`), build of the shipped binaries (`prebuild.*`), Windows toolchain helper (`build-local.ps1`), test of the entry script (`check.sh`) |
| `Dockerfile` | Definition of the runtime Docker image `dc_determinism-cpu:local` |
| `THIRD_PARTY_NOTICES.md` | Licenses of the third-party software in the bundled sources and the prebuilt binaries |

`logs/` (game logs), `prep/`, `results/`, `out/` and `build/` are not tracked by Git.

## Requirements

The binaries are prebuilt, so evaluating needs neither a compiler nor Python.

| How to run | Requirements |
|---|---|
| Native Windows | Windows 10 / 11 (x86-64), PowerShell 7. Node.js 18 or later for `--compiler wasm` |
| Docker (Windows / Linux / macOS) | Docker (for example Docker Desktop). Build the image `dc_determinism-cpu:local` with `image build` |
| Apple silicon Mac | Docker Desktop. Build the linux-arm64 binaries on that Mac first ([Apple silicon Mac](#apple-silicon-mac)) |

Put the game logs (ZIP files) in `logs/`.

## Quick start

```powershell
# Native Windows
.\dc_determinism-native.ps1 verify --compiler msvc
.\dc_determinism-native.ps1 prepare --logs-dir logs --output prep\full
.\dc_determinism-native.ps1 run simulator --compiler msvc --engine fcv1 --prepared prep\full --repeats 2 --output results\fcv1-msvc.json
```

```bash
# Docker (Linux / macOS)
bash dc_determinism.sh image build
bash dc_determinism.sh verify --compiler gcc
bash dc_determinism.sh prepare --logs-dir logs --output prep/full
bash dc_determinism.sh run simulator --compiler gcc --engine fcv1 --prepared prep/full --repeats 2 --output results/fcv1-gcc.json
```

To compare two environments, collect the result files made from the same `prep/full` (or the same logs) in one place
and run `compare simulator`.

```powershell
.\dc_determinism-native.ps1 compare simulator results\fcv1-msvc.json results\fcv1-gcc.json --output results\cmp-fcv1-msvc-vs-gcc.json
```

## Entry scripts

Every command is `dc_determinism <subcommand>`. It is normally run through one of these entry scripts (they all take
the same arguments).

| Script | What it does |
|---|---|
| `dc_determinism-native.ps1` | Runs `prebuilt/dc_determinism/windows-amd64/dc_determinism.exe` directly on Windows (PowerShell 7) |
| `dc_determinism.ps1` | Runs in a Linux container through Docker, from Windows |
| `dc_determinism.sh` | Runs through Docker from Linux / macOS (works with the bash 3.2 of macOS) |

Behaviour shared by the Docker entry scripts:

- The image is `--image NAME` (written first), else the environment variable `DC_DETERMINISM_IMAGE`, else
  `dc_determinism-cpu:local`. The image architecture (`amd64` / `arm64`) selects
  `prebuilt/dc_determinism/linux-<arch>/dc_determinism`.
- An image whose architecture differs from the host is refused unless it is named explicitly (exit code 2).
- The repository is mounted at `/w/dc_determinism` and a current directory outside it at `/data`; path arguments are
  translated to container paths. A path in neither is an error.
- Only `prebuild` runs the host binary instead of the container, because it drives Docker itself.
- The exit code is the one of `dc_determinism`.

The image is built from the `Dockerfile` (Ubuntu 24.04 with g++, clang, cmake, Node.js, Emscripten, Python 3 and
numpy). Downloads happen only while the image is built; nothing is downloaded at run time.

```bash
bash dc_determinism.sh image build [--tag dc_determinism-cpu:local] [--platform linux/amd64|linux/arm64]
```
```powershell
.\dc_determinism.ps1 image build
```

## Commands

| Subcommand | Description |
|---|---|
| `prepare --logs-dir DIR [--limit N] --output PREP_DIR` | Builds the corpus (the initial state of every shot) from the game logs and freezes it in `PREP_DIR/prepared.bin` and `prepared.json` |
| `verify [--compiler X] [--verbose]` | Checks the worker (and the loader of `--compiler`) against the manifests and runs a self-test. Prints one line, `<name>: verified` or `<name>: unverified (reason)` |
| `run simulator` | Runs the simulator repeatedly and writes the per-shot reproducibility to a result file |
| `compare simulator A B` | Compares two `run simulator` results, or two `capture simulator` archives |
| `compare engines` | Compares FCV1 and FCV1Plus in lockstep |
| `capture simulator` | Saves the full trajectories of selected shots in a ZIP archive |
| `run player` / `capture player` / `compare player` | Checks the reproducibility of the random output of the players and compares it across configurations |
| `benchmark` | Measures the simulation speed (serially) |
| `compare pybind` | Compares the C API version with the official pybind version (FCV1-Simulator); linux-amd64 only |
| `prebuild` | Builds the shipped loaders, workers and players ([Prebuilt binaries](#prebuilt-binaries)) |

The loader is selected with `--compiler msvc|gcc|clang|gcc-nofma|clang-nofma|wasm`. It is always a prebuilt loader
from `prebuilt/loaders/`, verified against its manifest.

| Platform | `--compiler` |
|---|---|
| windows-amd64 | `msvc`, `gcc`, `clang`, `wasm` |
| linux-amd64 | `gcc`, `clang`, `wasm` |
| linux-arm64 | `gcc`, `clang`, `gcc-nofma`, `clang-nofma`, `wasm` |

Output files are never overwritten (an existing file is an error). Results are written atomically through a temporary
file.

### Exit codes

| Command | 0 | 1 | 2 |
|---|---|---|---|
| `run simulator` | Every shot reproduced and stopped | A shot did not reproduce, or was truncated at the step limit | Error |
| `compare simulator` | No difference | Differences or exclusions | Error |
| `compare engines`, `compare pybind` | Success (also with differences) | — | Error |
| `run player` | Reproducible | Not reproducible | Error |
| `compare player` | Equal | Not equal, or a side is not reproducible | Error |
| `capture simulator`, `capture player` | Success | Does not match `--expect` | Error |
| `verify` | Verified | — | Verification failed, or error |
| `benchmark` | Success | — | Error |

## `run simulator`

```powershell
.\dc_determinism-native.ps1 run simulator --compiler msvc --engine fcv1 `
  --prepared prep\full --workers 24 --repeats 10 --output results\fcv1-msvc.json
```

| Option | Description |
|---|---|
| `--engine fcv1\|fcv1plus` | Engine |
| `--compiler X` | Loader (required) |
| `--prepared DIR` / `--logs-dir DIR [--limit N]` | The corpus: a prepared one, or built from the logs on every run |
| `--repeats R` | Number of repetitions (2 or more, default 2) |
| `--workers W` | Number of parallel workers (default: number of logical CPUs) |
| `--max-steps N` | Maximum number of steps per shot (default 60000) |
| `--timeout S` | Time limit of one repetition in seconds (0 = none) |
| `--compare-logs` | Adds a comparison with the trajectories in the game logs ([Comparison with the game logs](#comparison-with-the-game-logs)) |

Each repetition runs every shot in fresh worker processes. Progress is printed to standard error as
`repetition i/n; shots k/N`.

Main contents of the result file (`dc-determinism-evaluation-v1`):

- `contract`: engine, observation contract, `max_steps`, corpus identifiers (`prepared_sha256`, `record_digest`,
  `corpus_input_sha256`), SHA-256 of the loader
- `loader` / `worker` / `preparer`: SHA-256 of the binaries used and their build records
- `environment`: OS, CPU, number of logical CPUs, whether it ran in a container, kernel
- `shots[]`: `shot_id`, `status` (`stopped` / `truncated`), `steps`, `trajectory_digest`, `final_digest`,
  `reproducible_trajectory`, `reproducible_final`
- `reproducible`: whether every shot reproduced

Reproducibility: `reproducible_trajectory` means `(status, steps, trajectory_digest)` is the same in every
repetition; `reproducible_final` means `(status, final_digest)` is the same in every repetition.

## `compare simulator`

`compare simulator A B --output FILE [--workers W]`. A and B are both `run simulator` result files or both
`capture simulator` archives (the kind is detected from the content).

**Result files**: only results with the same corpus, engine, observation contract and `max_steps` are compared. For
the shots that reproduced and stopped on both sides, the trajectory and final digests are compared separately, and the
number, rate and list of mismatching shots are written as `dc-determinism-comparison-v2`. The pair is `same-binary`
when the loader SHA-256 are equal and `cross-binary` otherwise (for `cross-binary`, the loaders must have been built
from the same simulator source).

**Archives**: for every common shot, the comparison reports the first differing step and field (bitwise), the maximum
difference per field, the final position difference per stone, stone presence differences, step count differences,
and whether the outcome (No. 1, score, tie) is equal. Archives of different engines (for example FCV1 and pybind) can
be compared. The summary has the distributions (max, p50, p90, p99) of the final displacement and the maximum
trajectory displacement, and the counts above the thresholds 0, 1e-6 m, 1e-4 m, 1 mm and 1 cm
(`dc-determinism-capture-comparison-v2`).

Definitions:

- Stone presence: a stone is absent when the absolute values of its 6 state values are all 1e-7 or less (per frame).
- The position difference is the Euclidean distance (m), the velocity difference the norm of the (vx, vy) difference
  (m/s), the angle difference the absolute difference folded into [0, π] (rad), and the angular velocity difference the
  absolute difference (rad/s). Differences are measured only in frames where the stone is present on both sides.
- Frame k is the state after k steps (frame 0 is the initial state). When the trajectories differ in length, the extra
  frames are compared with the final state of the shorter one.
- The outcome is decided on the final board with the tee at y = 38.405 m and a house radius of 1.974 m.

## `capture simulator`

Saves the raw state of every step for the shots that differ between environments. Only the selected shots are saved.

```powershell
.\dc_determinism-native.ps1 capture simulator --compiler msvc --engine fcv1 --prepared prep\full `
  --shots results\cmp-fcv1-msvc-vs-gcc.json --expect results\fcv1-msvc.json `
  --output results\capture-fcv1-msvc.zip
```

- `--shots` takes a `compare simulator` result (its mismatching shots are selected) or a file with one `shot_id` per
  line; `--all` selects every shot. Use the same `--shots` in both environments that are compared.
- `--expect RESULT.json` checks that the digests of the captured trajectories match a `run simulator` result (exit
  code 1 otherwise).
- `--engine pybind-py27|pybind-py36` captures with a build of the official pybind version extended with read-only
  recording (`prebuilt/pybind/`). It runs on linux-amd64 only and needs `--logs-dir`.

The archive holds `index.json` (corpus identifiers, selected shots, records of the binaries used) and one
`shots/<index>.dctr` per shot. The `dctr-v1` format is the magic `DCTR0001`, a JSON header, and, per stone and field,
the byte planes of the frame-to-frame XOR differences, compressed with zlib (about 24 times smaller;
`src/dc_determinism/capture/dctr.cpp`).

## `compare engines`

```powershell
.\dc_determinism-native.ps1 compare engines --compiler msvc --prepared prep\full `
  --logs-dir logs --workers 24 --output results\compare-engines-msvc.json
```

FCV1 and FCV1Plus are advanced together one step at a time from the same initial state, and the stone position
differences are computed in double precision after every step (an engine that has stopped keeps its final state). No
trajectory is saved. For every shot it records:

- the first step with a bit difference, and the first steps where the position difference exceeds 1e-6 m, 1 mm and
  1 cm
- the maximum position difference (value, step, stone), the number of presence mismatches and their first step
- the distance and presence class of each stone on the final board, and the outcome comparison
- with `--logs-dir`: the comparison of both engines' final boards with the game logs

The output is `dc-determinism-engine-deviation-v1`. Its summary has the counts and distributions of these metrics and
the lists of shots whose outcome changes.

## Comparison with the game logs

`run simulator --compare-logs` (and `compare engines --logs-dir`) compares the simulation with the trajectories
recorded in the game logs.

- Give the original `--logs-dir` together with `--prepared`. It is an error when the corpus rebuilt from the logs does
  not match `prepared.json`.
- Only repetition 0 is compared, and only the shots that reproduced and stopped. The comparison does not affect the
  exit code.
- Per shot, the final board is compared with the last frame of the log (`trajectory_last`) and with the finish board
  of the log (`finish`), and the trajectory frame by frame (`trajectory`). Log frame k is matched with simulation step
  `round((k+1) × log seconds per frame ÷ simulator seconds per frame)`.
- The primary basis is `--compare-logs-basis trajectory-last|finish` (default `trajectory-last`) and the tolerance
  `--compare-logs-tolerance` (default 0.01 m). With `--compare-logs-basis-fallback`, a shot that fails the primary basis
  is also judged by the other one.

## Player comparison

Compares the random output of the DigitalCurling players (`normal_dist`, `nd2`) across configurations. The prebuilt
players are in `prebuilt/players/<platform>-<compiler>/<player>/` (wasm: `prebuilt/players/wasm/<player>/`).

```powershell
.\dc_determinism-native.ps1 run player --player nd2 --compiler msvc --repeats 5 --output results\player-nd2-msvc.json
.\dc_determinism-native.ps1 compare player results\player-nd2-msvc.json results\player-nd2-gcc.json --output results\cmp-nd2.json
# Only to look at a difference in detail: save the output itself and compare it
.\dc_determinism-native.ps1 capture player --player normal_dist --compiler msvc --output out\normal_dist-msvc.bin
.\dc_determinism-native.ps1 compare player out\normal_dist-msvc.bin out\normal_dist-gcc.bin --output out\cmp-normal_dist.json
```

- `run player` runs the player in a fresh process per repetition and judges reproducibility by the SHA-256 of the
  output (`--repeats` 2 or more, `--samples` 640 to 1,000,000, default 4096). It keeps no output file and writes only the
  result file (`dc-determinism-player-result-v1`).
- `compare player` checks whether the output SHA-256 are equal (result files), or counts the mismatching samples and
  the maximum difference (output files, `.bin`).

## `benchmark`

```powershell
.\dc_determinism-native.ps1 benchmark --compiler msvc --prepared prep\full --repeats 5 --output results\benchmark-msvc.json
```

One worker measures FCV1 and FCV1Plus of the same loader serially. Per shot and engine, one warm-up is followed by
`--repeats` measurements (the engine order alternates), timing from `set_stones` until the stop is confirmed. The
output is `dc-determinism-benchmark-v1` with, among others, the median of the per-shot medians per engine. Keep the
machine otherwise idle while measuring.

## `compare pybind`

Compares the final boards of the C API FCV1 (path A) and the official pybind version of FCV1-Simulator (path B:
v1.0.27 = `py27`, v1.0.36 = `py36`) on every shot. The official pybind version exists only for x86-64 Linux, so this
runs only inside the linux-amd64 Docker image.

```bash
bash dc_determinism.sh compare pybind --prepared prep/full --logs-dir logs --compiler gcc \
  --trajectory-shots results/traj-shots.txt --output results/compare-pybind-gcc.json
```

- For the shots in `--trajectory-shots`, trajectories are also taken with a build of the official version extended
  with read-only frame recording (recording build v2) and compared with the same metrics as the archive comparison of
  `compare simulator`.
- `--check-recording` checks on every shot that the recording build returns the same final board as the official one.
- `py36` receives the angular velocity with its sign flipped (to follow the sign convention of the C API).
- Known implementation differences (the pybind version takes no sheet width, and treats a stone whose coordinates are
  both within 1e-7 as absent) are written to `known_implementation_differences` in the report.

The recipe and the patches of the recording build v2 are in `src/worker/pybind/` (it is built only inside Docker).

## WebAssembly (`--compiler wasm`)

`dc_worker` built with Emscripten to WebAssembly, with the loader and both engines linked statically into one module
(`prebuilt/worker/wasm/`), run with `node`. The protocol and the observation are those of the native worker, so the
results compare directly with native results.

- Available in `verify`, `run simulator`, `benchmark`, `capture simulator` and `compare engines`.
- Every run checks the SHA-256 of the bundle, and that the worker and simulator sources are those of the native
  loaders.

## Apple silicon Mac

On an Apple silicon Mac the evaluator runs in an arm64 Linux container of Docker Desktop, without emulation (platform
`linux-arm64`). The linux-arm64 binaries are not shipped, so build them on that Mac first.

```bash
bash dc_determinism.sh image build      # the arm64 image
bash scripts/prebuild.sh                # the evaluator (prebuilt/dc_determinism/linux-arm64/)
bash dc_determinism.sh prebuild         # the worker, 4 loaders and 8 players
```

aarch64 has fused multiply-add (FMA) instructions, and compilers may contract `a*b+c` into an FMA. The loaders and
players therefore come in two kinds.

| `--compiler` | Description |
|---|---|
| `gcc`, `clang` | Default flags (contraction into FMA allowed) |
| `gcc-nofma`, `clang-nofma` | With `-ffp-contract=off` (as on x86-64). The build checks with `objdump` that there is no FMA instruction |

`-ffp-contract=off` applies only to the code of the loaders and players; the dynamically linked libm and libstdc++ may
still use FMA. The pybind features are not available on arm64.

## Prebuilt binaries

Every binary in `prebuilt/` is built from the bundled sources (`src/`) with a fixed toolchain.
`prebuilt/manifest.json` and the per-loader `manifest.json` record the source tree hashes, the SHA-256 of every binary,
the compiler, the flags and the toolchain (MSVC, Windows SDK, MSYS2 package versions, Docker image ids, and so on).

| What | Command | Host |
|---|---|---|
| Loaders, workers, players, wasm | `dc_determinism prebuild` | Windows: windows-amd64 and linux-amd64 (Docker). Linux / Mac: linux of the host architecture |
| The evaluator | `scripts/prebuild.ps1` / `scripts/prebuild.sh` | Windows: windows-amd64 and linux-amd64. Linux / Mac: linux of the host |

```powershell
.\dc_determinism-native.ps1 prebuild          # Windows host
.\scripts\prebuild.ps1                        # the evaluator
```

- Nothing is built under emulation; the Docker image must have the architecture of the host.
- Only a Windows host accepts source changes. Linux and Mac hosts refuse to build when the sources differ from the
  manifests (exit code 2).
- The sources are copied to a scratch directory at a fixed absolute path and built there (MSVC with `/Brepro`, MinGW
  with `-ffile-prefix-map` and `--no-insert-timestamp`). With the same toolchain, the bytes do not depend on where the
  repository is.

### Reproducibility check (`--check`)

```powershell
.\dc_determinism-native.ps1 prebuild --check   # loaders, workers, players, wasm
.\scripts\prebuild.ps1 -Check                  # the evaluator
```
```bash
bash dc_determinism.sh prebuild --check
bash scripts/prebuild.sh --check
```

Without installing anything, the check compares the recorded toolchain with the current one (`toolchain differs` and
exit code 2 when they differ), rebuilds the artifacts from scratch, and compares them byte for byte with the installed
ones, printing `identical` / `differs` per artifact. Exit code: 0 = all identical, 1 = differences, 2 = error or
toolchain mismatch.

## Tests

```powershell
.\scripts\build.ps1 -Test        # Windows (MSVC)
```
```bash
docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD:/work" -w /work dc_determinism-cpu:local scripts/build.sh --test   # Linux (g++)
```

CTest runs the unit tests of the evaluator and the test of the entry script `dc_determinism.sh` (`scripts/check.sh`,
with fake `docker`, `uname` and `sysctl`; Unix only). The pybind features are tested without Python, with a fake
worker (`tool_fake_pybind`).

## Bundled sources and licenses

The sources in `src/vendor/` (DigitalCurling 762eef6, fpm, Box2D, nlohmann/json, xxHash, miniz) are the needed parts
of upstream revisions. `src/vendor/manifest.json` records their revisions and tree hashes, which are checked before
every build.

The licenses of the third-party software are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The code of this
repository is licensed under the MIT License ([LICENSE](LICENSE)).
