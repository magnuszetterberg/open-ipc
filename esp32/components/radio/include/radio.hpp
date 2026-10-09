// Raw 802.11 transmit for wfb-ng on the ESP32 (platform layer, R4).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"
#include "wfb_80211.hpp"
#include "wfb_core.hpp"

struct radio_config
{
    uint8_t channel = 6;                           // 2.4 GHz only on the ESP32 (protocol table)
    wifi_phy_rate_t rate = WIFI_PHY_RATE_MCS3_LGI; // HT20 MCS 3, as link.sh's MCS default
    int8_t tx_power_dbm = 20;
};

// Starts WiFi without joining a network, on a fixed channel and rate. Call once, before radio_tx.
esp_err_t radio_start(const radio_config &config);

// Sends each finished wfb-ng packet as one 802.11 frame (R5): wfb_core hands packets here.
class radio_tx : public wfb_sink
{
public:
    explicit radio_tx(uint32_t channel_id);
    void send_frame(const uint8_t *buf, size_t size) override;

    uint32_t frames = 0;     // sent
    uint32_t bytes = 0;      // sent, 802.11 header included
    uint32_t dropped = 0;    // refused by the WiFi driver (its queue was full)
    esp_err_t last_error = ESP_OK;

private:
    wfb_80211_framer framer;
    uint8_t frame[1500];     // esp_wifi_80211_tx()'s limit
};
