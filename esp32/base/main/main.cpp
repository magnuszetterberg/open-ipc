// Base-station firmware (app layer, R4). M4 step 1 (#17): receive one camera's wfb-ng frames in
// promiscuous mode, decode them with wfb_rx, and report once a second what came through: the
// counter (numbered payloads) or RTP/JPEG video. Ethernet output follows in #18.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keys.hpp"
#include "radio.hpp"
#include "wfb_rx.hpp"

static const char *TAG = "base";

// Recognizes what the camera sends: "esp32 counter <n>" payloads, or RTP/JPEG (payload type 26).
struct payload_check : wfb_payload_sink
{
    uint32_t payloads = 0, bytes = 0;
    uint32_t counters = 0, missing = 0;  // counter mode
    uint32_t rtp = 0, video_frames = 0;  // video mode: RTP packets, and frames (marker bits)
    uint32_t other = 0;
    long last_counter = -1;

    void send_payload(const uint8_t *buf, size_t size) override
    {
        payloads += 1;
        bytes += size;
        if (size > 14 && memcmp(buf, "esp32 counter ", 14) == 0)
        {
            long n = strtol((const char *)buf + 14, nullptr, 10);
            if (last_counter >= 0 && n > last_counter + 1)
            {
                missing += n - last_counter - 1;
            }
            last_counter = n;
            counters += 1;
        }
        else if (size >= 12 && (buf[0] & 0xc0) == 0x80 && (buf[1] & 0x7f) == 26)
        {
            rtp += 1;
            video_frames += (buf[1] & 0x80) ? 1 : 0;
        }
        else
        {
            other += 1;
        }
    }
};

static void receive_task(void *arg)
{
    radio_rx *radio = static_cast<radio_rx *>(arg);
    const uint32_t channel_id = ((uint32_t)CONFIG_LINK_ID << 8) + CONFIG_LINK_RADIO_PORT;

    static payload_check check;
    static wfb_rx rx(check);
    wfb_rx_config cfg;
    cfg.channel_id = channel_id;
    cfg.ring_size = CONFIG_LINK_RING_SIZE;
    size_t key_size = 0;
    const uint8_t *key = keys_link_key(&key_size);
    wfb_status rc = rx.init(key, key_size, cfg);
    if (rc != WFB_OK)
    {
        ESP_LOGE(TAG, "wfb_rx init: %s", wfb_status_name(rc));
        vTaskDelete(nullptr);
    }
    ESP_LOGI(TAG, "listening for link 0x%06x port %d; %u bytes of internal RAM left", CONFIG_LINK_ID,
             CONFIG_LINK_RADIO_PORT, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    uint64_t next_stats_us = esp_timer_get_time() + 1000000;
    uint32_t ours = 0, others = 0;
    int rssi_sum = 0;
    uint32_t last_heard = 0, last_overflow = 0;
    wfb_rx_stats last = rx.stats;

    for (;;)
    {
        radio_frame *f = radio->receive(pdMS_TO_TICKS(100));
        if (f != nullptr)
        {
            if (f->channel_id == channel_id)
            {
                ours += 1;
                rssi_sum += f->rssi;
                rx.process_packet(f->data, f->size);
            }
            else
            {
                others += 1;  // another camera or link
            }
            radio->release(f);
        }

        uint64_t now = esp_timer_get_time();
        if (now < next_stats_us)
        {
            continue;
        }
        next_stats_us += 1000000;
        const wfb_rx_stats &s = rx.stats;
        uint32_t heard = radio->heard, overflow = radio->overflow;
        ESP_LOGI(TAG, "%lu frames for us (rssi %d), %lu other wfb-ng, %lu data frames heard, %lu dropped (slots full) | "
                      "wfb: data %lu, dec_err %lu, fec_rec %lu, lost %lu, out %lu",
                 (unsigned long)ours, ours ? rssi_sum / (int)ours : 0, (unsigned long)others,
                 (unsigned long)(heard - last_heard), (unsigned long)(overflow - last_overflow),
                 (unsigned long)(s.count_p_data - last.count_p_data),
                 (unsigned long)(s.count_p_dec_err - last.count_p_dec_err),
                 (unsigned long)(s.count_p_fec_recovered - last.count_p_fec_recovered),
                 (unsigned long)(s.count_p_lost - last.count_p_lost),
                 (unsigned long)(s.count_p_outgoing - last.count_p_outgoing));
        if (check.counters)
        {
            ESP_LOGI(TAG, "counter: %lu received, last %ld, %lu missing", (unsigned long)check.counters, check.last_counter,
                     (unsigned long)check.missing);
        }
        if (check.rtp)
        {
            ESP_LOGI(TAG, "video: %lu fps, %lu RTP packets, %lu kbit/s", (unsigned long)check.video_frames,
                     (unsigned long)check.rtp, (unsigned long)(check.bytes * 8 / 1000));
        }
        if (check.other)
        {
            ESP_LOGW(TAG, "%lu payloads neither counter nor RTP/JPEG", (unsigned long)check.other);
        }
        last = s;
        last_heard = heard;
        last_overflow = overflow;
        ours = others = 0;
        rssi_sum = 0;
        check.payloads = check.bytes = check.counters = check.missing = check.rtp = check.video_frames = check.other = 0;
    }
}

extern "C" void app_main(void)
{
    radio_config radio_cfg;
    radio_cfg.channel = CONFIG_LINK_CHANNEL;
    ESP_ERROR_CHECK(radio_start(radio_cfg));
    static radio_rx radio;
    ESP_ERROR_CHECK(radio.start(CONFIG_LINK_RX_SLOTS));
    xTaskCreatePinnedToCore(receive_task, "receive", 6144, &radio, 5, nullptr, 1);  // core 1 (Coding conventions)
}
