#!/usr/bin/env python3
"""The radar camera's measurements, live, in the browser: is something moving, and how far away?

Usage: pc/radar_view.py [port] [web port]     (default 5600 and 8090; opens http://localhost:8090/)
  5600: the base station sends camera 0 there, as does link.sh rx.

Reads the radar's "R1" payloads (esp32/radar/src/radar.hpp), one per measurement, 20 a second. The still
background (walls, furniture) is learned slowly and taken away, so what is left is what moves, counted only
where there is a real reflection, well above the noise floor. The page shows the strongest moving object and
its distance, movement against distance per receive antenna, and that distance over the last 20 seconds.
Distance counts from the radar's own direct signal (transmitter straight into receiver: the strongest still
peak), at 15 cm per tap. Python's standard library and numpy only; Ctrl-C stops it.
"""
import json
import queue
import socket
import struct
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import numpy as np

port = int(sys.argv[1]) if len(sys.argv) > 1 else 5600
web_port = int(sys.argv[2]) if len(sys.argv) > 2 else 8090
TAP_M = 0.15            # distance per CIR tap: ~1 ns per tap, half of it there and half back
BACKGROUND = 0.02       # how fast the background follows what the radar sees (per measurement)
THRESHOLD_DB = 1.5      # movement above this counts as an object
REFLECTION_DB = 12.0    # a tap counts only this far above the noise floor: beyond the reflections, the noise
                        # alone swings by several dB
NEAR_M = 0.3            # closer than this is the radar's own direct signal, not an object
MAX_M = 8.0             # farther than this the SR250-ARD's radar hears mostly noise

PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Radar view</title>
<style>
  :root { color-scheme: dark; --bg: #101318; --panel: #181c23; --grid: #2a303a; --text: #e8eaed; --dim: #8a919c;
          --a0: #ffa028; --a1: #3cc8ff; --mark: #ffffff; }
  html, body { margin: 0; height: 100%; background: var(--bg); color: var(--text);
               font-family: system-ui, sans-serif; }
  body { display: flex; flex-direction: column; padding: 16px; box-sizing: border-box; gap: 10px; }
  #readout { font-size: clamp(24px, 5vw, 48px); font-weight: 600; }
  #status { color: var(--dim); font-size: 14px; }
  canvas { width: 100%; background: var(--panel); border-radius: 8px; display: block; }
  #graph { flex: 3; min-height: 160px; } #trace { flex: 2; min-height: 110px; }
  .legend span { margin-right: 16px; font-size: 14px; }
</style></head><body>
<div id="readout">waiting for the radar...</div>
<div class="legend"><span style="color:var(--a0)">&#9644; antenna 1</span><span style="color:var(--a1)">&#9644;
antenna 2</span><span style="color:var(--dim)">movement against distance; dotted: detection threshold</span></div>
<canvas id="graph"></canvas>
<div class="legend"><span style="color:var(--dim)">distance of what moves, last 20 s (newest right)</span></div>
<canvas id="trace"></canvas>
<div id="status"></div>
<script>
const css = n => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
const graph = document.getElementById('graph'), trace = document.getElementById('trace');
const HISTORY = 400, history = [];
let last = null;
function fit(c) {
  const r = c.getBoundingClientRect(), d = devicePixelRatio || 1;
  if (c.width !== Math.round(r.width * d) || c.height !== Math.round(r.height * d)) {
    c.width = Math.round(r.width * d); c.height = Math.round(r.height * d);
  }
  const g = c.getContext('2d'); g.setTransform(d, 0, 0, d, 0, 0); return [g, r.width, r.height];
}
function axes(g, w, h, maxM, vertical) {
  const L = 44, B = 22, T = 8, R = 12;
  g.font = '12px system-ui'; g.fillStyle = css('--dim'); g.strokeStyle = css('--grid'); g.lineWidth = 1;
  for (let m = 0; m <= maxM; m++) {
    if (vertical) { const y = h - B - (h - B - T) * m / maxM;
      g.beginPath(); g.moveTo(L, y); g.lineTo(w - R, y); g.stroke(); g.fillText(m + ' m', 6, y + 4); }
    else { const x = L + (w - L - R) * m / maxM;
      g.beginPath(); g.moveTo(x, T); g.lineTo(x, h - B); g.stroke(); g.fillText(m + ' m', x - 10, h - 6); }
  }
  return { L, B, T, R };
}
function draw() {
  if (!last) return;
  const maxM = last.max_m;
  let [g, w, h] = fit(graph);
  g.clearRect(0, 0, w, h);
  let a = axes(g, w, h, maxM, false);
  const top = Math.max(6, last.threshold * 4), X = m => a.L + (w - a.L - a.R) * m / maxM,
        Y = v => h - a.B - (h - a.B - a.T) * Math.min(v, top) / top;
  g.setLineDash([4, 4]); g.strokeStyle = css('--dim');
  g.beginPath(); g.moveTo(a.L, Y(last.threshold)); g.lineTo(w - a.R, Y(last.threshold)); g.stroke();
  g.setLineDash([]); g.lineWidth = 2;
  last.movement.forEach((row, i) => {
    g.strokeStyle = css(i ? '--a1' : '--a0'); g.beginPath();
    let started = false;
    row.forEach((v, t) => { const m = last.metres[t]; if (m < 0 || m > maxM) return;
      started ? g.lineTo(X(m), Y(v)) : g.moveTo(X(m), Y(v)); started = true; });
    g.stroke();
  });
  if (last.found !== null) {
    g.strokeStyle = css('--mark'); g.lineWidth = 2; g.beginPath();
    g.moveTo(X(last.found), a.T); g.lineTo(X(last.found), h - a.B); g.stroke();
  }
  [g, w, h] = fit(trace);
  g.clearRect(0, 0, w, h);
  a = axes(g, w, h, maxM, true);
  g.fillStyle = css('--mark');
  history.forEach((m, k) => { if (m === null) return;
    const x = a.L + (w - a.L - a.R) * (k + HISTORY - history.length) / (HISTORY - 1),
          y = h - a.B - (h - a.B - a.T) * Math.min(m, maxM) / maxM;
    g.beginPath(); g.arc(x, y, 2.5, 0, 7); g.fill(); });
}
const events = new EventSource('events');
events.onmessage = e => {
  last = JSON.parse(e.data);
  history.push(last.found); if (history.length > HISTORY) history.shift();
  document.getElementById('readout').textContent = last.found !== null
    ? 'moving object at ' + last.found.toFixed(2) + ' m'
    : (last.learning ? 'learning the room... keep still' : 'nothing moving');
  document.getElementById('status').textContent = last.received + ' measurements, ' + last.lost +
    ' lost; distance 0 at tap ' + last.zero_tap + ' (15 cm per tap, not yet calibrated)';
};
events.onerror = () => { document.getElementById('readout').textContent = 'radar_view.py stopped'; };
(function frame() { draw(); requestAnimationFrame(frame); })();
</script></body></html>
"""

clients = set()
clients_lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/":
            body = PAGE.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            q = queue.Queue(maxsize=50)
            with clients_lock:
                clients.add(q)
            try:
                while True:
                    self.wfile.write(b"data: " + q.get().encode() + b"\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                with clients_lock:
                    clients.discard(q)
        else:
            self.send_error(404)


def publish(event):
    text = json.dumps(event)
    with clients_lock:
        for q in clients:
            if not q.full():   # a slow browser skips measurements rather than falling behind
                q.put(text)


server = ThreadingHTTPServer(("127.0.0.1", web_port), Handler)
server.daemon_threads = True
threading.Thread(target=server.serve_forever, daemon=True).start()
url = f"http://localhost:{web_port}/"
print(f"radar view: {url} (waiting for the radar on UDP {port})", file=sys.stderr)
subprocess.Popen(["xdg-open", url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("0.0.0.0", port))
background = None
recent = []
zero_tap = None
seen = received = lost = 0
last_seq = None
smoothed = None
try:
    while True:
        data = sock.recv(65536)
        if len(data) < 8 or data[:2] != b"R1":
            continue
        seq, count, taps, start = struct.unpack_from("<HBBH", data, 2)
        if len(data) < 8 + count * (1 + taps):
            continue
        strength = np.frombuffer(data, np.uint8, count * (1 + taps), 8).reshape(count, 1 + taps)[:, 1:] / 2.0
        gap = (seq - last_seq - 1) & 0xFFFF if last_seq is not None else 0
        lost += gap if gap < 1000 else 0     # a big jump: the radar restarted
        last_seq = seq
        received += 1

        if background is None or background.shape != strength.shape:
            background = strength.copy()
            recent = []
            seen = 0
        change = np.abs(strength - background)
        background += BACKGROUND * (strength - background)
        seen += 1
        # The radar's own direct signal is the strongest still peak: distance 0
        if seen == 50 or zero_tap is None:
            zero_tap = int(np.argmax(background.mean(axis=0)))
        metres = (np.arange(taps) - zero_tap) * TAP_M
        # Averaged over the last few measurements, and only where there is a reflection: the noise floor is
        # the median level over all distances, per antenna
        recent = (recent + [change])[-3:]
        floor = np.median(background, axis=1, keepdims=True)
        movement = np.where(strength >= floor + REFLECTION_DB, np.mean(recent, axis=0), 0)

        # The strongest movement within range, over both antennas and a few taps
        combined = np.convolve(movement.mean(axis=0), np.ones(3) / 3, mode="same")
        usable = (metres >= NEAR_M) & (metres <= MAX_M)
        found = None
        if seen > 20 and usable.any():
            i = int(np.argmax(np.where(usable, combined, -1)))
            if combined[i] >= THRESHOLD_DB:
                found = float(metres[i])
        smoothed = None if found is None else found if smoothed is None else smoothed + 0.4 * (found - smoothed)
        publish({"metres": np.round(metres, 3).tolist(), "movement": np.round(movement, 2).tolist(),
                 "found": None if smoothed is None else round(smoothed, 3), "threshold": THRESHOLD_DB,
                 "max_m": MAX_M, "learning": seen <= 20, "received": received, "lost": lost,
                 "zero_tap": zero_tap})
except KeyboardInterrupt:
    pass
