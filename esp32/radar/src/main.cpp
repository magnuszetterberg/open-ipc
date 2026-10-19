// Radar camera firmware for the FRDM-RW612 (app layer, R4). Kconfig "What to send":
//   - the radar's data (M9): per measurement, what the SR250 finds (a "D1" payload, radar_picture.hpp) on
//     camera N's video port, and the finding as camera N's telemetry; the web page draws it, and on a PC
//     radar_render draws it as video;
//   - the radar picture: the same drawn here and sent as camera N's RTP/JPEG video, for receivers that
//     only play video;
//   - the radar's raw measurements, one "R1" payload each (pc/radar_view.py);
//   - a numbered counter, to test the link itself (M8).
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/version.h>

#include "keys.hpp"
#include "radar.hpp"
#include "radar_picture.hpp"
#include "radio.hpp"
#include "radio_nxp.h"
#include "rtp_jpeg.hpp"
#include "wfb_tx.hpp"

static uint64_t now_us()
{
    return k_ticks_to_us_floor64(k_uptime_ticks());
}

// One wfb-ng stream (R5): camera N's video, or its telemetry
struct stream
{
    radio_tx radio;
    wfb_tx tx;
    explicit stream(uint32_t channel_id) : radio(channel_id), tx(radio) {}

    bool start(uint32_t channel_id, uint8_t fec_k, uint8_t fec_n)
    {
        wfb_tx_config cfg;
        cfg.fec_k = fec_k;
        cfg.fec_n = fec_n;
        cfg.channel_id = channel_id;
        cfg.max_packet = radio_max_packet();
        size_t key_size = 0;
        const uint8_t *key = keys_link_key(&key_size);
        wfb_status rc = tx.init(key, key_size, cfg);
        if (rc != WFB_OK)
        {
            printk("wfb_tx init: %s\n", wfb_status_name(rc));
            return false;
        }
        return true;
    }
};

static const uint32_t VIDEO_ID = ((uint32_t)CONFIG_LINK_ID << 8) + CONFIG_LINK_RADIO_PORT;
static stream video(VIDEO_ID);

#if CONFIG_LINK_PAYLOAD_RADAR_DATA || CONFIG_LINK_PAYLOAD_RADAR_PICTURE || CONFIG_LINK_PAYLOAD_RADAR_RAW

static const uint32_t TELEMETRY_ID = ((uint32_t)CONFIG_LINK_ID << 8) + 0x10 + CONFIG_LINK_RADIO_PORT;
static stream telemetry(TELEMETRY_ID);
static radar_detector detector;

#if CONFIG_LINK_PAYLOAD_RADAR_PICTURE
// A picture's RTP packets, each to become one wfb-ng payload, queued and sent CONFIG_RADAR_PACKET_GAP_MS apart
// rather than in one burst: the base station streams each picture to its web viewers as it arrives, with
// room for about 8 KB per viewer, and a picture arriving within a few milliseconds outran that and was cut.
struct paced_sink : rtp_jpeg_sink
{
    static constexpr int MAX = 32;
    uint8_t packets[MAX][WFB_MAX_PACKET];
    uint16_t sizes[MAX];
    int count = 0, next = 0;
    uint64_t next_at = 0;
    uint32_t sent = 0, errors = 0, overflow = 0;

    bool pending() const { return next < count; }

    void send_rtp(const uint8_t *packet, size_t size) override
    {
        if (count == MAX || size > WFB_MAX_PACKET)
        {
            overflow += 1;
            return;
        }
        memcpy(packets[count], packet, size);
        sizes[count++] = (uint16_t)size;
    }

    // Sends the next packet when its time has come; after the last, closes the FEC block so the picture
    // needs no later one to be recoverable
    void poll(uint64_t now)
    {
        if (!pending() || now < next_at)
        {
            return;
        }
        if (video.tx.send(packets[next], sizes[next], now) == WFB_OK)
        {
            sent += 1;
        }
        else
        {
            errors += 1;
        }
        next_at = now + CONFIG_RADAR_PACKET_GAP_MS * 1000;
        if (++next == count)
        {
            video.tx.close_block();
            count = next = 0;
        }
    }
};
#endif

// The finding as one JSON object (docs/DESIGN.md: Telemetry); the base station adds its signal quality
// fps and frame_kb: the payloads sent per second (pictures, or measurements) and their size
static void send_telemetry(uint32_t seq, uint32_t pictures_s, uint32_t picture_bytes)
{
    char msg[256];
    const radar_finding &f = detector.finding;
    int n = snprintf(msg, sizeof(msg),
                     "{\"cam\":%d,\"seq\":%lu,\"uptime_s\":%llu,\"fps\":%lu,\"frame_kb\":%.1f,\"temp_c\":null,"
                     "\"radar\":true,\"learning\":%s,\"moving\":%s,\"distance_m\":%.2f,\"strength_db\":%.1f}",
                     CONFIG_LINK_RADIO_PORT, (unsigned long)seq, (unsigned long long)(now_us() / 1000000),
                     (unsigned long)pictures_s, picture_bytes / 1024.0, f.learning ? "true" : "false",
                     f.moving ? "true" : "false", f.moving ? (double)f.distance_m : 0.0,
                     f.moving ? (double)f.strength_db : 0.0);
    telemetry.tx.send((const uint8_t *)msg, (size_t)n, now_us());
}

static void radar_loop()
{
    static uint8_t r1[WFB_MAX_PACKET];
#if CONFIG_LINK_PAYLOAD_RADAR_PICTURE
    static radar_picture picture;     // static: 150 KB of YCbCr
    static uint8_t jpeg[32 * 1024];
    static paced_sink sink;   // static: 45 KB of queued packets
    rtp_jpeg_config rtp_cfg;
    rtp_cfg.max_packet = video.tx.max_payload();
    static rtp_jpeg_packetizer packetizer(sink, rtp_cfg);
#endif
    uint64_t next_stats = now_us() + 1000000;
    radar_stats last_counts;
    uint16_t seq = 0;
    uint32_t telemetry_seq = 0, pictures = 0, picture_bytes = 0, pictures_s = 0, picture_kb_s = 0;
    uint32_t sent = 0, unreadable = 0, send_errors = 0, too_big = 0, encode_us = 0;
    bool was_moving = false;
#if CONFIG_LINK_PAYLOAD_RADAR_PICTURE
    int since_picture = 0;
#endif

    for (;;)
    {
        radar_cirs cirs;
#if CONFIG_LINK_PAYLOAD_RADAR_PICTURE
        sink.poll(now_us());
        bool got = radar_next(&cirs, sink.pending() ? 2 : 1000);   // short waits while packets are queued
#else
        bool got = radar_next(&cirs, 1000);
#endif
        if (got)
        {
            size_t n = radar_pack(cirs, seq++, r1, sizeof(r1));
            if (n == 0 || !detector.add_r1(r1, n))
            {
                unreadable += 1;
                continue;
            }
#if CONFIG_LINK_PAYLOAD_RADAR_RAW
            if (n <= video.tx.max_payload() && video.tx.send(r1, n, now_us()) == WFB_OK)
            {
                sent += 1;
            }
            else
            {
                send_errors += 1;
            }
#elif CONFIG_LINK_PAYLOAD_RADAR_DATA
            static uint8_t data[RADAR_DATA_MAX];
            size_t size = radar_data_pack(radar_data_of(detector), (uint16_t)(seq - 1), data, sizeof(data));
            if (size > 0 && size <= video.tx.max_payload() && video.tx.send(data, size, now_us()) == WFB_OK)
            {
                sent += 1;
                pictures += 1;   // for telemetry's fps and frame_kb: measurements sent, and their size
                picture_bytes += size;
            }
            else
            {
                send_errors += 1;
            }
#else
            radar_data now = radar_data_of(detector);
            picture.add(now);
            // A picture every CONFIG_RADAR_PICTURE_EVERY measurements; one that comes due while the last one's
            // packets are still going out waits for them, rather than being skipped
            if (++since_picture >= CONFIG_RADAR_PICTURE_EVERY && !sink.pending())
            {
                since_picture = 0;
                uint64_t t0 = now_us();
                picture.draw(now);
                size_t size = jpeg_encode_422(picture.y, picture.cb, picture.cr, radar_picture::WIDTH,
                                              radar_picture::HEIGHT, CONFIG_RADAR_JPEG_QUALITY, jpeg, sizeof(jpeg));
                encode_us = (uint32_t)(now_us() - t0);
                if (size == 0)
                {
                    too_big += 1;
                }
                else if (packetizer.send_frame(jpeg, size, (uint32_t)(now_us() * 9 / 100)) == RTP_JPEG_OK &&
                         sink.overflow == 0)
                {
                    sent += 1;
                    pictures += 1;
                    picture_bytes += size;
                }
                else
                {
                    send_errors += 1;
                    sink.overflow = 0;
                }
            }
#endif
            // The finding as telemetry once a second, and at once when something starts or stops moving
            if (detector.finding.moving != was_moving)
            {
                was_moving = detector.finding.moving;
                send_telemetry(telemetry_seq++, pictures_s, picture_kb_s);
            }
        }

        if (now_us() >= next_stats)
        {
            next_stats += 1000000;
            pictures_s = pictures;
            picture_kb_s = pictures ? picture_bytes / pictures : 0;
            send_telemetry(telemetry_seq++, pictures_s, picture_kb_s);
            printk("radar: %lu measurements/s, %lu sent (%s), %lu frames on air, %lu unreadable, %lu send errors, "
                   "%lu too big; %s",
                   (unsigned long)(radar_counts.notifications - last_counts.notifications), (unsigned long)sent,
                   IS_ENABLED(CONFIG_LINK_PAYLOAD_RADAR_PICTURE) ? "pictures" : "measurements", (unsigned long)video.radio.frames,
                   (unsigned long)unreadable, (unsigned long)send_errors, (unsigned long)too_big,
                   detector.finding.learning ? "learning" : detector.finding.moving ? "moving" : "nothing moving");
            if (detector.finding.moving)
            {
                printk(" at %.2f m", (double)detector.finding.distance_m);
            }
            if (pictures && IS_ENABLED(CONFIG_LINK_PAYLOAD_RADAR_PICTURE))
            {
                printk("; %lu bytes/picture, drawn and encoded in %lu ms", (unsigned long)(picture_bytes / pictures),
                       (unsigned long)(encode_us / 1000));
            }
            else if (pictures)
            {
                printk("; %lu bytes each", (unsigned long)(picture_bytes / pictures));
            }
            printk("\n");
            last_counts = radar_counts;
            sent = unreadable = send_errors = too_big = pictures = picture_bytes = 0;
            video.radio.frames = 0;
        }
    }
}

#else

// Payload: "rw612 counter <n>" then padding to CONFIG_LINK_COUNTER_SIZE (at most the max payload), so
// esp32/tools/counter_check.py on the receiver can spot gaps.
static void counter_loop()
{
    static uint8_t payload[CONFIG_LINK_COUNTER_SIZE];
    size_t payload_size = MIN(sizeof(payload), video.tx.max_payload());
    memset(payload, '.', sizeof(payload));
    const int64_t period_us = 1000000 / CONFIG_LINK_COUNTER_RATE;
    int64_t wake_us = now_us();
    uint64_t next_stats_us = wake_us + 1000000;
    uint32_t counter = 0, sent = 0, errors = 0, frames = 0, dropped = 0;

    for (;;)
    {
        int n = snprintf((char *)payload, payload_size, "rw612 counter %lu ", (unsigned long)counter);
        payload[n] = '.';
        uint64_t now = now_us();
        if (video.tx.send(payload, payload_size, now) == WFB_OK)
        {
            sent += 1;
        }
        else
        {
            errors += 1;
        }
        counter += 1;

        if (now >= next_stats_us)
        {
            next_stats_us += 1000000;
#if CONFIG_RADIO_RATE_SWEEP
            static uint32_t seconds = 0;
            if (seconds++ % 4 == 0)
            {
                uint32_t index = (seconds / 4) % 32;
                radio_nxp_tx_control = (1u << 15) | (index << 16);
                printk("rate sweep: tx_control 0x%08x (index %u)\n", radio_nxp_tx_control, (unsigned)index);
            }
#endif
            printk("counter %lu: %lu payloads/s, %lu frames/s, %lu dropped by WiFi (%d), %lu errors\n",
                   (unsigned long)counter, (unsigned long)sent, (unsigned long)(video.radio.frames - frames),
                   (unsigned long)(video.radio.dropped - dropped), video.radio.last_error, (unsigned long)errors);
            frames = video.radio.frames;
            dropped = video.radio.dropped;
            sent = errors = 0;
        }

        wake_us += period_us;
        int64_t wait = wake_us - (int64_t)now_us();
        if (wait > 0)
        {
            k_usleep(wait);
        }
        else
        {
            wake_us = now_us();   // behind: don't try to catch up in a burst
            k_yield();
        }
    }
}

#endif

int main(void)
{
    printk("radar camera, FRDM-RW612, Zephyr %s\n", KERNEL_VERSION_STRING);

#if CONFIG_LINK_PAYLOAD_RADAR_DATA || CONFIG_LINK_PAYLOAD_RADAR_PICTURE || CONFIG_LINK_PAYLOAD_RADAR_RAW
    // The SR250 first, before WiFi starts
    if (radar_start() != 0)
    {
        printk("radar: not started\n");
        return 0;
    }
#endif

    radio_config radio_cfg;
    radio_cfg.channel = CONFIG_LINK_CHANNEL;
    radio_cfg.mcs = CONFIG_LINK_MCS;
    if (radio_start(radio_cfg) != 0 || !video.start(VIDEO_ID, CONFIG_LINK_FEC_K, CONFIG_LINK_FEC_N))
    {
        return 0;
    }
    printk("wfb-ng link 0x%06x port %d, FEC %d/%d, max payload %u\n", CONFIG_LINK_ID, CONFIG_LINK_RADIO_PORT,
           CONFIG_LINK_FEC_K, CONFIG_LINK_FEC_N, (unsigned)video.tx.max_payload());

#if CONFIG_LINK_PAYLOAD_RADAR_DATA || CONFIG_LINK_PAYLOAD_RADAR_PICTURE || CONFIG_LINK_PAYLOAD_RADAR_RAW
    // Telemetry: its own stream (radio port 0x10 + N, FEC 1/2), as the cameras send theirs
    if (telemetry.start(TELEMETRY_ID, 1, 2))
    {
        printk("telemetry on port %d\n", 0x10 + CONFIG_LINK_RADIO_PORT);
    }
    radar_loop();
#else
    counter_loop();
#endif
    return 0;
}
