#!/usr/bin/env bash
# RTP/UDP video -> screen, tuned for low latency.
# Listens where wfb_rx delivers by default (port 5600).
#
#   ./receiver.sh                 H.264 (sender.sh); GStreamer if all its plugins are installed, else ffplay
#   VIDEO=jpeg ./receiver.sh      RTP/JPEG (RFC 2435), what the ESP32-CAM sends
#   VIDEO=radar ./receiver.sh     the radar camera's data, drawn by radar_render (docs/DESIGN.md, M9)
#   VIDEO=auto ./receiver.sh      whichever arrives: waits for the first packet and looks at its type
#   PLAYER=ffplay ./receiver.sh
set -euo pipefail
cd "$(dirname "$0")"

PORT=${PORT:-5600}
PLAYER=${PLAYER:-auto}    # auto | gst | ffplay
VIDEO=${VIDEO:-h264}      # h264 | jpeg | radar | auto

# auto: RTP payload type 26 is JPEG (the ESP32-CAM), other RTP H.264 (sender.sh uses 96), and a "D1"
# payload the radar camera's data.
if [[ $VIDEO == auto ]]; then
  echo "waiting for video on UDP $PORT..." >&2
  VIDEO=$(python3 -c '
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", int(sys.argv[1])))
while True:
    d = s.recv(2048)
    if len(d) >= 12 and d[0] >> 6 == 2:  # RTP version 2
        print("jpeg" if d[1] & 0x7f == 26 else "h264")
        break
    if d[:2] == b"D1":
        print("radar")
        break
' "$PORT")
  echo "video: ${VIDEO^^}" >&2
fi

if [[ $VIDEO == radar ]]; then
  ./build-radar-render.sh
  # 10 pictures a second, drawn as the radar camera's own picture was
  ./build/radar_render udp "$PORT" | exec ffplay -hide_banner -loglevel warning -window_title "radar :$PORT" \
    -fflags nobuffer -flags low_delay -framedrop -f mjpeg -framerate 10 -i -
fi

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
  *) echo "unknown VIDEO: $VIDEO (h264, jpeg, radar or auto)" >&2; exit 1 ;;
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
