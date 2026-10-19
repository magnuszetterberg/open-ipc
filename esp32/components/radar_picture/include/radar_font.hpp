// The radar picture's text font: printable ASCII as 8-bit coverage per pixel, one fixed-size cell per
// character (radar_font.cpp, made by tools/make_font.py).
#pragma once

#include <stdint.h>

struct radar_font
{
    uint8_t width, height;     // cell, in pixels; the advance from one character to the next is the width
    uint8_t first, last;       // the characters it holds
    const uint8_t *glyphs;     // per character, height rows of width coverage values (0..255)
};

extern const radar_font radar_font_small;  // 12 px, regular: labels
extern const radar_font radar_font_large;  // 20 px, bold: the readout
