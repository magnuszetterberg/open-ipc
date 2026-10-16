#include "rtp_jpeg.hpp"

#include <string.h>

#include "jpeg_tables.hpp"

static const size_t RTP_HEADER = 12;
static const size_t JPEG_HEADER = 8;
static const size_t QUANT_HEADER = 4 + 128;  // two 8-bit tables

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

const char *rtp_jpeg_status_name(rtp_jpeg_status status)
{
    switch (status)
    {
    case RTP_JPEG_OK:
        return "ok";
    case RTP_JPEG_NOT_JPEG:
        return "not a JPEG";
    case RTP_JPEG_UNSUPPORTED:
        return "JPEG layout RFC 2435 can't carry";
    case RTP_JPEG_BAD_CONFIG:
        return "bad packet size";
    }
    return "unknown";
}

// A DHT table must be the standard one for its class and id: receivers rebuild those, not ours.
static bool standard_dht(const uint8_t *table, size_t size)
{
    const uint8_t *expect = nullptr;
    size_t expect_size = 0;
    switch (table[0])
    {
    case 0x00:
        expect = jpeg_dht_luma_dc, expect_size = sizeof(jpeg_dht_luma_dc);
        break;
    case 0x10:
        expect = jpeg_dht_luma_ac, expect_size = sizeof(jpeg_dht_luma_ac);
        break;
    case 0x01:
        expect = jpeg_dht_chroma_dc, expect_size = sizeof(jpeg_dht_chroma_dc);
        break;
    case 0x11:
        expect = jpeg_dht_chroma_ac, expect_size = sizeof(jpeg_dht_chroma_ac);
        break;
    default:
        return false;
    }
    return size == expect_size && memcmp(table, expect, size) == 0;
}

rtp_jpeg_status jpeg_parse(const uint8_t *jpeg, size_t size, jpeg_info *info)
{
    const uint8_t *qt[4] = {};
    uint8_t y_table = 0xff, c_table = 0xff;
    *info = jpeg_info();

    if (size < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8)
    {
        return RTP_JPEG_NOT_JPEG;
    }
    size_t i = 2;
    while (i + 4 <= size)
    {
        if (jpeg[i] != 0xff)
        {
            return RTP_JPEG_NOT_JPEG;
        }
        uint8_t marker = jpeg[i + 1];
        if (marker == 0xff)
        {
            i += 1;  // fill byte
            continue;
        }
        size_t len = be16(jpeg + i + 2);
        const uint8_t *seg = jpeg + i + 4;
        if (len < 2 || i + 2 + len > size)
        {
            return RTP_JPEG_NOT_JPEG;
        }
        size_t seg_size = len - 2;

        if (marker == 0xdb)  // DQT: one or more tables
        {
            for (size_t j = 0; j < seg_size;)
            {
                uint8_t precision = seg[j] >> 4, id = seg[j] & 0x0f;
                if (precision != 0 || id > 3 || j + 65 > seg_size)
                {
                    return RTP_JPEG_UNSUPPORTED;
                }
                qt[id] = seg + j + 1;
                j += 65;
            }
        }
        else if (marker == 0xc4)  // DHT: one or more tables
        {
            for (size_t j = 0; j < seg_size;)
            {
                if (j + 17 > seg_size)
                {
                    return RTP_JPEG_NOT_JPEG;
                }
                size_t count = 0;
                for (int k = 1; k <= 16; k++)
                {
                    count += seg[j + k];
                }
                if (j + 17 + count > seg_size || !standard_dht(seg + j, 17 + count))
                {
                    return RTP_JPEG_UNSUPPORTED;
                }
                j += 17 + count;
            }
        }
        else if (marker == 0xc0)  // SOF0: baseline
        {
            if (seg_size < 15 || seg[0] != 8 || seg[5] != 3)
            {
                return RTP_JPEG_UNSUPPORTED;
            }
            info->height = be16(seg + 1);
            info->width = be16(seg + 3);
            uint8_t y = seg[7], cb = seg[10], cr = seg[13];
            if (cb != 0x11 || cr != 0x11 || seg[11] != seg[14])
            {
                return RTP_JPEG_UNSUPPORTED;
            }
            if (y == 0x21)
            {
                info->type = 0;
            }
            else if (y == 0x22)
            {
                info->type = 1;
            }
            else
            {
                return RTP_JPEG_UNSUPPORTED;
            }
            y_table = seg[8];
            c_table = seg[11];
        }
        else if ((marker >= 0xc1 && marker <= 0xcf) && marker != 0xc4 && marker != 0xc8 && marker != 0xcc)
        {
            return RTP_JPEG_UNSUPPORTED;  // progressive, lossless, arithmetic coding, ...
        }
        else if (marker == 0xdd)  // DRI: restart markers need RFC 2435 types 64+, not implemented
        {
            if (seg_size < 2 || be16(seg) != 0)
            {
                return RTP_JPEG_UNSUPPORTED;
            }
        }
        else if (marker == 0xda)  // SOS: the scan follows its header and runs to EOI
        {
            if (info->width == 0 || seg_size < 1 || seg[0] != 3 || seg_size < 1 + 2 * 3 + 3)
            {
                return RTP_JPEG_UNSUPPORTED;
            }
            if (seg[2] != 0x00 || seg[4] != 0x11 || seg[6] != 0x11)  // the standard tables' ids per component
            {
                return RTP_JPEG_UNSUPPORTED;
            }
            if (y_table > 3 || c_table > 3 || qt[y_table] == nullptr || qt[c_table] == nullptr)
            {
                return RTP_JPEG_NOT_JPEG;
            }
            if (info->width > 2040 || info->height > 2040 || info->width % 8 || info->height % 8)
            {
                return RTP_JPEG_UNSUPPORTED;
            }
            info->qtable[0] = qt[y_table];
            info->qtable[1] = qt[c_table];
            info->scan = jpeg + i + 2 + len;
            size_t end = size;
            while (end >= 2 && !(jpeg[end - 2] == 0xff && jpeg[end - 1] == 0xd9))
            {
                end--;  // the camera's buffer may run past EOI
            }
            if (end < 2 || jpeg + end - 2 < info->scan)
            {
                return RTP_JPEG_NOT_JPEG;
            }
            info->scan_size = (size_t)(jpeg + end - 2 - info->scan);
            return RTP_JPEG_OK;
        }
        i += 2 + len;
    }
    return RTP_JPEG_NOT_JPEG;
}

rtp_jpeg_packetizer::rtp_jpeg_packetizer(rtp_jpeg_sink &sink, const rtp_jpeg_config &config)
    : sink(sink), config(config)
{
}

rtp_jpeg_status rtp_jpeg_packetizer::send_frame(const uint8_t *jpeg, size_t size, uint32_t timestamp)
{
    if (config.max_packet > sizeof(packet) || config.max_packet < RTP_HEADER + JPEG_HEADER + QUANT_HEADER + 1)
    {
        return RTP_JPEG_BAD_CONFIG;
    }
    jpeg_info info;
    rtp_jpeg_status rc = jpeg_parse(jpeg, size, &info);
    if (rc != RTP_JPEG_OK)
    {
        return rc;
    }

    for (size_t offset = 0; offset < info.scan_size;)
    {
        uint8_t *p = packet;
        // RTP header (RFC 3550); the marker bit is set below on the frame's last packet
        p[0] = 0x80;
        p[1] = config.payload_type;
        p[2] = seq >> 8, p[3] = seq & 0xff;
        p[4] = timestamp >> 24, p[5] = timestamp >> 16, p[6] = timestamp >> 8, p[7] = timestamp;
        p[8] = config.ssrc >> 24, p[9] = config.ssrc >> 16, p[10] = config.ssrc >> 8, p[11] = config.ssrc;
        p += RTP_HEADER;
        // JPEG header (RFC 2435 3.1): type-specific, fragment offset, type, Q, width/8, height/8
        p[0] = 0;
        p[1] = offset >> 16, p[2] = offset >> 8, p[3] = offset;
        p[4] = info.type;
        p[5] = 255;  // quantization tables in-band, in the first packet
        p[6] = info.width / 8;
        p[7] = info.height / 8;
        p += JPEG_HEADER;
        if (offset == 0)
        {
            // Quantization table header (3.1.8): MBZ, precision (8-bit), length, the tables
            p[0] = 0, p[1] = 0, p[2] = 0, p[3] = 128;
            memcpy(p + 4, info.qtable[0], 64);
            memcpy(p + 4 + 64, info.qtable[1], 64);
            p += QUANT_HEADER;
        }
        size_t room = config.max_packet - (size_t)(p - packet);
        size_t chunk = info.scan_size - offset < room ? info.scan_size - offset : room;
        memcpy(p, info.scan + offset, chunk);
        offset += chunk;
        if (offset == info.scan_size)
        {
            packet[1] |= 0x80;
        }
        sink.send_rtp(packet, (size_t)(p - packet) + chunk);
        seq++;
    }
    return RTP_JPEG_OK;
}

// The fields of one RTP/JPEG packet that the receiving side needs.
struct rtp_jpeg_fields
{
    bool marker;
    uint32_t timestamp;
    size_t offset;
    uint8_t type;
    int width, height;
    const uint8_t *qtables;  // in-band tables (first packet, Q >= 128), else null
    const uint8_t *data;     // scan data
    size_t data_size;
};

static bool parse_packet(const uint8_t *packet, size_t size, rtp_jpeg_fields *f)
{
    if (size < RTP_HEADER + JPEG_HEADER || (packet[0] & 0xc0) != 0x80)
    {
        return false;
    }
    const uint8_t *j = packet + RTP_HEADER;
    f->marker = packet[1] & 0x80;
    f->timestamp = (uint32_t)packet[4] << 24 | packet[5] << 16 | packet[6] << 8 | packet[7];
    f->offset = (size_t)j[1] << 16 | j[2] << 8 | j[3];
    f->type = j[4];
    f->width = j[6] * 8;
    f->height = j[7] * 8;
    f->qtables = nullptr;
    f->data = j + JPEG_HEADER;
    f->data_size = size - RTP_HEADER - JPEG_HEADER;
    // Only in-band 8-bit tables, as the packetizer sends them
    if (f->offset == 0 && j[5] >= 128 && f->data_size >= QUANT_HEADER && f->data[1] == 0 && be16(f->data + 2) == 128)
    {
        f->qtables = f->data + 4;
        f->data += QUANT_HEADER;
        f->data_size -= QUANT_HEADER;
    }
    return true;
}

// Appends bytes to out, if they fit.
static bool put(uint8_t *out, size_t cap, size_t *n, const void *data, size_t size)
{
    if (*n + size > cap)
    {
        return false;
    }
    memcpy(out + *n, data, size);
    *n += size;
    return true;
}

size_t jpeg_headers(uint8_t *out, size_t cap, uint8_t type, int width, int height, const uint8_t *qtables)
{
    size_t n = 0;
    const uint8_t soi[] = {0xff, 0xd8};
    const uint8_t dqt[] = {0xff, 0xdb, 0, 2 + 2 * 65};
    const uint8_t sof[] = {0xff, 0xc0, 0, 17, 8, (uint8_t)(height >> 8), (uint8_t)height, (uint8_t)(width >> 8),
                           (uint8_t)width, 3, 1, (uint8_t)(type == 0 ? 0x21 : 0x22), 0, 2, 0x11, 1, 3, 0x11, 1};
    const size_t dht_len = 2 + sizeof(jpeg_dht_luma_dc) + sizeof(jpeg_dht_luma_ac) + sizeof(jpeg_dht_chroma_dc) +
                           sizeof(jpeg_dht_chroma_ac);
    const uint8_t dht[] = {0xff, 0xc4, (uint8_t)(dht_len >> 8), (uint8_t)dht_len};
    const uint8_t sos[] = {0xff, 0xda, 0, 12, 3, 1, 0x00, 2, 0x11, 3, 0x11, 0, 63, 0};
    uint8_t id0 = 0, id1 = 1;
    bool ok = put(out, cap, &n, soi, sizeof(soi)) && put(out, cap, &n, dqt, sizeof(dqt)) &&
              put(out, cap, &n, &id0, 1) && put(out, cap, &n, qtables, 64) && put(out, cap, &n, &id1, 1) &&
              put(out, cap, &n, qtables + 64, 64) && put(out, cap, &n, sof, sizeof(sof)) &&
              put(out, cap, &n, dht, sizeof(dht)) && put(out, cap, &n, jpeg_dht_luma_dc, sizeof(jpeg_dht_luma_dc)) &&
              put(out, cap, &n, jpeg_dht_luma_ac, sizeof(jpeg_dht_luma_ac)) &&
              put(out, cap, &n, jpeg_dht_chroma_dc, sizeof(jpeg_dht_chroma_dc)) &&
              put(out, cap, &n, jpeg_dht_chroma_ac, sizeof(jpeg_dht_chroma_ac)) && put(out, cap, &n, sos, sizeof(sos));
    return ok ? n : 0;
}

static const uint8_t jpeg_eoi[] = {0xff, 0xd9};

rtp_jpeg_depacketizer::rtp_jpeg_depacketizer(uint8_t *frame, size_t frame_size, uint8_t *scan, size_t scan_size)
    : frame(frame), frame_cap(frame_size), scan(scan), scan_cap(scan_size)
{
}

size_t rtp_jpeg_depacketizer::push(const uint8_t *packet, size_t size)
{
    rtp_jpeg_fields f;
    if (!parse_packet(packet, size, &f))
    {
        return 0;
    }
    if (f.offset == 0)
    {
        if (collecting)
        {
            frames_dropped++;  // the previous frame never got its last packet
        }
        collecting = false;
        if (f.qtables == nullptr)
        {
            frames_dropped++;
            return 0;
        }
        memcpy(qtables, f.qtables, 128);
        collecting = true;
        timestamp = f.timestamp;
        type = f.type;
        width = f.width;
        height = f.height;
        scan_size = 0;
    }
    if (!collecting)
    {
        return 0;
    }
    if (f.timestamp != timestamp || f.offset != scan_size || scan_size + f.data_size > scan_cap)
    {
        frames_dropped++;  // a packet went missing, or the frame doesn't fit
        collecting = false;
        return 0;
    }
    memcpy(scan + scan_size, f.data, f.data_size);
    scan_size += f.data_size;
    if (!f.marker)
    {
        return 0;
    }
    collecting = false;

    // A JPEG file around the scan: the headers, the scan, EOI
    size_t n = jpeg_headers(frame, frame_cap, type, width, height, qtables);
    if (n == 0 || !put(frame, frame_cap, &n, scan, scan_size) || !put(frame, frame_cap, &n, jpeg_eoi, sizeof(jpeg_eoi)))
    {
        frames_dropped++;
        return 0;
    }
    return n;
}

rtp_jpeg_streamer::rtp_jpeg_streamer(jpeg_stream_sink &sink) : sink(sink)
{
}

void rtp_jpeg_streamer::push(const uint8_t *packet, size_t size)
{
    rtp_jpeg_fields f;
    if (!parse_packet(packet, size, &f))
    {
        return;
    }
    if (f.offset == 0)
    {
        if (streaming)
        {
            streaming = false;  // the previous frame never got its last packet
            frames_cut++;
            sink.jpeg_end(false);
        }
        size_t n = f.qtables ? jpeg_headers(headers, sizeof(headers), f.type, f.width, f.height, f.qtables) : 0;
        if (n == 0)
        {
            frames_cut++;
            return;
        }
        streaming = true;
        timestamp = f.timestamp;
        next_offset = 0;
        sink.jpeg_begin();
        sink.jpeg_data(headers, n);
    }
    if (!streaming)
    {
        return;  // joined mid-frame: wait for the next one
    }
    if (f.timestamp != timestamp || f.offset != next_offset)
    {
        streaming = false;  // a packet went missing
        frames_cut++;
        sink.jpeg_end(false);
        return;
    }
    if (f.data_size > 0)
    {
        sink.jpeg_data(f.data, f.data_size);
    }
    next_offset += f.data_size;
    if (f.marker)
    {
        sink.jpeg_data(jpeg_eoi, sizeof(jpeg_eoi));
        streaming = false;
        frames++;
        sink.jpeg_end(true);
    }
}
