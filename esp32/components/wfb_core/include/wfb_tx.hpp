// Copyright (C) 2017 - 2026 Vasily Evseenko <svpcom@p2ptech.org>
// Adaptation for the ESP32: Copyright (C) 2026 Magnus Zetterberg
//
// This program is free software; you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation; version 3.
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details: wfb-ng/LICENSE.txt, or <https://www.gnu.org/licenses/>.
//
// wfb-ng transmit core: session key, packet assembly, FEC and encryption (core layer, R4).
//
// Adapted from class Transmitter in wfb-ng/src/tx.hpp and tx.cpp, at submodule commit
// 59adeac09a35f416b396f06da7963dedc8fa3920 (R3). Kept close to the original so it diffs against it.
// Changes: returned status codes instead of exceptions; buffers allocated once in init(); the key
// pair is passed in as bytes instead of read from a file (R9); the caller passes the time (it decides
// when the session key is announced); finished packets go to a wfb_sink (R5). Not carried over:
// session tags, FEC packet delay, qdisc marks and output selection, which this link doesn't use.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "wfb_core.hpp"
#include "zfex.h"

class wfb_tx
{
public:
    explicit wfb_tx(wfb_sink &sink);
    ~wfb_tx();

    // keypair: the 64 bytes of a drone.key file (tx secret key, then rx public key).
    // Allocates everything the transmitter needs; nothing is allocated after this (Coding conventions).
    wfb_status init(const uint8_t *keypair, size_t keypair_size, const wfb_tx_config &config);

    // Send one payload (at most config.max_payload bytes). Announces the session key first when it is
    // due, every SESSION_KEY_ANNOUNCE_MSEC as in wfb_tx. now_us: a monotonic clock in microseconds.
    wfb_status send(const uint8_t *buf, size_t size, uint64_t now_us);

    // Finish a half-full FEC block with empty packets, so the receiver can recover it without waiting
    // for more data (what wfb_tx does on its FEC timeout). Does nothing if no block is open.
    wfb_status close_block(void);

    // Announce the session key now, outside the schedule.
    wfb_status send_session_key(void);

    size_t max_payload(void) const { return config.max_payload; }

private:
    wfb_tx(const wfb_tx &);
    wfb_tx &operator=(const wfb_tx &);

    wfb_status send_packet(const uint8_t *buf, size_t size, uint8_t flags, bool *sent);
    wfb_status send_block_fragment(size_t packet_size);
    wfb_status init_session(void);
    void deinit_session(void);

    wfb_sink &sink;
    wfb_tx_config config;
    uint64_t next_announce_us;

    fec_t *fec_p;
    uint64_t block_idx;    // (block_idx << 8) + fragment_idx = nonce (64bit)
    uint8_t fragment_idx;
    uint8_t **block;       // fec_n buffers of max_fec_payload bytes
    uint8_t *ciphertext;   // one encrypted packet; on the stack in tx.cpp, too big for an ESP32 task stack
    size_t max_fec_payload;
    size_t max_packet_size;

    // tx->rx keypair
    uint8_t tx_secretkey[32];
    uint8_t rx_publickey[32];
    uint8_t session_key[32];
    uint8_t *session_packet;
    uint16_t session_packet_size;
};
