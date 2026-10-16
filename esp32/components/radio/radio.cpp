#include "radio.hpp"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include <string.h>

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

static radio_rx *receiver = nullptr;  // the driver's callback has no context argument

esp_err_t radio_rx::start(int slots)
{
    free_slots = xQueueCreate(slots, sizeof(radio_frame *));
    full_slots = xQueueCreate(slots, sizeof(radio_frame *));
    if (free_slots == nullptr || full_slots == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < slots; i++)
    {
        // Internal RAM, so the driver's callback doesn't wait on PSRAM; and byte-addressable: on the
        // ESP32, MALLOC_CAP_INTERNAL alone may hand out instruction RAM, where byte writes fault.
        radio_frame *f = static_cast<radio_frame *>(
            heap_caps_malloc(sizeof(radio_frame), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (f == nullptr)
        {
            return ESP_ERR_NO_MEM;
        }
        xQueueSend(free_slots, &f, 0);
    }
    receiver = this;

    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;  // wfb-ng sends data frames only
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(&radio_rx::callback));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_LOGI(TAG, "receiving wfb-ng frames, %d slots of %u bytes", slots, (unsigned)sizeof(radio_frame));
    return ESP_OK;
}

void radio_rx::callback(void *buf, wifi_promiscuous_pkt_type_t type)
{
    radio_rx *self = receiver;
    const wifi_promiscuous_pkt_t *pkt = static_cast<const wifi_promiscuous_pkt_t *>(buf);
    if (self == nullptr || type != WIFI_PKT_DATA)
    {
        return;
    }
    if (pkt->rx_ctrl.rx_state != 0 || pkt->rx_ctrl.sig_len < 4)
    {
        self->bad = self->bad + 1;
        return;
    }
    self->heard = self->heard + 1;

    uint32_t channel_id;
    const uint8_t *packet;
    size_t packet_size;
    // sig_len includes the 4-byte FCS (esp_wifi_types_native.h); rx.cpp drops it the same way
    if (!wfb_80211_unframe(pkt->payload, pkt->rx_ctrl.sig_len - 4, &channel_id, &packet, &packet_size) ||
        packet_size > sizeof(radio_frame::data))
    {
        return;
    }
    radio_frame *f;
    if (xQueueReceive(self->free_slots, &f, 0) != pdTRUE)
    {
        self->overflow = self->overflow + 1;
        return;
    }
    f->channel_id = channel_id;
    f->rssi = pkt->rx_ctrl.rssi;
    f->size = packet_size;
    memcpy(f->data, packet, packet_size);
    xQueueSend(self->full_slots, &f, 0);
    self->accepted = self->accepted + 1;
}

radio_frame *radio_rx::receive(TickType_t timeout)
{
    radio_frame *f;
    return xQueueReceive(full_slots, &f, timeout) == pdTRUE ? f : nullptr;
}

void radio_rx::release(radio_frame *frame)
{
    xQueueSend(free_slots, &frame, 0);
}
