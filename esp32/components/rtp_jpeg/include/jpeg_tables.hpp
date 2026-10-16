// The JPEG standard's Huffman tables (Annex K.3), as DHT segment bodies: class and id, 16 code counts, the
// symbols. Shared by rtp_jpeg's headers and radar_picture's encoder; see jpeg_tables.cpp.
#pragma once
#include <stdint.h>

extern const uint8_t jpeg_dht_luma_dc[29];
extern const uint8_t jpeg_dht_luma_ac[179];
extern const uint8_t jpeg_dht_chroma_dc[29];
extern const uint8_t jpeg_dht_chroma_ac[179];
