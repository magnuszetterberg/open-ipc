#!/usr/bin/env bash
# ESP32-CAM video with its telemetry and signal quality burned in, encoded as H.264.
#
#   ./burnin.sh rtmp://server/live/cam0     push to an RTMP server
#   ./burnin.sh cam0.mkv                    record to a file
#   ./burnin.sh                             show it in a window
#
# Video in: RTP/JPEG on UDP 5600 (sudo ./link.sh rx), or VIDEO_IN=rtsp://... (the base station, M5).
# Telemetry in: UDP 5610 and signal from /tmp/open-ipc-rx.stats, both from link.sh rx.
# Use it instead of receiver.sh/rx.sh: both listen on port 5600.
set -euo pipefail
cd "$(dirname "$0")"

OUT=${1:-}
PORT=${PORT:-5600}
VIDEO_IN=${VIDEO_IN:-}
TELEMETRY_PORT=${TELEMETRY_PORT:-5610}
STATS_FILE=${STATS_FILE:-/tmp/open-ipc-rx.stats}
BITRATE=${BITRATE:-2M}
FPS=${FPS:-25}               # the camera's rate: 25 at 640x480 and 800x600, 50 at 320x240

text=$(mktemp --suffix=.txt)
sdp=$(mktemp --suffix=.sdp)
overlay=
cleanup() {
  [[ -n $overlay ]] && kill "$overlay" 2>/dev/null || true
  rm -f "$text" "$text.tmp" "$sdp"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

echo "waiting for telemetry..." >"$text"
python3 telemetry_overlay.py --out "$text" --port "$TELEMETRY_PORT" --stats "$STATS_FILE" &
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
else
  input=(-rtsp_transport tcp -fflags nobuffer -flags low_delay -i "$VIDEO_IN")
fi

# Re-read the text every frame (reload=1); telemetry_overlay.py replaces it whole, never half-written.
# expansion=none: the text is shown as is (drawtext would read the % in "LOSS 0.0%" as a template).
# Output at a fixed FPS: RTP/JPEG carries only a 90 kHz clock, which x264 would otherwise take as the
# frame rate (and starve each frame of bits).
overlay_filter="drawtext=textfile=$text:reload=1:expansion=none:font=monospace:fontsize=18:fontcolor=white:line_spacing=6"
overlay_filter+=":box=1:boxcolor=black@0.6:boxborderw=6:x=10:y=10"
h264=(-c:v libx264 -preset ultrafast -tune zerolatency -pix_fmt yuv420p -b:v "$BITRATE" -maxrate "$BITRATE"
      -bufsize "$BITRATE" -g "$FPS" -r "$FPS")

case $OUT in
  rtmp://*|rtmps://*)
    ffmpeg -hide_banner -loglevel warning -stats "${input[@]}" -vf "$overlay_filter" "${h264[@]}" -f flv "$OUT" ;;
  "")
    ffmpeg -hide_banner -loglevel warning "${input[@]}" -vf "$overlay_filter" \
      -c:v rawvideo -pix_fmt yuv420p -r "$FPS" -f nut pipe:1 \
    | ffplay -hide_banner -loglevel warning -window_title "ESP32-CAM + telemetry" \
        -fflags nobuffer -flags low_delay -framedrop -sync ext -f nut -i - ;;
  *)
    ffmpeg -hide_banner -loglevel warning -stats "${input[@]}" -vf "$overlay_filter" "${h264[@]}" "$OUT" ;;
esac
