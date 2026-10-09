// Copyright (C) 2017 - 2026 Vasily Evseenko <svpcom@p2ptech.org>
// Adaptation for the ESP32: Copyright (C) 2026 Magnus Zetterberg
//
// This program is free software; you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation; version 3.
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details: wfb-ng/LICENSE.txt, or <https://www.gnu.org/licenses/>.
//
// Adapted from RawSocketTransmitter::inject_packet in wfb-ng/src/tx.cpp and the capture filter and
// Receiver::loop_iter in rx.cpp at submodule commit
// 59adeac09a35f416b396f06da7963dedc8fa3920 (R3). See wfb_80211.hpp.
#include "wfb_80211.hpp"

#include <string.h>

#include "wifibroadcast.hpp"

static_assert(sizeof(ieee80211_header) == WFB_80211_HEADER_SIZE, "802.11 header size");

wfb_80211_framer::wfb_80211_framer(uint32_t channel_id) : ieee80211_seq(0)
{
    // fill default values
    memcpy(header, ieee80211_header, sizeof(ieee80211_header));

    // frame_type
    header[0] = FRAME_TYPE_DATA;

    // channel_id
    uint32_t channel_id_be = htobe32(channel_id);
    memcpy(header + SRC_MAC_THIRD_BYTE, &channel_id_be, sizeof(uint32_t));
    memcpy(header + DST_MAC_THIRD_BYTE, &channel_id_be, sizeof(uint32_t));
}

size_t wfb_80211_framer::frame(uint8_t *out, size_t out_size, const uint8_t *packet, size_t packet_size)
{
    if (sizeof(header) + packet_size > out_size)
    {
        return 0;
    }

    memcpy(out, header, sizeof(header));

    // sequence number
    out[FRAME_SEQ_LB] = ieee80211_seq & 0xff;
    out[FRAME_SEQ_HB] = (ieee80211_seq >> 8) & 0xff;
    ieee80211_seq += 16;

    memcpy(out + sizeof(header), packet, packet_size);
    return sizeof(header) + packet_size;
}

bool wfb_80211_unframe(const uint8_t *frame, size_t frame_size, uint32_t *channel_id, const uint8_t **packet,
                       size_t *packet_size)
{
    if (frame_size <= sizeof(ieee80211_header))
    {
        return false;  // "Short packet (ieee header)" in rx.cpp
    }
    // "ether[0x0a:2]==0x5742": the second address starts with W:B
    if (frame[0] != FRAME_TYPE_DATA || frame[SRC_MAC_THIRD_BYTE - 2] != 0x57 || frame[SRC_MAC_THIRD_BYTE - 1] != 0x42)
    {
        return false;
    }
    uint32_t channel_id_be;
    memcpy(&channel_id_be, frame + SRC_MAC_THIRD_BYTE, sizeof(channel_id_be));
    *channel_id = be32toh(channel_id_be);
    *packet = frame + sizeof(ieee80211_header);
    *packet_size = frame_size - sizeof(ieee80211_header);
    return true;
}
