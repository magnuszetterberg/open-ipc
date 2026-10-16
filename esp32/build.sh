#!/usr/bin/env bash
# Build, flash or monitor one firmware: ./build.sh camera [idf.py commands...]
#   ./build.sh camera                  build
#   ./build.sh camera flash monitor    build, flash and open the serial monitor (Ctrl-] quits)
#   CAM=1 ./build.sh camera flash      camera 1 or 2 of the base station: its own build folder and sdkconfig
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

# Camera N (radio ports N and 0x10 + N): sdkconfig.defaults.camN on top of sdkconfig.defaults, built apart
# from camera 0 so switching between them never mixes settings.
cam=()
if [[ -n ${CAM:-} && $CAM != 0 ]]; then
  if [[ ! -f $app/sdkconfig.defaults.cam$CAM ]]; then
    echo "no $app/sdkconfig.defaults.cam$CAM: CAM=$CAM isn't a camera of '$app'" >&2
    exit 1
  fi
  cam=(-B "$PWD/$app/build-cam$CAM" -D "SDKCONFIG=$PWD/$app/sdkconfig.cam$CAM"
       -D "SDKCONFIG_DEFAULTS=$PWD/$app/sdkconfig.defaults;$PWD/$app/sdkconfig.defaults.cam$CAM")
fi

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
./idf.sh idf.py -C "$app" "${cam[@]}" "$@"
