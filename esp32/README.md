# ESP32 firmware

ESP32 firmware that speaks the wfb-ng protocol. The design, its rules and the milestones are in [../docs/DESIGN.md](../docs/DESIGN.md).

**ESP-IDF is pinned to v6.0.3** (set in `idf-env.sh`). Python 3.14 works with it.

## Setup

```sh
esp32/setup.sh
```

This fetches ESP-IDF v6.0.3 into `~/esp/esp-idf-v6.0.3` and installs its tools (compiler, cmake, ninja, Python packages) into `~/.espressif`. Nothing is installed system-wide and nothing needs sudo, except joining the serial port group, which the script tells you about if needed. Run it again at any time; it skips what is already there.

Set `IDF_PATH` or `IDF_TOOLS_PATH` to install somewhere else.

## Build, flash, monitor

```sh
esp32/build.sh camera                  # build
esp32/build.sh camera flash            # build if needed, then flash
esp32/monitor.sh                       # serial log; leave it running
esp32/idf.sh idf.py -C camera menuconfig
```

`idf.sh` runs any command with ESP-IDF set up, from any shell (fish included), so nothing has to be sourced first. The serial port is found automatically when flashing; set `ESPPORT=/dev/ttyUSB0` to choose one.

`monitor.sh` stays open: while `build.sh` flashes it lets go of the port, and afterwards it reconnects. It also waits through unplugging. Connecting restarts the board, so the whole boot log shows: on Linux, opening the port switches the USB adapter's DTR and RTS lines on, which holds the ESP32-CAM in reset until the monitor switches them off. `-t` puts the arrival time in front of each line. Ctrl-C quits. ESP-IDF's own monitor (`build.sh camera monitor`, Ctrl-] quits) is still there for decoding crash backtraces.

To flash an ESP32-CAM on its ESP32-CAM-MB USB board, just plug it in. With a plain USB serial adapter instead, connect IO0 to GND, then press reset before flashing; disconnect IO0 and reset again to run.

## VS Code

Open the repo folder. VS Code suggests the C/C++ extension (`ms-vscode.cpptools`); install it.

- **Ctrl+Shift+B** builds the camera firmware.
- **Terminal > Run Task** has flash, serial monitor and menuconfig. Start the serial monitor once and leave it open; flash as often as you like.
- IntelliSense reads `camera/build/compile_commands.json`, so build once before the ESP-IDF headers resolve.

The tasks call `build.sh`, so the editor and the terminal build the same way. The Espressif VS Code extension isn't needed.

## Tests on Linux

```sh
esp32/test.sh              # build the core for Linux and run every test
esp32/test.sh -R fec       # only the tests whose name matches
```

The core components build for Linux too (R4), against the system libsodium, using the cmake that `setup.sh` installed. The tests live in `host/tests/`.

## Testing against the real wfb-ng without a radio

`wfb_rx -a <port>` takes packets over UDP instead of from a WiFi card, and `wfb_tx -D <port>` sends to a UDP port instead of a card. `test_wfb_ng` uses both to check our transmitter against the real `wfb_rx`, and the real `wfb_tx` against our receiver, with the test keys. That path carries everything but the 802.11 header, which the on-air test (M1) covers. `test.sh` builds wfb-ng first if needed.

| test | checks |
|---|---|
| `test_fec` | zfex from the submodule recovers any 4 lost packets of 12 |
| `test_tx` | our transmitter's packet sequence; the ground key opens what it sends |
| `test_rx` | our transmitter into our receiver, with and without loss; wrong channel or key gets nothing |
| `test_wfb_ng` | our transmitter into the real `wfb_rx`, and the real `wfb_tx` into our receiver |
| `test_80211` | the 802.11 header: the address bytes `wfb_rx` filters on, the sequence number |
| `test_rtp_jpeg` | RTP/JPEG: which JPEGs are accepted, packet layout, round trip, loss, GStreamer decoding our packets; the streamer's JPEGs the same as the depacketizer's |
| `test_rtsp` | the RTSP server's replies, and ffmpeg playing a stream over TCP and UDP |
| `test_web` | the web server's page, status and errors; ffmpeg playing a camera's MJPEG; JPEGs cut off by lost packets; a stuck viewer not holding up the others |

## Camera video (M2)

The camera firmware sends the OV2640's JPEG frames as RTP/JPEG (RFC 2435) over wfb-ng: 640x480 at about 25 fps and 10 KB a frame, about 2 Mbit/s. On the NUC:

```sh
VIDEO=jpeg ./rx.sh
```

The video window opens with the first frame. `esp32/monitor.sh` shows the ESP32's side once a second: `video: 25 fps, 10 KB/frame, 200 packets/s, 300 frames/s on air, 0 dropped by WiFi`. Frame size, JPEG quality and whether each frame finishes its FEC block are in `idf.py menuconfig` under "Camera".

## Base station (M4, M5, M7)

`base/` is the base-station firmware for the Olimex ESP32-POE. It receives up to three cameras' wfb-ng streams in promiscuous mode (camera N: video on radio port N, telemetry on 0x10 + N), decodes them with the same core as the NUC, and serves them over Ethernet:

- each camera's video as RTSP, `rtsp://<base station>:8554/cam0` ... `cam2`, RTP/JPEG passed through (VLC, ffmpeg and GStreamer play it);
- each camera's telemetry as UDP to the host set under "Ethernet out" (port 5610 + N), with the signal quality the base station measures added (`rssi_dbm`, `loss_pct`, `video_kbps`);
- camera 0's video also as UDP to that host's port 5600, for `VIDEO=jpeg ./receiver.sh` (M4's check);
- a web view at `http://<base station>/` (M7): a tile per camera with its live video and its telemetry and signal quality, in any browser. Each camera's video is also at `http://<base station>/camN.mjpg` (MJPEG, the cameras' JPEGs rebuilt from RTP/JPEG, not re-encoded; `ffplay` and VLC play it too), and the telemetry at `/status.json`.

```sh
ESPPORT=/dev/ttyUSB1 esp32/build.sh base flash
esp32/monitor.sh /dev/ttyUSB1
```

Each camera needs its own radio ports. `build.sh camera` builds camera 0; `CAM=1` and `CAM=2` build cameras 1 and 2 (`camera/sdkconfig.defaults.cam1`, `.cam2`), each in its own build folder and sdkconfig, so switching between them never mixes settings:

```sh
ESPPORT=/dev/ttyUSB0 esp32/build.sh camera flash          # camera 0: rtsp://<base station>:8554/cam0
CAM=1 ESPPORT=/dev/ttyUSB0 esp32/build.sh camera flash    # camera 1: .../cam1
CAM=2 ESPPORT=/dev/ttyUSB0 esp32/build.sh camera flash    # camera 2: .../cam2
```

To push a camera to the RTMP server with its telemetry burned in, on the host the telemetry goes to:

```sh
VIDEO_IN=rtsp://<base station>:8554/cam0 TELEMETRY_PORT=5610 ./burnin.sh rtmp://<server>/<app>/cam0
```

Once a second the base station prints, per camera, frames and RSSI, frame rate, bitrate, FEC repairs and losses, telemetry messages, RTSP clients and web viewers (and how many of their JPEGs were cut off by packets lost on air); and overall, what the radio heard, RTSP packets sent and dropped, web JPEGs sent whole, cut off and skipped, Ethernet link drops, and free RAM with its lowest point in that second.

The ESP32-POE has no PSRAM, so RAM decides how much it can serve. Three cameras fit with a receive ring of one FEC block per stream (enough on a radio that delivers in order; `test_rx` checks it), 8 receive slots, and telemetry packets of at most 512 bytes: 76 KB of RAM is left before Ethernet, and about 30 KB once Ethernet, RTSP (2 clients of 4 KB) and the web view (4 connections of 2 KB) are up. Video for clients sits in TCP's buffers, on the heap, while it waits to be acknowledged; run dry, and the radio loses frames for every camera. So the servers hold data back while sending it would leave less than "RAM kept free for receiving" (10 KB), and a viewer that falls behind loses frames, not the others.

What it can serve, measured on the ESP32-POE with one camera (25 fps, about 2.1 Mbit/s) and the clients on WiFi, 20 s each:

| clients at once | each got | lowest free RAM |
|---|---|---|
| 1 web viewer | 473 of 495 JPEGs whole; 11 of the rest cut by packets lost on air | 14.6 KB |
| 3 web viewers | 92 % of JPEGs whole | 5.2 KB |
| 2 RTSP clients | 451 of 500 frames, 2 % of packets dropped | 5.8 KB |
| 1 web viewer + 1 RTSP client | 450 JPEGs whole; 448 RTSP frames | 4.3 KB |
| 3 web viewers + 1 RTSP client | overloaded: RTSP 121 frames, the radio loses frames | 3.0 KB |

So about three video connections at once, in any mix; a browser showing three cameras uses three. With three cameras, receiving takes three times the CPU, which isn't measured yet. A browser's JPEG with a packet lost on air arrives cut off where the packet went missing, and the next one is whole.

"Ethernet" can be turned off in menuconfig, to run the firmware on an ESP32-CAM as a stand-in receiver (no RTSP and no web view).

On the original ESP32, generating the Ethernet clock (as the ESP32-POE does) can be unstable while WiFi runs (ESP32 errata). The stats line counts Ethernet link drops to show it (#18).

## On-air test (M1)

Set "What to send" to "Counter" in `idf.py menuconfig` ("wfb-ng link"). The camera firmware then sends a counter over wfb-ng instead of video: 100 payloads of 1000 bytes a second, on channel 6 at HT20 MCS 3, link 0, port 0, FEC 8/12, with `keys/drone.key`. Those are the receiver's defaults in `link.sh`, so the NUC needs no changes. The settings are in `idf.py menuconfig` under "wfb-ng link" (R8).

1. Flash it: `esp32/build.sh camera flash`, then watch `esp32/monitor.sh`. Each second it prints a line like `counter 402: 100 payloads/s, 153 frames/s, 0 dropped by WiFi`.
2. On the NUC, either:
   - `./rx.sh`: the `[rx]` lines show wfb_rx's stats every second, `<time> PKT all:bytes:dec_err:session:data:uniq:fec_rec:lost:bad:out:out_bytes`. With the ESP32 heard, `data` is about 150, `dec_err` 0 and `out` about 100. The video window stays empty, because the counter isn't video.
   - or, to check every counter value: `sudo ./link.sh rx` in one terminal and `esp32/tools/counter_check.py` in another (rx.sh's video window would hold port 5600).

Keep the ESP32 a metre or more from the Ralink stick: at 20 dBm, centimetres can overload the receiver.

## Layout

| path | what |
|---|---|
| `camera/` | camera firmware (ESP-IDF project) |
| `components/wfb_core/` | wfb-ng protocol, encryption and FEC; builds for the ESP32 and for Linux |
| `host/` | Linux build of the core components, and their tests |
| `test.sh` | builds `host/` and runs the tests |
| `setup.sh` | installs the pinned ESP-IDF |
| `build.sh` | builds, flashes or monitors one firmware |
| `monitor.sh`, `monitor.py` | serial monitor that gives the port up while flashing |
| `idf.sh`, `idf-env.sh` | run a command with ESP-IDF set up; the pin |
| `components/radio/` | raw 802.11 transmit with `esp_wifi_80211_tx()` |
| `components/keys/` | embeds the link key at build time (R9) |
| `components/cam/` | OV2640 JPEG capture, AI-Thinker pin map |
| `components/rtp_jpeg/` | JPEG frames as RTP packets (RFC 2435) and back |
| `components/eth/` | Ethernet on the Olimex ESP32-POE (LAN8720), and UDP out |
| `components/rtsp_server/` | RTSP for the base station's camera streams (builds for Linux too) |
| `components/web_server/` | the base station's web view: MJPEG, the page, the status document over HTTP (builds for Linux too) |
| `base/` | base-station firmware (ESP-IDF project) |
| `tools/counter_check.py` | checks the M1 counter stream on the receiver |

