#!/usr/bin/env python3
"""The base station's web view, served by this PC from what its own WiFi adapter hears (./ipc view radio).

link.sh rx with CAMS="0 1 2" delivers camera N's RTP/JPEG video to UDP 5600 + N and its telemetry to
5610 + N, and keeps its receiving wfb_rx's latest stats lines in /tmp/open-ipc-rx.camN.stats. This puts
the JPEGs back together (RFC 2435, as the ESP32 cameras send them: tables in each frame's first packet)
and serves the base station's own page (esp32/base/main/web/index.html) with what it asks for:
/camN.mjpg, /camN.data and /status.json. A frame with a packet missing is dropped whole, never shown cut
off. A payload that isn't RTP is the camera's data (the radar's, docs/DESIGN.md M9): /camN.data passes it
on as the base station does, a 16-bit little-endian length and then the payload, for the page to draw.

Usage: webview.py [--cams 3] [--video-port 5600] [--telemetry-port 5610]
                  [--stats /tmp/open-ipc-rx.cam{}.stats] [--bind 127.0.0.1] [--http-port 8080]
--bind 0.0.0.0 shows it to other devices on the network too.
"""
import argparse
import collections
import http.server
import json
import os
import select
import socket
import struct
import threading
import time

from telemetry_overlay import read_signal

STALE_S = 3  # older than this, a camera's video or telemetry counts as gone
PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "esp32", "base", "main", "web", "index.html")

# The JPEG standard's Huffman tables (ITU T.81 Annex K.3) as DHT segment bodies, which RFC 2435 receivers
# assume: the same bytes as esp32/components/rtp_jpeg/jpeg_tables.cpp.
DHT = bytes.fromhex(
    "0000010501010101010100000000000000000102030405060708090a0b"
    "100002010303020403050504040000017d01020300041105122131410613516107227114328191a1082342b1c11552d1f0"
    "2433627282090a161718191a25262728292a3435363738393a434445464748494a535455565758595a636465666768696a"
    "737475767778797a838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7"
    "c8c9cad2d3d4d5d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9fa"
    "0100030101010101010101010000000000000102030405060708090a0b"
    "1100020102040403040705040400010277000102031104052131061241510761711322328108144291a1b1c109233352f0"
    "156272d10a162434e125f11718191a262728292a35363738393a434445464748494a535455565758595a636465666768696a"
    "737475767778797a82838485868788898a92939495969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7"
    "c8c9cad2d3d4d5d6d7d8d9dae2e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9fa"
)


def jpeg_headers(jtype, width, height, qtables):
    """The headers of the JPEG an RTP/JPEG frame stands for (RFC 2435 appendix A), as rtp_jpeg.cpp makes them."""
    return b"".join([
        b"\xff\xd8",
        b"\xff\xdb", struct.pack(">H", 2 + 2 * 65), b"\x00", qtables[:64], b"\x01", qtables[64:128],
        b"\xff\xc0", struct.pack(">HBHHB", 17, 8, height, width, 3),
        bytes([1, 0x21 if jtype == 0 else 0x22, 0, 2, 0x11, 1, 3, 0x11, 1]),
        b"\xff\xc4", struct.pack(">H", 2 + len(DHT)), DHT,
        bytes([0xff, 0xda, 0, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0]),
    ])


class Camera:
    """One camera: its frames as they are put back together, the latest whole one, and its telemetry."""

    def __init__(self, stats):
        self.stats = stats
        self.changed = threading.Condition()
        self.jpeg = None          # the latest whole frame
        self.number = 0           # counts whole frames, so a viewer knows when there's a new one
        self.heard = 0.0          # time of the latest video packet
        self.viewers = 0
        self.data = collections.deque(maxlen=64)   # the latest data payloads, numbered
        self.data_number = 0
        self.telemetry = None
        self.telemetry_at = 0.0
        self.whole = self.dropped = self.payloads = 0
        self._collecting = False
        self._timestamp = 0
        self._headers = b""
        self._scan = []
        self._size = 0

    def packet(self, p):
        if len(p) < 12 or p[0] & 0xc0 != 0x80:  # not RTP version 2: the camera's data
            self.heard = time.monotonic()
            self.payloads += 1
            with self.changed:
                self.data_number += 1
                self.data.append((self.data_number, p))
                self.changed.notify_all()
            return
        if len(p) < 20 or p[1] & 0x7f != 26:  # RTP, but not JPEG
            return
        self.heard = time.monotonic()
        cc, ext = p[0] & 0x0f, p[0] & 0x10
        j = 12 + 4 * cc
        if ext:
            if len(p) < j + 4:
                return
            j += 4 + 4 * struct.unpack_from(">H", p, j + 2)[0]
        if len(p) < j + 8:
            return
        marker = p[1] & 0x80
        timestamp = struct.unpack_from(">I", p, 4)[0]
        offset = int.from_bytes(p[j + 1:j + 4], "big")
        jtype, q, width, height = p[j + 4], p[j + 5], p[j + 6] * 8, p[j + 7] * 8
        data = p[j + 8:]
        if offset == 0:
            if self._collecting:
                self.dropped += 1  # the previous frame never got its last packet
            self._collecting = False
            # Only in-band 8-bit tables: luma and chroma as the cameras send them, or one for both (ffmpeg)
            qlen = struct.unpack_from(">H", data, 2)[0] if len(data) >= 4 else 0
            if q < 128 or data[1] != 0 or qlen not in (64, 128) or len(data) < 4 + qlen:
                self.dropped += 1
                return
            qtables = data[4:4 + qlen]
            self._headers = jpeg_headers(jtype & 1, width, height, qtables if qlen == 128 else qtables * 2)
            data = data[4 + qlen:]
            self._collecting, self._timestamp, self._scan, self._size = True, timestamp, [], 0
        if not self._collecting:
            return
        if timestamp != self._timestamp or offset != self._size:
            self._collecting = False  # a packet went missing: this frame is dropped whole
            self.dropped += 1
            return
        self._scan.append(data)
        self._size += len(data)
        if marker:
            self._collecting = False
            jpeg = self._headers + b"".join(self._scan) + b"\xff\xd9"
            with self.changed:
                self.jpeg = jpeg
                self.number += 1
                self.changed.notify_all()
            self.whole += 1

    def status(self):
        now = time.monotonic()
        telemetry = None
        if self.telemetry is not None and now - self.telemetry_at < STALE_S:
            telemetry = dict(self.telemetry)
            signal = read_signal(self.stats)
            if signal is not None:
                rssi, loss, mbit = signal
                telemetry.update({k: v for k, v in (("rssi_dbm", rssi), ("loss_pct", loss)) if v is not None})
                if mbit is not None:
                    telemetry["video_kbps"] = round(mbit * 1000)
        return {"video": now - self.heard < STALE_S, "viewers": self.viewers, "telemetry": telemetry}


def listen(cams, video_port, telemetry_port):
    """The UDP sockets link.sh delivers to: camera N's video on video_port + N, its telemetry on telemetry_port + N."""
    socks = {}
    for i, cam in enumerate(cams):
        for port, kind in ((video_port + i, "video"), (telemetry_port + i, "telemetry")):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
            try:
                s.bind(("127.0.0.1", port))
            except OSError as e:
                raise SystemExit(f"webview: can't listen on UDP {port} ({e.strerror}): is another receiver running?")
            socks[s] = (cam, kind)
    return socks


def receive(socks):
    """Video and telemetry from link.sh, for every camera, in one thread."""
    while True:
        for s in select.select(list(socks), [], [])[0]:
            cam, kind = socks[s]
            data = s.recv(65536)
            if kind == "video":
                cam.packet(data)
                continue
            try:
                message = json.loads(data)
            except ValueError:
                continue
            if isinstance(message, dict):
                cam.telemetry, cam.telemetry_at = message, time.monotonic()


def report(cams):
    """Each camera's whole and dropped frames (or its data) every 10 s, so the terminal shows the link works."""
    last = [(0, 0, 0)] * len(cams)
    while True:
        time.sleep(10)
        parts = []
        for i, cam in enumerate(cams):
            whole, dropped, payloads = cam.whole - last[i][0], cam.dropped - last[i][1], cam.payloads - last[i][2]
            last[i] = (cam.whole, cam.dropped, cam.payloads)
            if whole or dropped:
                parts.append(f"cam{i}: {whole / 10:.1f} fps, {dropped} dropped, {cam.viewers} viewers")
            elif payloads:
                parts.append(f"cam{i}: {payloads / 10:.0f} data/s, {cam.viewers} viewers")
        print("  " + (" | ".join(parts) if parts else "no camera heard in the last 10 s"), flush=True)


def handler(cams, page, started):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def send(self, body, kind):
            self.send_response(200)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?")[0]
            if path in ("/", "/index.html"):
                self.send(page, "text/html; charset=utf-8")
            elif path == "/status.json":
                status = {"uptime_s": int(time.monotonic() - started), "reset": "./ipc view radio",
                          "cams": [cam.status() for cam in cams]}
                self.send(json.dumps(status).encode(), "application/json")
            elif path.startswith("/cam") and path[-5:] in (".mjpg", ".data") and path[4:-5].isdigit() \
                    and int(path[4:-5]) < len(cams):
                cam = cams[int(path[4:-5])]
                self.mjpeg(cam) if path.endswith(".mjpg") else self.data(cam)
            else:
                self.send_error(404)

        def gone(self):
            """The viewer closed the connection: a stream with nothing to send would never find out."""
            try:
                if not select.select([self.connection], [], [], 0)[0]:
                    return False
                return self.connection.recv(1, socket.MSG_PEEK) == b""
            except OSError:
                return True

        def mjpeg(self, cam):
            self.close_connection = True
            self.send_response(200)
            self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            with cam.changed:
                cam.viewers += 1
            seen = 0
            try:
                while True:
                    with cam.changed:
                        # The latest whole frame only: a slow viewer skips frames rather than falling behind
                        cam.changed.wait_for(lambda: cam.number != seen, timeout=1)
                        jpeg, number = cam.jpeg, cam.number
                    if number == seen or jpeg is None:
                        if self.gone():
                            break
                        continue
                    seen = number
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n" % len(jpeg)
                                     + jpeg + b"\r\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                with cam.changed:
                    cam.viewers -= 1

        def data(self, cam):
            self.close_connection = True
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            with cam.changed:
                cam.viewers += 1
                seen = cam.data_number   # from the next payload on
            try:
                while True:
                    with cam.changed:
                        cam.changed.wait_for(lambda: cam.data_number != seen, timeout=1)
                        new = [(n, p) for n, p in cam.data if n > seen]
                    if not new:
                        if self.gone():
                            break
                        continue
                    seen = new[-1][0]
                    self.wfile.write(b"".join(struct.pack("<H", len(p)) + p for _, p in new))
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                with cam.changed:
                    cam.viewers -= 1

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cams", type=int, default=3)
    ap.add_argument("--video-port", type=int, default=5600)
    ap.add_argument("--telemetry-port", type=int, default=5610)
    ap.add_argument("--stats", default="/tmp/open-ipc-rx.cam{}.stats")
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--http-port", type=int, default=8080)
    a = ap.parse_args()

    cams = [Camera(a.stats.format(i)) for i in range(a.cams)]
    with open(PAGE, encoding="utf-8") as f:
        page = f.read()
    # The base station's page, named for where it comes from
    page = page.replace("<title>Base station</title>", "<title>Radio view</title>")
    page = page.replace("<h1>Base station</h1>", "<h1>Radio view (this PC's WiFi adapter)</h1>")
    page = page.replace("base station not answering", "not answering (./ipc view radio stopped?)")

    socks = listen(cams, a.video_port, a.telemetry_port)
    try:
        server = http.server.ThreadingHTTPServer((a.bind, a.http_port), handler(cams, page.encode(), time.monotonic()))
    except OSError as e:
        raise SystemExit(f"webview: can't serve on port {a.http_port} ({e.strerror}): HTTP_PORT=<another> ./ipc view radio")
    threading.Thread(target=receive, args=(socks,), daemon=True).start()
    threading.Thread(target=report, args=(cams,), daemon=True).start()
    server.daemon_threads = True
    print(f"web view: http://{'localhost' if a.bind == '127.0.0.1' else a.bind}:{a.http_port}/", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
