# ESP32 wfb-ng link: design

Status: M0–M7 done. Updated 2026-10-16: M8 and M9, NXP's FRDM-RW612 as a radar camera; M10, radar and camera in one video.

We are building ESP32 firmware that speaks the wfb-ng protocol. ESP32-CAMs send video and telemetry to the receiver that already works (the NUC with the Ralink stick), and later to an ESP32 with Ethernet that serves each camera on the LAN. A PC burns the telemetry into the video and pushes it to the RTMP video server.

## Goals

1. **Camera firmware.** The AI-Thinker ESP32-CAM (4 MB PSRAM, OV2640) sends JPEG video over a link that wfb-ng can receive.
2. **Reference receiver.** An unmodified `wfb_rx` on the NUC decodes that link, and `rx.sh` shows the video.
3. **Base-station firmware.** An ESP32 with Ethernet receives up to 3 cameras at once and serves each one on the LAN as RTSP, with its telemetry next to it.
4. **Measurable.** Latency is measured the same way as today, with `clock.html`.
5. **Telemetry.** Each camera sends telemetry next to its video, on its own radio port: uptime, frame rate and frame size to start with, and temperature where the board has a sensor it can read. The ESP32-CAM's classic ESP32 has none, so it sends `null`. Signal quality is added where the link is received. The format may later mimic DJI's video-system telemetry field for field, once there is a sample to match.
6. **To the video server.** A Linux script burns the telemetry into each camera's video, encodes it as H.264 and pushes it to the RTMP video server.
7. **Web view.** The base station serves a simple web page that shows each camera's live video and telemetry, in any browser on the LAN, with nothing installed.
8. **Radar camera.** An NXP FRDM-RW612 board with the Trimension SR250 UWB radar shield watches a space instead of a camera. It draws the radar's measurements as a picture and sends it over wfb-ng itself, as camera N, so everything that shows a camera (the base station, its web view, RTSP, the burn-in script, `./ipc receive`) shows the radar too, unchanged. It sees movement and presence in the dark, and shows no faces. What it finds (something moving, and how far away) also goes out as its telemetry, and on the PC the burn-in script can put that, and the radar's picture, next to a real camera's video (M10).

**Not goals**

- H.264 or H.265 on the ESP32. The classic ESP32 has no video encoder, so we send JPEG.
- Burn-in or RTMP on the ESP32. Both need the video re-encoded, which needs a PC.
- 5 GHz. The ESP32 is 2.4 GHz only.
- A return link (ground to camera: commands, `wfb_tun`, MAVLink). Everything flows from camera to ground.
- Changes to the wfb-ng protocol. We stay compatible with upstream; we don't fork it.
- Replacing the Linux scripts. `tx.sh`, `rx.sh` and `link.sh` stay as they are.
- UWB as a radio link, or UWB ranging between devices. The SR250 is used as a radar only; the picture goes over wfb-ng.
- NXP's UWB software, in the firmware or in the repo. It is under NXP's proprietary licence, which doesn't mix with GPLv3; the firmware talks UCI to the SR250 itself.

## System overview

First (M1–M3), the ESP32 replaces only the tx machine. The NUC is the receiver it is tested against:

```
ESP32-CAM ── wfb-ng frames, ch 6 ──► Ralink stick ─► NUC: wfb_rx ─► :5600 ─► receiver.sh (JPEG mode)
```

Later (M4–M5), the base station replaces the NUC:

```
ESP32-CAM 1 (ports 0, 0x10) ─┐
ESP32-CAM 2 (ports 1, 0x11) ─┼─ ch 6 ─► base station: ESP32 + Ethernet ─► LAN
ESP32-CAM 3 (ports 2, 0x12) ─┘          RTSP per camera (RTP/JPEG, untouched)
                                        telemetry per camera (UDP), with signal quality added
                                        web page: each camera as MJPEG over HTTP (M7)
LAN ─► PC: burn telemetry into the video, encode H.264 ─► RTMP ─► video server
```

The radar camera (M8–M9) is a third firmware, on NXP's RW612 instead of an ESP32. It reads the SR250 over SPI and sends the way an ESP32-CAM does:

```
SR250 shield ─SPI, UCI─► RW612: radar CIRs ─► picture ─► JPEG ─► RTP/JPEG ─► wfb core ─► radio (monitor-mode transmit)
                         presence, distance (telemetry) ─────────────────► wfb core, its own stream ─► radio
```

Inside the camera firmware, each stage is a task, and stages are connected by queues of fixed-size buffers:

```
camera (OV2640 JPEG, PSRAM) ─► RTP/JPEG packetizer ─► wfb core (FEC, encrypt) ─► radio (esp_wifi_80211_tx)
telemetry (once a second)   ─► wfb core, its own stream ──────────────────────────► radio
```

| part | layer (R4) | runs on | does |
|---|---|---|---|
| `wfb_core` | core | ESP32 and Linux | session key, packet format, FEC, encryption; send and receive |
| `rtp_jpeg` | core | ESP32 and Linux | splits a JPEG into RFC 2435 RTP packets, and back |
| `radio` | platform | ESP32, RW612 | raw send with `esp_wifi_80211_tx()`, receive in promiscuous mode; on the RW612, `wifi_inject_frame()` from NXP's WiFi driver |
| `radar_picture` | core | RW612 and Linux | radar measurements to what moves and how far, drawn as a picture, and the picture to a JPEG that `rtp_jpeg` sends (M9) |
| `uwb` | platform | RW612 | the SR250 over SPI: our own UCI client (M9) |
| `camera` app | app | ESP32-CAM | camera → packetizer → core → radio; telemetry → core → radio |
| `base` app | app | ESP32 + Ethernet | radio → core → RTSP, telemetry and the web view on Ethernet |
| `radar` app | app | FRDM-RW612 + SR250 | radar → picture → JPEG → packetizer → core → radio; presence and distance as telemetry (M8–M9) |
| burn-in script | — | Linux PC | video + telemetry in; overlay, H.264, RTMP out; a camera with the radar's finding and picture (M10) |

## Architecture rules

The protocol is the contract. Each rule is numbered so code reviews and commits can refer to it ("breaks R2").

1. **R1 Upstream decides.** Every frame we send must be accepted by an unmodified `wfb_rx` built from the pinned submodule. If our code and `wfb_rx` disagree, our code is wrong.
2. **R2 The submodule is read-only.** We compile `wfb-ng/src/zfex.c` and include `wifibroadcast.hpp` straight from the submodule. We never edit files in it. Moving the pin is a commit of its own, followed by the compatibility test (M1).
3. **R3 Adapted code says where it came from.** Code we must adapt from `tx.cpp` or `rx.cpp` (session setup, packet assembly, FEC blocks) is copied into our component and kept close to the original. Its header names the source file and the submodule commit, so it can be diffed against upstream.
4. **R4 Three layers, dependencies point down only.**
    - *app*: one per firmware. It wires the parts together and holds the config.
    - *core*: protocol, encryption, FEC, RTP/JPEG. Plain C/C++, no ESP-IDF headers, so it also builds and runs on Linux.
    - *platform*: radio, camera, Ethernet, clock, UWB. The only place ESP-IDF calls appear, or on the RW612, Zephyr and NXP driver calls.
5. **R5 The core never calls the hardware.** It hands finished frames to an interface (the role `inject_packet()` has in `tx.cpp`) and receives frames the same way. The platform layer implements that interface with `esp_wifi_80211_tx()` on the ESP32 and with a socket or a file in host tests.
6. **R6 Separate firmwares, shared components.** The camera and the base station are separate ESP-IDF projects that share the core and platform components. The radar camera is a Zephyr project for the RW612 that shares the core. Their differences live in the app layer, not in `#ifdef`s.
7. **R7 Standard payloads.** Video inside the link is RTP/JPEG (RFC 2435), so GStreamer and ffmpeg on Linux play it with no custom code.
8. **R8 Config is fixed at build time to start with.** Channel, rate, FEC, link and port IDs, and keys are set through Kconfig (`idf.py menuconfig`). Changing them at runtime comes later, if a milestone needs it.
9. **R9 Only the test keys are committed.** Firmware embeds `keys/drone.key` (camera) or `keys/gs.key` (base station) at build time. Real keys stay out of git, as they do today.

## Protocol and radio

The ESP32 uses the same settings `link.sh` uses today, so the first test needs no changes on the NUC. Values marked *start* are where we begin; testing may change them.

| setting | value | why |
|---|---|---|
| Channel | 6 (2.4 GHz) | The ESP32 has no 5 GHz radio; matches `CHANNEL` in `link.sh` |
| Frame | wfb-ng's 802.11 data frame, with `channel_id` in the MAC address bytes | `wfb_rx` filters on those bytes |
| Stream ID | `channel_id = (link_id << 8) + radio_port`, link 0. Camera N (0–2): video on port N, telemetry on port 0x10 + N | Each stream gets its own `radio_port`, so one receiver can tell them apart |
| Frame size | At most 1500 bytes including the 802.11 header | `esp_wifi_80211_tx()` limit; `wfb_rx` accepts packets smaller than its own maximum |
| Rate | HT20 MCS 3 (*start*), set with `esp_wifi_config_80211_tx_rate()` | Matches `MCS` in `link.sh`; the ESP32 has no radiotap header |
| FEC | Reed-Solomon (zfex): video k = 8, n = 12; telemetry k = 1, n = 2 | `link.sh` and `wfb_tx` defaults for video; telemetry goes out at once, as wfb-ng does for MAVLink |
| Encryption | Session key sent with `crypto_box`, each packet with ChaCha20-Poly1305 (libsodium) | wfb-ng protocol |
| Session announce | Every 1000 ms | `SESSION_KEY_ANNOUNCE_MSEC` in `wifibroadcast.hpp` |
| Video | RTP/JPEG (RFC 2435) from the OV2640, 640×480 (*start*) | R7 |
| Telemetry | One small JSON object per message, once a second (*start*): camera, sequence number, uptime, frame rate, frame size, transmit power; temperature where the board has a sensor, otherwise `null` | Readable anywhere; fields can be added without breaking readers |
| Signal quality | Measured where the link is received: RSSI, loss and bitrate, added by the base station (or from `wfb_rx`'s stats on the NUC) | The camera can't know how well it is heard |
| UDP port on the NUC | 5600, as now | `receiver.sh` gets a JPEG mode next to H.264 |
| Base station out (M5) | RTSP per camera, `rtsp://<base>:8554/camN`, RTP/JPEG passed through; telemetry as UDP | RTSP is standard; the re-encoding to H.264 for RTMP happens on a PC |
| RW612 radio (M8) | Channel 6, HT20 MCS 3, the same frames and FEC as the ESP32 | The base station is 2.4 GHz only, so the RW612's 5 GHz stays unused |
| Radar picture (M9) | Camera N's ports, camera 2 by default (Kconfig): the picture as RTP/JPEG (R7) on video port N, what it finds (moving or not, distance) as telemetry on 0x10 + N. The picture: what moves and how far, movement against distance, and that distance over the last 20 s. 320×240, about 5 pictures a second (*start*). Baseline JPEG, 4:2:2 with the standard Huffman tables, as the OV2640's | Nothing downstream needs to know it is a radar. RFC 2435 has no greyscale type, and this is the JPEG `rtp_jpeg` already sends |
| Base station web view (M7) | `http://<base>/`: a tile per camera. `http://<base>/camN.mjpg`: its video as MJPEG (`multipart/x-mixed-replace`), JPEGs rebuilt from RTP/JPEG, not re-encoded. `http://<base>/status.json`: each camera's telemetry and signal quality | Browsers can't play RTSP, but show MJPEG in an `<img>`; the cameras' JPEGs pass through as sent |

One camera at 640×480 and 10–15 fps sends an estimated 1.5–3.5 Mbit/s. Three cameras share one channel, so about 5–10 Mbit/s in total. These are estimates; M3 measures them.

## Coding conventions

Low latency and predictable timing come before features. A late frame is worth less than a dropped one.

**Language**

- The core is C++17, because the code adapted from `tx.cpp` is C++. zfex stays C.
- No exceptions and no RTTI (ESP-IDF's defaults). The `throw`s in adapted code become returned error codes.
- STL only where it allocates once, at startup. No `std::string`, `std::map` or `new` in the per-packet path.

**Memory**

- Everything is allocated at startup: FEC blocks, frame buffers, queues. Nothing is allocated per packet or per frame.
- Each large buffer has a comment saying whether it lives in internal RAM or PSRAM, and why.

**Tasks and timing**

- One FreeRTOS task per stage (capture, packetize, send), connected by queues of fixed-size buffers.
- Our tasks run on core 1. ESP-IDF's WiFi runs on core 0 by default.
- When a stage falls behind, drop whole video frames at the camera. Never drop packets in the middle of an FEC block.
- The core gets the time as a parameter (microseconds, from `esp_timer_get_time()`), so tests can control the clock.

**State, errors and logging**

- The core keeps no global state. Each stream's state is a struct the app owns and passes in.
- Platform code returns `esp_err_t`; the core returns its own status codes. `ESP_ERROR_CHECK` is used only during boot.
- Runtime errors are counted and logged; they never reboot the board. `assert` is for programmer errors only.
- `ESP_LOG*` with one tag per component. Nothing is logged per packet. Each firmware prints one stats line per second (packets, bytes, drops), like `wfb_tx` does.

**Style**

- Adapted code keeps wfb-ng's style, so it diffs cleanly against upstream (R3).
- New code uses `snake_case`, 4-space indents and one `.clang-format` at the root of `esp32/`. Public names start with their component (`wfb_`, `radio_`, `cam_`).
- Comments explain why, not what, in the same short style as the existing scripts.

## Repository layout and build

```
esp32/
  components/
    wfb_core/     core: session, packets, FEC (zfex.c from ../../wfb-ng/src), encryption
    rtp_jpeg/     core: RFC 2435 packetizer and depacketizer
    radio/        platform: raw send and promiscuous receive
    cam/          platform: OV2640 capture
    eth/          platform: Ethernet on the base-station board
    keys/         embeds keys/drone.key or keys/gs.key (R9)
  camera/         ESP-IDF project: camera firmware
  base/           ESP-IDF project: base-station firmware
  host/           CMake build of the core for Linux, plus its tests
  radar/          Zephyr project: radar camera firmware for the FRDM-RW612 (its app and RW612 platform code)
  build.sh        one command per firmware, like the top-level scripts
```

- ESP-IDF is pinned to one release, chosen in M0 and written in `esp32/README.md`.
- The radar firmware is built with Zephyr (`west`), pinned to one release in M8. NXP's WiFi driver for the RW612 (BSD-3-Clause, with raw send and a monitor mode) and the board support live there.
- Outside code comes from the ESP Component Manager (`esp32-camera`, `libsodium`, `lan87xx`), with versions pinned in each project's `idf_component.yml`.
- The Linux side keeps using its scripts, in `pc/`; `./ipc` at the repo root is the front door that runs them. `receiver.sh` gets a JPEG mode (`rtpjpegdepay ! jpegdec`) next to H.264. The burn-in script is a new script there, built on ffmpeg like `sender.sh`.

## Testing

1. **Host tests (every change to the core).** The core is built on Linux. Its frames are decoded by our own receive core and by the real `wfb_rx` built from the submodule. Real `wfb_tx` output is decoded by our receive core. How to feed frames to `wfb_rx` without a radio (a pcap file or its UDP aggregator mode) is worked out in M0.
2. **On-air test (every change to radio or core).** ESP32 → NUC with `rx.sh`. Pass: `wfb_rx` counts packets, with no decryption errors and no lost FEC blocks at 1–2 m.
3. **Latency (each milestone).** `clock.html`, as in the README. Results go into the milestone's GitHub issue, with the settings used; the README is for getting the system running, not for results.

## Milestones

Each milestone ends with something that can be shown working. M6 doesn't depend on M4 and M5, so it can be done before them. M7 builds on M5. M8 needs only M1's receiver; M9 builds on M8 (and M5, to show it on the base station); M10 builds on M6 and M9.

1. **M0 Toolchain and skeleton.** ESP-IDF installed, `esp32/` laid out, the core building on Linux with zfex and libsodium. Done when the host tests pass.
2. **M1 On-air compatibility.** Any ESP32 sends wfb-ng frames with a test payload (a counter). Done when the NUC's unmodified `wfb_rx` decodes them and the counter arrives on port 5600.
3. **M2 Camera video.** The ESP32-CAM sends RTP/JPEG. Done when `rx.sh` shows the video and the latency is measured.
4. **M3 Tuning.** Measure frame rate, bitrate and latency against resolution, MCS and FEC. Done when the results are in its issues.
5. **M4 Base-station receive.** An Ethernet ESP32 receives one camera and sends its RTP/JPEG over Ethernet to the NUC. Done when `receiver.sh` on the NUC shows it.
6. **M5 Three cameras to the video server.** The base station receives three cameras and serves each as RTSP, with its telemetry and signal quality as UDP. Done when all three play from another machine, and one reaches the RTMP video server through the burn-in script with its telemetry burned in.
7. **M6 Telemetry and burn-in.** The camera sends its uptime once a second on its telemetry port (and temperature, where the board has a sensor). On the NUC, the telemetry is received next to the video, and the burn-in script draws it, with signal quality, onto the video, encodes H.264 and pushes RTMP (or records a file when no server is given). Done when the output shows the telemetry burned in, updating every second.
8. **M7 Web view on the base station.** The base station serves a web page with a tile per camera: its live video as MJPEG over HTTP, and its telemetry and signal quality, refreshed once a second. Without PSRAM, three whole frames don't fit in RAM, so each frame goes to the browser as its packets arrive, and a frame with a lost packet is cut off and skipped. The number of viewers is capped. The web view needs Ethernet, so an ESP32-CAM standing in as the base station (Ethernet off) has none. Done when a browser on the LAN shows every connected camera's live video and telemetry from the base station's page.
9. **M8 RW612 on air.** The FRDM-RW612 joins as a third firmware, built with Zephyr: the core builds for it, and its radio sends wfb-ng frames with `wifi_inject_frame()` on channel 6, with a test payload (a counter), as in M1. Done when the NUC's unmodified `wfb_rx` decodes them and the counter arrives on port 5600.
10. **M9 Radar picture.** The SR250 shield sits on the FRDM-RW612, with SPI routed to its Arduino header. The firmware talks UCI to the SR250 itself, runs a radar session, and from the channel impulse responses (CIRs) finds what moves and how far away: the still background is learned and taken away, where there is a reflection well above the noise. It draws that as a picture (what moves and how far, movement against distance, that distance over the last 20 s; tried out with the user in `pc/radar_view.py`), encodes it as JPEG and sends it as camera N, with what it finds as camera N's telemetry. The drawing and the encoder are core code, tested on Linux with recorded CIRs. Done when a person walking towards and away from the sensor shows as a moving trace, live, in `./ipc receive` or in the base station's web view.
11. **M10 Radar and camera in one video.** On the PC, the burn-in script takes a camera's video and the radar camera's: the radar's finding as a line of text on the camera's picture, next to the camera's own telemetry, and the radar's picture beside the camera's (a setting chooses text only, or both). It goes out the same way, H.264 to the RTMP server or a file. Done when the output shows a camera's live video with the radar's finding and picture, both following someone walking in front of the radar.

## Open questions and risks

| question or risk | answered by |
|---|---|
| Does `esp_wifi_80211_tx()` send wfb-ng's data frame header with our MAC address bytes unchanged? | M1: yes, the NUC's `wfb_rx` decodes the ESP32's frames |
| Does `wfb_rx` check any field the ESP32 fills in differently (sequence number, duration)? | M1: none found |
| Is the ESP32 fast enough for encryption plus FEC at a few Mbit/s? | M3 (25 fps at 640×480, ~2 Mbit/s, works at 160 MHz) |
| OV2640 JPEGs must be parsed for RFC 2435, and their quantization tables sent in-band | M2: done; baseline 4:2:2 with the standard Huffman tables |
| Most Ethernet ESP32 boards have no PSRAM, which leaves about 300 KB for three streams | M4 (about 140 KB per stream with the receiver's default ring) |
| Which Ethernet ESP32 board? (Decides the Ethernet pin config.) | Olimex ESP32-POE (#16) |
| Which video server, and what input does it take? | RTMP (#19): the base station serves RTSP, and a PC re-encodes to H.264 and pushes RTMP |
| Which ESP-IDF release to pin | v6.0.3 |
| wfb-ng is GPLv3, so firmware built from its code is GPLv3. | The repo is GPL-3.0 (#10) |
| The ESP32-POE generates its Ethernet clock on the ESP32, which the ESP32 errata says can be unstable while WiFi runs; the base station needs both | M4 test (#18) |
| Both ESP32-CAMs hear and transmit weakly (likely their antenna-select resistors) | #23 |
| RAM for the web view: HTTP connections next to three receivers and RTSP, without PSRAM (about 45 KB free with Ethernet up) | M7: viewers capped, measured on the ESP32-POE |
| CPU on the base station: at 160 MHz with `-Og`, the ESP32-POE dropped received frames from one camera sending about 600 frames/s; 240 MHz with `-O2` dropped far fewer | M5 and M7: measure with three cameras |
| DJI-style telemetry: which DJI system, and a sample to match field for field | when there is a sample |
| The classic ESP32 has no supported temperature sensor | M6: `temp_c` is `null` on the ESP32-CAM; an external sensor later, if needed |
| Does the RW612's `wifi_inject_frame()` send wfb-ng's data frame unchanged (the MAC address bytes `wfb_rx` filters on), on a fixed channel and rate, without joining a network? NXP's driver says injected frames go out on the station interface | M8 |
| Is the RW612 fast enough for encryption, FEC and injection at a few hundred kbit/s, next to the radar? | M8, then M9 |
| The RW612's WiFi firmware is an NXP binary the driver loads into the WiFi core: fine next to GPLv3 firmware, as ESP-IDF's WiFi libraries are? | M8 (with #10's reasoning) |
| The SR250 needs current firmware, and NXP's updater is a program of its own (Zephyr `demo_sr2xx_fw_update`, or a Windows tool) | M9: the user's SR250 already runs 01.41.04, the version NXP's update installs (the boot loader's GET_INFO and GET_INFO_EXT) |
| The FRDM-RW612 doesn't route SPI to the Arduino headers: it needs a rework, or two jumper wires (D0–D11, D1–D12) | M9: jumper wires, on the user's board |
| Which UCI commands and fields the radar needs, without copying NXP's code | M9: from NXP's plug-and-play demo's UCI messages; a command the SR250 answers with STATUS_COMMAND_RETRY (it was asleep) is sent again at once |
| Zephyr v4.4.2 on the FRDM-RW612: the board's SPI pin setting ends on chip select 1, and the SPI driver's DMA path drops each transfer's first received byte | M9: the radar's own pin setting (chip select 0 only) and SPI without DMA |
| The RW612's WiFi firmware sends frames from the host at 1 Mbit/s unless the frame asks for a rate: too slow for a picture | M9: a fixed rate in the frame's tx_control, measured with the base station's received-rate report |
| Is a software JPEG encoder on the RW612 fast enough for 320×240 at 10 pictures a second? | M9 |
| What picture is most useful: distance over time, distance and speed, or a top-down view from the shield's three receive antennas (3D angle of arrival)? | M9: what moves and how far, movement against distance, distance over time (tried out with the user); a top-down view needs the complex samples and both antenna pairs, later if wanted |
