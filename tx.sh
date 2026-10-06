#!/usr/bin/env bash
# Transmitter machine in one command: radio link (tx) + webcam sender.
# Ctrl-C stops both and hands the WiFi adapter back.
set -euo pipefail
cd "$(dirname "$0")"

sudo -v                                       # ask for the password up front
while sleep 60; do sudo -n -v; done &         # keep sudo alive for the cleanup
keepalive=$!

sudo env TX_POWER="${TX_POWER:-2000}" ./link.sh tx &
link=$!

cleanup() {
  trap '' INT TERM
  sudo pkill -TERM -f '[l]ink.sh tx' || true  # [l] keeps pkill from matching its own sudo
  wait "$link" 2>/dev/null || true
  kill "$keepalive" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT TERM

sleep 2
PORT=5602 ./sender.sh
