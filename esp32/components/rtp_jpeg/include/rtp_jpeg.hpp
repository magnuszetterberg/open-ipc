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

// The headers of the JPEG file an RTP/JPEG frame stands for (RFC 2435 appendix A's MakeHeaders): SOI,
// DQT with the frame's two tables, SOF0, DHT with the standard tables, SOS. The scan data and EOI follow.
// qtables: luma then chroma, 64 bytes each. Returns the size written, 0 if cap is too small.
static const size_t RTP_JPEG_HEADERS_MAX = 600;
size_t jpeg_headers(uint8_t *out, size_t cap, uint8_t type, int width, int height, const uint8_t *qtables);

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

// Receives JPEG files a piece at a time, as rtp_jpeg_streamer rebuilds them.
class jpeg_stream_sink
{
public:
    virtual void jpeg_begin() = 0;
    virtual void jpeg_data(const uint8_t *data, size_t size) = 0;
    // complete: the JPEG ended with EOI. Otherwise a packet went missing and it stops where it was cut off.
    virtual void jpeg_end(bool complete) = 0;

protected:
    ~jpeg_stream_sink() = default;
};

// Rebuilds JPEG files from RTP/JPEG packets as they arrive, without holding a frame (the base station has
// no room for one per camera): the headers when a frame's first packet arrives, then each packet's scan
// data, then EOI. A frame with a packet missing is cut off; the next frame starts afresh. For whole frames,
// the bytes are the same as rtp_jpeg_depacketizer's.
class rtp_jpeg_streamer
{
public:
    explicit rtp_jpeg_streamer(jpeg_stream_sink &sink);

    void push(const uint8_t *packet, size_t size);

    uint32_t frames = 0;      // complete
    uint32_t frames_cut = 0;  // started but cut off, or refused (no in-band tables)

private:
    jpeg_stream_sink &sink;
    bool streaming = false;
    uint32_t timestamp = 0;
    size_t next_offset = 0;
    uint8_t headers[RTP_JPEG_HEADERS_MAX];
};
