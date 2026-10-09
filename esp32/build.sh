#!/usr/bin/env bash
# Build, flash or monitor one firmware: ./build.sh camera [idf.py commands...]
#   ./build.sh camera                  build
#   ./build.sh camera flash monitor    build, flash and open the serial monitor (Ctrl-] quits)
# The serial port is found automatically; set ESPPORT=/dev/ttyUSB0 to pick one.
set -euo pipefail
cd "$(dirname "$0")"

app=${1:-camera}
shift || true
if [[ ! -f $app/CMakeLists.txt ]]; then
  echo "no firmware '$app' (have: $(ls -d */main 2>/dev/null | cut -d/ -f1 | tr '\n' ' '))" >&2
  exit 1
fi

(( $# )) || set -- build

# Commands that open the serial port first ask a running monitor.sh to let go of it (see monitor.py).
if [[ " $* " =~ \ (flash|app-flash|bootloader-flash|erase-flash|monitor)\  ]]; then
  lock=${XDG_RUNTIME_DIR:-/tmp}/open-ipc-esp32-port
  touch "$lock.request"
  trap 'rm -f "$lock.request"' EXIT
  exec 9>>"$lock"
  if ! flock -w 5 9; then
    echo "a serial monitor still holds the port after 5 s" >&2
    exit 1
  fi
fi
./idf.sh idf.py -C "$app" "$@"
