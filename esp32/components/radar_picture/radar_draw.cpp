// The radar picture (core layer, R4). See radar_picture.hpp. Laid out as pc/radar_view.py's page: the
// finding, movement against distance per antenna, and the distance of what moves over the last 20 s.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "radar_font.hpp"
#include "radar_picture.hpp"

namespace
{
const int W = radar_picture::WIDTH, H = radar_picture::HEIGHT;

struct ycc
{
    uint8_t y, cb, cr;
};

constexpr ycc rgb(int r, int g, int b)
{
    // JFIF's full-range BT.601
    return {(uint8_t)(0.299 * r + 0.587 * g + 0.114 * b + 0.5),
            (uint8_t)(128 - 0.168736 * r - 0.331264 * g + 0.5 * b + 0.5),
            (uint8_t)(128 + 0.5 * r - 0.418688 * g - 0.081312 * b + 0.5)};
}

const ycc BACKGROUND = rgb(16, 19, 24);
const ycc PANEL = rgb(24, 28, 35);
const ycc GRID = rgb(48, 54, 64);
const ycc DIM = rgb(138, 145, 156);
const ycc TEXT = rgb(232, 234, 237);
const ycc MARK = rgb(255, 255, 255);
const ycc ANTENNA[radar_detector::MAX_ANTENNAS] = {rgb(255, 160, 40), rgb(60, 200, 255), rgb(160, 255, 120)};

// The two panels, and the distance shown across the graph and up the trace
const int L = 30, R = W - 8;
const int G_TOP = 34, G_BOTTOM = 128;
const int T_TOP = 162, T_BOTTOM = 222;
const float MAX_M = radar_detector::MAX_M;
const float GRAPH_DB = 6.0f;   // the graph's top: four times the threshold

struct canvas
{
    radar_picture &p;

    void dot(int x, int yy, ycc c)
    {
        if (x < 0 || x >= W || yy < 0 || yy >= H)
        {
            return;
        }
        p.y[yy * W + x] = c.y;
        p.cb[yy * (W / 2) + x / 2] = c.cb;
        p.cr[yy * (W / 2) + x / 2] = c.cr;
    }

    void blend(int x, int yy, ycc c, int alpha)
    {
        if (x < 0 || x >= W || yy < 0 || yy >= H || alpha == 0)
        {
            return;
        }
        uint8_t &py = p.y[yy * W + x];
        py = (uint8_t)((py * (255 - alpha) + c.y * alpha) / 255);
        if ((x & 1) == 0)   // chroma is shared by two pixels across: take it from the first
        {
            uint8_t &pb = p.cb[yy * (W / 2) + x / 2], &pr = p.cr[yy * (W / 2) + x / 2];
            pb = (uint8_t)((pb * (255 - alpha) + c.cb * alpha) / 255);
            pr = (uint8_t)((pr * (255 - alpha) + c.cr * alpha) / 255);
        }
    }

    void rect(int x0, int y0, int x1, int y1, ycc c)
    {
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 > W ? W : x1;
        y1 = y1 > H ? H : y1;
        if (x0 >= x1 || y0 >= y1)
        {
            return;
        }
        for (int yy = y0; yy < y1; yy++)
        {
            memset(p.y + yy * W + x0, c.y, x1 - x0);
            // chroma for every pair of pixels the rectangle touches
            memset(p.cb + yy * (W / 2) + x0 / 2, c.cb, (x1 + 1) / 2 - x0 / 2);
            memset(p.cr + yy * (W / 2) + x0 / 2, c.cr, (x1 + 1) / 2 - x0 / 2);
        }
    }

    // A line two pixels thick (Bresenham)
    void line(int x0, int y0, int x1, int y1, ycc c)
    {
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
        for (;;)
        {
            dot(x0, y0, c);
            dot(x0, y0 + 1, c);
            if (x0 == x1 && y0 == y1)
            {
                break;
            }
            int e2 = 2 * err;
            if (e2 >= dy)
            {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx)
            {
                err += dx;
                y0 += sy;
            }
        }
    }

    void text(int x, int yy, const char *s, const radar_font &f, ycc c)
    {
        for (; *s; s++, x += f.width)
        {
            unsigned ch = (unsigned char)*s;
            if (ch < f.first || ch > f.last)
            {
                continue;
            }
            const uint8_t *g = f.glyphs + (ch - f.first) * f.width * f.height;
            for (int r = 0; r < f.height; r++)
            {
                for (int k = 0; k < f.width; k++)
                {
                    blend(x + k, yy + r, c, g[r * f.width + k]);
                }
            }
        }
    }
};

int x_of(float m)
{
    if (m < 0)
    {
        m = 0;
    }
    if (m > MAX_M)
    {
        m = MAX_M;
    }
    return L + (int)((R - L - 1) * m / MAX_M);
}

int trace_y(float m)
{
    if (m > MAX_M)
    {
        m = MAX_M;
    }
    return T_BOTTOM - 2 - (int)((T_BOTTOM - T_TOP - 4) * m / MAX_M);
}
}  // namespace

void radar_picture::add(const radar_detector &d)
{
    if (history_count < HISTORY)
    {
        history[history_count++] = d.finding.moving ? d.finding.distance_m : -1;
    }
    else
    {
        memmove(history, history + 1, (HISTORY - 1) * sizeof(float));
        history[HISTORY - 1] = d.finding.moving ? d.finding.distance_m : -1;
    }
}

void radar_picture::draw(const radar_detector &d)
{
    canvas c{*this};
    c.rect(0, 0, W, H, BACKGROUND);

    // The finding
    char line[40];
    if (d.measurements == 0)
    {
        snprintf(line, sizeof(line), "waiting for the radar");
    }
    else if (d.finding.learning)
    {
        snprintf(line, sizeof(line), "learning the room...");
    }
    else if (d.finding.moving)
    {
        snprintf(line, sizeof(line), "moving at %.2f m", (double)d.finding.distance_m);
    }
    else
    {
        snprintf(line, sizeof(line), "nothing moving");
    }
    c.text(8, 6, line, radar_font_large, TEXT);

    // Movement against distance: the panel, a line per metre, the threshold dotted, a line per antenna
    c.rect(L, G_TOP, R, G_BOTTOM, PANEL);
    for (int m = 0; m <= (int)MAX_M; m++)
    {
        c.rect(x_of(m), G_TOP, x_of(m) + 1, G_BOTTOM, GRID);
        if (m % 2 == 0)
        {
            char label[8];
            snprintf(label, sizeof(label), m == (int)MAX_M ? "%dm" : "%d", m);
            int width = (int)strlen(label) * radar_font_small.width;
            c.text(std::min(x_of(m) - 3, W - width - 1), G_BOTTOM + 2, label, radar_font_small, DIM);
        }
    }
    auto graph_y = [](float db) {
        if (db > GRAPH_DB)
        {
            db = GRAPH_DB;
        }
        return G_BOTTOM - 2 - (int)((G_BOTTOM - G_TOP - 4) * db / GRAPH_DB);
    };
    int threshold_y = graph_y(radar_detector::THRESHOLD_DB);
    for (int x = L; x < R; x += 4)
    {
        c.dot(x, threshold_y, DIM);
    }
    for (int a = 0; a < d.antennas; a++)
    {
        int px = -1, py = 0;
        for (int k = 0; k < d.taps; k++)
        {
            float m = d.metres(k);
            if (m < 0 || m > MAX_M)
            {
                continue;
            }
            int x = x_of(m), yy = graph_y(d.movement[a][k]);
            if (px >= 0)
            {
                c.line(px, py, x, yy, ANTENNA[a]);
            }
            px = x;
            py = yy;
        }
    }
    if (d.finding.moving)
    {
        int x = x_of(d.finding.distance_m);
        c.rect(x - 1, G_TOP, x + 1, G_BOTTOM, MARK);
    }

    // The distance of what moves over the last 20 s: newest on the right, near at the bottom
    c.rect(L, T_TOP, R, T_BOTTOM, PANEL);
    for (int m = 0; m <= (int)MAX_M; m += 2)
    {
        c.rect(L, trace_y(m), R, trace_y(m) + 1, GRID);
    }
    c.text(2, T_TOP - 4, "8m", radar_font_small, DIM);
    c.text(2, T_BOTTOM - 12, "0m", radar_font_small, DIM);
    c.text(L, T_BOTTOM + 2, "last 20 s", radar_font_small, DIM);
    c.text(L, T_TOP - 16, "distance of what moves", radar_font_small, DIM);
    for (int k = 0; k < history_count; k++)
    {
        if (history[k] < 0)
        {
            continue;
        }
        int x = L + (R - L - 2) * (HISTORY - history_count + k) / (HISTORY - 1), yy = trace_y(history[k]);
        c.rect(x, yy - 1, x + 2, yy + 1, MARK);
    }
}
