// The SR250 UWB radar over SPI (platform layer, R4): reset, then UCI messages in and out. Our own client,
// from the SR250's host interface as NXP's documents and open board files describe it; none of NXP's UWB
// software is in it (docs/DESIGN.md, Not goals).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The largest UCI message (header and payload) uwb_receive() takes; a radar notification with CIRs from
// three antennas fits.
#define UWB_MAX_MESSAGE 4096

// Resets the SR250 and waits for its boot loader's ready message, after which it runs its firmware and
// speaks UCI. 0, or a negative error after logging what failed.
int uwb_start(void);

// Sends one UCI packet (4-byte header and payload). 0, or a negative error.
int uwb_send(const uint8_t *packet, size_t size);

// Receives one UCI packet into buf (header first). Its size, 0 on timeout, or a negative error.
int uwb_receive(uint8_t *buf, size_t size, int timeout_ms);

#ifdef __cplusplus
}
#endif
