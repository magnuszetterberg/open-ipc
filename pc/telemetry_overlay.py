#!/usr/bin/env python3
"""Keeps a text file with the camera's latest telemetry and the link's signal quality, for burnin.sh:
ffmpeg's drawtext re-reads the file every frame. The file is replaced whole, never half-written.

Telemetry: the ESP32-CAM's JSON messages on UDP (link.sh, or the base station, delivers them to 5610).
Signal: the base station adds rssi_dbm, loss_pct and video_kbps to each message; on a PC receiving with
link.sh, the receiving wfb_rx's latest RX_ANT and PKT lines (link.sh keeps them in /tmp/open-ipc-rx.stats).

Radar: with --radar-port, the radar camera's telemetry (its own port, 5610 + its camera number) adds a line
with what it finds: "RADAR moving at 2.31 m", or "RADAR nothing moving".

Usage: telemetry_overlay.py --out FILE [--port 5610] [--bind 127.0.0.1] [--stats /tmp/open-ipc-rx.stats]
                            [--radar-port 5612]
--bind 0.0.0.0 for the base station, which sends over the LAN; link.sh delivers to 127.0.0.1.
"""
import argparse
import json
import os
import select
import socket
import time

STALE_S = 3  # older than this, a source counts as gone


def read_signal(path):
    """RSSI (dBm), payload loss (%) and bitrate (Mbit/s) from wfb_rx's stats lines, or None."""
    try:
        if time.time() - os.path.getmtime(path) > STALE_S:
            return None
        rssi = loss = mbit = None
        with open(path) as f:
            for line in f:
                fields = line.rstrip("\n").split("\t")
                if len(fields) >= 5 and fields[1] == "RX_ANT":
                    rssi = int(fields[4].split(":")[2])  # count:min:avg:max:...
                elif len(fields) >= 3 and fields[1] == "PKT":
                    p = [int(x) for x in fields[2].split(":")]  # all:bytes:dec_err:session:data:uniq:fec_rec:lost:bad:out:out_bytes
                    lost, out, out_bytes = p[7], p[9], p[10]
                    loss = 100.0 * lost / (lost + out) if lost + out else 0.0
                    mbit = out_bytes * 8 / 1e6  # wfb_rx reports once a second
        return rssi, loss, mbit
    except (OSError, ValueError, IndexError):
        return None


def uptime(s):
    return f"{s // 3600:02d}:{s % 3600 // 60:02d}:{s % 60:02d}"


def radar_line(r):
    if r is None:
        return "RADAR --  NO TELEMETRY"
    if r.get("learning"):
        return "RADAR learning the room"
    if r.get("moving"):
        return f"RADAR moving at {r.get('distance_m', 0):.2f} m"
    return "RADAR nothing moving"


def compose(t, signal, radar=None, with_radar=False):
    lines = []
    if t is None:
        lines.append("CAM --  NO TELEMETRY")
    else:
        temp = t.get("temp_c")
        lines.append(f"CAM {t.get('cam', '?')}  UP {uptime(int(t.get('uptime_s', 0)))}  "
                     f"{t.get('fps', 0)} FPS  {t.get('frame_kb', 0):.1f} KB  "
                     f"TEMP {'--' if temp is None else f'{temp:.0f}C'}")
    if signal is None or signal[0] is None:
        lines.append("SIG --  NO RECEIVER STATS")
    else:
        rssi, loss, mbit = signal
        tx = f"  TX {t['tx_dbm']:.1f} dBm" if t and "tx_dbm" in t else ""
        lines.append(f"SIG {rssi} dBm  LOSS {loss:.1f}%  {mbit:.1f} Mbit/s{tx}")
    if with_radar:
        lines.append(radar_line(radar))
    elif t is not None and t.get("radar"):   # this camera is the radar itself
        lines.append(radar_line(t))
    return "\n".join(lines)


def write_atomic(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--port", type=int, default=5610)
    ap.add_argument("--bind", default="127.0.0.1", help="address to listen on: 0.0.0.0 for the base station")
    ap.add_argument("--stats", default="/tmp/open-ipc-rx.stats")
    ap.add_argument("--radar-port", type=int, help="the radar camera's telemetry port, for its finding")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    radar_sock = None
    if args.radar_port:
        radar_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        radar_sock.bind((args.bind, args.radar_port))
    socks = [s for s in (sock, radar_sock) if s]

    telemetry, telemetry_at = None, 0.0
    radar, radar_at = None, 0.0
    last = None
    while True:
        for s in select.select(socks, [], [], 0.25)[0]:
            try:
                message = json.loads(s.recv(2048))
            except ValueError:
                continue  # not JSON: keep the last good message
            if s is sock:
                telemetry, telemetry_at = message, time.monotonic()
            else:
                radar, radar_at = message, time.monotonic()
        current = telemetry if time.monotonic() - telemetry_at <= STALE_S else None
        current_radar = radar if time.monotonic() - radar_at <= STALE_S else None
        if current is not None and "rssi_dbm" in current:  # through the base station: signal included
            signal = (current["rssi_dbm"], current.get("loss_pct", 0.0), current.get("video_kbps", 0) / 1000)
        else:
            signal = read_signal(args.stats)
        text = compose(current, signal, current_radar, radar_sock is not None)
        if text != last:
            write_atomic(args.out, text)
            last = text


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
