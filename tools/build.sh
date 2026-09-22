#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
profile="${1:-reference}"
case "$profile" in
  reference) low=OFF ;;
  lowmemory) low=ON ;;
  *) printf 'Usage: sh tools/build.sh [reference|lowmemory]\n' >&2; exit 2 ;;
esac
cmake -S . -B "build/$profile" -DCMAKE_BUILD_TYPE=MinSizeRel -DPICO_LOW_MEMORY="$low"
cmake --build "build/$profile" --parallel
ctest --test-dir "build/$profile" --output-on-failure
