# open-ipc latency playground

Webcam → H.264 → RTP/UDP → player, built to drop a wfb-ng (OpenIPC FPV) WiFi link in the middle later.

```
now:    sender.sh → 127.0.0.1:5600 ─────────────────────────────────────→ receiver.sh
later:  sender.sh → :5602 → wfb_tx → WiFi ))) ((( WiFi → wfb_rx → :5600 → receiver.sh
```

The receiver listens on port 5600, where wfb_rx delivers by default. With the link in place, the sender targets port 5602 (`PORT=5602`), where `link.sh` runs `wfb_tx`. That way both ends can run on one machine without a port clash.

## Quick start (two machines)

```sh
./tx.sh     # machine with the webcam + injection-capable adapter: preview of what is sent
./rx.sh     # other machine: the received video opens here
```

Each one asks for your sudo password, runs its half of the radio link and the sender or receiver, and stops it all on Ctrl-C, when you close its window, or when the radio link stops. Compare the `TX` timestamp in both windows to see what the link adds. The WiFi card is offline while the link runs.

## Setup

```sh
git clone --recursive https://github.com/magnuszetterberg/open-ipc.git
cd open-ipc
make -C wfb-ng all_bin        # needs libpcap and libsodium
```

`keys/` holds test keys derived from the password `change-me` (`cd keys && ../wfb-ng/wfb_keygen change-me`). The same password always gives the same pair, so every clone can talk to every other. The transmitter uses `drone.key` and the receiver uses `gs.key`. For anything beyond desk tests, generate your own pair with a real password (or none, for a random pair) and copy it to both machines. Anyone with these test keys can read and inject into the link.

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

To split the two ends across two machines, pass `tx` or `rx`. If the default interface name doesn't exist, the script uses the machine's first WiFi interface. Set `TX_IF`/`RX_IF` to choose another, and use the same `CHANNEL` on both:

```sh
# transmitter machine
sudo ./link.sh tx
PORT=5602 ./sender.sh

# receiver machine
sudo ./link.sh rx
./receiver.sh
```

Across a room, raise the transmit power: `sudo TX_POWER=2000 ./link.sh tx` (20 dBm). Press Ctrl-C in terminal 1 to hand the adapters back to NetworkManager. The keys in `keys/` come from `wfb-ng/wfb_keygen`: `drone.key` is used by the transmitter and `gs.key` by the receiver.

## Check what the WiFi card hears

```sh
sudo ./sniff.sh [iface]       # default wlan0; counts frames on channel 36 (wfb-ng) and 128 (control)
```

If the card sees no beacons even on a channel with a nearby router, its monitor mode doesn't capture anything. That was the case for the Intel AX201 here.

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
