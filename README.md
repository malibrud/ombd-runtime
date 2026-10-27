# ombd-runtime

Implementation work for [OMBD](../OMBD_Specification): the Unreal Engine plugin, the `ombdc`
front end, and the tools that size them. The language specification and the backend design live
in the specification repo. `doc/Decisions.md` there is authoritative for the language, and
`doc/backends/Unreal.md` (U1–U14) for this backend.

Current contents:

| Path | What |
|---|---|
| `bench/` | `chrono_rt_bench`: real-time cost vs. fidelity for off-road vehicles on SCM soil |
| `cmake/ChronoImport.cmake` | Imports the prebuilt Chrono libraries (see "Why not `find_package(Chrono)`") |

## `chrono_rt_bench`

U14 makes Chrono the real-time backend that drives the motion bases, with real-time performance
as the knob that sets fidelity. This harness measures that trade-off. It runs one Chrono vehicle,
the HMMWV (wheeled) or the M113 (tracked), over a flat SCM soil patch. A scripted driver makes
the run deterministic: zero inputs while the vehicle settles, then a 1 s throttle ramp, then sine
steering. The loop runs as fast as possible and records:

- **The wall time of every step against the step size.** A fixed-step real-time loop fails on its
  slowest step, so the summary reports p99, p99.9, the maximum and the overrun count, not just
  the mean.
- **The motion-cue signal at the driver's seat** (optional `--cues` CSV). These are specific
  force, angular velocity and angular acceleration in the chassis frame, at the chassis's declared
  driver position. This is the signal a washout filter consumes, so it is where fidelity is judged.

Every knob is numerics or a backend choice (D35, D102 in the specification). None of them changes
what the model means:

| Knob | Flag |
|---|---|
| step size (= real-time tick) | `--step` |
| SCM grid spacing | `--grid` |
| SCM active domain around the chassis | `--active-domain` / `--no-active-domain` |
| SCM bulldozing | `--bulldozing` |
| contact method | `--contact nsc\|smc` |
| NSC solver iterations | `--iterations` |
| threads | `--threads` |
| M113 track shoe | `--shoe single-pin\|double-pin\|band-bushing\|band-ancf` |
| HMMWV tire | `--tire rigid\|rigid-mesh` |

Run `chrono_rt_bench --help` for all options.

### Prerequisites

- **Chrono 10.0.0, Win64 installer**, at `C:\dev\chrono`. It ships Release libraries only, so
  there are no Debug builds.
- **Eigen** at `C:\dev\eigen-5.0.0`. Chrono 10.0.0 was built against Eigen **5.0.1**, and Eigen
  types cross the DLL boundary. Moving to 5.0.1 is advisable (set `EIGEN3_INCLUDE_DIR`).
- **Visual Studio 2026.** Its bundled CMake (4.2) is used. CMake isn't on `PATH` by default, so
  use a *Developer PowerShell for VS 2026* or the full path to the bundled `cmake.exe`.

### Build

```bash
cmake --preset vs2026
cmake --build --preset release
```

The executable lands in `build/bench/Release/`, and the Chrono DLLs it needs are copied next to
it.

### Run

```bash
build/bench/Release/chrono_rt_bench.exe --vehicle hmmwv --duration 12 --cues results/hmmwv_ref.csv --results results/sweep.csv --tag ref
```

```bash
build/bench/Release/chrono_rt_bench.exe --vehicle hmmwv --duration 12 --grid 0.10 --cues results/hmmwv_g10.csv --results results/sweep.csv --tag grid10
```

```bash
python -I bench/tools/compare_cues.py results/hmmwv_ref.csv results/hmmwv_g10.csv --lowpass 20
```

Each run with `--results` appends one row to the CSV:
- the configuration;
- the RTF (wall time divided by simulated time);
- the step-time percentiles and overruns;
- SCM's mean share of the step;
- the final state.

The final position is printed to 17 digits. Two runs with the same options and one thread should
match exactly, which is the replay-determinism check.

### Timing hygiene

The tail figures are sensitive to whatever else the machine is doing.
- Measure on the PCs that drive the motion bases.
- Use the High Performance power plan, on AC power, with other applications closed.
- Don't run two benchmarks at once.

### First smoke results (dev PC, 2026-10-25, one thread)

These come from short runs on a development machine, not the simulator PCs. They are indicative
only.

| Vehicle | Settings | Mean step | p99.9 / max | Verdict |
|---|---|---|---|---|
| HMMWV, rigid tire | SMC, 1 ms, grid 5 cm | 1.29 ms | 3.2 / 3.8 ms | not real-time |
| HMMWV, rigid tire | SMC, 1 ms, grid 10 cm | 0.57 ms | 2.4 / 4.4 ms | mean fits; 3% of steps overrun |
| M113, single-pin | SMC, 0.5 ms, grid 5 cm | 4.7 ms* | 8.0 / 11.0 ms | about 9× slower than real time |
| M113, single-pin | NSC, 0.5 ms, 150 iterations | 16.5 ms* | 26.7 / 32.5 ms | about 33× slower than real time |

\* Measured with two benchmarks running at once, so these are inflated.

What was learned getting the M113 to drive:
- **The simple-map powertrain stalls it from rest in soft soil.** It has no torque converter, so
  the harness uses the shaft-based engine and automatic transmission.
- **SMC diverges at a 1 ms step with individual track shoes.**
- **NSC needs a small step and many iterations** before the sprocket stops slipping on the shoes.

A coarser grid changes the cue signal noticeably: `compare_cues` reports tens of percent relative
RMS error in the lateral and rotational channels. A converged reference, from a finer grid and a
smaller step, is still to be established before those numbers mean much.

## Why not `find_package(Chrono)`

The Win64 installer's CMake package doesn't work for a consumer:
- `ChronoConfig.cmake` contains Windows paths with backslashes (`C:\gitlab\...`), which CMake 4
  rejects as invalid escape sequences.
- Behind that, `ChronoTargets.cmake` declares every Chrono module, with link interfaces on
  dependencies the installer doesn't ship: VSG, HDF5, CUDA, MKL, urdfdom.

`cmake/ChronoImport.cmake` declares only the libraries used here. It copies their compile settings
from the installer's target file, and they must match: `/arch:AVX2` changes Eigen's alignment, and
Eigen types cross the DLL boundary. The same constraint will apply when Chrono goes into Unreal
behind its own DLL.
