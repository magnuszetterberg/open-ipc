#!/usr/bin/env bash
# RTP/UDP video -> screen, tuned for low latency.
# Listens where wfb_rx delivers by default (port 5600).
#
#   ./receiver.sh                 H.264 (sender.sh); GStreamer if all its plugins are installed, else ffplay
#   VIDEO=jpeg ./receiver.sh      RTP/JPEG (RFC 2435), what the ESP32-CAM sends
#   PLAYER=ffplay ./receiver.sh
set -euo pipefail
cd "$(dirname "$0")"

PORT=${PORT:-5600}
PLAYER=${PLAYER:-auto}    # auto | gst | ffplay
VIDEO=${VIDEO:-h264}      # h264 | jpeg

case $VIDEO in
  h264)
    caps="application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000"
    decode=(rtph264depay ! h264parse ! avdec_h264 max-threads=1)
    elements=(rtph264depay h264parse avdec_h264)
    rtpmap="96 H264/90000" ;;
  jpeg)
    caps="application/x-rtp,media=video,encoding-name=JPEG,payload=26,clock-rate=90000"
    decode=(rtpjpegdepay ! jpegdec)
    elements=(rtpjpegdepay jpegdec)
    rtpmap="26 JPEG/90000" ;;
  *) echo "unknown VIDEO: $VIDEO" >&2; exit 1 ;;
esac

if [[ $PLAYER == auto ]]; then
  PLAYER=gst
  for el in udpsrc "${elements[@]}" videoconvert autovideosink; do
    gst-inspect-1.0 "$el" &>/dev/null || { PLAYER=ffplay; break; }
  done
fi

case $PLAYER in
  gst)
    exec gst-launch-1.0 -q \
      udpsrc port="$PORT" caps="$caps" \
      ! "${decode[@]}" \
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
m=video $PORT RTP/AVP ${rtpmap%% *}
a=rtpmap:$rtpmap
EOF
    # low_delay also disables frame threading in the decoder, which otherwise adds frames of delay.
    ffplay -hide_banner -loglevel warning -window_title "RX :$PORT" \
      -fflags nobuffer -flags low_delay -framedrop -sync ext \
      -probesize 32 -analyzeduration 0 \
      -protocol_whitelist file,udp,rtp -i "$sdp" ;;
  *) echo "unknown PLAYER: $PLAYER" >&2; exit 1 ;;
esac
