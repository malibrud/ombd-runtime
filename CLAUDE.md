# CLAUDE.md

Implementation repo for OMBD: the Unreal Engine plugin, the `ombdc` front end, and the tools that
size them. The specification is in the sibling repo `../OMBD_Specification`:
- `doc/Decisions.md` is authoritative on the language. When code and the log disagree, the log
  wins.
- `doc/backends/Unreal.md` (U1–U14) is the design log for this backend. Record backend decisions
  there, not here.

## Build

### `ombdc` (the front end)

- **A unity build, not CMake (Mark).** Run `ombdc\build.bat`, or `ombdc\build.bat debug`. It finds
  `cl` through vswhere when `cl` isn't on `PATH`.
  - `src\unity.cpp` `#include`s every front-end `.cpp` and builds `build\ombdc.exe`.
    `tests\unity_tests.cpp` builds `build\ombdc_tests.exe`.
  - Add a new `.cpp` to the unity file, not to a build list.
- **Linux.** `ombdc/build.sh` (or `build.sh debug`) is the POSIX twin. It uses `$CXX`, else
  `c++`, with `-Wall -Wextra -Wpedantic -Werror`, and runs the same checks.
  - It's tested with g++ 13 and clang 18 in WSL `Ubuntu-24.04`. From Git Bash, set
    `MSYS_NO_PATHCONV=1` before calling `wsl.exe --cd /mnt/c/...`.
  - Keep `build.sh` LF and `build.bat` CRLF. `ombdc/.gitattributes` pins both.
  - Keep the two scripts in step when either changes.
- **Pipeline so far.** `tryReadFile` → `tryLex` → `tryParse` → `tryLoadModel` → `tryCheckTypes`.
  `ombdc --tokens`, `--ast`, `--names` and `--types` print each stage. The syntax tree is a flat
  node array linked by index (`src/ast.h`); name resolution and type checking keep their results in
  arrays parallel to it (`src/resolve.h`, `src/types.h`). `src/units.h` holds dimensions and unit
  runs, and knows nothing about the tree.
- **The catalog grows with the models** (`src/catalog.cpp`). Only §4.1's built-in names are complete,
  so until the rest is filled in, `--names` and `--types` reject models for types they don't know
  yet.
- The build fails unless every check passes:
  - `/W4 /WX`;
  - the unit tests, including the golden dumps `tests\*.tokens`, `tests\*.ast`, `tests\*.names` and
    `tests\*.types`;
  - every `.ombd` under `../OMBD_Specification/Examples` parses.
- **Unity-build names.** Every `static` shares one translation unit, so module helpers carry a
  prefix (`Parser_peek`), and test helpers `test_`.
- **Style.** C-style C++: structs and free functions, `bool tryX(in, out*)` stages,
  `Type_function` operations. No classes, templates, exceptions or STL. No Chrono.
- **Goldens.** Regenerate a golden with `build\ombdc.exe --tokens <model> > tests\<name>.tokens`
  (or `--ast`, `--names` or `--types` into the matching file) from cmd or bash. Review it by hand before checking it in.
  PowerShell's `>` re-encodes the output, so don't use it.

### `chrono_rt_bench`

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
- It has no test suite yet. `chrono_rt_bench` runs end to end.

## Conventions

- **Timing.** The benchmark reports wall time per step against the step size, judged on the tail
  (p99.9, maximum, overruns), never only the mean.
- **What a knob may change.** Only numerics and backend choices (D35, D102). A change that alters
  what the model means, such as dropping soil deformation memory (D101), isn't a knob.
- **Determinism.** Runs are single-threaded by default, and the same options must reproduce the
  same final state to 17 digits.
