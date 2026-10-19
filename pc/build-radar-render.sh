#!/usr/bin/env bash
# Build pc/build/radar_render (radar_render.cpp: the radar camera's data drawn as video) if it is missing or
# older than its sources. Needs only a C++ compiler: the drawing is the core's, compiled in.
set -euo pipefail
cd "$(dirname "$0")/.."                       # the repo root
out=pc/build/radar_render
core=esp32/components
srcs=(pc/radar_render.cpp "$core"/radar_picture/*.cpp "$core"/rtp_jpeg/rtp_jpeg.cpp "$core"/rtp_jpeg/jpeg_tables.cpp)

if [[ -x $out && -z $(find "${srcs[@]}" "$core"/radar_picture/include "$core"/rtp_jpeg/include -newer "$out" 2>/dev/null) ]]; then
  exit 0
fi
command -v g++ >/dev/null || { echo "missing to build radar_render: g++ (build-essential, base-devel or gcc-c++)" >&2; exit 1; }
echo "building radar_render..." >&2
mkdir -p pc/build
g++ -O2 -std=c++17 -Wall -I"$core"/radar_picture/include -I"$core"/rtp_jpeg/include "${srcs[@]}" -o "$out"
