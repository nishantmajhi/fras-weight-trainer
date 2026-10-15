#!/bin/sh
# Builds and runs the test suite. Works from the project root or from
# tests/; everything is resolved relative to this script.
#
# src/json.c is compiled twice: once normally for the probe and once with
# its allocation hooks redirected into the test binary, which is what lets
# json_test.c fail an allocation on demand.
set -e

cd "$(dirname "$0")/.."

CC=${CC:-cc}
CFLAGS="-std=c11 -O1 -g -Wall -Wextra -Iinclude -Isrc -Itests"
LDFLAGS="-lpthread -lm"

mkdir -p tests/build

# The probe: plain build, used by tools/gen_vectors.php and tools/check_php.php.
$CC $CFLAGS -o tests/build/probe tests/probe.c src/scoring.c $LDFLAGS

# The suite. json.c gets the failing-allocator hooks.
$CC $CFLAGS \
    -include tests/alloc_hooks.h \
    -DJSON_MALLOC=json_test_malloc \
    -DJSON_CALLOC=json_test_calloc \
    -DJSON_REALLOC=json_test_realloc \
    -c -o tests/build/json_hooked.o src/json.c

$CC $CFLAGS -c -o tests/build/scoring.o src/scoring.c
$CC $CFLAGS -c -o tests/build/fit.o src/fit.c

$CC $CFLAGS -o tests/build/run \
    tests/run.c tests/scoring_test.c tests/json_test.c tests/fit_test.c tests/vectors_test.c \
    tests/build/json_hooked.o tests/build/scoring.o tests/build/fit.o \
    $LDFLAGS

./tests/build/run tests/vectors.json
