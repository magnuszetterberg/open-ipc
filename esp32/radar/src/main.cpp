// Radar camera firmware for the FRDM-RW612 (app layer, R4). M8: a numbered counter over wfb-ng, as the
// ESP32 sent in M1, to show the RW612's frames reach an unmodified wfb_rx. The radar picture comes in M9.
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/version.h>
#include <zephyr/sys/printk.h>

#include "keys.hpp"
#include "radar.hpp"
#include "radio.hpp"
#include "radio_nxp.h"
#include "wfb_tx.hpp"

static uint64_t now_us()
{
    return k_ticks_to_us_floor64(k_uptime_ticks());
}

int main(void)
{
    printk("radar camera, FRDM-RW612, Zephyr %s\n", KERNEL_VERSION_STRING);

#if CONFIG_LINK_PAYLOAD_RADAR
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
    if (radio_start(radio_cfg) != 0)
    {
        return 0;
    }

    const uint32_t channel_id = ((uint32_t)CONFIG_LINK_ID << 8) + CONFIG_LINK_RADIO_PORT;
    static radio_tx radio(channel_id);
    static wfb_tx tx(radio);
    wfb_tx_config tx_cfg;
    tx_cfg.fec_k = CONFIG_LINK_FEC_K;
    tx_cfg.fec_n = CONFIG_LINK_FEC_N;
    tx_cfg.channel_id = channel_id;
    tx_cfg.max_packet = radio_max_packet();
    size_t key_size = 0;
    const uint8_t *key = keys_link_key(&key_size);
    wfb_status rc = tx.init(key, key_size, tx_cfg);
    if (rc != WFB_OK)
    {
        printk("wfb_tx init: %s\n", wfb_status_name(rc));
        return 0;
    }
    printk("wfb-ng link 0x%06x port %d, FEC %d/%d, max payload %u\n", CONFIG_LINK_ID, CONFIG_LINK_RADIO_PORT,
           CONFIG_LINK_FEC_K, CONFIG_LINK_FEC_N, (unsigned)tx.max_payload());

#if CONFIG_LINK_PAYLOAD_RADAR
    // Each measurement goes out as one payload (radar_pack)
    uint64_t next_radar_stats = now_us() + 1000000;
    radar_stats last_counts;
    uint16_t seq = 0;
    uint32_t sent = 0, unpacked = 0, send_errors = 0;
    static uint8_t payload[WFB_MAX_PACKET];
    for (;;)
    {
        radar_cirs cirs;
        if (radar_next(&cirs, 1000))
        {
            size_t n = radar_pack(cirs, seq++, payload, MIN(sizeof(payload), tx.max_payload()));
            if (n == 0)
            {
                unpacked += 1;
            }
            else if (tx.send(payload, n, now_us()) == WFB_OK)
            {
                sent += 1;
            }
            else
            {
                send_errors += 1;
            }
        }
        if (now_us() >= next_radar_stats)
        {
            next_radar_stats += 1000000;
            printk("radar: %lu measurements/s, %lu sent (%lu frames on air), %lu unreadable, %lu send errors, "
                   "%lu other UCI, %lu SPI errors\n",
                   (unsigned long)(radar_counts.notifications - last_counts.notifications), (unsigned long)sent,
                   (unsigned long)radio.frames, (unsigned long)unpacked, (unsigned long)send_errors,
                   (unsigned long)(radar_counts.other - last_counts.other),
                   (unsigned long)(radar_counts.errors - last_counts.errors));
            last_counts = radar_counts;
            sent = unpacked = send_errors = 0;
            radio.frames = 0;
        }
    }
#else
    // Payload: "rw612 counter <n>" then padding to CONFIG_LINK_COUNTER_SIZE (at most the max payload), so
    // esp32/tools/counter_check.py on the receiver can spot gaps.
    static uint8_t payload[CONFIG_LINK_COUNTER_SIZE];
    size_t payload_size = MIN(sizeof(payload), tx.max_payload());
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
        rc = tx.send(payload, payload_size, now);
        if (rc == WFB_OK)
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
                   (unsigned long)counter, (unsigned long)sent, (unsigned long)(radio.frames - frames),
                   (unsigned long)(radio.dropped - dropped), radio.last_error, (unsigned long)errors);
            frames = radio.frames;
            dropped = radio.dropped;
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
#endif
}
