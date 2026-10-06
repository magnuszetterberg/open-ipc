#!/usr/bin/env bash
# Receiver machine in one command: radio link (rx) + video window.
# Ctrl-C (or closing the window) stops both and hands the WiFi back.
set -euo pipefail
cd "$(dirname "$0")"

./build.sh                                    # fetch + build wfb-ng on first run
sudo -v                                       # ask for the password up front
while sleep 60; do sudo -n -v; done &         # keep sudo alive for the cleanup
keepalive=$!

sudo ./link.sh rx &
link=$!

cleanup() {
  trap '' INT TERM
  sudo pkill -TERM -f '[l]ink.sh rx' || true  # [l] keeps pkill from matching its own sudo
  wait "$link" 2>/dev/null || true
  kill "$keepalive" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT TERM

sleep 2
./receiver.sh
