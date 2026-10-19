#!/usr/bin/env bash
# ESP32-CAM video with its telemetry and signal quality burned in, encoded as H.264.
#
#   ./burnin.sh rtmp://server/live/cam0     push to an RTMP server
#   ./burnin.sh cam0.mkv                    record to a file
#   ./burnin.sh                             show it in a window
#
# Video in: RTP/JPEG on UDP 5600 (sudo ./link.sh rx), or VIDEO_IN=rtsp://... (the base station, M5).
# Telemetry in: UDP 5610 and signal from /tmp/open-ipc-rx.stats, both from link.sh rx; with VIDEO_IN,
# UDP 5610 from the LAN, signal included (the base station sends to 5610 + camera).
# Use it instead of receiver.sh/rx.sh: both listen on port 5600.
#
# With the radar camera (docs/DESIGN.md, M10): RADAR=N adds its finding as a line of text (its telemetry,
# 5610 + N); RADAR_IN also puts its picture beside the camera's (RADAR_VIEW=beside, the default;
# RADAR_VIEW=text for the line only). The radar sends its data, which radar_render draws here:
# RADAR_IN=http://<base>/camN.data from the base station, or udp:PORT from link.sh rx (CAMS="0 2": 5602).
# A radar camera that sends its picture instead: RADAR_IN=rtsp://<base>:8554/camN. ./ipc stream N URL
# --radar M sets them from the base station.
set -euo pipefail
cd "$(dirname "$0")"

OUT=${1:-}
PORT=${PORT:-5600}
VIDEO_IN=${VIDEO_IN:-}
TELEMETRY_PORT=${TELEMETRY_PORT:-5610}
# link.sh delivers on loopback; the base station sends over the LAN.
TELEMETRY_BIND=${TELEMETRY_BIND:-$([[ -n $VIDEO_IN ]] && echo 0.0.0.0 || echo 127.0.0.1)}
STATS_FILE=${STATS_FILE:-/tmp/open-ipc-rx.stats}
BITRATE=${BITRATE:-2M}
FPS=${FPS:-25}               # the camera's rate: 25 at 640x480 and 800x600, 50 at 320x240
RADAR=${RADAR:-}
RADAR_IN=${RADAR_IN:-}
RADAR_VIEW=${RADAR_VIEW:-beside}
HEIGHT=${HEIGHT:-480}        # with the radar's picture beside it, both are scaled to this height

text=$(mktemp --suffix=.txt)
sdp=$(mktemp --suffix=.sdp)
radar_fifo=$(mktemp -u --suffix=.mjpg)
overlay= render=
cleanup() {
  [[ -n $overlay ]] && kill "$overlay" 2>/dev/null || true
  [[ -n $render ]] && kill "$render" 2>/dev/null || true
  rm -f "$text" "$text.tmp" "$sdp" "$radar_fifo"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

echo "waiting for telemetry..." >"$text"
radar_args=()
[[ -n $RADAR ]] && radar_args=(--radar-port "$((5610 + RADAR))")
python3 telemetry_overlay.py --out "$text" --port "$TELEMETRY_PORT" --bind "$TELEMETRY_BIND" --stats "$STATS_FILE" \
  "${radar_args[@]}" &
overlay=$!
sleep 0.3
kill -0 "$overlay" 2>/dev/null || { echo "telemetry_overlay.py failed to start (is port $TELEMETRY_PORT in use?)" >&2; exit 1; }

if [[ -z $VIDEO_IN ]]; then
  cat >"$sdp" <<EOF
v=0
o=- 0 0 IN IP4 127.0.0.1
s=esp32-cam
c=IN IP4 127.0.0.1
t=0 0
m=video $PORT RTP/AVP 26
a=rtpmap:26 JPEG/90000
EOF
  input=(-protocol_whitelist file,udp,rtp -fflags nobuffer -flags low_delay -probesize 32 -analyzeduration 0 -i "$sdp")
elif [[ $VIDEO_IN == rtsp://* ]]; then
  input=(-rtsp_transport tcp -fflags nobuffer -flags low_delay -i "$VIDEO_IN")
else
  input=(-re -stream_loop -1 -i "$VIDEO_IN")   # a file, played as if live (for trying it out)
fi
if [[ -n $RADAR_IN && $RADAR_VIEW == beside ]]; then
  if [[ $RADAR_IN == rtsp://* ]]; then
    input+=(-rtsp_transport tcp -fflags nobuffer -flags low_delay -i "$RADAR_IN")
  elif [[ $RADAR_IN == http://* || $RADAR_IN == udp:* ]]; then
    # The radar's data, drawn by radar_render at 10 pictures a second, into ffmpeg through a FIFO
    ./build-radar-render.sh
    mkfifo "$radar_fifo"
    if [[ $RADAR_IN == http://* ]]; then
      ./build/radar_render http "$RADAR_IN" >"$radar_fifo" &
    else
      ./build/radar_render udp "${RADAR_IN#udp:}" >"$radar_fifo" &
    fi
    render=$!
    input+=(-f mjpeg -framerate 10 -fflags nobuffer -probesize 65536 -analyzeduration 0 -i "$radar_fifo")
  else
    input+=(-re -stream_loop -1 -i "$RADAR_IN")
  fi
fi

# Re-read the text every frame (reload=1); telemetry_overlay.py replaces it whole, never half-written.
# expansion=none: the text is shown as is (drawtext would read the % in "LOSS 0.0%" as a template).
# Output at a fixed FPS: RTP/JPEG carries only a 90 kHz clock, which x264 would otherwise take as the
# frame rate (and starve each frame of bits).
overlay_filter="drawtext=textfile=$text:reload=1:expansion=none:font=monospace:fontsize=18:fontcolor=white:line_spacing=6"
overlay_filter+=":box=1:boxcolor=black@0.6:boxborderw=6:x=10:y=10"
# The radar's picture beside the camera's: both at HEIGHT, side by side; the radar's 10 pictures a second
# are repeated to the camera's rate, and the text goes over the camera's part.
if [[ -n $RADAR_IN && $RADAR_VIEW == beside ]]; then
  vf=(-filter_complex "[0:v]scale=-2:$HEIGHT,format=yuv420p[cam];[1:v]scale=-2:$HEIGHT,format=yuv420p[radar];[cam][radar]hstack=inputs=2,$overlay_filter")
else
  vf=(-vf "$overlay_filter")
fi
h264=(-c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -b:v "$BITRATE" -maxrate "$BITRATE"
      -bufsize "$BITRATE" -g "$FPS" -r "$FPS")

case $OUT in
  rtmp://*|rtmps://*)
    ffmpeg -hide_banner -loglevel warning -stats "${input[@]}" "${vf[@]}" "${h264[@]}" -f flv "$OUT" ;;
  "")
    ffmpeg -hide_banner -loglevel warning "${input[@]}" "${vf[@]}" \
      -c:v rawvideo -pix_fmt yuv420p -r "$FPS" -f nut pipe:1 \
    | ffplay -hide_banner -loglevel warning -window_title "ESP32-CAM + telemetry" \
        -fflags nobuffer -flags low_delay -framedrop -sync ext -f nut -i - ;;
  *)
    ffmpeg -hide_banner -loglevel warning -stats "${input[@]}" "${vf[@]}" "${h264[@]}" "$OUT" ;;
esac
