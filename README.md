# open-ipc latency playground

Webcam → H.264 → RTP/UDP → player, built to drop a wfb-ng (OpenIPC FPV) WiFi link in the middle later.

```
now:    sender.sh → 127.0.0.1:5600 ─────────────────────────────────────→ receiver.sh
later:  sender.sh → :5600 → wfb_tx → WiFi ))) ((( WiFi → wfb_rx → :5600 → receiver.sh
```

Both scripts already use port 5600, which is wfb-ng's default, so nothing changes when the link is added.

## Setup

```sh
git clone --recursive https://github.com/magnuszetterberg/open-ipc.git
cd open-ipc
make -C wfb-ng all_bin        # needs libpcap and libsodium
```

The `keys/` folder isn't tracked in git. Generate a key pair once with `mkdir -p keys && (cd keys && ../wfb-ng/wfb_keygen)`, then copy it to the other machine (e.g. `scp -r keys other-host:open-ipc/`). The transmitter uses `drone.key` and the receiver uses `gs.key`, so both machines must have keys from the same pair.

## Run

```sh
./receiver.sh                 # terminal 1
./sender.sh                   # terminal 2 (webcam, 720p, x264)
```

Options for the sender, set as environment variables: `SOURCE=webcam|test`, `ENCODER=x264|vaapi`, `SIZE=1280x720`, `FPS=30`, `BITRATE=4M`, `HOST`, `PORT`, `DEVICE`.
Options for the receiver: `PLAYER=auto|gst|ffplay`, `PORT`.

The GStreamer receiver needs `sudo pacman -S gst-plugins-base gst-plugins-good`; without those plugins it falls back to ffplay.

## Over a real wfb-ng link (one machine, two adapters)

`link.sh` puts the Ralink stick (`wlan1`, transmitting) and the Intel card (`wlan0`, receiving) into monitor mode on channel 36, then runs `wfb_tx` and `wfb_rx` between them. Build wfb-ng first (`make -C wfb-ng all_bin`) and make sure ethernet is up, since `wlan0` drops off the network while the link runs.

```sh
sudo ./link.sh                # terminal 1: the radio link
./receiver.sh                 # terminal 2
PORT=5602 ./sender.sh         # terminal 3: into wfb_tx instead of straight to the receiver
```

To split the two ends across two machines, set `ROLE` and point the interface at the adapter on that machine (check with `iw dev`). Use the same `CHANNEL` on both:

```sh
# transmitter machine
sudo ROLE=tx TX_IF=wlan1 ./link.sh
PORT=5602 ./sender.sh

# receiver machine
sudo ROLE=rx RX_IF=wlan0 ./link.sh
./receiver.sh
```

Across a room, raise the transmit power, e.g. `TX_POWER=2000` (20 dBm). Press Ctrl-C in terminal 1 to hand the adapters back to NetworkManager. The keys in `keys/` come from `wfb-ng/wfb_keygen`: `drone.key` is used by the transmitter and `gs.key` by the receiver.

## Measure latency

1. Open `clock.html` in a browser. It shows a millisecond clock in UTC.
2. Point the webcam at the clock and put the receiver window next to it.
3. Take a screenshot. You'll see three times:
   - **clock**: the time of the screenshot.
   - **clock as seen by the webcam, in the RX window**: when the light hit the sensor.
   - **`TX hh:mm:ss.mmm` overlay**: when ffmpeg got the frame.

   | difference                    | measures                                     |
   |-------------------------------|----------------------------------------------|
   | clock − webcam-seen clock     | full camera-to-screen latency                |
   | TX overlay − webcam-seen clock | webcam latency (sensor, USB, MJPEG)          |
   | clock − TX overlay            | encode + network + decode + display          |

   `SOURCE=test` leaves out the camera and measures only the last row.

Take several screenshots and average them. A 60 Hz screen limits each reading to about ±17 ms, and a 30 fps camera to about ±33 ms. The overlay and the clock use the same wall clock, so the readings are only directly comparable when sender and receiver run on the same machine. Across two machines, sync their clocks with chrony first.
