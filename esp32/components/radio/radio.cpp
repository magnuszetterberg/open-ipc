#include "radio.hpp"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

static const char *TAG = "radio";

esp_err_t radio_start(const radio_config &config)
{
    esp_err_t err = nvs_flash_init();  // the WiFi driver keeps calibration data in NVS
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));  // station that never joins: raw frames only
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20));
    ESP_ERROR_CHECK(esp_wifi_config_80211_tx_rate(WIFI_IF_STA, config.rate));  // before esp_wifi_start()
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // power save would delay frames
    ESP_ERROR_CHECK(esp_wifi_set_channel(config.channel, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(config.tx_power_dbm * 4));  // in 0.25 dBm steps

    int8_t power = 0;
    esp_wifi_get_max_tx_power(&power);
    ESP_LOGI(TAG, "raw transmit on channel %u, rate 0x%02x, %d.%02d dBm", config.channel, config.rate, power / 4,
             (power % 4) * 25);
    return ESP_OK;
}

radio_tx::radio_tx(uint32_t channel_id) : framer(channel_id)
{
}

void radio_tx::send_frame(const uint8_t *buf, size_t size)
{
    size_t frame_size = framer.frame(frame, sizeof(frame), buf, size);
    if (frame_size == 0)
    {
        dropped += 1;
        last_error = ESP_ERR_INVALID_SIZE;
        return;
    }
    // en_sys_seq false: keep wfb-ng's sequence numbers, as wfb_tx does
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, frame, frame_size, false);
    if (err != ESP_OK)
    {
        dropped += 1;
        last_error = err;
        return;
    }
    frames += 1;
    bytes += frame_size;
}
