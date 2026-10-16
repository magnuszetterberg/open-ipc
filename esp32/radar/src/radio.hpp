// Raw 802.11 transmit for wfb-ng on the RW612 (platform layer, R4), with NXP's WiFi driver: the radio in
// monitor mode on a fixed channel, no network joined, and each wfb-ng packet sent as one 802.11 frame.
// The RW612's counterpart of the ESP32's radio component (esp_wifi_80211_tx); the core doesn't know which.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wfb_80211.hpp"
#include "wfb_core.hpp"

struct radio_config
{
    uint8_t channel = 6;   // 2.4 GHz: the base station and the ESP32 cameras have no 5 GHz (protocol table)
    uint8_t mcs = 3;       // HT20 MCS 3, as link.sh's MCS default
};

// Waits for the WiFi driver, then puts the radio in monitor mode on the channel. Call once, before radio_tx.
// Returns 0, or a negative error after logging what failed.
int radio_start(const radio_config &config);

// The largest wfb-ng packet one frame carries on this driver path (wfb_tx_config.max_packet).
size_t radio_max_packet();

// Sends each finished wfb-ng packet as one 802.11 frame (R5): wfb_core hands packets here.
class radio_tx : public wfb_sink
{
public:
    explicit radio_tx(uint32_t channel_id);
    void send_frame(const uint8_t *buf, size_t size) override;

    uint32_t frames = 0;   // sent
    uint32_t bytes = 0;    // sent, 802.11 header included
    uint32_t dropped = 0;  // refused by the WiFi driver
    int last_error = 0;

private:
    wfb_80211_framer framer;
    uint8_t frame[1500];   // the 802.11 frame as wfb_80211_framer builds it
};
