#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
project_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
run_tests=OFF
if [ "${1:-}" = "--test" ]; then
    run_tests=ON
elif [ "$#" -gt 0 ]; then
    echo "Usage: $0 [--test]" >&2
    exit 2
fi
cmake -S "$project_root" -B "$project_root/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING="$run_tests"
cmake --build "$project_root/build" --parallel
if [ "$run_tests" = ON ]; then
    ctest --test-dir "$project_root/build" --output-on-failure
fi
