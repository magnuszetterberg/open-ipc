// Copyright (C) 2017 - 2026 Vasily Evseenko <svpcom@p2ptech.org>
// Adaptation for the ESP32: Copyright (C) 2026 Magnus Zetterberg
//
// This program is free software; you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation; version 3.
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details: wfb-ng/LICENSE.txt, or <https://www.gnu.org/licenses/>.
//
// The 802.11 header wfb-ng puts in front of each packet on air (core layer, R4).
//
// Adapted from RawSocketTransmitter::inject_packet in wfb-ng/src/tx.cpp, at submodule commit
// 59adeac09a35f416b396f06da7963dedc8fa3920 (R3), minus the radiotap header: the ESP32 sets the rate
// with esp_wifi_config_80211_tx_rate() instead. wfb_rx keeps only frames whose second address is
// 57:42 followed by channel_id, so these bytes are what decides whether the NUC hears the ESP32.
#pragma once

#include <stddef.h>
#include <stdint.h>

static const size_t WFB_80211_HEADER_SIZE = 24;

class wfb_80211_framer
{
public:
    explicit wfb_80211_framer(uint32_t channel_id);

    // Writes the header and the packet into out. Returns the frame size, or 0 if it doesn't fit.
    size_t frame(uint8_t *out, size_t out_size, const uint8_t *packet, size_t packet_size);

private:
    uint8_t header[WFB_80211_HEADER_SIZE];
    uint16_t ieee80211_seq;
};
