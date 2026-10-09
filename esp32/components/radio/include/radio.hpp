// Raw 802.11 transmit and receive for wfb-ng on the ESP32 (platform layer, R4).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
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

// One received wfb-ng packet: the 802.11 header and FCS are already stripped.
struct radio_frame
{
    uint32_t channel_id;  // (link_id << 8) + radio_port, from the second address
    int8_t rssi;          // dBm
    uint16_t size;
    uint8_t data[1500];
};

// Receives wfb-ng frames in promiscuous mode (R5). The WiFi driver's callback only checks and copies;
// the frames are handed over through a queue, to be decrypted and decoded in the caller's task.
// One instance: the driver's callback has no context pointer. Call radio_start() first.
class radio_rx
{
public:
    // slots: frames that can wait at once; allocated here, nothing later (Coding conventions).
    esp_err_t start(int slots);

    // The next frame, or nullptr after timeout. Hand it back with release() once processed.
    radio_frame *receive(TickType_t timeout);
    void release(radio_frame *frame);

    // Written by the WiFi driver's task, read by anyone: counts only, so no locking.
    volatile uint32_t heard = 0;     // data frames with a good FCS
    volatile uint32_t accepted = 0;  // wfb-ng frames queued
    volatile uint32_t overflow = 0;  // wfb-ng frames dropped: every slot was waiting
    volatile uint32_t bad = 0;       // frames received with errors

private:
    static void callback(void *buf, wifi_promiscuous_pkt_type_t type);
    QueueHandle_t free_slots = nullptr, full_slots = nullptr;
};
