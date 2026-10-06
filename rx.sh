#!/usr/bin/env bash
# Receiver machine in one command: radio link (rx) + video window.
# Ctrl-C, closing the window, or the link stopping ends everything and hands the WiFi back.
set -euo pipefail
cd "$(dirname "$0")"

./build.sh                                    # fetch + build wfb-ng on first run
sudo -v                                       # ask for the password up front
while sleep 60; do sudo -n -v; done &         # keep sudo alive for the cleanup
keepalive=$!

link= app=
cleanup() {
  trap '' INT TERM
  if [[ $app ]]; then
    pkill -TERM -P "$app" 2>/dev/null || true
    kill "$app" 2>/dev/null || true
  fi
  sudo pkill -TERM -f '[l]ink.sh rx' || true  # [l] keeps pkill from matching its own sudo
  if [[ $link ]]; then wait "$link" 2>/dev/null || true; fi
  pkill -P "$keepalive" 2>/dev/null || true; kill "$keepalive" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT TERM

sudo ./link.sh rx &
link=$!
sleep 2
./receiver.sh &
app=$!

wait -n "$link" "$app" || true                # whichever stops first ends the session
