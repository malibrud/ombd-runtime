#!/bin/sh
# ---------------------------------------------------------------------------
#  build.sh -- unity build of ombdc, the OMBD front end, and its tests.
#
#  The POSIX twin of build.bat. Each program is one translation unit,
#  compiled from scratch:
#      src/unity.cpp          -> build/ombdc
#      tests/unity_tests.cpp  -> build/ombdc_tests
#  Then the checks run, and any failure fails the build:
#      the unit tests and the golden token dumps (tests/*.tokens)
#      every .ombd under ../../OMBD_Specification/Examples must lex
#
#  Uses $CXX if set, else c++ (g++ or clang++).
#      ./build.sh           release
#      ./build.sh debug     -O0 -g
#
#  LF only: sh chokes on CRLF line endings.
# ---------------------------------------------------------------------------
set -e

cd "$(dirname "$0")"

EXAMPLES="../../OMBD_Specification/Examples"

CXX=${CXX:-c++}
if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "build: \`$CXX\` not found. Install g++ or clang++, or set CXX." >&2
    exit 2
fi

mkdir -p build

# -Wall -Wextra -Wpedantic -Werror    every warning, as an error (as /W4 /WX)
FLAGS="-std=c++20 -Wall -Wextra -Wpedantic -Werror"
if [ "$1" = "debug" ]; then
    FLAGS="$FLAGS -O0 -g"
else
    FLAGS="$FLAGS -O2 -DNDEBUG"
fi

echo "=== ombdc ==="
if ! $CXX $FLAGS -o build/ombdc src/unity.cpp; then
    echo "build: ombdc FAILED" >&2
    exit 1
fi

echo "=== ombdc_tests ==="
if ! $CXX $FLAGS -o build/ombdc_tests tests/unity_tests.cpp; then
    echo "build: ombdc_tests FAILED" >&2
    exit 1
fi

echo "=== unit tests ==="
if ! build/ombdc_tests "$EXAMPLES" tests; then
    echo "build: UNIT TESTS FAILED" >&2
    exit 1
fi

echo "=== lex every example ==="
# Each failing path is printed on stdout and collected; ombdc's own error
# message and the "ok" lines go to stderr.
failed=$(find "$EXAMPLES" -name '*.ombd' | sort | while IFS= read -r f; do
    if build/ombdc --tokens "$f" >/dev/null; then
        echo "  ok  $f" >&2
    else
        echo "$f"
    fi
done)
if [ -n "$failed" ]; then
    echo "build: AN EXAMPLE FAILED TO LEX" >&2
    exit 1
fi

echo "build: build/ombdc, all checks passed"
