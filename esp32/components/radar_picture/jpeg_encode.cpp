// Baseline JPEG encoder for the radar picture (core layer, R4). See radar_picture.hpp.
//
// The JPEG standard (ITU T.81): 8x8 DCT, quantization, zigzag, and Huffman coding with the standard tables
// (Annex K), in MCUs of two luma blocks and one block each of Cb and Cr (Y sampled 2x1). The headers are
// rtp_jpeg's own (jpeg_headers), so the file is exactly what RFC 2435 type 0 describes.
#include <math.h>
#include <string.h>

#include "jpeg_tables.hpp"
#include "radar_picture.hpp"
#include "rtp_jpeg.hpp"

namespace
{
// Annex K.1, K.2: the example quantization tables, in natural (row) order
const uint8_t LUMA_Q[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                            14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                            18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                            49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
const uint8_t CHROMA_Q[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                              24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                              99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                              99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};
// Position in the block (natural order) of the n-th coefficient in zigzag order
const uint8_t ZIGZAG[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                            12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                            35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                            58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct huffman
{
    uint16_t code[256];
    uint8_t length[256];

    // From a DHT table body (class/id, 16 counts, symbols): the canonical codes of Annex C
    void build(const uint8_t *dht)
    {
        memset(length, 0, sizeof(length));
        const uint8_t *counts = dht + 1, *symbols = dht + 17;
        uint16_t c = 0;
        int n = 0;
        for (int len = 1; len <= 16; len++, c <<= 1)
        {
            for (int i = 0; i < counts[len - 1]; i++, c++)
            {
                code[symbols[n]] = c;
                length[symbols[n]] = (uint8_t)len;
                n++;
            }
        }
    }
};

struct bit_writer
{
    uint8_t *out;
    size_t cap, n = 0;
    uint32_t bits = 0;
    int count = 0;
    bool overflow = false;

    void byte(uint8_t b)
    {
        if (n + 2 > cap)
        {
            overflow = true;
            return;
        }
        out[n++] = b;
        if (b == 0xff)
        {
            out[n++] = 0;   // byte stuffing: 0xff in the scan is followed by 0
        }
    }
    void put(uint32_t value, int len)
    {
        bits = (bits << len) | (value & ((1u << len) - 1));
        count += len;
        while (count >= 8)
        {
            byte((uint8_t)(bits >> (count - 8)));
            count -= 8;
        }
    }
    void flush()
    {
        if (count > 0)
        {
            put(0x7f, 8 - count);   // padded with 1s
        }
    }
};

// cos((2x + 1) u pi / 16), with C(0) = 1/sqrt(2), and the 1/2 of the 1D DCT
float dct_table[8][8];

void init_dct()
{
    static bool done = false;
    if (done)
    {
        return;
    }
    for (int u = 0; u < 8; u++)
    {
        for (int x = 0; x < 8; x++)
        {
            dct_table[u][x] = 0.5f * (u == 0 ? 0.70710678f : 1.0f) * cosf((2 * x + 1) * u * 3.14159265f / 16);
        }
    }
    done = true;
}

// One 8x8 block: level shift, DCT, quantization, zigzag, Huffman. pred: the component's previous DC.
void encode_block(bit_writer &w, const uint8_t *plane, int stride, int x0, int y0, const float *divisors,
                  const huffman &dc, const huffman &ac, int *pred)
{
    float in[8][8], tmp[8][8];
    int q[64];
    // Most of a drawn picture is flat: a block of one value has only its DC coefficient (8 x the value)
    const uint8_t first = plane[y0 * stride + x0];
    bool flat = true;
    for (int yy = 0; yy < 8 && flat; yy++)
    {
        const uint8_t *row = plane + (y0 + yy) * stride + x0;
        for (int x = 0; x < 8; x++)
        {
            flat = flat && row[x] == first;
        }
    }
    if (flat)
    {
        memset(q, 0, sizeof(q));
        float dc = 8.0f * (first - 128.0f) / divisors[0];
        q[0] = (int)(dc < 0 ? dc - 0.5f : dc + 0.5f);
    }
    else
    {
    for (int yy = 0; yy < 8; yy++)
    {
        for (int x = 0; x < 8; x++)
        {
            in[yy][x] = plane[(y0 + yy) * stride + x0 + x] - 128.0f;
        }
    }
    for (int yy = 0; yy < 8; yy++)   // rows
    {
        for (int u = 0; u < 8; u++)
        {
            float s = 0;
            for (int x = 0; x < 8; x++)
            {
                s += dct_table[u][x] * in[yy][x];
            }
            tmp[yy][u] = s;
        }
    }
    for (int u = 0; u < 8; u++)   // columns, then quantization
    {
        for (int v = 0; v < 8; v++)
        {
            float s = 0;
            for (int yy = 0; yy < 8; yy++)
            {
                s += dct_table[v][yy] * tmp[yy][u];
            }
            s /= divisors[v * 8 + u];
            q[v * 8 + u] = (int)(s < 0 ? s - 0.5f : s + 0.5f);
        }
    }
    }

    // DC: the difference from the previous block, as its size category and then its bits
    auto put_value = [&](int v, int size) { w.put(v < 0 ? v + (1 << size) - 1 : v, size); };
    auto size_of = [](int v) {
        int a = v < 0 ? -v : v, s = 0;
        while (a)
        {
            s++;
            a >>= 1;
        }
        return s;
    };
    int diff = q[0] - *pred;
    *pred = q[0];
    int s = size_of(diff);
    w.put(dc.code[s], dc.length[s]);
    if (s)
    {
        put_value(diff, s);
    }
    // AC: runs of zeros and the next value, 16 zeros as ZRL (0xf0), the rest of the block as EOB (0x00)
    int run = 0;
    for (int i = 1; i < 64; i++)
    {
        int v = q[ZIGZAG[i]];
        if (v == 0)
        {
            run++;
            continue;
        }
        while (run > 15)
        {
            w.put(ac.code[0xf0], ac.length[0xf0]);
            run -= 16;
        }
        s = size_of(v);
        int sym = (run << 4) | s;
        w.put(ac.code[sym], ac.length[sym]);
        put_value(v, s);
        run = 0;
    }
    if (run)
    {
        w.put(ac.code[0], ac.length[0]);
    }
}
}  // namespace

size_t jpeg_encode_422(const uint8_t *y, const uint8_t *cb, const uint8_t *cr, int width, int height, int quality,
                       uint8_t *out, size_t out_size)
{
    if (width % 16 || height % 8 || width <= 0 || height <= 0)
    {
        return 0;
    }
    init_dct();
    static huffman luma_dc, luma_ac, chroma_dc, chroma_ac;
    static bool built = false;
    if (!built)
    {
        luma_dc.build(jpeg_dht_luma_dc);
        luma_ac.build(jpeg_dht_luma_ac);
        chroma_dc.build(jpeg_dht_chroma_dc);
        chroma_ac.build(jpeg_dht_chroma_ac);
        built = true;
    }

    // The quality's tables (libjpeg's scaling), in zigzag order for DQT; the divisors in natural order
    if (quality < 1)
    {
        quality = 1;
    }
    if (quality > 100)
    {
        quality = 100;
    }
    int scale = quality < 50 ? 5000 / quality : 200 - 2 * quality;
    uint8_t qtables[128];
    float luma_div[64], chroma_div[64];
    for (int i = 0; i < 64; i++)
    {
        int l = (LUMA_Q[ZIGZAG[i]] * scale + 50) / 100, c = (CHROMA_Q[ZIGZAG[i]] * scale + 50) / 100;
        l = l < 1 ? 1 : l > 255 ? 255 : l;
        c = c < 1 ? 1 : c > 255 ? 255 : c;
        qtables[i] = (uint8_t)l;
        qtables[64 + i] = (uint8_t)c;
        luma_div[ZIGZAG[i]] = (float)l;
        chroma_div[ZIGZAG[i]] = (float)c;
    }

    size_t n = jpeg_headers(out, out_size, 0, width, height, qtables);
    if (n == 0)
    {
        return 0;
    }
    bit_writer w{out + n, out_size - n};
    int pred_y = 0, pred_cb = 0, pred_cr = 0;
    for (int my = 0; my < height; my += 8)
    {
        for (int mx = 0; mx < width; mx += 16)
        {
            encode_block(w, y, width, mx, my, luma_div, luma_dc, luma_ac, &pred_y);
            encode_block(w, y, width, mx + 8, my, luma_div, luma_dc, luma_ac, &pred_y);
            encode_block(w, cb, width / 2, mx / 2, my, chroma_div, chroma_dc, chroma_ac, &pred_cb);
            encode_block(w, cr, width / 2, mx / 2, my, chroma_div, chroma_dc, chroma_ac, &pred_cr);
        }
    }
    w.flush();
    if (w.overflow || n + w.n + 2 > out_size)
    {
        return 0;
    }
    n += w.n;
    out[n++] = 0xff;
    out[n++] = 0xd9;   // EOI
    return n;
}
