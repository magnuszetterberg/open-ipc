#!/usr/bin/env bash
# wfb-ng link: wfb_tx injects on one adapter, wfb_rx captures on another (same or other machine).
#
#   sudo ./link.sh        both ends on this machine
#   sudo ./link.sh tx     transmitter only (two-machine setup)
#   sudo ./link.sh rx     receiver only
#
# The adapters: TX_IF / RX_IF if given, else the ones wifi.sh picks for each role (Intel never receives).
# The keys: your own pair in keys/own/ (./ipc keys) if there is one, else the repo's test keys.
#
# Video in:  UDP 127.0.0.1:5602  (PORT=5602 ./sender.sh)
# Video out: UDP 127.0.0.1:5600  (./receiver.sh)
# Telemetry out: UDP 127.0.0.1:5610, from an ESP32-CAM's telemetry stream (radio port 16)
# Receiver stats: the video wfb_rx's latest RX_ANT and PKT lines in /tmp/open-ipc-rx.stats
# Ctrl-C hands the adapter(s) back to NetworkManager.
set -euo pipefail
cd "$(dirname "$0")"

ROLE=${1:-${ROLE:-both}}   # both | tx | rx
TX_IF=${TX_IF:-}            # empty: picked below (wifi.sh)
RX_IF=${RX_IF:-}
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

source ./wifi.sh

# An adapter for a role: the one named, if it exists, else wifi.sh's pick. $2: one the other end uses.
adapter() {
  local role=$1 named=$2 other=${3:-} found
  if [[ -n $named ]]; then
    [[ -e /sys/class/net/$named ]] || { echo "no WiFi interface $named" >&2; exit 1; }
    echo "$named"
    return
  fi
  found=$(wifi_pick "$role" "$other")
  [[ -n $found ]] || { echo "$(wifi_none "$role")" >&2; exit 1; }
  echo "$found"
}

case $ROLE in
  both) RX_IF=$(adapter rx "$RX_IF"); TX_IF=$(adapter tx "$TX_IF" "$RX_IF"); ifaces=("$TX_IF" "$RX_IF") ;;
  tx)   TX_IF=$(adapter tx "$TX_IF"); ifaces=("$TX_IF") ;;
  rx)   RX_IF=$(adapter rx "$RX_IF"); ifaces=("$RX_IF") ;;
  *)    echo "unknown ROLE: $ROLE" >&2; exit 1 ;;
esac
for i in "${ifaces[@]}"; do echo "using $i ($(wifi_driver "$i"))"; done
GS_KEY=$(key_file gs.key)
DRONE_KEY=$(key_file drone.key)
[[ $GS_KEY == */own/* ]] && echo "keys: your own (keys/own)" || echo "keys: the repo's shared test keys (./ipc keys makes your own)"

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
  "$WFB/wfb_rx" -K "$GS_KEY" -p 0 -c 127.0.0.1 -u "$OUT_PORT" "$RX_IF" \
    > >(tee >(awk -v f="$STATS_FILE" '/\tRX_ANT\t/ {ant = $0} /\tPKT\t/ {print ant > f; print > f; close(f)}') \
        | sed -u 's/^/[rx] /') 2>&1 &
  pids+=($!)
  # Telemetry: its per-second stats would crowd the terminal, so only its errors show.
  "$WFB/wfb_rx" -K "$GS_KEY" -p "$TELEMETRY_RADIO_PORT" -c 127.0.0.1 -u "$TELEMETRY_PORT" "$RX_IF" \
    > /dev/null 2> >(sed -u 's/^/[telemetry] /') &
  pids+=($!)
fi
if [[ $ROLE != rx ]]; then
  iw dev "$TX_IF" set txpower fixed "$TX_POWER" 2>/dev/null || echo "note: $TX_IF ignored the txpower setting"
  iw dev "$TX_IF" info | grep -E 'Interface|type|channel|txpower'
  "$WFB/wfb_tx" -K "$DRONE_KEY" -p 0 -u "$IN_PORT" -k "$FEC_K" -n "$FEC_N" -B 20 -M "$MCS" "$TX_IF" > >(sed -u 's/^/[tx] /') 2>&1 &
  pids+=($!)
fi

echo "link up ($ROLE, channel $CHANNEL). Video in: UDP :$IN_PORT, out: UDP :$OUT_PORT. Telemetry out: UDP :$TELEMETRY_PORT. Ctrl-C to stop."
wait
