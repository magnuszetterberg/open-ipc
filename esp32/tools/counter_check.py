#!/usr/bin/env python3
"""Check the ESP32's M1 counter stream as wfb_rx delivers it: prints, once a second, how many
payloads arrived and whether any counter values were missing, repeated or out of order.

Usage: esp32/tools/counter_check.py [port]     (default 5600, link.sh's OUT_PORT)
Run it next to `sudo ./link.sh rx`; rx.sh's video window holds port 5600 itself. Ctrl-C quits.
"""
import re
import socket
import sys
import time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 5600
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", port))
sock.settimeout(0.2)
print(f"listening on 127.0.0.1:{port} for 'esp32 counter <n>' payloads")

pattern = re.compile(rb"^esp32 counter (\d+) ")
last = None
total = {"received": 0, "missing": 0, "repeated": 0, "other": 0}
second = dict.fromkeys(total, 0)
next_report = time.monotonic() + 1

try:
    while True:
        try:
            data = sock.recv(65536)
        except socket.timeout:
            data = None
        if data is not None:
            m = pattern.match(data)
            if not m:
                second["other"] += 1
            else:
                n = int(m.group(1))
                second["received"] += 1
                if last is not None:
                    if n > last + 1:
                        second["missing"] += n - last - 1
                    elif n <= last:
                        second["repeated"] += 1  # repeated, out of order, or the ESP32 restarted
                last = n
        if time.monotonic() >= next_report:
            next_report += 1
            for k in total:
                total[k] += second[k]
            print(f"{second['received']:4d}/s  last {last}  missing {second['missing']}  "
                  f"repeated {second['repeated']}  other {second['other']}  |  total: {total['received']} received, "
                  f"{total['missing']} missing", flush=True)
            second = dict.fromkeys(total, 0)
except KeyboardInterrupt:
    pass
