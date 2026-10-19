// Base-station firmware (app layer, R4): receives up to three cameras' wfb-ng streams in promiscuous mode,
// decodes them with wfb_rx, and serves them on the LAN (docs/DESIGN.md, Base station out):
// - each camera's video as RTSP, rtsp://<base>:8554/camN (RTP/JPEG passed through),
// - each camera's telemetry as UDP, with the signal quality measured here added,
// - camera 0's video also as UDP to one host, as in M4 (#18),
// - a web view, http://<base>/: a tile per camera with its video as MJPEG and its telemetry (M7).
// One task does it all, so nothing is locked: it takes frames off the radio and polls the RTSP and web servers.
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "eth.hpp"
#include "keys.hpp"
#include "radio.hpp"
#include "rtp_jpeg.hpp"
#include "rtsp_server.hpp"
#include "web_server.hpp"
#include "wfb_rx.hpp"

static const char *TAG = "base";

static rtsp_server rtsp;
static bool rtsp_up = false;
static udp_out *video_out = nullptr;  // camera 0's video to BASE_OUT_HOST, when set
static web_server web;
static bool web_up = false;

extern const char web_page_start[] asm("_binary_index_html_start");
extern const char web_page_end[] asm("_binary_index_html_end");

// Signal quality of one camera over the last second, measured here (the camera can't know it).
struct signal_quality
{
    int rssi_dbm = 0;
    float loss_pct = 0;
    uint32_t video_kbps = 0;
    bool valid = false;
};

// Rebuilt JPEGs, a piece at a time, to the camera's web viewers.
struct web_sink : jpeg_stream_sink
{
    int index = 0;
    void jpeg_begin() override
    {
        web.jpeg_begin(index);
    }
    void jpeg_data(const uint8_t *data, size_t size) override
    {
        web.jpeg_data(index, data, size);
    }
    void jpeg_end(bool complete) override
    {
        web.jpeg_end(index, complete);
    }
};

// Recovered video: RTP/JPEG to the camera's RTSP clients and web viewers (and camera 0's to video_out). A
// payload that isn't RTP is the camera's data (the radar's, M9): to its web viewers as it is, for the page
// to draw.
struct video_sink : wfb_payload_sink
{
    int index = 0;
    rtp_jpeg_streamer *streamer = nullptr;
    uint32_t packets = 0, frames = 0, bytes = 0;
    void send_payload(const uint8_t *buf, size_t size) override
    {
        packets += 1;
        bytes += size;
        if (index == 0 && video_out != nullptr)
        {
            video_out->send_payload(buf, size);
        }
        if (size < 12 || (buf[0] & 0xc0) != 0x80)  // not RTP version 2
        {
            frames += 1;  // each payload is one
            if (web_up && web.viewers(index) > 0)
            {
                web.data(index, buf, size);
            }
            return;
        }
        frames += (buf[1] & 0x80) ? 1 : 0;  // the RTP marker ends a frame
        if (rtsp_up)
        {
            rtsp.send_rtp(index, buf, size);
        }
        if (web_up && web.viewers(index) > 0)
        {
            streamer->push(buf, size);  // a viewer joining mid-frame starts with the next one
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
    uint64_t last_us = 0;  // when the last message arrived
    char last[320] = "";
    void send_payload(const uint8_t *buf, size_t size) override
    {
        messages += 1;
        last_us = esp_timer_get_time();
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
    web_sink jpeg_out;
    rtp_jpeg_streamer streamer{jpeg_out};  // its 600-byte header buffer is the only per-camera web state
    telemetry_sink telemetry_out;
    wfb_rx video{video_out};
    wfb_rx telemetry{telemetry_out};
    signal_quality signal;
    uint32_t frames_heard = 0;
    uint32_t last_cut = 0;  // streamer.frames_cut at the last report
    int rssi_sum = 0;
    uint8_t rate = 0;       // of the last frame heard (radio_frame::rate)
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
        c.video_out.streamer = &c.streamer;
        c.jpeg_out.index = i;
        c.telemetry_out.signal = &c.signal;

        wfb_rx_config video_cfg;
        video_cfg.channel_id = ((uint32_t)CONFIG_LINK_ID << 8) + i;
        video_cfg.ring_size = CONFIG_LINK_RING_SIZE;
        wfb_rx_config telemetry_cfg;
        telemetry_cfg.channel_id = ((uint32_t)CONFIG_LINK_ID << 8) + 0x10 + i;
        telemetry_cfg.ring_size = 1;
        telemetry_cfg.max_fec_n = 2;  // FEC 1/2 (docs/DESIGN.md)
        // One small JSON message (the camera's is about 200 bytes; telemetry_sink takes up to 224): 512-byte
        // packets need 4 KB less RAM per camera than full-size ones, which the web view needs (M7).
        telemetry_cfg.max_packet = 512;
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

#if CONFIG_BASE_ETHERNET
// Asked by the RTSP and web servers before they hand data to TCP (see BASE_RAM_RESERVE).
static bool may_send(size_t size)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= CONFIG_BASE_RAM_RESERVE + size;
}
#endif

// The console without waiting: a log line goes into a ring buffer, and a task of the lowest priority writes
// it to the UART whenever nothing else needs core 1. ESP_LOG itself waits for the UART (115200 baud), and the
// stats lines held the receive task for 37 ms each second: the receive slots filled and frames were dropped.
// Other tasks wait for room (as they waited for the UART before), so lines at boot aren't lost.
static RingbufHandle_t log_ring = nullptr;
static TaskHandle_t receive_handle = nullptr;

static int log_to_ring(const char *format, va_list args)
{
    char line[320];
    int n = vsnprintf(line, sizeof(line), format, args);
    if (n > 0)
    {
        TickType_t wait = xTaskGetCurrentTaskHandle() == receive_handle ? 0 : pdMS_TO_TICKS(500);
        xRingbufferSend(log_ring, line, n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1, wait);
    }
    return n;
}

static void log_task(void *arg)
{
    for (;;)
    {
        size_t size = 0;
        char *line = (char *)xRingbufferReceive(log_ring, &size, portMAX_DELAY);
        if (line != nullptr)
        {
            fwrite(line, 1, size, stdout);
            fflush(stdout);
            vRingbufferReturnItem(log_ring, line);
        }
    }
}

// Why the base station last started, for the log and /status.json: a crash shows without a serial console
static const char *reset_name(esp_reset_reason_t r)
{
    switch (r)
    {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_EXT: return "reset pin";
    default: return "other";
    }
}

// /status.json for the web view: per camera, whether its video is heard and its latest telemetry (with the
// signal quality added, as sent over UDP), or null when none came for 3 s.
static void update_status(const uint32_t *video_frames)
{
    static char json[web_server::status_max];
    uint64_t now = esp_timer_get_time();
    int n = snprintf(json, sizeof(json), "{\"uptime_s\":%llu,\"reset\":\"%s\",\"cams\":[",
                     (unsigned long long)(now / 1000000), reset_name(esp_reset_reason()));
    for (int i = 0; i < CONFIG_BASE_CAMERAS && n > 0 && n < (int)sizeof(json); i++)
    {
        const telemetry_sink &t = cameras[i].telemetry_out;
        bool fresh = t.last_us != 0 && now - t.last_us < 3000000 && t.last[0] != '\0';
        n += snprintf(json + n, sizeof(json) - n, "%s{\"video\":%s,\"viewers\":%d,\"telemetry\":%s}", i ? "," : "",
                      video_frames[i] ? "true" : "false", web.viewers(i), fresh ? t.last : "null");
    }
    if (n > 0 && n < (int)sizeof(json) - 2)
    {
        n += snprintf(json + n, sizeof(json) - n, "]}");
        web.set_status(json, (size_t)n);
    }
}

static void report(radio_rx *radio)
{
    static uint32_t last_heard = 0, last_overflow = 0, last_rtsp_sent = 0, last_rtsp_dropped = 0;
    static uint32_t last_web_sent = 0, last_web_cut = 0, last_web_skipped = 0, last_data_sent = 0, last_data_skipped = 0;
    uint32_t video_frames[CONFIG_BASE_CAMERAS];
    for (int i = 0; i < CONFIG_BASE_CAMERAS; i++)
    {
        video_frames[i] = cameras[i].video_out.frames;
    }
    if (web_up)
    {
        update_status(video_frames);
    }
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
            char rate[16];
            ESP_LOGI(TAG, "cam%d: %lu frames (rssi %d, %s) | video %lu fps, %lu kbit/s, fec_rec %lu, lost %lu, dec_err %lu | "
                          "telemetry %lu | rtsp clients %d, web viewers %d (%lu JPEGs cut on air)",
                     i, (unsigned long)c.frames_heard, c.signal.rssi_dbm, radio_rate_name(c.rate, rate, sizeof(rate)),
                     (unsigned long)c.video_out.frames,
                     (unsigned long)c.signal.video_kbps,
                     (unsigned long)(s.count_p_fec_recovered - c.last.count_p_fec_recovered), (unsigned long)lost,
                     (unsigned long)(s.count_p_dec_err - c.last.count_p_dec_err),
                     (unsigned long)c.telemetry_out.messages, rtsp_up ? rtsp.playing(i) : 0, web_up ? web.viewers(i) : 0,
                     (unsigned long)(c.streamer.frames_cut - c.last_cut));
        }
        c.last = s;
        c.last_cut = c.streamer.frames_cut;
        c.frames_heard = 0;
        c.rssi_sum = 0;
        c.video_out.packets = c.video_out.frames = c.video_out.bytes = 0;
        c.telemetry_out.messages = 0;
    }
    uint32_t heard = radio->heard, overflow = radio->overflow;
    ESP_LOGI(TAG, "radio: %lu data frames heard, %lu dropped (slots full) | rtsp: %lu sent, %lu dropped | "
                  "web: %lu JPEGs sent, %lu cut, %lu skipped, %lu data sent, %lu skipped, %lu refused | ethernet link drops %lu | RAM free %u (lowest %u)",
             (unsigned long)(heard - last_heard), (unsigned long)(overflow - last_overflow),
             (unsigned long)(rtsp.sent - last_rtsp_sent), (unsigned long)(rtsp.dropped - last_rtsp_dropped),
             (unsigned long)(web.jpegs_sent - last_web_sent), (unsigned long)(web.jpegs_cut - last_web_cut),
             (unsigned long)(web.jpegs_skipped - last_web_skipped), (unsigned long)(web.data_sent - last_data_sent),
             (unsigned long)(web.data_skipped - last_data_skipped), (unsigned long)web.refused,
             (unsigned long)eth_link_drops(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    // "lowest" covers the second since the last report: viewers' TCP data lives on the heap for moments only
    heap_caps_monitor_local_minimum_free_size_stop();
    heap_caps_monitor_local_minimum_free_size_start();
    last_web_sent = web.jpegs_sent;
    last_web_cut = web.jpegs_cut;
    last_web_skipped = web.jpegs_skipped;
    last_data_sent = web.data_sent;
    last_data_skipped = web.data_skipped;
    last_heard = heard;
    last_overflow = overflow;
    last_rtsp_sent = rtsp.sent;
    last_rtsp_dropped = rtsp.dropped;
}

static void receive_task(void *arg)
{
    radio_rx *radio = static_cast<radio_rx *>(arg);
    uint64_t next_report_us = esp_timer_get_time() + 1000000, next_poll_us = 0;
    heap_caps_monitor_local_minimum_free_size_start();
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
                c.rate = f->rate;
                (telemetry ? c.telemetry : c.video).process_packet(f->data, f->size);
            }
            radio->release(f);
        }
        // Video goes out as it arrives (send_rtp, jpeg_data); polling only accepts clients, reads their requests
        // and sends what waited. select() costs 0.5 ms, sometimes 4 ms: every 5 ms that was 100 ms of each
        // second, and a camera's burst of packets arriving meanwhile overflowed the receive slots. 20 ms.
        if (esp_timer_get_time() >= next_poll_us)
        {
            next_poll_us = esp_timer_get_time() + 20000;
            if (rtsp_up)
            {
                rtsp.poll(0);
            }
            if (web_up)
            {
                web.poll(0);
            }
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
    log_ring = xRingbufferCreate(2048, RINGBUF_TYPE_NOSPLIT);  // a second's stats lines
    if (log_ring != nullptr && xTaskCreatePinnedToCore(log_task, "log", 3072, nullptr, 1, nullptr, 1) == pdPASS)
    {
        esp_log_set_vprintf(log_to_ring);
    }
    ESP_LOGI(TAG, "started after: %s", reset_name(esp_reset_reason()));
    radio_config radio_cfg;
    radio_cfg.channel = CONFIG_LINK_CHANNEL;
    ESP_ERROR_CHECK(radio_start(radio_cfg));
    static radio_rx radio;
    ESP_ERROR_CHECK(radio.start(CONFIG_LINK_RX_SLOTS, CONFIG_LINK_RX_SMALL_SLOTS));
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
    // Serve even without an address yet: DHCP keeps trying, so a cable plugged in late still works.
    esp_err_t eth_err = eth_start(eth_config(), 15000);
    if (eth_err == ESP_OK || eth_err == ESP_ERR_TIMEOUT)
    {
        rtsp_config rtsp_cfg;
        rtsp_cfg.port = CONFIG_BASE_RTSP_PORT;
        rtsp_cfg.max_clients = CONFIG_BASE_RTSP_CLIENTS;
        rtsp_cfg.client_buffer = CONFIG_BASE_RTSP_CLIENT_BUFFER;
        rtsp_cfg.may_send = may_send;
        rtsp_up = rtsp.start(rtsp_cfg, rtsp_names, CONFIG_BASE_CAMERAS);
        ESP_LOGI(TAG, "rtsp %s on port %d: rtsp://<this address>:%d/cam0..cam%d", rtsp_up ? "serving" : "FAILED",
                 CONFIG_BASE_RTSP_PORT, CONFIG_BASE_RTSP_PORT, CONFIG_BASE_CAMERAS - 1);

#if CONFIG_BASE_WEB
        web_config web_cfg;
        web_cfg.port = CONFIG_BASE_WEB_PORT;
        web_cfg.max_clients = CONFIG_BASE_WEB_CLIENTS;
        web_cfg.client_buffer = CONFIG_BASE_WEB_CLIENT_BUFFER;
        web_cfg.may_send = may_send;
        web_up = web.start(web_cfg, rtsp_names, CONFIG_BASE_CAMERAS);
        web.set_page(web_page_start, (size_t)(web_page_end - web_page_start - 1));  // EMBED_TXTFILES adds a NUL
        ESP_LOGI(TAG, "web view %s on port %d: http://<this address>:%d/", web_up ? "serving" : "FAILED",
                 CONFIG_BASE_WEB_PORT, CONFIG_BASE_WEB_PORT);
#endif

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
    xTaskCreatePinnedToCore(receive_task, "receive", 6144, &radio, 5, &receive_handle, 1);  // core 1 (Coding conventions)
}
