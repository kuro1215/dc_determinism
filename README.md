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

## Usage

The requirements, a quick start, every command with its options and outputs, the platform notes and the tests are in
[docs/HOW_TO_USE.md](docs/HOW_TO_USE.md) (also on the Wiki of this repository).

## Results

The corpus is the 45,913 shots of the game logs. It was evaluated on three machines, 17 configurations in all.

| Machine | CPU | Configurations |
|---|---|---|
| `linux-amd` | AMD Ryzen Threadripper PRO 7965WX | Linux (Docker): gcc, clang, wasm |
| `win-amd` | AMD Ryzen 9 9950X | Windows native: msvc, gcc, clang, wasm. Docker: gcc, clang, wasm |
| `win-intel` | Intel Core i9-13900K | Windows native: msvc, gcc, clang, wasm. Docker: gcc, clang, wasm |

### Reproducibility within a configuration

In every configuration and for both engines, every shot gave the same trajectory and final digests in all 10
repetitions (2 for `win-intel` native wasm), and every shot stopped before the step limit.

### Reproducibility across configurations

Each configuration compared with `linux-amd` gcc (`compare simulator`):

| Engine | Bit-identical to the reference | Different |
|---|---|---|
| FCV1Plus | All 17 configurations (the reference included) | — |
| FCV1 | 11 (the reference included): every Docker configuration and wasm on native Windows | 6: native Windows msvc, gcc and clang on both machines. Trajectories differ in 662 shots (1.44 %), final boards in 12 shots (0.026 %) |

The six native Windows configurations of FCV1 are bit-identical to each other. The split follows the platform
(native Windows against Linux and WebAssembly), not the CPU vendor or the compiler. The fixed-point FCV1Plus is
bit-identical everywhere.

### FCV1 against FCV1Plus

`compare engines` gives the same summary in every configuration. Of the 45,913 shots:

| Metric | Shots |
|---|---|
| Bit difference in the trajectory | 45,913 (all) |
| Different step count | 40,923 |
| Different final board | 44,233; by more than 1 cm: 9,161 |
| Different stone presence on the final board | 214 |
| No. 1 stone changed / score changed | 161 / 227 |

The largest final displacement per shot has a median of 3.5 mm and a 99th percentile of 22 cm.

Against the final positions recorded in the game logs (last log frame, tolerance 1 cm), FCV1 agrees in 43,059 shots
(93.8 %) and FCV1Plus in 35,623 (77.6 %); the score agrees in 45,906 (FCV1) and 45,679 (FCV1Plus) shots.

### Official pybind version against the C API

`compare pybind` (FCV1, linux-amd64 Docker; the same result on all three machines):

| Official version | Different final board | By more than 1 cm | Score changed |
|---|---|---|---|
| v1.0.27 (`py27`) | 19,204 | 5,525 | 497 |
| v1.0.36 (`py36`) | 2,041 | 26 | 13 |

### Players

Both players reproduced their output in every configuration. `nd2` gives the same output in all 17 configurations.
`normal_dist` gives four different outputs: Linux gcc / clang (Docker), WebAssembly, native Windows gcc / clang, and
native Windows msvc.

### Speed

`benchmark`: median over the shots of the per-shot median time (ms, 5 repetitions, one worker). Machines differ, so
compare within a machine.

| Machine | Configuration | FCV1 | FCV1Plus |
|---|---|---|---|
| `linux-amd` | Docker gcc / clang / wasm | 15.03 / 14.85 / 9.23 | 5.55 / 4.37 / 6.96 |
| `win-amd` | Docker gcc / clang / wasm | 13.74 / 13.36 / 8.23 | 5.08 / 3.90 / 6.24 |
| `win-amd` | Native msvc / gcc / clang / wasm | 13.10 / 13.20 / 13.46 / 6.67 | 5.28 / 3.72 / 3.73 / 4.87 |
| `win-intel` | Docker gcc / clang / wasm | 12.52 / 12.62 / 6.47 | 5.31 / 4.54 / 8.68 |
| `win-intel` | Native msvc / gcc / clang / wasm | 12.48 / 12.43 / 12.76 / 5.69 | 7.34 / 4.43 / 4.32 / 5.14 |

## Bundled sources and licenses

The sources in `src/vendor/` (DigitalCurling 762eef6, fpm, Box2D, nlohmann/json, xxHash, miniz) are the needed parts
of upstream revisions. `src/vendor/manifest.json` records their revisions and tree hashes, which are checked before
every build.

The licenses of the third-party software are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The code of this
repository is licensed under the MIT License ([LICENSE](LICENSE)).
