"""Count 802.11 frames on a monitor-mode interface: python3 sniff.py IFACE SECONDS"""
import collections
import socket
import struct
import sys
import time

iface, secs = sys.argv[1], float(sys.argv[2])
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))  # ETH_P_ALL
s.bind((iface, 0))
s.settimeout(0.5)

kinds = collections.Counter()
senders = collections.Counter()
total = beacons = wfb = 0
end = time.time() + secs
while time.time() < end:
    try:
        pkt = s.recv(4096)
    except socket.timeout:
        continue
    frame = pkt[struct.unpack_from('<H', pkt, 2)[0]:]  # skip the radiotap header
    if len(frame) < 10:
        continue
    total += 1
    ftype, subtype = (frame[0] >> 2) & 3, (frame[0] >> 4) & 0xf
    kinds[{0: 'mgmt', 1: 'ctrl', 2: 'data'}.get(ftype, 'other')] += 1
    if ftype == 0 and subtype == 8:
        beacons += 1
    if ftype != 1 and len(frame) >= 16:
        addr2 = frame[10:16]
        senders[addr2.hex(':')] += 1
        if ftype == 2 and addr2[:2] == b'WB':  # wfb-ng marks its frames with 57:42
            wfb += 1

print(f'  frames: {total}  ({", ".join(f"{k} {v}" for k, v in kinds.items()) or "none"})')
print(f'  beacons: {beacons}   wfb-ng frames: {wfb}')
for addr, n in senders.most_common(5):
    print(f'    {addr}  {n}')
