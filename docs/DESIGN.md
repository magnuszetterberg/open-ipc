# ESP32 wfb-ng link: design

Status: draft, 2026-10-09. Nothing here is built yet.

We are building ESP32 firmware that speaks the wfb-ng protocol. ESP32-CAMs then send video to the receiver that already works (the NUC with the Ralink stick), and later to an ESP32 with Ethernet that forwards the video to the video server.

## Goals

1. **Camera firmware.** The AI-Thinker ESP32-CAM (4 MB PSRAM, OV2640) sends JPEG video over a link that wfb-ng can receive.
2. **Reference receiver.** An unmodified `wfb_rx` on the NUC decodes that link, and `rx.sh` shows the video.
3. **Base-station firmware.** An ESP32 with Ethernet receives up to 3 cameras at once and serves each one on the LAN for the video server.
4. **Measurable.** Latency is measured the same way as today, with `clock.html`.

**Not goals**

- H.264 or H.265 on the ESP32. The classic ESP32 has no video encoder, so we send JPEG.
- 5 GHz. The ESP32 is 2.4 GHz only.
- A return link (telemetry, `wfb_tun`, MAVLink). This is one-way video for now.
- Changes to the wfb-ng protocol. We stay compatible with upstream; we don't fork it.
- Replacing the Linux scripts. `tx.sh`, `rx.sh` and `link.sh` stay as they are.

## System overview

First (M1–M3), the ESP32 replaces only the tx machine. The NUC is the receiver it is tested against:

```
ESP32-CAM ── wfb-ng frames, ch 6 ──► Ralink stick ─► NUC: wfb_rx ─► :5600 ─► receiver.sh (JPEG mode)
```

Later (M4–M5), the base station replaces the NUC:

```
ESP32-CAM 1 (port 0) ─┐
ESP32-CAM 2 (port 1) ─┼─ ch 6 ─► base station: ESP32 + Ethernet ─► LAN ─► video server
ESP32-CAM 3 (port 2) ─┘          (wfb receive core, one stream per camera out)
```

Inside the camera firmware, each stage is a task, and stages are connected by queues of fixed-size buffers:

```
camera (OV2640 JPEG, PSRAM) ─► RTP/JPEG packetizer ─► wfb core (FEC, encrypt) ─► radio (esp_wifi_80211_tx)
```

| part | layer (R4) | runs on | does |
|---|---|---|---|
| `wfb_core` | core | ESP32 and Linux | session key, packet format, FEC, encryption; send and receive |
| `rtp_jpeg` | core | ESP32 and Linux | splits a JPEG into RFC 2435 RTP packets, and back |
| `radio` | platform | ESP32 | raw send with `esp_wifi_80211_tx()`, receive in promiscuous mode |
| `camera` app | app | ESP32-CAM | camera → packetizer → core → radio |
| `base` app | app | ESP32 + Ethernet | radio → core → streams on Ethernet |

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
| Stream ID | `channel_id = (link_id << 8) + radio_port`; link 0, port 0 for camera 1 | Each camera gets its own `radio_port`, so one receiver can tell them apart |
| Frame size | At most 1500 bytes including the 802.11 header | `esp_wifi_80211_tx()` limit; `wfb_rx` accepts packets smaller than its own maximum |
| Rate | HT20 MCS 3 (*start*), set with `esp_wifi_config_80211_tx_rate()` | Matches `MCS` in `link.sh`; the ESP32 has no radiotap header |
| FEC | Reed-Solomon (zfex), k = 8, n = 12 | `link.sh` and `wfb_tx` defaults |
| Encryption | Session key sent with `crypto_box`, each packet with ChaCha20-Poly1305 (libsodium) | wfb-ng protocol |
| Session announce | Every 1000 ms | `SESSION_KEY_ANNOUNCE_MSEC` in `wifibroadcast.hpp` |
| Video | RTP/JPEG (RFC 2435) from the OV2640, 640×480 (*start*) | R7 |
| UDP port on the NUC | 5600, as now | `receiver.sh` gets a JPEG mode next to H.264 |

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
    keys/         embeds keys/drone.key or keys/gs.key (R9)
  camera/         ESP-IDF project: camera firmware
  base/           ESP-IDF project: base-station firmware
  host/           CMake build of the core for Linux, plus its tests
  build.sh        one command per firmware, like the top-level scripts
```

- ESP-IDF is pinned to one release, chosen in M0 and written in `esp32/README.md`.
- Outside code comes from the ESP Component Manager (`esp32-camera`, `libsodium`), with versions pinned in each project's `idf_component.yml`.
- The Linux side keeps using the top-level scripts. `receiver.sh` gets a JPEG mode (`rtpjpegdepay ! jpegdec`) next to H.264.

## Testing

1. **Host tests (every change to the core).** The core is built on Linux. Its frames are decoded by our own receive core and by the real `wfb_rx` built from the submodule. Real `wfb_tx` output is decoded by our receive core. How to feed frames to `wfb_rx` without a radio (a pcap file or its UDP aggregator mode) is worked out in M0.
2. **On-air test (every change to radio or core).** ESP32 → NUC with `rx.sh`. Pass: `wfb_rx` counts packets, with no decryption errors and no lost FEC blocks at 1–2 m.
3. **Latency (each milestone).** `clock.html`, as in the README. Results go into the README's results table, with the settings used.

## Milestones

Each milestone ends with something that can be shown working.

1. **M0 Toolchain and skeleton.** ESP-IDF installed, `esp32/` laid out, the core building on Linux with zfex and libsodium. Done when the host tests pass.
2. **M1 On-air compatibility.** Any ESP32 sends wfb-ng frames with a test payload (a counter). Done when the NUC's unmodified `wfb_rx` decodes them and the counter arrives on port 5600.
3. **M2 Camera video.** The ESP32-CAM sends RTP/JPEG. Done when `rx.sh` shows the video and the latency is measured.
4. **M3 Tuning.** Measure frame rate, bitrate and latency against resolution, MCS and FEC. Done when the results are in the README.
5. **M4 Base-station receive.** An Ethernet ESP32 receives one camera and sends its RTP/JPEG over Ethernet to the NUC. Done when `receiver.sh` on the NUC shows it.
6. **M5 Three cameras to the video server.** The base station handles three `radio_port`s and serves them in the format the video server reads. Done when all three show up in the video server.

## Open questions and risks

| question or risk | answered by |
|---|---|
| Does `esp_wifi_80211_tx()` send wfb-ng's data frame header with our MAC address bytes unchanged? | M1 |
| Does `wfb_rx` check any field the ESP32 fills in differently (sequence number, duration)? | M1 |
| Is the ESP32 fast enough for encryption plus FEC at a few Mbit/s? | M3 |
| OV2640 JPEGs must be parsed for RFC 2435, and their quantization tables sent in-band | M2 |
| Most Ethernet ESP32 boards have no PSRAM, which leaves about 300 KB for three streams | M4 |
| Which Ethernet ESP32 board? (Decides the Ethernet pin config.) | before M4 |
| Which video server, and what input does it take (MJPEG over HTTP, RTSP, ...)? | before M5 |
| Which ESP-IDF release to pin | M0 |
| wfb-ng is GPLv3, so firmware built from its code is GPLv3. This repo has no license file yet. | before publishing firmware |
