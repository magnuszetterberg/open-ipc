// The SR250's radar session (app layer, R4). See radar.hpp.
//
// The command sequence and settings are those NXP's plug-and-play demo uses for raw radar CIRs on the
// SR250-ARD shield (demo_radar_raw.py and UwbConfig_SR250ARD.py in nxp-appcodehub/an-sr250-uwb-plug-and-play-
// demo): UCI messages as bytes, sent by our own code.
#include "radar.hpp"

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "uwb.h"

radar_stats radar_counts;

static uint8_t msg[UWB_MAX_MESSAGE];
static uint8_t cirs[16384];   // one CIR notification, reassembled from its segments
static uint32_t session = 0;

static void print_message(const char *what, const uint8_t *m, int n)
{
    printk("%s", what);
    for (int i = 0; i < n && i < 24; i++)
    {
        printk(" %02x", m[i]);
    }
    printk(n > 24 ? " ... (%d bytes)\n" : "\n", n);
}

// Sends a command and waits for its response (message type 2: 0x4X), printing notifications on the way.
// Returns the response's status byte, or a negative error.
static int command(const char *name, const uint8_t *cmd, size_t size)
{
    int rc = uwb_send(cmd, size);
    if (rc != 0)
    {
        printk("radar: %s: send failed (%d)\n", name, rc);
        return rc;
    }
    int resent = 0;
    for (int tries = 0; tries < 10; tries++)
    {
        int n = uwb_receive(msg, sizeof(msg), 1000);
        if (n <= 0)
        {
            printk("radar: %s: no response (%d)\n", name, n);
            return n < 0 ? n : -ETIMEDOUT;
        }
        if ((msg[0] & 0xE0) == 0x40 && (msg[0] & 0x0F) == (cmd[0] & 0x0F) && (msg[1] & 0x3F) == (cmd[1] & 0x3F))
        {
            int status = n > 4 ? msg[4] : 0;
            if (status != 0)
            {
                print_message("radar: response", msg, n);
            }
            printk("radar: %s: status %d\n", name, status);
            if ((cmd[0] & 0x0F) == 1 && cmd[1] == 0x00 && n >= 9)
            {
                // SESSION_INIT's response carries the session handle the later commands use
                memcpy(&session, msg + 5, sizeof(session));
            }
            return status;
        }
        // GENERIC_ERROR with STATUS_COMMAND_RETRY: the SR250 was asleep, and the transfer only woke it. Send
        // the command again at once, as NXP's host code does.
        if (n == 5 && msg[0] == 0x60 && msg[1] == 0x07 && msg[4] == 0x0A && resent < 5)
        {
            resent += 1;
            uwb_send(cmd, size);
            continue;
        }
        print_message("radar: notification", msg, n);
    }
    return -EIO;
}

// A session command: the handle goes in bytes 4..7
static int session_command(const char *name, uint8_t *cmd, size_t size)
{
    memcpy(cmd + 4, &session, sizeof(session));
    return command(name, cmd, size);
}

int radar_start()
{
    int rc = uwb_start();
    if (rc != 0)
    {
        return rc;
    }
    // Device: init (NXP), reset, info, low power mode off
    static const uint8_t device_init[] = {0x2E, 0x00, 0x00, 0x02, 0x00, 0x00};
    static const uint8_t device_reset[] = {0x20, 0x00, 0x00, 0x01, 0x00};
    static const uint8_t device_info[] = {0x20, 0x02, 0x00, 0x00};
    static const uint8_t low_power_off[] = {0x20, 0x04, 0x00, 0x04, 0x01, 0x01, 0x01, 0x00};
    command("device init", device_init, sizeof(device_init));
    command("device reset", device_reset, sizeof(device_reset));
    command("device info", device_info, sizeof(device_info));
    command("low power off", low_power_off, sizeof(low_power_off));

    // The SR250-ARD's antennas: RXC, RXB and TRA2 for receiving, TRA1 the radar's transmitter
    static const uint8_t antennas[] = {
        0x20, 0x04, 0x00, 0x3D, 0x03,
        0xE4, 0x60, 0x19, 0x04,
        0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00,
        0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00,
        0xE4, 0x62, 0x0D, 0x02,
        0x01, 0x01, 0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x03, 0x00, 0x00,
        0xE4, 0x63, 0x0D, 0x02,
        0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00};
    command("antennas", antennas, sizeof(antennas));
    // Channel 9 calibration for the shield: clock accuracy, RX antenna delays, TX power per antenna
    static const uint8_t cal_clock[] = {0x2F, 0x21, 0x00, 0x0A, 0x09, 0x01, 0x07, 0x03, 0x24, 0x00, 0x24, 0x00, 0x04, 0x00};
    static const uint8_t cal_rx_delay[] = {0x2F, 0x21, 0x00, 0x10, 0x09, 0x02, 0x0D, 0x04, 0x01, 0x1D,
                                           0x48, 0x02, 0x1C, 0x48, 0x03, 0x24, 0x48, 0x04, 0x2A, 0x48};
    static const uint8_t cal_tx_power[] = {0x2F, 0x21, 0x00, 0x0E, 0x09, 0x04, 0x0B, 0x02, 0x01,
                                           0x00, 0x00, 0x2F, 0x00, 0x02, 0x00, 0x00, 0x39, 0x00};
    command("calibration: clock", cal_clock, sizeof(cal_clock));
    command("calibration: rx delay", cal_rx_delay, sizeof(cal_rx_delay));
    command("calibration: tx power", cal_tx_power, sizeof(cal_tx_power));

    // The radar session (type 0xF0)
    static const uint8_t session_init[] = {0x21, 0x00, 0x00, 0x05, 0x44, 0x33, 0x22, 0x11, 0xF0};
    if (command("session init", session_init, sizeof(session_init)) != 0)
    {
        return -EIO;
    }
    printk("radar: session handle 0x%08x\n", session);
    static uint8_t app_config[] = {0x21, 0x03, 0x00, 0x0B, 0, 0, 0, 0, 0x02,
                                   0x04, 0x01, 0x09,    // channel 9
                                   0x14, 0x01, 0x1A};   // preamble code index 26
    static uint8_t radar_config[] = {0x2F, 0x00, 0x00, 0x24, 0, 0, 0, 0, 0x06,
                                     0xA0, 0x01, 0x01,                                    // radar mode
                                     0xA5, 0x01, 0x00,                                    // single frame notifications
                                     0xA8, 0x07, 0x03, 0x76, 0x02, 0x76, 0x02, 0x76, 0x02,  // CIR start offset per antenna
                                     0xA9, 0x07, 0x32, 0x00, 0x00, 0x00, 0xE0, 0x2E, 0x01,  // frame repetition
                                     0xAD, 0x01, 0x01,                                    // performance
                                     0xB2, 0x02, 0xCD, 0x0C};                             // drift compensation
    static uint8_t radar_b3[] = {0x2F, 0x00, 0x00, 0x1A, 0, 0, 0, 0, 0x01, 0xB3, 0x13, 0x01, 0x01, 0x1A, 0x01,
                                 0x76, 0x02, 0x76, 0x02, 0x76, 0x02, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x00};
    static uint8_t radar_antennas[] = {0x2F, 0x00, 0x00, 0x10, 0, 0, 0, 0, 0x02,
                                       0x02, 0x02, 0x01, 0x01,                     // TX: TRA1
                                       0x03, 0x05, 0x02, 0x03, 0x01, 0x02, 0x00};  // RX: three antennas
    static uint8_t session_start[] = {0x22, 0x00, 0x00, 0x04, 0, 0, 0, 0};
    session_command("app config", app_config, sizeof(app_config));
    session_command("radar config", radar_config, sizeof(radar_config));
    session_command("radar config B3", radar_b3, sizeof(radar_b3));
    session_command("radar antennas", radar_antennas, sizeof(radar_antennas));
    if (session_command("session start", session_start, sizeof(session_start)) != 0)
    {
        return -EIO;
    }
    return 0;
}

size_t radar_pack(const radar_cirs &cirs, uint16_t seq, uint8_t *out, size_t out_size)
{
    // The notification: status, (reserved), number of CIRs (u16), CIR size in 32-bit words, (reserved), then
    // the CIRs: a 32-byte header (receive antenna at 18, start offset at 30) and complex int16 taps (I, Q)
    const uint8_t *d = cirs.data;
    if (cirs.size < 6 || d[0] != 0)
    {
        return 0;
    }
    size_t count = d[2] | (d[3] << 8);
    size_t cir_size = d[4] * 4;
    if (count == 0 || cir_size <= 32 || 6 + count * cir_size > cirs.size)
    {
        return 0;
    }
    size_t taps = (cir_size - 32) / 4;
    if (taps > 255 || 8 + count * (1 + taps) > out_size)
    {
        return 0;
    }
    const uint8_t *first = d + 6;
    out[0] = 'R';
    out[1] = '1';
    out[2] = seq & 0xFF;
    out[3] = seq >> 8;
    out[4] = (uint8_t)count;
    out[5] = (uint8_t)taps;
    out[6] = first[30];
    out[7] = first[31];
    uint8_t *p = out + 8;
    for (size_t c = 0; c < count; c++)
    {
        const uint8_t *cir = d + 6 + c * cir_size;
        *p++ = cir[18];
        const uint8_t *t = cir + 32;
        for (size_t i = 0; i < taps; i++, t += 4)
        {
            float re = (int16_t)(t[0] | (t[1] << 8));
            float im = (int16_t)(t[2] | (t[3] << 8));
            float half_db = 20.0f * log10f(re * re + im * im + 1.0f);   // 2 * 10 log10(power)
            *p++ = half_db > 255.0f ? 255 : (uint8_t)half_db;
        }
    }
    return p - out;
}

bool radar_next(radar_cirs *out, int timeout_ms)
{
    size_t have = 0;
    int64_t end = k_uptime_get() + timeout_ms;
    for (;;)
    {
        int left = (int)(end - k_uptime_get());
        if (left <= 0)
        {
            return false;
        }
        int n = uwb_receive(msg, sizeof(msg), left);
        if (n == 0)
        {
            return false;
        }
        if (n < 0)
        {
            radar_counts.errors += 1;
            return false;
        }
        // CIR notifications: 0x69 0x8A, segmented (bit 4 of byte 0 set) while more segments follow
        if ((msg[0] & 0xEF) != 0x69 || (msg[1] & 0x3F) != 0x0A)
        {
            radar_counts.other += 1;
            if (radar_counts.other <= 5)
            {
                print_message("radar: message", msg, n);
            }
            continue;
        }
        size_t part = n - 4;
        if (have + part > sizeof(cirs))
        {
            radar_counts.errors += 1;
            have = 0;
            continue;
        }
        memcpy(cirs + have, msg + 4, part);
        have += part;
        if (msg[0] & 0x10)
        {
            continue;   // more segments
        }
        if (have < 4)
        {
            radar_counts.errors += 1;
            have = 0;
            continue;
        }
        memcpy(&out->session, cirs, 4);
        out->data = cirs + 4;
        out->size = have - 4;
        radar_counts.notifications += 1;
        radar_counts.bytes += have;
        return true;
    }
}
