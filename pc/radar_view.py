#!/usr/bin/env python3
"""The radar camera's measurements, live, in a window: is something moving, and how far away?

Usage: pc/radar_view.py [port] [max metres]     (default 5600 and 8 m)
  5600: the base station sends camera 0 there, as does link.sh rx.

Reads the radar's "R1" payloads (esp32/radar/src/radar.hpp), one per measurement, 20 a second. The still
background (walls, furniture) is learned slowly and taken away, so what is left is what moves:
  - the readout: the strongest moving object and its distance;
  - the graph: movement against distance, per receive antenna, with the object marked;
  - the trace: that distance over the last 20 seconds (walking towards the radar draws a falling line).
Distance counts from the radar's own direct signal (transmitter straight into receiver: the strongest still
peak), at 15 cm per tap. The window is ffplay's; close it or Ctrl-C to stop.
"""
import os
import socket
import struct
import subprocess
import sys

import numpy as np

port = int(sys.argv[1]) if len(sys.argv) > 1 else 5600
MAX_M = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0
TAP_M = 0.15            # distance per CIR tap: ~1 ns per tap, half of it there and half back
BACKGROUND = 0.02       # how fast the background follows what the radar sees (per measurement)
THRESHOLD_DB = 3.0      # movement above this counts as an object
NEAR_M = 0.3            # closer than this is the radar's own direct signal, not an object
HISTORY = 400           # measurements in the trace: 20 s

W, H = 800, 500
X0, X1 = 70, W - 30                 # plot area, both panels
G0, G1 = 90, 330                    # graph: movement against distance
T0, T1 = 380, 480                   # trace: distance over time
GRAPH_DB = 15.0                     # top of the graph
COLOURS = [(255, 160, 40), (60, 200, 255), (160, 255, 120)]

FONT = next((f for f in ["/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                         "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"] if os.path.exists(f)), None)
STATE = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "open-ipc-radar")
os.makedirs(STATE, exist_ok=True)
READOUT = os.path.join(STATE, "readout.txt")


def put_text(text):
    tmp = READOUT + ".new"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, READOUT)   # drawtext rereads the file each frame: never let it see half a file


def x_of(metres):
    return int(X0 + (X1 - X0) * min(max(metres, 0), MAX_M) / MAX_M)


def labels():
    """drawtext filters for the fixed labels: distance ticks under both panels, titles."""
    font = f"fontfile={FONT}:" if FONT else ""
    out = [f"drawtext={font}textfile={READOUT}:reload=1:x=20:y=18:fontsize=40:fontcolor=white"]
    step = 1 if MAX_M <= 10 else 2
    for m in range(0, int(MAX_M) + 1, step):     # the graph: distance across
        out.append(f"drawtext={font}text='{m} m':x={x_of(m) - 10}:y={G1 + 6}:fontsize=14:fontcolor=gray")
    for text, x, y in ((f"{MAX_M:g} m", 12, T0), ("0 m", 12, T1 - 16),       # the trace: distance up,
                       ("20 s ago", X0, T1 + 4), ("now", X1 - 28, T1 + 4)):   # time across
        out.append(f"drawtext={font}text='{text}':x={x}:y={y}:fontsize=14:fontcolor=gray")
    out.append(f"drawtext={font}text='movement by distance':x={X0}:y={G0 - 22}:fontsize=16:fontcolor=gray")
    out.append(f"drawtext={font}text='distance of what moves':x={X0}:y={T0 - 22}:fontsize=16:fontcolor=gray")
    return ",".join(out)


def line(img, xs, ys, colour, thick=2):
    """A polyline through (xs, ys): each column filled between its point and the next."""
    for i in range(len(xs) - 1):
        x, y0, y1 = xs[i], ys[i], ys[i + 1]
        lo, hi = min(y0, y1), max(y0, y1)
        img[lo:hi + thick, x:max(xs[i + 1], x + 1)] = colour


put_text("waiting for the radar...")
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(("0.0.0.0", port))
print(f"waiting for the radar on UDP {port}...", file=sys.stderr)
player = subprocess.Popen(
    ["ffplay", "-loglevel", "error", "-fflags", "nobuffer", "-flags", "low_delay",
     "-window_title", "radar: what moves, and how far away",
     "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", f"{W}x{H}", "-framerate", "20",
     "-i", "-", "-vf", labels()], stdin=subprocess.PIPE)

background = None
zero_tap = None
trace = np.full(HISTORY, np.nan)
seen = 0
received = lost = 0
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
        rows = np.frombuffer(data, np.uint8, count * (1 + taps), 8).reshape(count, 1 + taps)
        strength = rows[:, 1:].astype(np.float32) / 2          # dB, per antenna and tap
        gap = (seq - last_seq - 1) & 0xFFFF if last_seq is not None else 0
        lost += gap if gap < 1000 else 0                       # a big jump: the radar restarted
        last_seq = seq
        received += 1

        if background is None or background.shape != strength.shape:
            background = strength.copy()
            seen = 0
        movement = np.abs(strength - background)
        background += BACKGROUND * (strength - background)
        seen += 1
        # The radar's own direct signal is the strongest still peak: distance 0
        if seen == 50 or zero_tap is None:
            zero_tap = int(np.argmax(background.mean(axis=0)))
        metres = (np.arange(taps) - zero_tap) * TAP_M

        # The strongest movement, beyond the direct signal, smoothed over a few taps
        combined = np.convolve(movement.mean(axis=0), np.ones(3) / 3, mode="same")
        usable = (metres >= NEAR_M) & (metres <= MAX_M)
        found = None
        if seen > 20 and usable.any():
            i = int(np.argmax(np.where(usable, combined, -1)))
            if combined[i] >= THRESHOLD_DB:
                found = metres[i]
        if found is not None:
            smoothed = found if smoothed is None else smoothed + 0.4 * (found - smoothed)
            put_text(f"moving object at {smoothed:.2f} m")
        else:
            smoothed = None
            put_text("nothing moving" if seen > 20 else "learning the room... keep still")
        trace = np.roll(trace, -1)
        trace[-1] = smoothed if smoothed is not None else np.nan

        img = np.zeros((H, W, 3), np.uint8)
        img[G0:G1, X0:X1] = (18, 18, 24)
        img[T0:T1, X0:X1] = (18, 18, 24)
        for m in range(0, int(MAX_M) + 1):             # metre grid
            img[G0:G1, x_of(m)] = (45, 45, 55)
            img[int(T1 - 1 - (T1 - T0 - 2) * m / MAX_M), X0:X1] = (45, 45, 55)
        y_thr = int(G1 - (G1 - G0) * THRESHOLD_DB / GRAPH_DB)
        img[y_thr, X0:X1:4] = (90, 90, 90)              # the threshold, dotted
        show = np.where((metres >= 0) & (metres <= MAX_M))[0]
        if len(show) > 1:
            xs = [x_of(metres[t]) for t in show]
            for a in range(count):
                ys = [int(G1 - 1 - (G1 - G0 - 2) * min(movement[a, t], GRAPH_DB) / GRAPH_DB) for t in show]
                line(img, xs, ys, COLOURS[a % len(COLOURS)])
        if smoothed is not None:
            x = x_of(smoothed)
            img[G0:G1, x - 1:x + 2] = (255, 255, 255)
        # The trace: distance (near at the bottom) over time (newest on the right)
        for k in np.where(~np.isnan(trace))[0]:
            x = int(X0 + (X1 - X0 - 1) * k / (HISTORY - 1))
            y = int(T1 - 1 - (T1 - T0 - 2) * min(trace[k], MAX_M) / MAX_M)
            img[y - 1:y + 2, x:x + 2] = (255, 255, 255)
        try:
            player.stdin.write(img.tobytes())
            player.stdin.flush()
        except BrokenPipeError:
            break   # the window was closed
        if received % 200 == 0:
            print(f"radar: {received} measurements, {lost} lost; distance 0 at tap {zero_tap}", file=sys.stderr)
except KeyboardInterrupt:
    pass
finally:
    try:
        player.stdin.close()
    except BrokenPipeError:
        pass
    player.wait()
