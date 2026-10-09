// The JPEG standard's Huffman tables (Annex K.3); see jpeg_tables.cpp.
#pragma once
#include <stdint.h>

extern const uint8_t jpeg_dht_luma_dc[29];
extern const uint8_t jpeg_dht_luma_ac[179];
extern const uint8_t jpeg_dht_chroma_dc[29];
extern const uint8_t jpeg_dht_chroma_ac[179];
