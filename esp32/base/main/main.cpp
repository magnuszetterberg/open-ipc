// Base-station firmware (app layer, R4): receives up to three cameras' wfb-ng streams in promiscuous mode,
// decodes them with wfb_rx, and serves them on the LAN (docs/DESIGN.md, Base station out):
// - each camera's video as RTSP, rtsp://<base>:8554/camN (RTP/JPEG passed through),
// - each camera's telemetry as UDP, with the signal quality measured here added,
// - camera 0's video also as UDP to one host, as in M4 (#18).
// One task does it all, so nothing is locked: it takes frames off the radio and polls the RTSP server.
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "eth.hpp"
#include "keys.hpp"
#include "radio.hpp"
#include "rtsp_server.hpp"
#include "wfb_rx.hpp"

static const char *TAG = "base";

static rtsp_server rtsp;
static bool rtsp_up = false;
static udp_out *video_out = nullptr;  // camera 0's video to BASE_OUT_HOST, when set

// Signal quality of one camera over the last second, measured here (the camera can't know it).
struct signal_quality
{
    int rssi_dbm = 0;
    float loss_pct = 0;
    uint32_t video_kbps = 0;
    bool valid = false;
};

// Recovered video: RTP/JPEG to the camera's RTSP clients (and camera 0's to video_out).
struct video_sink : wfb_payload_sink
{
    int index = 0;
    uint32_t packets = 0, frames = 0, bytes = 0;
    void send_payload(const uint8_t *buf, size_t size) override
    {
        packets += 1;
        bytes += size;
        frames += (size >= 12 && (buf[1] & 0x80)) ? 1 : 0;  // the RTP marker ends a frame
        if (rtsp_up)
        {
            rtsp.send_rtp(index, buf, size);
        }
        if (index == 0 && video_out != nullptr)
        {
            video_out->send_payload(buf, size);
        }
    }
};

// Recovered telemetry: the camera's JSON, with this camera's signal quality added before the closing brace.
struct telemetry_sink : wfb_payload_sink
{
    const signal_quality *signal = nullptr;
    udp_out out;
    bool out_open = false;
    uint32_t messages = 0;
    char last[320] = "";
    void send_payload(const uint8_t *buf, size_t size) override
    {
        messages += 1;
        const uint8_t *close = size > 0 ? (const uint8_t *)memrchr(buf, '}', size) : nullptr;
        size_t head = close ? (size_t)(close - buf) : 0;
        if (close == nullptr || head > sizeof(last) - 96)
        {
            return;  // not the JSON object the camera sends
        }
        memcpy(last, buf, head);
        int n = signal->valid ? snprintf(last + head, sizeof(last) - head,
                                         ",\"rssi_dbm\":%d,\"loss_pct\":%.1f,\"video_kbps\":%lu}", signal->rssi_dbm,
                                         signal->loss_pct, (unsigned long)signal->video_kbps)
                              : snprintf(last + head, sizeof(last) - head, "}");
        if (out_open)
        {
            out.send_payload((const uint8_t *)last, head + n);
        }
    }
};

struct camera
{
    video_sink video_out;
    telemetry_sink telemetry_out;
    wfb_rx video{video_out};
    wfb_rx telemetry{telemetry_out};
    signal_quality signal;
    uint32_t frames_heard = 0;
    int rssi_sum = 0;
    wfb_rx_stats last;
};

static camera cameras[CONFIG_BASE_CAMERAS];
static const char *const rtsp_names[] = {"cam0", "cam1", "cam2"};

static bool init_cameras(void)
{
    size_t key_size = 0;
    const uint8_t *key = keys_link_key(&key_size);
    for (int i = 0; i < CONFIG_BASE_CAMERAS; i++)
    {
        camera &c = cameras[i];
        c.video_out.index = i;
        c.telemetry_out.signal = &c.signal;

        wfb_rx_config video_cfg;
        video_cfg.channel_id = ((uint32_t)CONFIG_LINK_ID << 8) + i;
        video_cfg.ring_size = CONFIG_LINK_RING_SIZE;
        wfb_rx_config telemetry_cfg;
        telemetry_cfg.channel_id = ((uint32_t)CONFIG_LINK_ID << 8) + 0x10 + i;
        telemetry_cfg.ring_size = 1;
        telemetry_cfg.max_fec_n = 2;  // FEC 1/2 (docs/DESIGN.md)
        wfb_status rc = c.video.init(key, key_size, video_cfg);
        if (rc == WFB_OK)
        {
            rc = c.telemetry.init(key, key_size, telemetry_cfg);
        }
        if (rc != WFB_OK)
        {
            ESP_LOGE(TAG, "camera %d: wfb_rx init: %s", i, wfb_status_name(rc));
            return false;
        }
    }
    return true;
}

// The camera a frame belongs to, and whether it's its telemetry. -1 if it's none of ours.
static int camera_of(uint32_t channel_id, bool *telemetry)
{
    if ((channel_id >> 8) != (uint32_t)CONFIG_LINK_ID)
    {
        return -1;
    }
    int port = channel_id & 0xff;
    *telemetry = port >= 0x10;
    int i = *telemetry ? port - 0x10 : port;
    return i >= 0 && i < CONFIG_BASE_CAMERAS ? i : -1;
}

static void report(radio_rx *radio)
{
    static uint32_t last_heard = 0, last_overflow = 0, last_rtsp_sent = 0, last_rtsp_dropped = 0;
    for (int i = 0; i < CONFIG_BASE_CAMERAS; i++)
    {
        camera &c = cameras[i];
        const wfb_rx_stats &s = c.video.stats;
        uint32_t lost = s.count_p_lost - c.last.count_p_lost;
        uint32_t out = s.count_p_outgoing - c.last.count_p_outgoing;
        c.signal.valid = c.frames_heard > 0;
        c.signal.rssi_dbm = c.frames_heard ? c.rssi_sum / (int)c.frames_heard : 0;
        c.signal.loss_pct = lost + out ? 100.0f * lost / (lost + out) : 0;
        c.signal.video_kbps = c.video_out.bytes * 8 / 1000;
        if (c.frames_heard > 0)
        {
            ESP_LOGI(TAG, "cam%d: %lu frames (rssi %d) | video %lu fps, %lu kbit/s, fec_rec %lu, lost %lu, dec_err %lu | "
                          "telemetry %lu | rtsp clients %d",
                     i, (unsigned long)c.frames_heard, c.signal.rssi_dbm, (unsigned long)c.video_out.frames,
                     (unsigned long)c.signal.video_kbps,
                     (unsigned long)(s.count_p_fec_recovered - c.last.count_p_fec_recovered), (unsigned long)lost,
                     (unsigned long)(s.count_p_dec_err - c.last.count_p_dec_err),
                     (unsigned long)c.telemetry_out.messages, rtsp_up ? rtsp.playing(i) : 0);
        }
        c.last = s;
        c.frames_heard = 0;
        c.rssi_sum = 0;
        c.video_out.packets = c.video_out.frames = c.video_out.bytes = 0;
        c.telemetry_out.messages = 0;
    }
    uint32_t heard = radio->heard, overflow = radio->overflow;
    ESP_LOGI(TAG, "radio: %lu data frames heard, %lu dropped (slots full) | rtsp: %lu sent, %lu dropped | "
                  "ethernet link drops %lu | RAM free %u",
             (unsigned long)(heard - last_heard), (unsigned long)(overflow - last_overflow),
             (unsigned long)(rtsp.sent - last_rtsp_sent), (unsigned long)(rtsp.dropped - last_rtsp_dropped),
             (unsigned long)eth_link_drops(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    last_heard = heard;
    last_overflow = overflow;
    last_rtsp_sent = rtsp.sent;
    last_rtsp_dropped = rtsp.dropped;
}

static void receive_task(void *arg)
{
    radio_rx *radio = static_cast<radio_rx *>(arg);
    uint64_t next_report_us = esp_timer_get_time() + 1000000;
    for (;;)
    {
        // One tick at least: pdMS_TO_TICKS(5) is 0 at FreeRTOS's 100 Hz, and a task that never blocks
        // starves this core's idle task (the task watchdog fires). A tick is 10 ms of RTSP reply delay at most.
        radio_frame *f = radio->receive(1);
        if (f != nullptr)
        {
            bool telemetry = false;
            int i = camera_of(f->channel_id, &telemetry);
            if (i >= 0)
            {
                camera &c = cameras[i];
                c.frames_heard += 1;
                c.rssi_sum += f->rssi;
                (telemetry ? c.telemetry : c.video).process_packet(f->data, f->size);
            }
            radio->release(f);
        }
        if (rtsp_up)
        {
            rtsp.poll(0);
        }
        if (esp_timer_get_time() >= next_report_us)
        {
            next_report_us += 1000000;
            report(radio);
        }
    }
}

extern "C" void app_main(void)
{
    radio_config radio_cfg;
    radio_cfg.channel = CONFIG_LINK_CHANNEL;
    ESP_ERROR_CHECK(radio_start(radio_cfg));
    static radio_rx radio;
    ESP_ERROR_CHECK(radio.start(CONFIG_LINK_RX_SLOTS));
    if (!init_cameras())
    {
        return;
    }

    // Ethernet: without it the base station still receives and reports, it just can't serve anything.
    // (RAM counts below are byte-addressable internal RAM: the ESP32's instruction RAM can't hold buffers.)
    ESP_LOGI(TAG, "RAM free before Ethernet: %u (largest block %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#if CONFIG_BASE_ETHERNET
    if (eth_start(eth_config(), 15000) == ESP_OK)
    {
        rtsp_config rtsp_cfg;
        rtsp_cfg.port = CONFIG_BASE_RTSP_PORT;
        rtsp_cfg.max_clients = CONFIG_BASE_RTSP_CLIENTS;
        rtsp_cfg.client_buffer = 8 * 1024;  // with lwIP's own send buffer, over half a 640x480 frame in flight
        rtsp_up = rtsp.start(rtsp_cfg, rtsp_names, CONFIG_BASE_CAMERAS);
        ESP_LOGI(TAG, "rtsp %s on port %d: rtsp://<this address>:%d/cam0..cam%d", rtsp_up ? "serving" : "FAILED",
                 CONFIG_BASE_RTSP_PORT, CONFIG_BASE_RTSP_PORT, CONFIG_BASE_CAMERAS - 1);

        if (strlen(CONFIG_BASE_OUT_HOST) > 0)
        {
            static udp_out out;
            if (out.open(CONFIG_BASE_OUT_HOST, CONFIG_BASE_OUT_PORT) == ESP_OK)
            {
                video_out = &out;
            }
            for (int i = 0; i < CONFIG_BASE_CAMERAS; i++)
            {
                cameras[i].telemetry_out.out_open =
                    cameras[i].telemetry_out.out.open(CONFIG_BASE_OUT_HOST, CONFIG_BASE_TELEMETRY_PORT + i) == ESP_OK;
            }
        }
    }
#else
    ESP_LOGW(TAG, "Ethernet off (menuconfig): receiving and reporting only");
#endif
    ESP_LOGI(TAG, "%d cameras on link 0x%06x; RAM free %u (largest block %u)", CONFIG_BASE_CAMERAS, CONFIG_LINK_ID,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    xTaskCreatePinnedToCore(receive_task, "receive", 6144, &radio, 5, nullptr, 1);  // core 1 (Coding conventions)
}
