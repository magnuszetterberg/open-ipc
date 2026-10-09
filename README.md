# open-ipc latency playground

Webcam → H.264 → wfb-ng (the OpenIPC FPV radio link) → another computer's screen, built to measure latency.

```
tx machine: webcam → sender.sh → :5602 → wfb_tx → WiFi ))) ((( WiFi → wfb_rx → :5600 → receiver.sh :rx machine
```

## Quick start (the setup that works)

```sh
./tx.sh     # XPS: sends its webcam with its built-in Intel WiFi card (wlan0)
./rx.sh     # NUC: receives with the Ralink USB stick (wlan1)
```

The defaults (channel 6, send on `wlan0`, receive on `wlan1`) match this setup. If a machine doesn't have the named interface, its first WiFi interface is used. Run both as your normal user, not with sudo; they ask for the password themselves. Each one runs its half of the radio link plus the sender or receiver. Everything stops on Ctrl-C, when you close its window, or when the radio link stops. `tx.sh` shows a preview of what is sent and `rx.sh` shows what arrives. The WiFi card is offline while the link runs. wfb-ng is fetched and built on first run.

## Results (2026-10-06)

| | |
|---|---|
| **Working link** | XPS 13 (Intel WiFi) sending on 2.4 GHz channel 6 → NUC with a Ralink RT5572 USB stick receiving |
| **Latency** | about **35 ms** (20–52 ms over 4 photos), from the XPS preview to the NUC window. That covers encoding, radio, decoding and display, but not the webcam's own delay. The XPS preview lags slightly behind the real send moment, so the true figure is a little higher. |

## Results: ESP32-CAM (2026-10-09)

| | |
|---|---|
| **Working link** | AI-Thinker ESP32-CAM (firmware in `esp32/`) sending 640x480 RTP/JPEG on channel 6, HT20 MCS 3 → the same NUC and Ralink stick, unmodified `wfb_rx`, `VIDEO=jpeg ./rx.sh` |
| **Video** | 25 fps, about 10 KB a frame: 175 RTP packets/s, about 1.9 Mbit/s delivered |
| **Link** | about 1 m apart: RSSI -54 dBm, ~295 of ~300 frames/s received, 0-5/s repaired by FEC, none lost |
| **Latency** | not measured yet; it looks acceptable by eye. The clock-photo measurement is part of M3 (#15). |

What the camera sends at each setting (JPEG quality 12, MCS 3, FEC 8/12; measured on the ESP32-CAM, #15):

| frame size | fps | KB/frame | RTP packets/s | frames on air/s, FEC block finished per frame | not finished |
|---|---|---|---|---|---|
| 320x240 | 50 | 4 | 200 | 599 | 298 |
| 640x480 | 25 | 12 | 226 | 604 | 340 |
| 800x600 | 25 | 18 | 326 | 598 | 484 |

What a second ESP32-CAM (running `esp32/base`) received, on the same desk, with both boards' antennas as they are (#23), 2026-10-09. Each row averages 11 s. "Heard" is the share of frames on air that reached the receiver; "lost" is packets FEC couldn't repair, per second and as a share of the RTP packets sent.

| setting | RSSI | heard | received fps (of sent) | lost |
|---|---|---|---|---|
| 640x480, MCS 3, FEC 8/12, block finished per frame | -81 dBm | 94% | 24.5 of 25 | 6/s (3%) |
| 640x480, MCS 3, FEC 8/12 | -81 dBm | 89% | 24.5 of 25 | 9/s (5%) |
| 640x480, MCS 1, FEC 8/12 | -83 dBm | 89% | 24.7 of 25 | 3/s (2%) |
| 640x480, MCS 3, FEC 6/12 | -83 dBm | 80% | 24.8 of 25 | 7/s (3%) |
| 320x240, MCS 3, FEC 8/12 | -84 dBm | 22-41% | 15-22 of 50 | most |
| 800x600, MCS 3, FEC 8/12 | -85 dBm | 13-21% | 4 of 25 | most |
| 640x480, MCS 5 | -83 dBm | nothing received | | |

How far to trust it: the link was marginal and drifted. RSSI moved between -81 and -90 dBm over half an hour with nothing changed in the firmware, and by the end the receiver heard almost nothing at any setting. So the rows compare only roughly. What holds:

- At about -81 dBm, 640x480 at 25 fps gets through with a few percent of packets lost after FEC.
- MCS 5 needs a stronger signal than these boards manage; MCS 1 copes best when the signal is weak.
- More FEC (6/12) repairs more but puts a third more frames on air for the same video.
- The rest is set by the antennas: the NUC's Ralink stick hears the same camera at -54 dBm, the second ESP32-CAM at -81 to -90 dBm.

Latency is not measured yet: it needs the clock-photo method (Measure latency, below).

An earlier run lost 10-20% of frames at -74 dBm; a scan from the ESP32-CAM heard the home router 28 dB weaker than the NUC did, which points at the board's antenna (#23).

What we learned about the hardware:

- **Intel cards can send but can't receive.** The NUC's Intel AX201 captured nothing in monitor mode, not even beacons from the home router (`sniff.sh` showed 0 frames). The XPS's Intel card also got 0 packets as a receiver. Sending from the XPS's Intel card works.
- **Intel cards won't transmit on 5 GHz channel 36.** It's marked "No IR" (no initiating radiation) for them, so use 2.4 GHz (`CHANNEL=6`) when an Intel card sends.
- **The Ralink RT5572 receives well.** Its `rt2800usb` driver is built into the kernel. Whether it also transmits is unconfirmed: on channel 36 it accepted around 775 packets/s, but the only listener was an Intel card, which can't hear anything.
- **Each end of a wfb-ng link needs a card that works in that role.** The best choice is still a pair of RTL8812AU/EU or AR9271 adapters, which handle both roles.

## Setup

```sh
git clone --recursive https://github.com/magnuszetterberg/open-ipc.git
cd open-ipc
make -C wfb-ng all_bin        # optional, tx.sh/rx.sh do this; needs libpcap and libsodium
```

`keys/` holds test keys derived from the password `change-me` (`cd keys && ../wfb-ng/wfb_keygen change-me`). The same password always gives the same pair, so every clone can talk to every other. The transmitter uses `drone.key` and the receiver uses `gs.key`. For anything beyond desk tests, generate your own pair with a real password (or none, for a random pair) and copy it to both machines. Anyone with these test keys can read and inject into the link.

## Options

Pass these as environment variables in front of `./tx.sh` / `./rx.sh`:

| variable | default | meaning |
|---|---|---|
| `CHANNEL` | 6 | WiFi channel, must match on both ends; keep it on 2.4 GHz when an Intel card sends |
| `TX_IF` / `RX_IF` | wlan0 / wlan1 | WiFi interface to use; if missing, the machine's first WiFi interface |
| `TX_POWER` | 2000 | transmit power in mBm (2000 = 20 dBm); some cards ignore it |
| `MCS` | 3 | radio data rate (HT20 MCS 3 = 26 Mbit/s) |

`link.sh rx` also receives an ESP32-CAM's telemetry stream (radio port 16) and delivers it to UDP 5610, and keeps the video receiver's latest signal stats in `/tmp/open-ipc-rx.stats`. To burn them into the video: `sudo ./link.sh rx` in one terminal, then `./burnin.sh rtmp://<server>/<app>/<name>` (or a file name, or nothing for a window) instead of `receiver.sh`. `burnin.sh` reads `FPS=25`, `BITRATE=2M`, `PORT`, `TELEMETRY_PORT`, `STATS_FILE`, and `VIDEO_IN=rtsp://...` for the base station's streams.

`sender.sh` also reads `SOURCE=webcam|test`, `ENCODER=x264|vaapi`, `SIZE=1280x720`, `FPS=30`, `BITRATE=4M`, `DEVICE` and `PREVIEW=1`. `receiver.sh` reads `PLAYER=auto|gst|ffplay` and `VIDEO=h264|jpeg` (`jpeg` for the ESP32-CAM's RTP/JPEG; `VIDEO=jpeg ./rx.sh` passes it on). The GStreamer receiver needs gst-plugins-base, -good, -bad and gst-libav; if any are missing, it falls back to ffplay.

## Pieces

| script | does |
|---|---|
| `tx.sh` / `rx.sh` | one command per machine (link + sender/receiver) |
| `link.sh [tx\|rx]` | puts the card in monitor mode and runs `wfb_tx` / `wfb_rx` (needs sudo); with no argument, runs both ends on one machine with two cards (untested: on the NUC that means Intel sending, Ralink receiving) |
| `sender.sh` | webcam or test pattern → H.264 → RTP; burns a `TX hh:mm:ss.mmm` timestamp into each frame |
| `receiver.sh` | RTP → low-latency video window |
| `burnin.sh` | ESP32-CAM video with its telemetry and signal quality burned in, as H.264: to RTMP, a file, or a window |
| `telemetry_overlay.py` | keeps the overlay text for `burnin.sh`: telemetry from UDP 5610, signal from `link.sh`'s stats file |
| `build.sh` | fetches and builds wfb-ng if needed |
| `sniff.sh [iface]` | counts what a card hears in monitor mode on channel 36 (wfb-ng) and 128 (a router, as a control) |
| `clock.html` | millisecond clock for latency photos |

Without a radio link, `./receiver.sh` and `./sender.sh` (port 5600) stream over localhost. That gives a baseline for encoding and decoding alone.

## Measure latency

**Two machines (no clock sync needed):** open `clock.html` on the **sending** machine and photograph it next to the receiving screen. The receiving window shows the sender's clock time burned into the frame, so:

> latency = clock on the sender − `TX` timestamp on the receiver

Comparing the two video windows (tx preview vs rx) is quicker, but it reads a bit low because the preview itself lags.

**Including the webcam:** point the webcam at `clock.html` and put the receiving window next to it. One photo then shows three times:

| difference | measures |
|---|---|
| clock − clock seen through the webcam | full camera-to-screen latency |
| `TX` overlay − clock seen through the webcam | webcam latency (sensor, USB, MJPEG) |
| clock − `TX` overlay | encode + radio + decode + display |

Take several photos and average them. A 60 Hz screen limits each reading to about ±17 ms, and a 30 fps camera to about ±33 ms.

## License

GPL-3.0 (see `LICENSE`), the same as wfb-ng. Files adapted from wfb-ng keep its copyright notice.
