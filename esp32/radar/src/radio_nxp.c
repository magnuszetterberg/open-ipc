// Raw 802.11 transmit with NXP's WiFi driver on the RW612 (platform layer, R4). See radio.hpp.
//
// The driver sends frames from the host only in monitor mode, with no network joined, in one layout: a
// 16-bit length, the 802.11 header with a fourth address, then the body (wlan_host_tx_frame_params_t).
// wfb-ng's header has three addresses; the fourth is sent as zeros, and the firmware leaves it out of a
// frame that isn't from-DS and to-DS. Two driver paths take that layout (Kconfig: RADIO_TX_*):
//   - wlan_mgmtframe_tx_cfg(): a firmware command per frame, at most 512 bytes; NXP's documented path for
//     "management and data frames in monitor mode";
//   - wifi_inject_frame(): the data path, no wait per frame, larger frames.
// Which one carries wfb-ng's frames unchanged, and how fast, is what M8 measures (docs/DESIGN.md).
#include "radio_nxp.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "wifi.h"
#include "wlan.h"

#define WFB_HEADER 24   // frame control, duration, three addresses, sequence control

// wlan_host_tx_frame_params_t's fixed part: the length field and a four-address 802.11 header
#define HOST_FRAME_HEADER (sizeof(uint16_t) + TXRX_MGMT_FRAME_HEADER_LEN)

#if CONFIG_RADIO_TX_COMMAND
// mlan_ds_misc_tx_frame's tx_buf is 512 bytes, and the driver puts an 8-byte packet header in front
#define HOST_FRAME_MAX (512 - TXRX_MGMT_FRAME_HEADER_SIZE)
#else
// One of the driver's 1536-byte buffers, less its own headers in front of the frame
#define HOST_FRAME_MAX 1400
#endif

size_t radio_nxp_max_body(void)
{
    return HOST_FRAME_MAX - HOST_FRAME_HEADER;
}

int radio_nxp_start(uint8_t channel, uint8_t mcs)
{
    // The driver starts by itself at boot (NXP_WIFI_AUTO_INIT), loading the WiFi core's firmware first
    for (int i = 0; !wlan_is_started(); i++)
    {
        if (i == 100)
        {
            printk("radio: the WiFi driver didn't start within 10 s\n");
            return -ETIMEDOUT;
        }
        k_msleep(100);
    }

    wlan_net_monitor_t monitor;
    memset(&monitor, 0, sizeof(monitor));
    monitor.action = 1;            // set
    monitor.monitor_activity = 1;  // on
    monitor.filter_flags = 7;      // management, control and data frames
    monitor.radio_type = 0;        // 2.4 GHz, 20 MHz, manual channel
    monitor.chan_number = channel;
    int rc = wlan_net_monitor_cfg(&monitor);
    if (rc != WM_SUCCESS)
    {
        printk("radio: monitor mode on channel %d failed (%d)\n", channel, rc);
        return -EIO;
    }

    // A fixed rate, as wfb_tx sets in its radiotap header. Whether the firmware applies it to frames from
    // the host is part of what M8 measures.
    wlan_ds_rate rate;
    memset(&rate, 0, sizeof(rate));
    rate.sub_command = WIFI_DS_RATE_CFG;
    rate.param.rate_cfg.rate_format = MLAN_RATE_FORMAT_HT;
    rate.param.rate_cfg.rate_index = mcs;
    rc = wlan_set_txratecfg(rate, MLAN_BSS_TYPE_STA);
    if (rc != WM_SUCCESS)
    {
        printk("radio: fixed rate HT MCS %d not accepted (%d); the firmware picks the rate\n", mcs, rc);
    }

    printk("radio: monitor mode, channel %d, HT20 MCS %d, %s, up to %u bytes per frame body\n", channel, mcs,
           IS_ENABLED(CONFIG_RADIO_TX_COMMAND) ? "wlan_mgmtframe_tx_cfg" : "wifi_inject_frame",
           (unsigned)radio_nxp_max_body());
    return 0;
}

int radio_nxp_send(const uint8_t *header, const uint8_t *body, size_t body_size)
{
    if (body_size > radio_nxp_max_body())
    {
        return -EMSGSIZE;
    }
    // The driver's layout, in a buffer of the size it copies from (frm_len + 2 bytes). One sender at a time.
    static uint8_t buf[HOST_FRAME_MAX] __aligned(4);
    wlan_host_tx_frame_params_t *frame = (wlan_host_tx_frame_params_t *)buf;
    frame->frm_len = TXRX_MGMT_FRAME_HEADER_LEN + body_size;
    // frame control, duration, three addresses and sequence control: the same 24 bytes in the same order
    memcpy(&frame->frm_ctl, header, WFB_HEADER);
    memset(frame->addr4, 0, sizeof(frame->addr4));
    memcpy(frame->payload, body, body_size);

#if CONFIG_RADIO_TX_COMMAND
    return wlan_mgmtframe_tx_cfg(frame);
#else
    return wifi_inject_frame(WLAN_BSS_TYPE_STA, buf, sizeof(uint16_t) + frame->frm_len);
#endif
}
