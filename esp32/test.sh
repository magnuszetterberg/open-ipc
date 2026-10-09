#!/usr/bin/env bash
# Build the core for Linux and run its tests: ./test.sh [ctest options, e.g. -R fec]
# Uses the cmake and ninja that setup.sh installed with ESP-IDF, and the system compiler and libsodium.
set -euo pipefail
cd "$(dirname "$0")"

./idf.sh cmake -S host -B host/build -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null
./idf.sh cmake --build host/build
./idf.sh ctest --test-dir host/build --output-on-failure "$@"
