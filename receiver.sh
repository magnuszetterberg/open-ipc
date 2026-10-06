#!/usr/bin/env bash
# RTP/UDP H.264 -> screen, tuned for low latency.
# Listens where wfb_rx delivers by default (port 5600).
#
#   ./receiver.sh                 GStreamer if all its plugins are installed, else ffplay
#   PLAYER=ffplay ./receiver.sh
set -euo pipefail
cd "$(dirname "$0")"

PORT=${PORT:-5600}
PLAYER=${PLAYER:-auto}    # auto | gst | ffplay

if [[ $PLAYER == auto ]]; then
  PLAYER=gst
  for el in udpsrc rtph264depay h264parse avdec_h264 videoconvert autovideosink; do
    gst-inspect-1.0 "$el" &>/dev/null || { PLAYER=ffplay; break; }
  done
fi

case $PLAYER in
  gst)
    exec gst-launch-1.0 -q \
      udpsrc port="$PORT" caps="application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000" \
      ! rtph264depay ! h264parse ! avdec_h264 max-threads=1 \
      ! videoconvert ! autovideosink sync=false ;;
  ffplay)
    sdp=$(mktemp --suffix=.sdp)
    trap 'rm -f "$sdp"' EXIT
    cat >"$sdp" <<EOF
v=0
o=- 0 0 IN IP4 127.0.0.1
s=open-ipc-test
c=IN IP4 127.0.0.1
t=0 0
m=video $PORT RTP/AVP 96
a=rtpmap:96 H264/90000
EOF
    # low_delay also disables frame threading in the decoder, which otherwise adds frames of delay.
    ffplay -hide_banner -loglevel warning -window_title "RX :$PORT" \
      -fflags nobuffer -flags low_delay -framedrop -sync ext \
      -probesize 32 -analyzeduration 0 \
      -protocol_whitelist file,udp,rtp -i "$sdp" ;;
  *) echo "unknown PLAYER: $PLAYER" >&2; exit 1 ;;
esac
