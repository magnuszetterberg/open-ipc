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
#if CONFIG_RADIO_TX_COMMAND
#include "mlan_api.h"
#include "wifi-imu.h"
#include "wifi-internal.h"
#endif

#define WFB_HEADER 24   // frame control, duration, three addresses, sequence control

// wlan_host_tx_frame_params_t's fixed part: the length field and a four-address 802.11 header
#define HOST_FRAME_HEADER (sizeof(uint16_t) + TXRX_MGMT_FRAME_HEADER_LEN)

// The largest frame handed to the driver: on the data path one of its 1536-byte buffers, less its own headers
// in front of the frame; on the command path the same, which the command buffer holds with room to spare
#define HOST_FRAME_MAX 1400

// Set in radio_nxp_start(): CONFIG_RADIO_TX_CONTROL, or HT20 MCS from the configured MCS
uint32_t radio_nxp_tx_control;

#if CONFIG_RADIO_TX_COMMAND
// Adapted from wifi_mgmtframe_tx_cfg() in hal_nxp's mcux/middleware/wifi_nxp/wifidriver/mlan_api.c
// (BSD-3-Clause, NXP), at hal_nxp commit 7554bc0f6f6c284972eba7b2c17b16fbb9cc0ad0 (Zephyr v4.4.2): the same
// firmware command, with the frame's tx_control set (TXPD_TXRATE_ENABLE and a rate) instead of left at 0,
// where the firmware sends at its lowest rate, 1 Mbit/s. wlan_mgmtframe_tx_cfg()'s checks (monitor mode on,
// no network joined) are radio_nxp_start()'s.
// mlan_ds_misc_tx_frame ends in a 512-byte tx_buf, which wfb-ng's packets outgrow three times over, and
// smaller packets mean more frames for every receiver. The command it becomes has room for a full frame
// (the RW612's command buffer is WIFI_FW_CMDBUF_SIZE, 3072 bytes), so the struct gets a longer buffer behind
// it: tx_buf is its last member.
#define TX_BUF_EXTRA (HOST_FRAME_MAX + TXRX_MGMT_FRAME_HEADER_SIZE - 512)
static union
{
    mlan_ds_misc_tx_frame frame;
    uint8_t bytes[sizeof(mlan_ds_misc_tx_frame) + TX_BUF_EXTRA];
} tx_mem;   // static: 1.5 KB, and there is one sender

static int send_command(const wlan_host_tx_frame_params_t *frame)
{
    mlan_ds_misc_tx_frame *tx_frame = &tx_mem.frame;
    (void)memset(tx_mem.bytes, 0, sizeof(tx_mem.bytes));
    uint8_t *buf = tx_frame->tx_buf;
    uint32_t pkt_type = MRVL_PKT_TYPE_MGMT_FRAME;
    (void)memcpy(buf, &pkt_type, sizeof(pkt_type));
    (void)memcpy(buf + sizeof(pkt_type), &radio_nxp_tx_control, sizeof(radio_nxp_tx_control));
    (void)memcpy(buf + TXRX_MGMT_FRAME_HEADER_SIZE, frame, sizeof(frame->frm_len) + frame->frm_len);
    tx_frame->data_len = sizeof(frame->frm_len) + TXRX_MGMT_FRAME_HEADER_SIZE + frame->frm_len;
    tx_frame->buf_type = MLAN_BUF_TYPE_RAW_DATA;
    tx_frame->priority = 7;

    (void)wifi_get_command_lock();
    HostCmd_DS_COMMAND *cmd = wifi_get_command_buffer();
    (void)memset(cmd, 0x00, sizeof(HostCmd_DS_COMMAND));
    if (wlan_ops_sta_prepare_cmd((mlan_private *)mlan_adap->priv[0], HostCmd_CMD_802_11_TX_FRAME,
                                 HostCmd_ACT_GEN_SET, 0, NULL, tx_frame, cmd) != MLAN_STATUS_SUCCESS)
    {
        wifi_put_command_lock();
        return -WM_FAIL;
    }
    (void)wifi_wait_for_cmdresp(NULL);  // hands the command lock back
    return wm_wifi.cmd_resp_status;
}
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

#if CONFIG_RADIO_TX_COMMAND
    // The rate goes in each frame's tx_control: bit 15 fixes it, and rate index 14 + n is HT MCS n (indexes
    // measured on air, see Kconfig). wlan_set_txratecfg() below doesn't reach frames from the host.
    radio_nxp_tx_control = CONFIG_RADIO_TX_CONTROL ? CONFIG_RADIO_TX_CONTROL : (1u << 15) | ((14u + mcs) << 16);
#endif

    // A fixed rate for the data path, as wfb_tx sets in its radiotap header
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
    return send_command(frame);
#else
    return wifi_inject_frame(WLAN_BSS_TYPE_STA, buf, sizeof(uint16_t) + frame->frm_len);
#endif
}
