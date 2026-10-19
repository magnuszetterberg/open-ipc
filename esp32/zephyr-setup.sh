#!/usr/bin/env bash
# Install the pinned Zephyr, its SDK (the Arm toolchain only) and libsodium for the radar firmware (FRDM-RW612).
# Run as your normal user (not sudo). Safe to run again: it skips what is already in place.
set -euo pipefail
cd "$(dirname "$0")"
source ./zephyr-env.sh

if [[ $EUID -eq 0 ]]; then
  echo "run this as your normal user (no sudo): Zephyr installs into your home directory" >&2
  exit 1
fi

missing=()
for tool in git python3 wget xz; do
  command -v "$tool" >/dev/null || missing+=("$tool")
done
python3 -c 'import venv' 2>/dev/null || missing+=(python-venv)
if (( ${#missing[@]} )); then
  echo "missing to install Zephyr: ${missing[*]}" >&2
  echo "install with one of:" >&2
  echo "  Arch/CachyOS:  sudo pacman -S --needed git python wget xz" >&2
  echo "  Debian/Ubuntu: sudo apt install git python3 python3-venv wget xz-utils" >&2
  echo "  Fedora:        sudo dnf install git python3 wget xz" >&2
  exit 1
fi

# west and Zephyr's Python packages (and cmake) in a venv of their own, next to the workspace
mkdir -p "$ZEPHYR_WS"
if [[ ! -x $ZEPHYR_WS/.venv/bin/west ]]; then
  echo "making a Python venv with west in $ZEPHYR_WS/.venv..."
  python3 -m venv "$ZEPHYR_WS/.venv"
  "$ZEPHYR_WS/.venv/bin/pip" install -q west cmake
fi
west() { "$ZEPHYR_WS/.venv/bin/west" "$@"; }

# Only the modules the RW612 needs: a full west update fetches every vendor's HAL (gigabytes).
if [[ ! -d $ZEPHYR_WS/.west ]]; then
  echo "fetching Zephyr $ZEPHYR_VERSION into $ZEPHYR_WS..."
  west init -m https://github.com/zephyrproject-rtos/zephyr --mr "$ZEPHYR_VERSION" "$ZEPHYR_WS"
fi
cd "$ZEPHYR_WS"
west config manifest.project-filter -- '-.*,+cmsis,+cmsis_6,+hal_nxp,+mbedtls,+tf-psa-crypto,+hostap,+picolibc'
west -q update --narrow -o=--depth=1
have=$(git -C zephyr describe --tags --exact-match 2>/dev/null || echo unknown)
if [[ $have != "$ZEPHYR_VERSION" ]]; then
  echo "$ZEPHYR_BASE holds Zephyr $have, not $ZEPHYR_VERSION; move $ZEPHYR_WS away or set ZEPHYR_WS" >&2
  exit 1
fi
"$ZEPHYR_WS/.venv/bin/pip" install -q -r zephyr/scripts/requirements-base.txt
# The RW612's WiFi firmware: an NXP binary the WiFi driver loads into the chip's WiFi core. It isn't in git;
# west fetches it from NXP and shows its licence.
west blobs fetch hal_nxp --auto-accept >/dev/null

# libsodium, the same release as the ESP32 builds; the radar firmware compiles the parts wfb_core uses
if [[ ! -d $LIBSODIUM_DIR ]]; then
  echo "fetching libsodium $LIBSODIUM_VERSION..."
  git -c advice.detachedHead=false clone -q -b "$LIBSODIUM_VERSION" --depth 1 https://github.com/jedisct1/libsodium.git "$LIBSODIUM_DIR"
fi

# The SDK: the minimal bundle, then only the Arm toolchain and the host tools (dtc)
if [[ ! -d $ZEPHYR_SDK_INSTALL_DIR ]]; then
  echo "fetching the Zephyr SDK $ZEPHYR_SDK_VERSION (Arm toolchain)..."
  sdk=zephyr-sdk-${ZEPHYR_SDK_VERSION}_linux-$(uname -m)_minimal.tar.xz
  dir=$(dirname "$ZEPHYR_SDK_INSTALL_DIR")
  mkdir -p "$dir"
  wget -q -O "$dir/$sdk" "https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v$ZEPHYR_SDK_VERSION/$sdk"
  tar -xf "$dir/$sdk" -C "$dir"
  rm -f "$dir/$sdk"
fi
if [[ ! -d $ZEPHYR_SDK_INSTALL_DIR/gnu/arm-zephyr-eabi && ! -d $ZEPHYR_SDK_INSTALL_DIR/arm-zephyr-eabi ]]; then
  PATH=$ZEPHYR_WS/.venv/bin:$PATH "$ZEPHYR_SDK_INSTALL_DIR/setup.sh" -t arm-zephyr-eabi -h -c >/dev/null   # needs cmake
fi

# Flashing goes through the board's J-Link, with SEGGER's tools (free, but SEGGER's licence: download them yourself)
if [[ -z $JLINK_DIR ]]; then
  echo
  echo "to flash the FRDM-RW612, get SEGGER's J-Link tools: https://www.segger.com/downloads/jlink/"
  echo "  (\"J-Link Software and Documentation Pack\", Linux 64-bit TGZ), and unpack it into $HOME/zephyr/"
fi
# The J-Link is a USB device that only root may open, unless a udev rule lets the serial port group in
jlink=""
for d in /sys/bus/usb/devices/*; do
  [[ $(cat "$d/idVendor" 2>/dev/null) == 1366 ]] && jlink=$(printf /dev/bus/usb/%03d/%03d "$(cat "$d/busnum")" "$(cat "$d/devnum")")
done
if [[ -n $jlink && ! -w $jlink ]] || [[ -z $jlink ]] && ! ls /etc/udev/rules.d/*jlink* >/dev/null 2>&1; then
  group=$(stat -c %G /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1 || true)
  [[ -n $group ]] || group=$(getent group dialout >/dev/null && echo dialout || echo uucp)
  echo
  echo "to reach the J-Link without sudo, once (and be in the $group group, as for the ESP32 boards):"
  echo "  echo 'SUBSYSTEM==\"usb\", ATTR{idVendor}==\"1366\", MODE=\"0660\", GROUP=\"$group\"' | sudo tee /etc/udev/rules.d/60-jlink.rules"
  echo "  sudo udevadm control --reload && sudo udevadm trigger"
fi
echo "Zephyr $ZEPHYR_VERSION ready: build the radar firmware with ./ipc flash radar (or esp32/build.sh radar)"
