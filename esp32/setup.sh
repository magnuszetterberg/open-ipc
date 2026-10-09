#!/usr/bin/env bash
# Install the pinned ESP-IDF and its tools for the ESP32 firmware. Run as your normal user (not sudo).
# Safe to run again: it skips what is already in place.
set -euo pipefail
cd "$(dirname "$0")"
source ./idf-env.sh                           # IDF_VERSION, IDF_PATH, IDF_TOOLS_PATH

if [[ $EUID -eq 0 ]]; then
  echo "run this as your normal user (no sudo): ESP-IDF installs into your home directory" >&2
  exit 1
fi

missing=()
for tool in git python3 wget flex bison; do
  command -v "$tool" >/dev/null || missing+=("$tool")
done
python3 -c 'import venv' 2>/dev/null || missing+=(python-venv)
if (( ${#missing[@]} )); then
  echo "missing to install ESP-IDF: ${missing[*]}" >&2
  echo "install with one of:" >&2
  echo "  Arch/CachyOS:  sudo pacman -S --needed git python wget flex bison libusb" >&2
  echo "  Debian/Ubuntu: sudo apt install git python3 python3-venv wget flex bison libusb-1.0-0" >&2
  echo "  Fedora:        sudo dnf install git python3 wget flex bison libusbx" >&2
  exit 1
fi

if [[ ! -d $IDF_PATH ]]; then
  echo "fetching ESP-IDF $IDF_VERSION into $IDF_PATH..."
  mkdir -p "$(dirname "$IDF_PATH")"
  git -c advice.detachedHead=false clone -q -b "$IDF_VERSION" --depth 1 --recursive --shallow-submodules \
    https://github.com/espressif/esp-idf.git "$IDF_PATH"
fi
have=$(git -C "$IDF_PATH" describe --tags --exact-match 2>/dev/null || echo unknown)
if [[ $have != "$IDF_VERSION" ]]; then
  echo "$IDF_PATH holds ESP-IDF $have, not $IDF_VERSION; move it away or set IDF_PATH" >&2
  exit 1
fi

echo "installing ESP-IDF tools for the esp32 target into $IDF_TOOLS_PATH..."
"$IDF_PATH/install.sh" esp32 >/dev/null
# cmake and ninja from ESP-IDF too, so the build doesn't depend on the system's versions
python3 "$IDF_PATH/tools/idf_tools.py" install cmake ninja >/dev/null

./idf.sh idf.py --version

# Flashing needs the USB serial adapter: on Arch it belongs to uucp, elsewhere to dialout.
port_group=$(getent group uucp >/dev/null && echo uucp || echo dialout)
if ! id -nG | grep -qw "$port_group"; then
  echo
  echo "to flash without sudo, add yourself to $port_group and log in again:"
  echo "  sudo usermod -aG $port_group $USER"
fi
echo "ESP-IDF $IDF_VERSION ready; build with ./build.sh or from VS Code (Terminal > Run Task)"
