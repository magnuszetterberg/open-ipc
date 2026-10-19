"""Count 802.11 frames on a monitor-mode interface: python3 sniff.py IFACE SECONDS

Also on an interface link.sh already holds in monitor mode (sudo python3 -I pc/sniff.py wlan0 5): it only
listens. For wfb-ng frames it shows the data rate and signal the adapter reports, per sender.
"""
import collections
import socket
import struct
import sys
import time

iface, secs = sys.argv[1], float(sys.argv[2])
s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))  # ETH_P_ALL
s.bind((iface, 0))
s.settimeout(0.5)

# Radiotap fields in the first presence word: (alignment, size), by bit
RADIOTAP_FIELDS = {0: (8, 8), 1: (1, 1), 2: (1, 1), 3: (2, 4), 4: (2, 2), 5: (1, 1), 6: (1, 1), 7: (2, 2),
                   8: (2, 2), 9: (2, 2), 10: (1, 1), 11: (1, 1), 12: (1, 1), 13: (1, 1), 14: (2, 2),
                   15: (2, 2), 16: (1, 1), 17: (1, 1), 18: (4, 8), 19: (1, 3)}


def radiotap_rate_signal(pkt):
    """The rate ("1 Mbit/s", "MCS 3") and signal (dBm) in a radiotap header, where present."""
    present = struct.unpack_from('<I', pkt, 4)[0]
    off = 8
    word = present
    while word & 0x80000000:  # more presence words follow; their fields aren't read here
        word = struct.unpack_from('<I', pkt, off)[0]
        off += 4
    rate = signal = None
    for bit in range(20):
        if not present & (1 << bit):
            continue
        align, size = RADIOTAP_FIELDS[bit]
        off = (off + align - 1) // align * align
        if bit == 2:
            rate = f'{pkt[off] / 2:g} Mbit/s'
        elif bit == 5:
            signal = struct.unpack_from('b', pkt, off)[0]
        elif bit == 19:
            rate = f'MCS {pkt[off + 2]}'
        off += size
    return rate, signal


kinds = collections.Counter()
senders = collections.Counter()
wfb_rates = collections.defaultdict(collections.Counter)
wfb_signal = {}
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
            rate, signal = radiotap_rate_signal(pkt)
            wfb_rates[addr2.hex(':')][rate or '?'] += 1
            if signal is not None:
                wfb_signal[addr2.hex(':')] = signal

print(f'  frames: {total}  ({", ".join(f"{k} {v}" for k, v in kinds.items()) or "none"})')
print(f'  beacons: {beacons}   wfb-ng frames: {wfb}')
for addr, n in senders.most_common(5):
    print(f'    {addr}  {n}')
for addr, rates in wfb_rates.items():
    sig = f', {wfb_signal[addr]} dBm' if addr in wfb_signal else ''
    print(f'  wfb-ng {addr}: {", ".join(f"{r} x{n}" for r, n in rates.most_common())}{sig}')
