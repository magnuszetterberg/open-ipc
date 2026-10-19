// What the radar finds, and the picture of it (core layer, R4): plain C++, no platform headers, tested on
// Linux with recorded measurements. The radar camera (esp32/radar) feeds it each measurement and sends what
// it found as radar_data (docs/DESIGN.md, M9); where it is watched, the page draws that, and on a PC
// pc/radar_render.cpp draws radar_picture from it for video. The camera can also send the picture itself.
#pragma once

#include <stddef.h>
#include <stdint.h>

// One measurement's finding.
struct radar_finding
{
    bool learning = true;     // still learning the room's still background
    bool moving = false;      // something moves, within range
    float distance_m = 0;     // how far, from the radar's own direct signal (smoothed over measurements)
    float strength_db = 0;    // how strongly it moves: change from the background, in dB
};

// Finds what moves, and how far away: the still background (walls, furniture) is learned slowly and taken
// away, where the radar sees a reflection well above its noise floor; the strongest change is the finding.
class radar_detector
{
public:
    static constexpr int MAX_ANTENNAS = 3;
    static constexpr int MAX_TAPS = 128;
    static constexpr float TAP_M = 0.15f;        // per CIR tap: ~1 ns, half of it there and half back
    static constexpr float MAX_M = 8.0f;         // beyond this the SR250-ARD's radar hears mostly noise
    static constexpr float THRESHOLD_DB = 1.5f;  // movement above this counts

    // One measurement in the radar's "R1" link format (esp32/radar/src/radar.hpp). false if it doesn't parse.
    bool add_r1(const uint8_t *payload, size_t size);

    radar_finding finding;
    int antennas = 0, taps = 0;
    int zero_tap = 0;                              // the radar's own direct signal: distance 0
    float movement[MAX_ANTENNAS][MAX_TAPS] = {};   // dB, 0 where there is no reflection
    uint32_t measurements = 0;

    float metres(int tap) const { return (tap - zero_tap) * TAP_M; }

private:
    void restart(int antennas, int taps);

    static constexpr int RECENT = 3;  // measurements the change is averaged over
    float background[MAX_ANTENNAS][MAX_TAPS] = {};
    float recent[RECENT][MAX_ANTENNAS][MAX_TAPS] = {};
    int seen = 0;
};

// What one measurement found, as the radar camera sends it and the picture draws it: the finding, and the
// movement against distance per antenna, from the radar's own direct signal (0 m) out to MAX_M.
struct radar_data
{
    static constexpr int MAX_TAPS = 64;   // 0 to MAX_M, TAP_M apart: 54
    radar_finding finding;
    uint32_t measurements = 0;            // since the radar camera started
    int antennas = 0, taps = 0;
    float movement[radar_detector::MAX_ANTENNAS][MAX_TAPS] = {};   // dB, tap k at k * TAP_M

    float metres(int tap) const { return tap * radar_detector::TAP_M; }
};

// The detector's latest measurement, as radar_data.
radar_data radar_data_of(const radar_detector &d);

// radar_data as a link payload, "D1" format (docs/DESIGN.md: Radar data), little-endian:
//   'D' '1', sequence (u16), flags (1: learning, 2: moving), antennas, taps, 0,
//   distance (u16, cm), strength (u16, 0.1 dB), measurements (u32),
//   then per antenna, per tap from 0 m, TAP_M apart: its movement in 0.1 dB (u8, up to 25.5 dB).
// At most 16 + 3 x 64 bytes; 124 for the SR250-ARD's two antennas. Returns the size, 0 if it doesn't fit.
static constexpr size_t RADAR_DATA_MAX = 16 + radar_detector::MAX_ANTENNAS * radar_data::MAX_TAPS;
size_t radar_data_pack(const radar_data &d, uint16_t seq, uint8_t *out, size_t out_size);
// false if the payload isn't "D1" or doesn't parse. seq: where to put its sequence number, if wanted.
bool radar_data_unpack(const uint8_t *payload, size_t size, radar_data *out, uint16_t *seq = nullptr);

// The picture: the finding as text, movement against distance per antenna, and the distance of what moves
// over the last 20 s. YCbCr, chroma halved across (JPEG 4:2:2, RFC 2435 type 0), for jpeg_encode_422().
class radar_picture
{
public:
    static constexpr int WIDTH = 320, HEIGHT = 240;
    static constexpr int HISTORY = 400;   // measurements in the trace: 20 s at 20 a second

    // Adds the latest finding to the trace: after each measurement.
    void add(const radar_data &d);
    // Draws everything: as often as pictures go out.
    void draw(const radar_data &d);

    uint8_t y[WIDTH * HEIGHT];
    uint8_t cb[WIDTH / 2 * HEIGHT];
    uint8_t cr[WIDTH / 2 * HEIGHT];

private:
    float history[HISTORY];   // distance per measurement, < 0 when nothing moved
    int history_count = 0;
};

// Baseline JPEG of a 4:2:2 picture, with the standard Huffman tables and the quality's scaled standard
// quantization tables, so rtp_jpeg sends it as RFC 2435 type 0. width a multiple of 16, height of 8.
// Returns the size written to out, 0 if it doesn't fit.
size_t jpeg_encode_422(const uint8_t *y, const uint8_t *cb, const uint8_t *cr, int width, int height,
                       int quality, uint8_t *out, size_t out_size);
