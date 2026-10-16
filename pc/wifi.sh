# WiFi adapters for wfb-ng, and the link keys: the one place that decides which adapter sends or receives and
# which key pair is used. Sourced by link.sh (and so by tx.sh and rx.sh) and by ipc.

# What each driver can do in monitor mode, as tested (README: Which WiFi adapter). Intel (iwlwifi) sends on
# 2.4 GHz but can't receive; the Ralink RT5572 (rt2800usb) receives, sending unconfirmed.
WIFI_BOTH_ROLES="rtl88xxau rtl88XXau 88XXau rtl8812au rtl88x2eu 8812eu ath9k_htc"
WIFI_RECEIVES="$WIFI_BOTH_ROLES rt2800usb"
WIFI_SENDS="$WIFI_BOTH_ROLES iwlwifi"

wifi_driver() {
  basename "$(readlink -f "/sys/class/net/$1/device/driver" 2>/dev/null)"
}

wifi_interfaces() {
  local i
  for i in /sys/class/net/*; do
    [[ -d $i/wireless || -d $i/phy80211 ]] && basename "$i"
  done
  return 0
}

# The adapters that can take a role (rx or tx), best first: known to work in it, then not known either way.
# Intel is never offered for rx. $2: an interface to leave out (the other end, on one machine).
wifi_candidates() {
  local role=$1 skip=${2:-} i drv known=() unknown=()
  local works=$WIFI_RECEIVES
  [[ $role == tx ]] && works=$WIFI_SENDS
  for i in $(wifi_interfaces); do
    [[ $i == "$skip" ]] && continue
    drv=$(wifi_driver "$i")
    [[ $role == rx && $drv == iwlwifi ]] && continue
    if [[ " $works " == *" $drv "* ]]; then known+=("$i"); else unknown+=("$i"); fi
  done
  printf '%s\n' "${known[@]}" "${unknown[@]}" | grep . || true
}

# The adapter for a role, or nothing: the best candidate.
wifi_pick() {
  wifi_candidates "$@" | head -1
}

# What to tell someone who has none for a role.
wifi_none() {
  if [[ $1 == rx ]]; then
    echo "no WiFi adapter here can receive (built-in Intel cards can't): plug in a Ralink RT5572, RTL8812AU/EU or AR9271 USB adapter"
  else
    echo "no WiFi adapter here to send with: plug in an RTL8812AU/EU or AR9271 USB adapter"
  fi
}

# Whether an interface is this PC's network connection right now (taking it over cuts the PC off).
wifi_in_use() {
  ip -4 -o addr show dev "$1" 2>/dev/null | grep -q inet
}

# The link keys: your own pair in keys/own/ (./ipc keys; never committed, R9) if there is one, else the
# repo's shared test keys. $1: drone.key (the sending end) or gs.key (the receiving end).
key_file() {
  local dir
  dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/keys
  if [[ -f $dir/own/$1 ]]; then echo "$dir/own/$1"; else echo "$dir/$1"; fi
}
