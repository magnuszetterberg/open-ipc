#!/usr/bin/env bash
# Webcam (or test pattern) -> H.264 -> RTP/UDP.
# The default target 127.0.0.1:5600 goes straight to receiver.sh on this machine.
# With the wfb-ng link in between, send to link.sh's input with PORT=5602 (tx.sh does this).
#
#   ./sender.sh                      webcam, 720p30, x264
#   SOURCE=test ./sender.sh          test pattern, no camera latency
#   ENCODER=vaapi ./sender.sh        VA-API hardware encoder (e.g. Intel graphics)
#   HOST=192.168.1.20 ./sender.sh    send to another machine
#   PREVIEW=1 ./sender.sh            also show the frames before encoding (closing it stops the sender)
set -euo pipefail
cd "$(dirname "$0")"

SOURCE=${SOURCE:-webcam}    # webcam | test
DEVICE=${DEVICE:-/dev/video0}
SIZE=${SIZE:-1280x720}
FPS=${FPS:-30}
BITRATE=${BITRATE:-4M}
ENCODER=${ENCODER:-x264}    # x264 | vaapi
HOST=${HOST:-127.0.0.1}
PORT=${PORT:-5600}
PREVIEW=${PREVIEW:-0}

case $SOURCE in
  webcam) input=(-fflags nobuffer -f v4l2 -input_format mjpeg -video_size "$SIZE" -framerate "$FPS" -i "$DEVICE") ;;
  test)   input=(-re -f lavfi -i "testsrc2=size=$SIZE:rate=$FPS") ;;
  *)      echo "unknown SOURCE: $SOURCE" >&2; exit 1 ;;
esac

# Burn the wall-clock time (UTC, ms) into each frame as ffmpeg sees it.
overlay="drawtext=textfile=$PWD/overlay.txt:font=monospace:fontsize=40:fontcolor=white:box=1:boxcolor=black@0.7:boxborderw=8:x=20:y=h-th-28"

hw=()
case $ENCODER in
  x264)
    upload="format=yuv420p"
    codec=(-c:v libx264 -preset ultrafast -tune zerolatency -x264-params repeat-headers=1) ;;
  vaapi)
    hw=(-vaapi_device /dev/dri/renderD128)
    upload="format=nv12,hwupload"
    codec=(-c:v h264_vaapi) ;;
  *) echo "unknown ENCODER: $ENCODER" >&2; exit 1 ;;
esac

# No B-frames, 1 s keyframe interval, small packets (wfb-ng payloads must fit one WiFi frame).
stream=(-map "[enc]" "${codec[@]}" -fps_mode passthrough -bf 0 -g "$FPS" -b:v "$BITRATE" -maxrate "$BITRATE"
        -f rtp -payload_type 96 "rtp://$HOST:$PORT?pkt_size=1200")

if [[ $PREVIEW == 1 ]]; then
  # Split after the overlay: one copy to the encoder, one uncompressed to a local window.
  ffmpeg -hide_banner -loglevel warning -stats "${hw[@]}" "${input[@]}" \
    -filter_complex "[0:v]$overlay,split[a][preview];[a]$upload[enc]" \
    "${stream[@]}" \
    -map "[preview]" -c:v rawvideo -pix_fmt yuv420p -fps_mode passthrough -f nut pipe:1 \
  | ffplay -hide_banner -loglevel warning -window_title "TX preview (before encoding)" \
      -fflags nobuffer -flags low_delay -framedrop -sync ext -f nut -i -
else
  exec ffmpeg -hide_banner -loglevel warning -stats "${hw[@]}" "${input[@]}" \
    -filter_complex "[0:v]$overlay,$upload[enc]" \
    "${stream[@]}"
fi
