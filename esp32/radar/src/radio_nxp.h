// The calls into NXP's WiFi driver, in C: its headers don't build as C++. radio.cpp is the C++ side.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Waits for the driver, then monitor mode on the channel and a fixed HT20 rate. 0, or a negative error.
int radio_nxp_start(uint8_t channel, uint8_t mcs);

// The largest frame body one frame carries on the configured driver path.
size_t radio_nxp_max_body(void);

// The firmware's per-frame tx_control on the command path: TXPD_TXRATE_ENABLE (bit 15) and a rate index from
// bit 16; 0 leaves the rate to the firmware. Kconfig RADIO_TX_CONTROL; the rate sweep changes it while running.
extern uint32_t radio_nxp_tx_control;

// One 802.11 frame: header (the 24 bytes wfb-ng builds) and body. 0, or the driver's error.
int radio_nxp_send(const uint8_t *header, const uint8_t *body, size_t body_size);

#ifdef __cplusplus
}
#endif
