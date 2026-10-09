# ESP32 wfb-ng link: design

Status: M0–M2 done. Updated 2026-10-09: RTSP out of the base station, a telemetry channel, and telemetry burned into the video on a PC.

We are building ESP32 firmware that speaks the wfb-ng protocol. ESP32-CAMs send video and telemetry to the receiver that already works (the NUC with the Ralink stick), and later to an ESP32 with Ethernet that serves each camera on the LAN. A PC burns the telemetry into the video and pushes it to the RTMP video server.

## Goals

1. **Camera firmware.** The AI-Thinker ESP32-CAM (4 MB PSRAM, OV2640) sends JPEG video over a link that wfb-ng can receive.
2. **Reference receiver.** An unmodified `wfb_rx` on the NUC decodes that link, and `rx.sh` shows the video.
3. **Base-station firmware.** An ESP32 with Ethernet receives up to 3 cameras at once and serves each one on the LAN as RTSP, with its telemetry next to it.
4. **Measurable.** Latency is measured the same way as today, with `clock.html`.
5. **Telemetry.** Each camera sends telemetry next to its video, on its own radio port: temperature and uptime to start with. Signal quality is added where the link is received. The format may later mimic DJI's video-system telemetry field for field, once there is a sample to match.
6. **To the video server.** A Linux script burns the telemetry into each camera's video, encodes it as H.264 and pushes it to the RTMP video server.

**Not goals**

- H.264 or H.265 on the ESP32. The classic ESP32 has no video encoder, so we send JPEG.
- Burn-in or RTMP on the ESP32. Both need the video re-encoded, which needs a PC.
- 5 GHz. The ESP32 is 2.4 GHz only.
- A return link (ground to camera: commands, `wfb_tun`, MAVLink). Everything flows from camera to ground.
- Changes to the wfb-ng protocol. We stay compatible with upstream; we don't fork it.
- Replacing the Linux scripts. `tx.sh`, `rx.sh` and `link.sh` stay as they are.

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
LAN ─► PC: burn telemetry into the video, encode H.264 ─► RTMP ─► video server
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
| `radio` | platform | ESP32 | raw send with `esp_wifi_80211_tx()`, receive in promiscuous mode |
| `camera` app | app | ESP32-CAM | camera → packetizer → core → radio; telemetry → core → radio |
| `base` app | app | ESP32 + Ethernet | radio → core → RTSP and telemetry on Ethernet |
| burn-in script | — | Linux PC | video + telemetry in; overlay, H.264, RTMP out |

## Architecture rules

The protocol is the contract. Each rule is numbered so code reviews and commits can refer to it ("breaks R2").

1. **R1 Upstream decides.** Every frame we send must be accepted by an unmodified `wfb_rx` built from the pinned submodule. If our code and `wfb_rx` disagree, our code is wrong.
2. **R2 The submodule is read-only.** We compile `wfb-ng/src/zfex.c` and include `wifibroadcast.hpp` straight from the submodule. We never edit files in it. Moving the pin is a commit of its own, followed by the compatibility test (M1).
3. **R3 Adapted code says where it came from.** Code we must adapt from `tx.cpp` or `rx.cpp` (session setup, packet assembly, FEC blocks) is copied into our component and kept close to the original. Its header names the source file and the submodule commit, so it can be diffed against upstream.
4. **R4 Three layers, dependencies point down only.**
    - *app*: one per firmware. It wires the parts together and holds the config.
    - *core*: protocol, encryption, FEC, RTP/JPEG. Plain C/C++, no ESP-IDF headers, so it also builds and runs on Linux.
    - *platform*: radio, camera, Ethernet, clock. The only place ESP-IDF calls appear.
5. **R5 The core never calls the hardware.** It hands finished frames to an interface (the role `inject_packet()` has in `tx.cpp`) and receives frames the same way. The platform layer implements that interface with `esp_wifi_80211_tx()` on the ESP32 and with a socket or a file in host tests.
6. **R6 Two firmwares, shared components.** The camera and the base station are separate ESP-IDF projects that share the core and platform components. Their differences live in the app layer, not in `#ifdef`s.
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
| Telemetry | One small JSON object per message, once a second (*start*): camera, sequence number, uptime, temperature | Readable anywhere; fields can be added without breaking readers |
| Signal quality | Measured where the link is received: RSSI, loss and bitrate, added by the base station (or from `wfb_rx`'s stats on the NUC) | The camera can't know how well it is heard |
| UDP port on the NUC | 5600, as now | `receiver.sh` gets a JPEG mode next to H.264 |
| Base station out (M5) | RTSP per camera, `rtsp://<base>:8554/camN`, RTP/JPEG passed through; telemetry as UDP | RTSP is standard; the re-encoding to H.264 for RTMP happens on a PC |

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
  build.sh        one command per firmware, like the top-level scripts
```

- ESP-IDF is pinned to one release, chosen in M0 and written in `esp32/README.md`.
- Outside code comes from the ESP Component Manager (`esp32-camera`, `libsodium`, `lan87xx`), with versions pinned in each project's `idf_component.yml`.
- The Linux side keeps using the top-level scripts. `receiver.sh` gets a JPEG mode (`rtpjpegdepay ! jpegdec`) next to H.264. The burn-in script is a new top-level script, built on ffmpeg like `sender.sh`.

## Testing

1. **Host tests (every change to the core).** The core is built on Linux. Its frames are decoded by our own receive core and by the real `wfb_rx` built from the submodule. Real `wfb_tx` output is decoded by our receive core. How to feed frames to `wfb_rx` without a radio (a pcap file or its UDP aggregator mode) is worked out in M0.
2. **On-air test (every change to radio or core).** ESP32 → NUC with `rx.sh`. Pass: `wfb_rx` counts packets, with no decryption errors and no lost FEC blocks at 1–2 m.
3. **Latency (each milestone).** `clock.html`, as in the README. Results go into the README's results table, with the settings used.

## Milestones

Each milestone ends with something that can be shown working. M6 doesn't depend on M4 and M5, so it can be done before them.

1. **M0 Toolchain and skeleton.** ESP-IDF installed, `esp32/` laid out, the core building on Linux with zfex and libsodium. Done when the host tests pass.
2. **M1 On-air compatibility.** Any ESP32 sends wfb-ng frames with a test payload (a counter). Done when the NUC's unmodified `wfb_rx` decodes them and the counter arrives on port 5600.
3. **M2 Camera video.** The ESP32-CAM sends RTP/JPEG. Done when `rx.sh` shows the video and the latency is measured.
4. **M3 Tuning.** Measure frame rate, bitrate and latency against resolution, MCS and FEC. Done when the results are in the README.
5. **M4 Base-station receive.** An Ethernet ESP32 receives one camera and sends its RTP/JPEG over Ethernet to the NUC. Done when `receiver.sh` on the NUC shows it.
6. **M5 Three cameras to the video server.** The base station receives three cameras and serves each as RTSP, with its telemetry and signal quality as UDP. Done when all three play from another machine, and one reaches the RTMP video server through the burn-in script with its telemetry burned in.
7. **M6 Telemetry and burn-in.** The camera sends temperature and uptime once a second on its telemetry port. On the NUC, the telemetry is received next to the video, and the burn-in script draws it, with signal quality, onto the video, encodes H.264 and pushes RTMP (or records a file when no server is given). Done when the output shows the telemetry burned in, updating every second.

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
| DJI-style telemetry: which DJI system, and a sample to match field for field | when there is a sample |
