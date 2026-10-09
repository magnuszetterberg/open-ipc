// Camera firmware (app layer, R4). M1: sends a counter over wfb-ng, the first on-air payload (#8).
#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keys.hpp"
#include "radio.hpp"
#include "wfb_tx.hpp"

static const char *TAG = "camera";

static const wifi_phy_rate_t mcs_rates[] = {
    WIFI_PHY_RATE_MCS0_LGI, WIFI_PHY_RATE_MCS1_LGI, WIFI_PHY_RATE_MCS2_LGI, WIFI_PHY_RATE_MCS3_LGI,
    WIFI_PHY_RATE_MCS4_LGI, WIFI_PHY_RATE_MCS5_LGI, WIFI_PHY_RATE_MCS6_LGI, WIFI_PHY_RATE_MCS7_LGI,
};

// Payload: "esp32 counter <n>" then padding to CONFIG_LINK_COUNTER_SIZE, so esp32/tools/counter_check.py
// on the receiver can spot gaps.
struct link
{
    wfb_tx *tx;
    radio_tx *radio;
};

static void counter_task(void *arg)
{
    wfb_tx *tx = static_cast<link *>(arg)->tx;
    radio_tx *radio = static_cast<link *>(arg)->radio;
    static uint8_t payload[CONFIG_LINK_COUNTER_SIZE];
    memset(payload, '.', sizeof(payload));

    const TickType_t period = pdMS_TO_TICKS(1000) / CONFIG_LINK_COUNTER_RATE > 0
                                  ? pdMS_TO_TICKS(1000) / CONFIG_LINK_COUNTER_RATE
                                  : 1;
    TickType_t wake = xTaskGetTickCount();
    uint64_t next_stats_us = esp_timer_get_time() + 1000000;
    uint32_t counter = 0, sent = 0, errors = 0, frames = 0, dropped = 0;

    for (;;)
    {
        int n = snprintf((char *)payload, sizeof(payload), "esp32 counter %lu ", (unsigned long)counter);
        payload[n] = '.';
        uint64_t now = esp_timer_get_time();
        wfb_status rc = tx->send(payload, sizeof(payload), now);
        if (rc == WFB_OK)
        {
            sent += 1;
        }
        else
        {
            errors += 1;
            ESP_LOGW(TAG, "send: %s", wfb_status_name(rc));
        }
        counter += 1;

        if (now >= next_stats_us)
        {
            next_stats_us += 1000000;
            ESP_LOGI(TAG, "counter %lu: %lu payloads/s, %lu frames/s, %lu dropped by WiFi (%s), %lu errors",
                     (unsigned long)counter, (unsigned long)sent, (unsigned long)(radio->frames - frames),
                     (unsigned long)(radio->dropped - dropped), esp_err_to_name(radio->last_error),
                     (unsigned long)errors);
            sent = errors = 0;
            frames = radio->frames;
            dropped = radio->dropped;
        }
        vTaskDelayUntil(&wake, period);
    }
}

extern "C" void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "ESP32 rev %d.%d, %d cores, PSRAM %u bytes", chip.revision / 100, chip.revision % 100, chip.cores,
             (unsigned)esp_psram_get_size());

    radio_config radio_cfg;
    radio_cfg.channel = CONFIG_LINK_CHANNEL;
    radio_cfg.rate = mcs_rates[CONFIG_LINK_MCS];
    radio_cfg.tx_power_dbm = CONFIG_LINK_TX_POWER_DBM;
    ESP_ERROR_CHECK(radio_start(radio_cfg));

    const uint32_t channel_id = ((uint32_t)CONFIG_LINK_ID << 8) + CONFIG_LINK_RADIO_PORT;
    static radio_tx radio(channel_id);
    static wfb_tx tx(radio);
    wfb_tx_config tx_cfg;
    tx_cfg.fec_k = CONFIG_LINK_FEC_K;
    tx_cfg.fec_n = CONFIG_LINK_FEC_N;
    tx_cfg.channel_id = channel_id;
    size_t key_size = 0;
    const uint8_t *key = keys_link_key(&key_size);
    wfb_status rc = tx.init(key, key_size, tx_cfg);
    if (rc != WFB_OK)
    {
        ESP_LOGE(TAG, "wfb_tx init: %s", wfb_status_name(rc));
        return;
    }
    ESP_LOGI(TAG, "wfb-ng link 0x%06x port %d, FEC %d/%d, max payload %u", CONFIG_LINK_ID, CONFIG_LINK_RADIO_PORT,
             CONFIG_LINK_FEC_K, CONFIG_LINK_FEC_N, (unsigned)tx.max_payload());

    static link counter_link = {&tx, &radio};
    xTaskCreatePinnedToCore(counter_task, "counter", 4096, &counter_link, 5, nullptr, 1);  // core 1 (Coding conventions)
}
