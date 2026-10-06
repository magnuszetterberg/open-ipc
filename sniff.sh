#!/usr/bin/env bash
# Listen on a WiFi interface in monitor mode and count what's on air, per channel.
# Channel 36 is the wfb-ng link; channel 128 (home AP) is a control that monitor mode works at all.
#
#   sudo ./sniff.sh [iface] [seconds per channel]
set -euo pipefail
cd "$(dirname "$0")"

IF=${1:-wlan0}
SECS=${2:-5}
CHANNELS=${CHANNELS:-36 128}

[[ $EUID -eq 0 ]] || { echo "run with sudo" >&2; exit 1; }

restore() {
  ip link set "$IF" down || true
  iw dev "$IF" set type managed || true
  ip link set "$IF" up || true
  nmcli device set "$IF" managed yes 2>/dev/null || true
}
trap restore EXIT

nmcli device set "$IF" managed no 2>/dev/null || true
ip link set "$IF" down
iw dev "$IF" set type monitor
ip link set "$IF" up

for ch in $CHANNELS; do
  iw dev "$IF" set channel "$ch" HT20
  echo "channel $ch on $IF, listening ${SECS}s:"
  python3 -I sniff.py "$IF" "$SECS"
done
