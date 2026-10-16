// The SR250's radar session (app layer, R4): which UCI commands start it, and the channel impulse responses
// (CIRs) it then reports.
#pragma once

#include <stddef.h>
#include <stdint.h>

// One CIR notification's contents (UCI 0x69 0x8A, reassembled), as the SR250 sends them.
struct radar_cirs
{
    uint32_t session;
    const uint8_t *data;   // the notification after the session handle: status, counts, then the CIRs
    size_t size;
};

// Resets the SR250, configures the shield's antennas and starts a radar session on channel 9. 0 or negative.
int radar_start();

// Waits for the next CIR notification; false on timeout or error. Other messages are counted and skipped.
bool radar_next(radar_cirs *out, int timeout_ms);

// One measurement as a link payload, "R1" format (pc/radar_view.py reads it):
//   'R' '1', sequence (u16), number of CIRs, taps per CIR, CIR start offset (u16, from the first CIR),
//   then per CIR: its receive antenna, and per tap its strength in half dB (2 * 10 log10(I^2 + Q^2)).
// Little-endian. Returns the size, or 0 if the measurement doesn't parse or doesn't fit.
size_t radar_pack(const radar_cirs &cirs, uint16_t seq, uint8_t *out, size_t out_size);

// Counters for the stats line
struct radar_stats
{
    uint32_t notifications = 0;   // CIR notifications
    uint32_t bytes = 0;           // in them
    uint32_t other = 0;           // other UCI messages
    uint32_t errors = 0;          // SPI or framing errors
};
extern radar_stats radar_counts;
