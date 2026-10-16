# open-ipc

Digital video links over [wfb-ng](https://github.com/svpcom/wfb-ng), the long-range WiFi broadcast link that OpenIPC FPV uses, kept compatible with an unmodified upstream `wfb_rx`:

- **ESP32 cameras and a base station.** ESP32-CAM boards send JPEG video and telemetry over wfb-ng. An ESP32 with Ethernet (the Olimex ESP32-POE) receives up to three of them and serves each camera on the LAN: in a web page with its telemetry, as RTSP, and its telemetry as UDP.
- **To a video server.** A PC burns each camera's telemetry and signal quality into its video, encodes H.264 and pushes it to an RTMP server.
- **PC to PC.** A webcam on one Linux PC, encoded as H.264, to another PC's screen, with off-the-shelf WiFi adapters.

```
ESP32-CAM 0..2 ─ wfb-ng, 2.4 GHz ─► base station (ESP32 + Ethernet) ─► LAN: web page, RTSP, telemetry
                                                                      └► PC: telemetry burned in, H.264 ─► RTMP server
ESP32-CAM 0 ─── wfb-ng, 2.4 GHz ─► a PC's own WiFi adapter (no base station)

sending PC: webcam → sender.sh → wfb_tx → WiFi ))) ((( WiFi → wfb_rx → receiver.sh :receiving PC
```

The design and its milestones are in [docs/DESIGN.md](docs/DESIGN.md), the ESP32 firmware in [esp32/](esp32/README.md).

## Start here

Everything below goes through one command, `./ipc`. Run it on its own to see the list.

### 1. Set up this computer (Linux, once)

```sh
git clone --recursive https://github.com/magnuszetterberg/open-ipc.git   # install git first
cd open-ipc
./ipc setup
```

`./ipc setup` installs the ESP32 tools into your home folder (about 2 GB, 10–20 minutes the first time; no sudo). If something is missing it says what, with the install command for Arch, Debian/Ubuntu and Fedora: install it and run `./ipc setup` again. If it asks you to join a group (`uucp` or `dialout`), do that and log out and back in, or flashing can't open the USB port.

### 2. Flash the boards (once each)

```sh
./ipc flash camera 0      # then camera 1 and camera 2, if you have them
./ipc flash base          # only if you use the base station (step 3)
```

Each one asks you to plug the board in, finds its USB port, flashes it, checks that it starts, and says what to do next. Use a USB data cable: with a charge-only cable no port appears. After flashing, a camera only needs power from any USB charger.

### 3. Receive the cameras: pick one way

| | with the base station | with this PC's WiFi adapter |
|---|---|---|
| you need | an Olimex ESP32-POE, with an Ethernet cable to your network | a USB WiFi adapter that receives in monitor mode (see Which WiFi adapter) |
| cameras | up to 3 | camera 0 |
| watch | `./ipc view`: every camera in a browser, on any device on the network | `./ipc receive`: camera 0 in a window on this PC |
| to a video server, telemetry burned in | `./ipc stream 0 rtmp://server/app/cam0` | `./ipc receive rtmp://server/app/cam0` |
| record to a file instead | `./ipc stream 0 cam0.mkv` | `./ipc receive cam0.mkv` |

With the base station, `./ipc flash base` sends the telemetry to the PC it runs on, and remembers the base station's address for `view` and `stream`. The base station has room for about three video streams at once in total: a browser showing three cameras, or a mix of browsers and streams.

With this PC's adapter, `./ipc receive` picks the adapter itself and asks for your password (the adapter is switched to monitor mode). While it receives, the adapter can't be used for the network; if it's the PC's connection, it asks first. Ctrl-C gives it back.

### Something wrong?

| what you see | what to do |
|---|---|
| `flash` waits and finds no board | use a USB data cable; or name the port: `./ipc flash camera 1 --port /dev/ttyUSB0` |
| flashing says permission denied | join the group `./ipc setup` names, then log out and back in |
| a board doesn't do what you expect | `./ipc log` shows what it's doing |
| "NO TELEMETRY" on the video | the base station sends telemetry to the PC that ran `./ipc flash base`; run it again on this one. A firewall must let it in: `sudo ufw allow from <base station> to any port 5600:5612 proto udp` |
| the picture breaks up or stutters | keep each camera a metre or more from the receiver (closer overloads it); weak antennas on the ESP32-CAM board shorten the range |
| `./ipc receive` finds no adapter | built-in Intel cards can't receive; see Which WiFi adapter |

## Which WiFi adapter

The base station and the cameras need nothing extra. A PC needs a WiFi adapter that works in monitor mode in its role:

- **To receive:** a Ralink RT5572 USB stick works (its `rt2800usb` driver is in the kernel). RTL8812AU/EU and AR9271 adapters work too, and can also send. **Built-in Intel cards can't receive** in monitor mode.
- **To send** (the PC-to-PC link): an Intel card can, on 2.4 GHz only (it refuses 5 GHz channel 36), so keep `CHANNEL=6`. RTL8812AU/EU and AR9271 adapters can send as well.

## The PC-to-PC link

```sh
./tx.sh     # on the sending PC: its webcam, over its WiFi adapter
./rx.sh     # on the receiving PC: the video window
```

Run both as your normal user; they ask for the password themselves. Each one runs its half of the radio link plus the sender or receiver, and everything stops on Ctrl-C, when you close its window, or when the radio link stops. `tx.sh` shows a preview of what is sent and `rx.sh` shows what arrives. The WiFi adapter is offline while the link runs. wfb-ng is built on first run (`./ipc setup` says which packages that needs).

By default the sender uses `wlan0` and the receiver `wlan1`, on channel 6. Pass these as environment variables in front of `./tx.sh` / `./rx.sh`:

| variable | default | meaning |
|---|---|---|
| `CHANNEL` | 6 | WiFi channel, must match on both ends; keep it on 2.4 GHz when an Intel card sends |
| `TX_IF` / `RX_IF` | wlan0 / wlan1 | WiFi interface to use; if missing, the machine's first WiFi interface |
| `TX_POWER` | 2000 | transmit power in mBm (2000 = 20 dBm); some cards ignore it |
| `MCS` | 3 | radio data rate (HT20 MCS 3 = 26 Mbit/s) |

`sender.sh` also reads `SOURCE=webcam|test`, `ENCODER=x264|vaapi`, `SIZE=1280x720`, `FPS=30`, `BITRATE=4M`, `DEVICE` and `PREVIEW=1`. `receiver.sh` reads `PLAYER=auto|gst|ffplay` and `VIDEO=h264|jpeg` (`jpeg` for the ESP32-CAM's RTP/JPEG). The GStreamer receiver needs gst-plugins-base, -good, -bad and gst-libav; if any are missing, it falls back to ffplay.

Without a radio link, `./receiver.sh` and `./sender.sh` (port 5600) stream over localhost.

## Keys

`keys/` holds test keys derived from the password `change-me` (`cd keys && ../wfb-ng/wfb_keygen change-me`). The same password always gives the same pair, so every clone can talk to every other. The sender uses `drone.key` (built into the camera firmware) and the receiver uses `gs.key` (built into the base station). For anything beyond desk tests, generate your own pair with a real password (or none, for a random pair), copy it to both ends and flash the boards again. Anyone with these test keys can read and inject into the link.

## Measure latency

**Two machines (no clock sync needed):** open `clock.html` on the **sending** machine and photograph it next to the receiving screen. The receiving window shows the sender's clock time burned into the frame, so:

> latency = clock on the sender − `TX` timestamp on the receiver

Comparing the two video windows (tx preview vs rx) is quicker, but it reads a bit low because the preview itself lags.

**Including the camera:** point the camera at `clock.html` and put the receiving window next to it. One photo then shows three times:

| difference | measures |
|---|---|
| clock − clock seen through the camera | full camera-to-screen latency |
| `TX` overlay − clock seen through the camera | the webcam's own latency (sensor, USB, MJPEG) |
| clock − `TX` overlay | encode + radio + decode + display |

Take several photos and average them. A 60 Hz screen limits each reading to about ±17 ms, and a 30 fps camera to about ±33 ms. For the ESP32-CAM, only the first row applies: it has no `TX` overlay.

## Scripts

`./ipc` covers the everyday tasks; these are what it uses, and work on their own:

| script | does |
|---|---|
| `ipc` | the front door: setup, flash, view, stream, receive, log |
| `tx.sh` / `rx.sh` | one command per PC for the PC-to-PC link (link + sender/receiver) |
| `link.sh [tx\|rx]` | puts the adapter in monitor mode and runs `wfb_tx` / `wfb_rx` (needs sudo); `rx` also receives camera 0's telemetry (UDP 5610) and keeps the latest signal stats in `/tmp/open-ipc-rx.stats` |
| `sender.sh` | webcam or test pattern → H.264 → RTP; burns a `TX hh:mm:ss.mmm` timestamp into each frame |
| `receiver.sh` | RTP → low-latency video window |
| `burnin.sh` | a camera's video with its telemetry and signal quality burned in, as H.264: to RTMP, a file, or a window. Reads `FPS=25`, `BITRATE=2M`, `PORT`, `TELEMETRY_PORT`, `STATS_FILE`, and `VIDEO_IN=rtsp://...` for the base station's streams |
| `telemetry_overlay.py` | keeps the overlay text for `burnin.sh` |
| `build.sh` | fetches and builds wfb-ng if needed |
| `sniff.sh [iface]` | counts what an adapter hears in monitor mode, to check it can receive at all |
| `clock.html` | millisecond clock for latency photos |
| `esp32/` | the firmware, its build and test scripts ([esp32/README.md](esp32/README.md)) |

## License

GPL-3.0 (see `LICENSE`), the same as wfb-ng. Files adapted from wfb-ng keep its copyright notice.
