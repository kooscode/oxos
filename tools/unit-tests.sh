#!/bin/sh
# Build and run the host unit tests of the OXOS core runtime (tests/unit/README.md).
#
#   tools/unit-tests.sh                 build and run every test
#   tools/unit-tests.sh --coverage      ... with llvm-cov line/function coverage of the kernel sources
#   tools/unit-tests.sh -- ARGS         pass ARGS to the test binary (e.g. --gtest_filter='Pool.*')
#   OXOS_KERNEL_OPT=-O2 tools/unit-tests.sh   the kernel sources at the console build's optimisation (own build dir)
#   OXOS_STRESS_SECONDS=N               length of each Stress.* test (default 5)
#
# The tests are a 32-bit (i386) build with plain make (tests/unit/Makefile). Without a local multilib toolchain
# (g++-multilib, googletest sources in /usr/src/googletest) the script builds and runs them in a Docker image
# (tests/unit/Dockerfile).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
COVERAGE=OFF
if [ "${1:-}" = "--coverage" ]; then COVERAGE=ON; shift; fi
if [ "${1:-}" = "--" ]; then shift; fi

inner() {
    BUILD=$ROOT/tests/unit/build$( [ "$COVERAGE" = ON ] && echo -cov || true )$( [ -n "${OXOS_KERNEL_OPT:-}" ] && echo "-opt$(echo "$OXOS_KERNEL_OPT" | tr -dc 'A-Za-z0-9')" || true )
    make -s -C "$ROOT/tests/unit" -j"$(nproc)" CC=clang CXX=clang++ BUILD="$BUILD" \
        COVERAGE=$( [ "$COVERAGE" = ON ] && echo 1 || echo 0 ) KERNEL_OPT="${OXOS_KERNEL_OPT:-}"
    if [ "$COVERAGE" = ON ]; then
        rm -f "$BUILD"/*.profraw
        LLVM_PROFILE_FILE="$BUILD/run.profraw" "$BUILD/oxos_unit_tests" "$@" || status=$?
        llvm-profdata merge -o "$BUILD/run.profdata" "$BUILD/run.profraw"
        llvm-cov report "$BUILD/oxos_unit_tests" -instr-profile="$BUILD/run.profdata" \
            -ignore-filename-regex='googletest|tests/unit' | tee "$BUILD/coverage.txt"
        llvm-cov show "$BUILD/oxos_unit_tests" -instr-profile="$BUILD/run.profdata" -format=html \
            -ignore-filename-regex='googletest|tests/unit' -output-dir="$BUILD/coverage-html" >/dev/null
        echo "coverage: $BUILD/coverage.txt, $BUILD/coverage-html/index.html"
        exit ${status:-0}
    fi
    exec "$BUILD/oxos_unit_tests" "$@"
}

if [ "${OXOS_UNIT_INNER:-}" = 1 ] || { [ -d /usr/src/googletest ] && echo 'int main(){}' | clang++ -m32 -x c++ - -o /dev/null 2>/dev/null; }; then
    inner "$@"
fi
docker image inspect oxos-unit-tests >/dev/null 2>&1 || docker build -t oxos-unit-tests "$ROOT/tests/unit"
exec docker run --rm -u "$(id -u):$(id -g)" -e OXOS_UNIT_INNER=1 -e OXOS_TEST_LOG="${OXOS_TEST_LOG:-}" -e OXOS_STRESS_SECONDS="${OXOS_STRESS_SECONDS:-}" -e OXOS_KERNEL_OPT="${OXOS_KERNEL_OPT:-}" \
    -v "$ROOT:$ROOT" -w "$ROOT" oxos-unit-tests sh tools/unit-tests.sh \
    $( [ "$COVERAGE" = ON ] && echo --coverage || true ) -- "$@"
