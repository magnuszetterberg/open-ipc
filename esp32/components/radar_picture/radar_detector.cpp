// What the radar finds (core layer, R4). See radar_picture.hpp. The same steps as pc/radar_view.py.
#include <math.h>
#include <string.h>

#include <algorithm>

#include "radar_picture.hpp"

static const float BACKGROUND = 0.02f;     // how fast the background follows what the radar sees
static const float REFLECTION_DB = 12.0f;  // a tap counts this far above the noise floor: beyond the
                                           // reflections the noise alone swings by several dB
static const float NEAR_M = 0.3f;          // closer is the radar's own direct signal, not an object
static const int LEARN = 20;               // measurements before anything counts
static const int ZERO_AT = 50;             // measurements before distance 0 is taken from the background

void radar_detector::restart(int a, int t)
{
    antennas = a;
    taps = t;
    seen = 0;
    zero_tap = 0;
    finding = radar_finding();
    memset(movement, 0, sizeof(movement));
    memset(recent, 0, sizeof(recent));
}

bool radar_detector::add_r1(const uint8_t *p, size_t size)
{
    // 'R' '1', sequence, number of CIRs, taps per CIR, start offset, then per CIR: antenna, half-dB per tap
    if (size < 8 || p[0] != 'R' || p[1] != '1')
    {
        return false;
    }
    int a = p[4], t = p[5];
    if (a < 1 || a > MAX_ANTENNAS || t < 8 || t > MAX_TAPS || size < 8 + (size_t)a * (1 + t))
    {
        return false;
    }
    if (a != antennas || t != taps)
    {
        restart(a, t);
    }
    float strength[MAX_ANTENNAS][MAX_TAPS];
    for (int i = 0; i < a; i++)
    {
        const uint8_t *row = p + 8 + i * (1 + t) + 1;
        for (int k = 0; k < t; k++)
        {
            strength[i][k] = row[k] / 2.0f;
        }
    }
    measurements += 1;

    if (seen == 0)
    {
        memcpy(background, strength, sizeof(background));
    }
    float *change = &recent[seen % RECENT][0][0];
    for (int i = 0; i < a; i++)
    {
        for (int k = 0; k < t; k++)
        {
            change[i * MAX_TAPS + k] = fabsf(strength[i][k] - background[i][k]);
            background[i][k] += BACKGROUND * (strength[i][k] - background[i][k]);
        }
    }
    seen += 1;

    // The radar's own direct signal is the strongest still peak: distance 0
    if (seen == 1 || seen == ZERO_AT)
    {
        float best = -1;
        for (int k = 0; k < t; k++)
        {
            float sum = 0;
            for (int i = 0; i < a; i++)
            {
                sum += background[i][k];
            }
            if (sum > best)
            {
                best = sum;
                zero_tap = k;
            }
        }
    }

    // Averaged over the last few measurements, and only where there is a reflection: the noise floor is the
    // median level over all distances, per antenna
    int have = std::min(seen, RECENT);
    for (int i = 0; i < a; i++)
    {
        float sorted[MAX_TAPS];
        memcpy(sorted, background[i], t * sizeof(float));
        std::nth_element(sorted, sorted + t / 2, sorted + t);
        float floor = sorted[t / 2];
        for (int k = 0; k < t; k++)
        {
            float sum = 0;
            for (int r = 0; r < have; r++)
            {
                sum += recent[r][i][k];
            }
            movement[i][k] = strength[i][k] >= floor + REFLECTION_DB ? sum / have : 0;
        }
    }

    // The strongest movement within range, over the antennas and three taps
    float combined[MAX_TAPS];
    for (int k = 0; k < t; k++)
    {
        float sum = 0;
        for (int i = 0; i < a; i++)
        {
            sum += movement[i][k];
        }
        combined[k] = sum / a;
    }
    int best_tap = -1;
    float best = 0;
    for (int k = 0; k < t; k++)
    {
        float m = metres(k);
        if (m < NEAR_M || m > MAX_M)
        {
            continue;
        }
        float v = (combined[k] + (k > 0 ? combined[k - 1] : combined[k]) + (k + 1 < t ? combined[k + 1] : combined[k])) / 3;
        if (best_tap < 0 || v > best)
        {
            best = v;
            best_tap = k;
        }
    }

    finding.learning = seen <= LEARN;
    bool moving = !finding.learning && best_tap >= 0 && best >= THRESHOLD_DB;
    if (moving)
    {
        float d = metres(best_tap);
        finding.distance_m = finding.moving ? finding.distance_m + 0.4f * (d - finding.distance_m) : d;
        finding.strength_db = best;
    }
    finding.moving = moving;
    return true;
}
