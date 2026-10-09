// Types shared by the wfb_core transmitter and receiver.
#pragma once

#include <stddef.h>
#include <stdint.h>

enum wfb_status
{
    WFB_OK = 0,
    WFB_ERR_CONFIG,    // a setting or the key is out of range
    WFB_ERR_NO_MEMORY, // an allocation in init() failed
    WFB_ERR_CRYPTO,    // libsodium failed
    WFB_ERR_TOO_BIG,   // the payload is larger than max_payload
    WFB_ERR_NOT_READY, // init() hasn't succeeded
};

const char *wfb_status_name(wfb_status status);

// Where finished packets go (R5): the radio on the ESP32, a socket or a list in the host tests.
// A packet is a wfb-ng packet without the 802.11 header; the sink adds whatever its medium needs.
class wfb_sink
{
public:
    virtual void send_frame(const uint8_t *buf, size_t size) = 0;

protected:
    ~wfb_sink() = default;
};

// Where the receiver hands recovered payloads (R5): an RTP stream, a socket, a test's list.
class wfb_payload_sink
{
public:
    virtual void send_payload(const uint8_t *buf, size_t size) = 0;

protected:
    ~wfb_payload_sink() = default;
};

// The largest packet handed to a sink: the ESP32 sends at most 1500 bytes per frame
// (esp_wifi_80211_tx), and the 802.11 header takes 24 of them (protocol table).
static const size_t WFB_MAX_PACKET = 1500 - 24;

struct wfb_tx_config
{
    uint8_t fec_k = 8;              // data packets per FEC block (protocol table)
    uint8_t fec_n = 12;             // data + FEC packets per block
    uint64_t epoch = 0;             // wfb_tx's default; receivers drop sessions from older epochs
    uint32_t channel_id = 0;        // (link_id << 8) + radio_port
    size_t max_packet = WFB_MAX_PACKET;
    size_t max_payload = 0;         // set by init() from max_packet
};
