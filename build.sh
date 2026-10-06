#!/usr/bin/env bash
# Fetch and build wfb-ng if it isn't built yet. Run as your normal user (not sudo).
set -euo pipefail
cd "$(dirname "$0")"

[[ -x wfb-ng/wfb_rx && -x wfb-ng/wfb_tx ]] && exit 0

if [[ ! -f wfb-ng/Makefile ]]; then
  echo "fetching wfb-ng..."
  git submodule update --init --depth 1 wfb-ng
fi

missing=()
for tool in make gcc g++ pkg-config; do
  command -v "$tool" >/dev/null || missing+=("$tool")
done
for lib in libpcap libsodium; do
  pkg-config --exists "$lib" 2>/dev/null || missing+=("$lib")
done
if (( ${#missing[@]} )); then
  echo "missing to build wfb-ng: ${missing[*]}" >&2
  echo "install with one of:" >&2
  echo "  Arch/CachyOS:  sudo pacman -S --needed base-devel pkgconf libpcap libsodium" >&2
  echo "  Debian/Ubuntu: sudo apt install build-essential pkg-config libpcap-dev libsodium-dev" >&2
  echo "  Fedora:        sudo dnf install gcc gcc-c++ make pkgconf libpcap-devel libsodium-devel" >&2
  exit 1
fi

echo "building wfb-ng..."
make -C wfb-ng -j"$(nproc)" all_bin >/dev/null
echo "wfb-ng built"
