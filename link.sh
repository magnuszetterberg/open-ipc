#!/usr/bin/env bash
# wfb-ng link: wfb_tx injects on one adapter, wfb_rx captures on another (same or other machine).
#
#   sudo ./link.sh        both ends on this machine
#   sudo ./link.sh tx     transmitter only (two-machine setup)
#   sudo ./link.sh rx     receiver only
#
# In tx/rx mode, if TX_IF/RX_IF doesn't exist, the first WiFi interface is used.
#
# Video in:  UDP 127.0.0.1:5602  (PORT=5602 ./sender.sh)
# Video out: UDP 127.0.0.1:5600  (./receiver.sh)
# Telemetry out: UDP 127.0.0.1:5610, from an ESP32-CAM's telemetry stream (radio port 16)
# Receiver stats: the video wfb_rx's latest RX_ANT and PKT lines in /tmp/open-ipc-rx.stats
# Ctrl-C hands the adapter(s) back to NetworkManager.
set -euo pipefail
cd "$(dirname "$0")"

ROLE=${1:-${ROLE:-both}}   # both | tx | rx
TX_IF=${TX_IF:-wlan0}       # built-in Intel card: can send (tested on 2.4 GHz), can't receive
RX_IF=${RX_IF:-wlan1}       # Ralink RT5572 USB stick: receives well
CHANNEL=${CHANNEL:-6}       # 2.4 GHz: Intel cards won't transmit on 5 GHz channel 36 ("No IR")
MCS=${MCS:-3}               # HT20 MCS3 = 26 Mbit/s on air
FEC_K=${FEC_K:-8}           # 8 data packets ...
FEC_N=${FEC_N:-12}          # ... + 4 parity per block
TX_POWER=${TX_POWER:-500}   # mBm; 5 dBm suits both ends on one machine (tx.sh uses 20 dBm)
IN_PORT=${IN_PORT:-5602}
OUT_PORT=${OUT_PORT:-5600}
TELEMETRY_PORT=${TELEMETRY_PORT:-5610}
TELEMETRY_RADIO_PORT=16      # ESP32-CAM 0's telemetry (docs/DESIGN.md: 0x10 + camera index)
STATS_FILE=${STATS_FILE:-/tmp/open-ipc-rx.stats}
WFB=wfb-ng

[[ $EUID -eq 0 ]] || { echo "run with sudo" >&2; exit 1; }

# $1 if that interface exists, else the first WiFi interface on this machine.
pick() {
  if [[ -e /sys/class/net/$1 ]]; then echo "$1"; else iw dev | awk '/Interface/ {print $2; exit}'; fi
}

case $ROLE in
  both) ifaces=("$TX_IF" "$RX_IF") ;;
  tx)   TX_IF=$(pick "$TX_IF"); ifaces=("$TX_IF") ;;
  rx)   RX_IF=$(pick "$RX_IF"); ifaces=("$RX_IF") ;;
  *)    echo "unknown ROLE: $ROLE" >&2; exit 1 ;;
esac

monitor() {
  nmcli device set "$1" managed no 2>/dev/null || true
  ip link set "$1" down
  iw dev "$1" set type monitor
  ip link set "$1" up
  iw dev "$1" set channel "$CHANNEL" HT20
}

pids=()
restore() {
  trap '' INT TERM
  kill "${pids[@]}" 2>/dev/null || true
  wait 2>/dev/null || true
  for i in "${ifaces[@]}"; do
    ip link set "$i" down || true
    iw dev "$i" set type managed || true
    ip link set "$i" up || true
    nmcli device set "$i" managed yes 2>/dev/null || true
  done
  echo "adapters restored"
}
trap restore EXIT
trap 'exit 130' INT TERM

for i in "${ifaces[@]}"; do
  monitor "$i"
done

if [[ $ROLE != tx ]]; then
  iw dev "$RX_IF" info | grep -E 'Interface|type|channel'
  # Its stats lines also go to $STATS_FILE: the latest RX_ANT (signal) and PKT (loss) pair, for burn-in.
  "$WFB/wfb_rx" -K keys/gs.key -p 0 -c 127.0.0.1 -u "$OUT_PORT" "$RX_IF" \
    > >(tee >(awk -v f="$STATS_FILE" '/\tRX_ANT\t/ {ant = $0} /\tPKT\t/ {print ant > f; print > f; close(f)}') \
        | sed -u 's/^/[rx] /') 2>&1 &
  pids+=($!)
  # Telemetry: its per-second stats would crowd the terminal, so only its errors show.
  "$WFB/wfb_rx" -K keys/gs.key -p "$TELEMETRY_RADIO_PORT" -c 127.0.0.1 -u "$TELEMETRY_PORT" "$RX_IF" \
    > /dev/null 2> >(sed -u 's/^/[telemetry] /') &
  pids+=($!)
fi
if [[ $ROLE != rx ]]; then
  iw dev "$TX_IF" set txpower fixed "$TX_POWER" 2>/dev/null || echo "note: $TX_IF ignored the txpower setting"
  iw dev "$TX_IF" info | grep -E 'Interface|type|channel|txpower'
  "$WFB/wfb_tx" -K keys/drone.key -p 0 -u "$IN_PORT" -k "$FEC_K" -n "$FEC_N" -B 20 -M "$MCS" "$TX_IF" > >(sed -u 's/^/[tx] /') 2>&1 &
  pids+=($!)
fi

echo "link up ($ROLE, channel $CHANNEL). Video in: UDP :$IN_PORT, out: UDP :$OUT_PORT. Telemetry out: UDP :$TELEMETRY_PORT. Ctrl-C to stop."
wait
