// What one measurement found, as a link payload (core layer, R4). See radar_picture.hpp.
#include <string.h>

#include "radar_picture.hpp"

radar_data radar_data_of(const radar_detector &d)
{
    radar_data out;
    out.finding = d.finding;
    out.measurements = d.measurements;
    out.antennas = d.antennas;
    // The taps from the direct signal (0 m) out to MAX_M: the rest is the radar itself, or out of range
    int taps = d.taps - d.zero_tap;
    int in_range = (int)(radar_detector::MAX_M / radar_detector::TAP_M) + 1;
    taps = taps < in_range ? taps : in_range;
    out.taps = taps < 0 ? 0 : taps > radar_data::MAX_TAPS ? radar_data::MAX_TAPS : taps;
    for (int a = 0; a < d.antennas; a++)
    {
        memcpy(out.movement[a], &d.movement[a][d.zero_tap], out.taps * sizeof(float));
    }
    return out;
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static unsigned get16(const uint8_t *p)
{
    return p[0] | p[1] << 8;
}

// A non-negative value in steps, rounded, at most max
static unsigned steps(float v, float step, unsigned max)
{
    float n = v / step + 0.5f;
    return n <= 0 ? 0 : n >= (float)max ? max : (unsigned)n;
}

size_t radar_data_pack(const radar_data &d, uint16_t seq, uint8_t *out, size_t out_size)
{
    size_t size = 16 + (size_t)d.antennas * d.taps;
    if (d.antennas < 0 || d.antennas > radar_detector::MAX_ANTENNAS || d.taps < 0 || d.taps > radar_data::MAX_TAPS ||
        size > out_size)
    {
        return 0;
    }
    out[0] = 'D';
    out[1] = '1';
    put16(out + 2, seq);
    out[4] = (d.finding.learning ? 1 : 0) | (d.finding.moving ? 2 : 0);
    out[5] = (uint8_t)d.antennas;
    out[6] = (uint8_t)d.taps;
    out[7] = 0;
    put16(out + 8, steps(d.finding.distance_m, 0.01f, 0xffff));
    put16(out + 10, steps(d.finding.strength_db, 0.1f, 0xffff));
    put16(out + 12, d.measurements & 0xffff);
    put16(out + 14, d.measurements >> 16);
    uint8_t *p = out + 16;
    for (int a = 0; a < d.antennas; a++)
    {
        for (int k = 0; k < d.taps; k++)
        {
            *p++ = (uint8_t)steps(d.movement[a][k], 0.1f, 255);
        }
    }
    return size;
}

bool radar_data_unpack(const uint8_t *p, size_t size, radar_data *out, uint16_t *seq)
{
    if (size < 16 || p[0] != 'D' || p[1] != '1')
    {
        return false;
    }
    int antennas = p[5], taps = p[6];
    if (antennas > radar_detector::MAX_ANTENNAS || taps > radar_data::MAX_TAPS || size < 16 + (size_t)antennas * taps)
    {
        return false;
    }
    if (seq != nullptr)
    {
        *seq = (uint16_t)get16(p + 2);
    }
    *out = radar_data();
    out->finding.learning = p[4] & 1;
    out->finding.moving = p[4] & 2;
    out->finding.distance_m = get16(p + 8) * 0.01f;
    out->finding.strength_db = get16(p + 10) * 0.1f;
    out->measurements = get16(p + 12) | (uint32_t)get16(p + 14) << 16;
    out->antennas = antennas;
    out->taps = taps;
    const uint8_t *m = p + 16;
    for (int a = 0; a < antennas; a++)
    {
        for (int k = 0; k < taps; k++)
        {
            out->movement[a][k] = *m++ * 0.1f;
        }
    }
    return true;
}
