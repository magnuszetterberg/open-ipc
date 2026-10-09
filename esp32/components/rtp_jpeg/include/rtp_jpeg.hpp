// JPEG frames as RTP packets, per RFC 2435 (core layer, R4, R7): plain C++, no ESP-IDF headers.
//
// Supports what the OV2640 produces: baseline JPEG, 8-bit quantization tables, Y sampled 2x1 (type 0)
// or 2x2 (type 1), Cb and Cr 1x1, the standard Huffman tables, no restart markers. The quantization
// tables travel in each frame's first packet (Q = 255); the Huffman tables don't travel at all, which
// is why a JPEG with other tables is refused rather than sent and decoded wrong.
#pragma once

#include <stddef.h>
#include <stdint.h>

enum rtp_jpeg_status
{
    RTP_JPEG_OK = 0,
    RTP_JPEG_NOT_JPEG,     // no SOI, a segment runs past the end, no scan, ...
    RTP_JPEG_UNSUPPORTED,  // valid JPEG, but outside what RFC 2435 types 0/1 carry (see above)
    RTP_JPEG_BAD_CONFIG,   // max_packet too small or too big
};

const char *rtp_jpeg_status_name(rtp_jpeg_status status);

// What the packetizer needs from a JPEG. The pointers point into the JPEG.
struct jpeg_info
{
    int width = 0, height = 0;
    uint8_t type = 0;                       // RFC 2435: 0 = Y 2x1, 1 = Y 2x2
    const uint8_t *qtable[2] = {};          // luma, chroma: 64 bytes each, zigzag order as in DQT
    const uint8_t *scan = nullptr;          // entropy-coded data, after the SOS header, before EOI
    size_t scan_size = 0;
};

rtp_jpeg_status jpeg_parse(const uint8_t *jpeg, size_t size, jpeg_info *info);

class rtp_jpeg_sink
{
public:
    virtual void send_rtp(const uint8_t *packet, size_t size) = 0;

protected:
    ~rtp_jpeg_sink() = default;
};

struct rtp_jpeg_config
{
    size_t max_packet = 1448;   // RTP packet size limit: wfb_tx's max_payload by default
    uint32_t ssrc = 0x45535033; // "ESP3"
    uint8_t payload_type = 26;  // RFC 3551's static type for JPEG
};

class rtp_jpeg_packetizer
{
public:
    rtp_jpeg_packetizer(rtp_jpeg_sink &sink, const rtp_jpeg_config &config);

    // Sends one JPEG as one or more RTP packets, the last one marked. timestamp: 90 kHz clock.
    rtp_jpeg_status send_frame(const uint8_t *jpeg, size_t size, uint32_t timestamp);

    uint16_t seq = 0;

private:
    rtp_jpeg_sink &sink;
    rtp_jpeg_config config;
    uint8_t packet[1500];
};

// Puts RTP/JPEG packets back together into JPEG files. Used by the host tests, and by anything that
// has to hand a receiver whole JPEGs.
class rtp_jpeg_depacketizer
{
public:
    // frame: where finished JPEGs are written; scan: room to collect a frame's scan data meanwhile.
    rtp_jpeg_depacketizer(uint8_t *frame, size_t frame_size, uint8_t *scan, size_t scan_size);

    // Returns the size of a finished JPEG in frame when this packet completes one, else 0.
    size_t push(const uint8_t *packet, size_t size);

    uint32_t frames_dropped = 0;  // incomplete: a packet missing, or out of room

private:
    uint8_t *frame;
    size_t frame_cap;
    uint8_t *scan;
    size_t scan_cap;
    size_t scan_size = 0;
    bool collecting = false;
    uint32_t timestamp = 0;
    uint8_t type = 0;
    int width = 0, height = 0;
    uint8_t qtables[128] = {};
};
