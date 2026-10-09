"""Serial monitor that stays open: it lets go of the port while build.sh flashes, then reconnects.

Run it through monitor.sh. Connecting restarts the board, so the whole boot log shows: on Linux,
opening a serial port switches DTR and RTS on, and on the ESP32-CAM-MB that holds the ESP32 in reset,
so the monitor switches both off again, which starts it. The same happens after each flash.
If the board is unplugged it waits and reconnects when it comes back. Ctrl-C quits.

The handshake with build.sh: build.sh creates REQUEST, then takes LOCK exclusively. The monitor
holds LOCK shared while the port is open, so it sees REQUEST, closes the port and drops LOCK,
then waits for REQUEST to go away before opening the port again.
"""
import argparse
import datetime
import fcntl
import os
import sys
import time

import serial

LOCK = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "open-ipc-esp32-port")
REQUEST = LOCK + ".request"


def say(msg):
    sys.stderr.write(f"\n-- {msg}\n")
    sys.stderr.flush()


def open_port(port, baud):
    s = serial.Serial(port, baud, timeout=0.1)  # opening switches DTR and RTS on: the board is held in reset
    s.dtr = False  # IO0 high: normal boot, not download mode
    s.rts = True   # EN low, as esptool's hard reset does
    time.sleep(0.1)
    s.rts = False  # EN high: the board starts
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("port", nargs="?", default=os.environ.get("ESPPORT", "/dev/ttyUSB0"))
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-t", "--timestamps", action="store_true", help="prefix each line with the time it arrived")
    args = ap.parse_args()

    out = sys.stdout.buffer
    at_line_start = True
    lock = open(LOCK, "a")
    last_error = None

    while True:
        if os.path.exists(REQUEST):
            time.sleep(0.05)
            continue
        fcntl.flock(lock, fcntl.LOCK_SH)
        try:
            s = open_port(args.port, args.baud)
        except (serial.SerialException, OSError) as e:
            fcntl.flock(lock, fcntl.LOCK_UN)
            if str(e) != last_error:
                say(f"waiting for {args.port}: {e}")
                last_error = str(e)
            time.sleep(0.5)
            continue

        last_error = None
        say(f"connected to {args.port} at {args.baud} baud")
        try:
            while not os.path.exists(REQUEST):
                data = s.read(s.in_waiting or 1)
                if not data:
                    continue
                if args.timestamps:
                    stamped = bytearray()
                    for b in data:
                        if at_line_start:
                            stamped += datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3].encode() + b" "
                        stamped.append(b)
                        at_line_start = b == 0x0A
                    data = bytes(stamped)
                out.write(data)
                out.flush()
            say("flashing: port released")
        except (serial.SerialException, OSError) as e:
            say(f"lost {args.port}: {e}")
        finally:
            s.close()
            fcntl.flock(lock, fcntl.LOCK_UN)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
