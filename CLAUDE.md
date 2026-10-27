# CLAUDE.md

Implementation repo for OMBD: the Unreal Engine plugin, the `ombdc` front end, and the tools that
size them. The specification is in the sibling repo `../OMBD_Specification`:
- `doc/Decisions.md` is authoritative on the language. When code and the log disagree, the log
  wins.
- `doc/backends/Unreal.md` (U1–U14) is the design log for this backend. Record backend decisions
  there, not here.

## Build

- Use CMake presets with Visual Studio 2026:
  - `cmake --preset vs2026`
  - `cmake --build --preset release`
- CMake isn't on `PATH`. Use VS's bundled copy:
  `C:/Program Files/Microsoft Visual Studio/18/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`.
- **Chrono 10.0.0** is a prebuilt installer at `C:\dev\chrono`. It has Release libraries only.
- **Don't use `find_package(Chrono)`.** The installer's package is broken for consumers.
  `cmake/ChronoImport.cmake` imports what is used. Add a library there, with its runtime DLLs
  checked using `dumpbin /dependents`, and keep its compile options matched to the installer's
  `ChronoTargets.cmake`, especially `/arch:AVX2`.
- There is no test suite yet. `chrono_rt_bench` runs end to end.

## Conventions

- **Timing.** The benchmark reports wall time per step against the step size, judged on the tail
  (p99.9, maximum, overruns), never only the mean.
- **What a knob may change.** Only numerics and backend choices (D35, D102). A change that alters
  what the model means, such as dropping soil deformation memory (D101), isn't a knob.
- **Determinism.** Runs are single-threaded by default, and the same options must reproduce the
  same final state to 17 digits.
