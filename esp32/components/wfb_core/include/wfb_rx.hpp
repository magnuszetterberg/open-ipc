// Copyright (C) 2017 - 2026 Vasily Evseenko <svpcom@p2ptech.org>
// Adaptation for the ESP32: Copyright (C) 2026 Magnus Zetterberg
//
// This program is free software; you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation; version 3.
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details: wfb-ng/LICENSE.txt, or <https://www.gnu.org/licenses/>.
//
// wfb-ng receive core: session decryption, packet decryption, FEC recovery (core layer, R4).
//
// Adapted from class Aggregator in wfb-ng/src/rx.hpp and rx.cpp, at submodule commit
// 59adeac09a35f416b396f06da7963dedc8fa3920 (R3). Kept close to the original so it diffs against it.
// Changes: returned status codes and counters instead of exceptions and log lines; the key pair is
// passed in as bytes (R9); payloads go to a wfb_payload_sink (R5). All buffers are allocated once in
// init(), sized for the largest FEC n and packet this receiver accepts, and the block ring depth is a
// setting (wfb_rx uses 40; on the ESP32 that would not fit). Only the FEC codec itself is created when
// a session with new k/n arrives, as in wfb_rx. Not carried over: per-antenna RSSI statistics, the
// forwarder deduplication records and the unique-packet set, which this link doesn't use.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wfb_core.hpp"
#include "zfex.h"

struct wfb_rx_config
{
    uint64_t epoch = 0;          // sessions from older epochs are dropped
    uint32_t channel_id = 0;     // (link_id << 8) + radio_port
    uint8_t max_fec_n = 12;      // the largest n a session may use; buffers are sized for it
    int ring_size = 8;           // unfinished FEC blocks kept at once (wfb_rx: 40)
    size_t max_packet = WFB_MAX_PACKET;
};

// Counters, as wfb_rx keeps them (its "PKT" stats line).
struct wfb_rx_stats
{
    uint32_t count_p_all = 0;           // packets in
    uint32_t count_b_all = 0;           // bytes in
    uint32_t count_p_dec_err = 0;       // packets that didn't decrypt, or sessions not for us
    uint32_t count_p_session = 0;       // session packets accepted
    uint32_t count_p_data = 0;          // data packets decrypted
    uint32_t count_p_fec_recovered = 0; // data packets rebuilt by FEC
    uint32_t count_p_lost = 0;          // data packets neither received nor recovered
    uint32_t count_p_bad = 0;           // malformed packets
    uint32_t count_p_override = 0;      // unfinished blocks pushed out of a full ring
    uint32_t count_p_outgoing = 0;      // payloads delivered
    uint32_t count_b_outgoing = 0;      // payload bytes delivered
};

class wfb_rx
{
public:
    explicit wfb_rx(wfb_payload_sink &sink);
    ~wfb_rx();

    // keypair: the 64 bytes of a gs.key file (rx secret key, then tx public key).
    wfb_status init(const uint8_t *keypair, size_t keypair_size, const wfb_rx_config &config);

    // One wfb-ng packet, without its 802.11 header. Payloads come out through the sink, in order.
    void process_packet(const uint8_t *buf, size_t size);

    wfb_rx_stats stats;

private:
    wfb_rx(const wfb_rx &);
    wfb_rx &operator=(const wfb_rx &);

    struct rx_ring_item_t
    {
        uint64_t block_idx;
        uint8_t **fragments;
        size_t *fragment_map;
        uint8_t fragment_to_send_idx;
        uint8_t has_fragments;
    };

    wfb_status init_fec(int k, int n);
    void deinit_fec(void);
    void send_packet(int ring_idx, int fragment_idx);
    void apply_fec(int ring_idx);
    int get_block_ring_idx(uint64_t block_idx);
    int rx_ring_push(void);

    wfb_payload_sink &sink;
    wfb_rx_config config;
    size_t max_fec_payload;
    size_t max_payload_size;

    fec_t *fec_p;
    int fec_k;  // RS number of primary fragments in block
    int fec_n;  // RS total number of fragments in block
    uint8_t session_hash[32];

    uint64_t seq;
    rx_ring_item_t *rx_ring;
    int rx_ring_front; // current packet
    int rx_ring_alloc; // number of allocated entries
    uint64_t last_known_block;  //id of last known block
    uint64_t epoch; // current epoch

    // rx->tx keypair
    uint8_t rx_secretkey[32];
    uint8_t tx_publickey[32];
    uint8_t session_key[32];

    // Scratch space: on the stack in rx.cpp, too big for an ESP32 task stack
    uint8_t *decrypted;
    uint8_t *session_tmp;
    unsigned *fec_index;
    uint8_t **fec_in_blocks;
    uint8_t **fec_out_blocks;
};
