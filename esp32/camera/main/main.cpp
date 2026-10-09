// Camera firmware (app layer, R4): camera -> RTP/JPEG -> wfb-ng -> radio. Or, for testing the link
// itself, a numbered counter instead of video (Kconfig: "What to send").
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
#if CONFIG_LINK_PAYLOAD_VIDEO
#include "cam.hpp"
#include "rtp_jpeg.hpp"
#endif

static const char *TAG = "camera";

static const wifi_phy_rate_t mcs_rates[] = {
    WIFI_PHY_RATE_MCS0_LGI, WIFI_PHY_RATE_MCS1_LGI, WIFI_PHY_RATE_MCS2_LGI, WIFI_PHY_RATE_MCS3_LGI,
    WIFI_PHY_RATE_MCS4_LGI, WIFI_PHY_RATE_MCS5_LGI, WIFI_PHY_RATE_MCS6_LGI, WIFI_PHY_RATE_MCS7_LGI,
};

struct link
{
    wfb_tx *tx;
    radio_tx *radio;
};

// Radio counters since the last stats line.
struct radio_delta
{
    uint32_t frames = 0, dropped = 0;
    void line(radio_tx *radio, uint32_t *frames_s, uint32_t *dropped_s)
    {
        *frames_s = radio->frames - frames;
        *dropped_s = radio->dropped - dropped;
        frames = radio->frames;
        dropped = radio->dropped;
    }
};

#if CONFIG_LINK_PAYLOAD_VIDEO

// Each RTP packet becomes one wfb-ng payload.
struct wfb_rtp_sink : rtp_jpeg_sink
{
    wfb_tx *tx;
    uint64_t now_us = 0;
    uint32_t packets = 0, errors = 0;
    void send_rtp(const uint8_t *packet, size_t size) override
    {
        if (tx->send(packet, size, now_us) == WFB_OK)
        {
            packets += 1;
        }
        else
        {
            errors += 1;
        }
    }
};

static void video_task(void *arg)
{
    link *l = static_cast<link *>(arg);
    static wfb_rtp_sink sink;
    sink.tx = l->tx;
    rtp_jpeg_config rtp_cfg;
    rtp_cfg.max_packet = l->tx->max_payload();
    static rtp_jpeg_packetizer packetizer(sink, rtp_cfg);  // static: its packet buffer is 1.5 KB

    radio_delta delta;
    uint64_t next_stats_us = esp_timer_get_time() + 1000000;
    uint32_t frames = 0, bytes = 0, refused = 0;
    rtp_jpeg_status last_refusal = RTP_JPEG_OK;

    for (;;)
    {
        camera_fb_t *fb = cam_capture();
        if (fb == nullptr)
        {
            ESP_LOGW(TAG, "camera: no frame");
            continue;
        }
        sink.now_us = esp_timer_get_time();
        uint32_t timestamp = (uint32_t)(sink.now_us * 9 / 100);  // RTP's 90 kHz clock
        rtp_jpeg_status rc = packetizer.send_frame(fb->buf, fb->len, timestamp);
#if CONFIG_CAMERA_CLOSE_BLOCK_PER_FRAME
        l->tx->close_block();  // the frame's last packets need no later frame to be recoverable
#endif
        if (rc == RTP_JPEG_OK)
        {
            frames += 1;
            bytes += fb->len;
        }
        else
        {
            refused += 1;
            last_refusal = rc;
        }
        cam_release(fb);

        if (sink.now_us >= next_stats_us)
        {
            next_stats_us += 1000000;
            uint32_t frames_s, dropped_s;
            delta.line(l->radio, &frames_s, &dropped_s);
            ESP_LOGI(TAG, "video: %lu fps, %lu KB/frame, %lu packets/s, %lu frames/s on air, %lu dropped by WiFi, "
                          "%lu send errors, %lu JPEGs refused%s%s",
                     (unsigned long)frames, (unsigned long)(frames ? bytes / frames / 1024 : 0),
                     (unsigned long)sink.packets, (unsigned long)frames_s, (unsigned long)dropped_s,
                     (unsigned long)sink.errors, (unsigned long)refused, refused ? ": " : "",
                     refused ? rtp_jpeg_status_name(last_refusal) : "");
            frames = bytes = refused = 0;
            sink.packets = sink.errors = 0;
        }
    }
}

#else

// Payload: "esp32 counter <n>" then padding to CONFIG_LINK_COUNTER_SIZE, so esp32/tools/counter_check.py
// on the receiver can spot gaps.
static void counter_task(void *arg)
{
    link *l = static_cast<link *>(arg);
    static uint8_t payload[CONFIG_LINK_COUNTER_SIZE];
    memset(payload, '.', sizeof(payload));

    const TickType_t period = pdMS_TO_TICKS(1000) / CONFIG_LINK_COUNTER_RATE > 0
                                  ? pdMS_TO_TICKS(1000) / CONFIG_LINK_COUNTER_RATE
                                  : 1;
    TickType_t wake = xTaskGetTickCount();
    uint64_t next_stats_us = esp_timer_get_time() + 1000000;
    uint32_t counter = 0, sent = 0, errors = 0;
    radio_delta delta;

    for (;;)
    {
        int n = snprintf((char *)payload, sizeof(payload), "esp32 counter %lu ", (unsigned long)counter);
        payload[n] = '.';
        uint64_t now = esp_timer_get_time();
        wfb_status rc = l->tx->send(payload, sizeof(payload), now);
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
            uint32_t frames_s, dropped_s;
            delta.line(l->radio, &frames_s, &dropped_s);
            ESP_LOGI(TAG, "counter %lu: %lu payloads/s, %lu frames/s, %lu dropped by WiFi (%s), %lu errors",
                     (unsigned long)counter, (unsigned long)sent, (unsigned long)frames_s, (unsigned long)dropped_s,
                     esp_err_to_name(l->radio->last_error), (unsigned long)errors);
            sent = errors = 0;
        }
        vTaskDelayUntil(&wake, period);
    }
}

#endif

extern "C" void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "ESP32 rev %d.%d, %d cores, PSRAM %u bytes", chip.revision / 100, chip.revision % 100, chip.cores,
             (unsigned)esp_psram_get_size());

#if CONFIG_LINK_PAYLOAD_VIDEO
    cam_config cam_cfg;
#if CONFIG_CAMERA_FRAME_QVGA
    cam_cfg.frame_size = FRAMESIZE_QVGA;
#elif CONFIG_CAMERA_FRAME_SVGA
    cam_cfg.frame_size = FRAMESIZE_SVGA;
#else
    cam_cfg.frame_size = FRAMESIZE_VGA;
#endif
    cam_cfg.jpeg_quality = CONFIG_CAMERA_JPEG_QUALITY;
    if (cam_start(cam_cfg) != ESP_OK)
    {
        return;
    }
#endif

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

    static link l = {&tx, &radio};
#if CONFIG_LINK_PAYLOAD_VIDEO
    xTaskCreatePinnedToCore(video_task, "video", 6144, &l, 5, nullptr, 1);  // core 1 (Coding conventions)
#else
    xTaskCreatePinnedToCore(counter_task, "counter", 4096, &l, 5, nullptr, 1);
#endif
}
