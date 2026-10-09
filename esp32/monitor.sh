#!/usr/bin/env bash
# Serial monitor you can leave open: it lets go of the port while build.sh flashes, then reconnects.
#   ./monitor.sh [port] [-t timestamps] [-r reset on connect] [-b baud]
# Port: the argument, else ESPPORT, else /dev/ttyUSB0. Ctrl-C quits.
set -euo pipefail
cd "$(dirname "$0")"
exec ./idf.sh python3 monitor.py "$@"
