// Copyright (C) 2017 - 2026 Vasily Evseenko <svpcom@p2ptech.org>
// Adaptation for the ESP32: Copyright (C) 2026 Magnus Zetterberg
//
// This program is free software; you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation; version 3.
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details: wfb-ng/LICENSE.txt, or <https://www.gnu.org/licenses/>.
//
// Adapted from wfb-ng/src/tx.cpp (class Transmitter) at submodule commit
// 59adeac09a35f416b396f06da7963dedc8fa3920 (R3). See wfb_tx.hpp for what changed and why.
#include "wfb_tx.hpp"

#include <stdlib.h>
#include <string.h>

#include "wifibroadcast.hpp"

wfb_tx::wfb_tx(wfb_sink &sink) : \
    sink(sink), config(), next_announce_us(0),
    fec_p(NULL), block_idx(0), fragment_idx(0), block(NULL), ciphertext(NULL),
    max_fec_payload(0), max_packet_size(0),
    tx_secretkey{}, rx_publickey{}, session_key{},
    session_packet(NULL), session_packet_size(0)
{
    static_assert(sizeof(tx_secretkey) == crypto_box_SECRETKEYBYTES, "drone.key layout");
    static_assert(sizeof(rx_publickey) == crypto_box_PUBLICKEYBYTES, "drone.key layout");
    static_assert(sizeof(session_key) == crypto_aead_chacha20poly1305_KEYBYTES, "session key size");
}

wfb_tx::~wfb_tx()
{
    if (fec_p != NULL)
    {
        deinit_session();
    }
    free(ciphertext);
    free(session_packet);
    sodium_memzero(tx_secretkey, sizeof(tx_secretkey));
    sodium_memzero(session_key, sizeof(session_key));
}

wfb_status wfb_tx::init(const uint8_t *keypair, size_t keypair_size, const wfb_tx_config &cfg)
{
    if (fec_p != NULL || ciphertext != NULL)
    {
        return WFB_ERR_CONFIG;  // init() runs once
    }
    if (keypair_size != sizeof(tx_secretkey) + sizeof(rx_publickey))
    {
        return WFB_ERR_CONFIG;
    }
    if (cfg.fec_k < 1 || cfg.fec_n < cfg.fec_k)
    {
        return WFB_ERR_CONFIG;
    }
    if (cfg.max_packet <= sizeof(wblock_hdr_t) + crypto_aead_chacha20poly1305_ABYTES + sizeof(wpacket_hdr_t) ||
        cfg.max_packet > MAX_FORWARDER_PACKET_SIZE)
    {
        return WFB_ERR_CONFIG;
    }
    if (sodium_init() < 0)
    {
        return WFB_ERR_CRYPTO;
    }

    config = cfg;
    max_fec_payload = config.max_packet - sizeof(wblock_hdr_t) - crypto_aead_chacha20poly1305_ABYTES;
    config.max_payload = max_fec_payload - sizeof(wpacket_hdr_t);

    memcpy(tx_secretkey, keypair, sizeof(tx_secretkey));
    memcpy(rx_publickey, keypair + sizeof(tx_secretkey), sizeof(rx_publickey));

    ciphertext = (uint8_t *)malloc(config.max_packet);
    session_packet = (uint8_t *)malloc(sizeof(wsession_hdr_t) + sizeof(wsession_data_t) + crypto_box_MACBYTES);
    if (ciphertext == NULL || session_packet == NULL)
    {
        return WFB_ERR_NO_MEMORY;
    }
    next_announce_us = 0;  // the first payload announces the session, as in wfb_tx
    return init_session();
}

void wfb_tx::deinit_session(void)
{
    if (block != NULL)
    {
        for(int i=0; i < config.fec_n; i++)
        {
            free(block[i]);
        }
    }

    free(block);

    fec_free(fec_p);

    block = NULL;
    fec_p = NULL;
}

wfb_status wfb_tx::init_session(void)
{
    if (fec_p != NULL)
    {
        deinit_session();
    }

    if (fec_new(config.fec_k, config.fec_n, &fec_p) != ZFEX_SC_OK)
    {
        return WFB_ERR_NO_MEMORY;
    }

    // Each buffer is about 1.5 KB, under ESP-IDF's 16 KB threshold for putting malloc() in PSRAM,
    // so on the ESP32 these stay in internal RAM, where FEC runs fastest.
    block = (uint8_t**)calloc(config.fec_n, sizeof(*block));  // calloc, not new: no exceptions to report failure
    if (block == NULL)
    {
        return WFB_ERR_NO_MEMORY;
    }
    for(int i=0; i < config.fec_n; i++)
    {
        if (posix_memalign((void**)&block[i], ZFEX_SIMD_ALIGNMENT, ZFEX_ROUND_UP_SIMD(max_fec_payload)) != 0)
        {
            return WFB_ERR_NO_MEMORY;
        }
        memset(block[i], '\0', ZFEX_ROUND_UP_SIMD(max_fec_payload));
    }

    block_idx = 0;
    fragment_idx = 0;
    max_packet_size = 0;

    // init session key
    randombytes_buf(session_key, sizeof(session_key));

    // fill packet header
    wsession_hdr_t *session_hdr = (wsession_hdr_t *)session_packet;
    session_hdr->packet_type = WFB_PACKET_SESSION;

    randombytes_buf(session_hdr->session_nonce, sizeof(session_hdr->session_nonce));

    // fill packet contents (no optional tags)
    wsession_data_t session_data;
    session_data.epoch = htobe64(config.epoch);
    session_data.channel_id = htobe32(config.channel_id);
    session_data.fec_type = WFB_FEC_VDM_RS;
    session_data.k = config.fec_k;
    session_data.n = config.fec_n;
    memcpy(session_data.session_key, session_key, sizeof(session_key));

    if (crypto_box_easy(session_packet + sizeof(wsession_hdr_t),
                        (uint8_t*)&session_data, sizeof(session_data),
                        session_hdr->session_nonce, rx_publickey, tx_secretkey) != 0)
    {
        sodium_memzero(&session_data, sizeof(session_data));
        return WFB_ERR_CRYPTO;
    }
    sodium_memzero(&session_data, sizeof(session_data));

    session_packet_size = sizeof(wsession_hdr_t) + sizeof(wsession_data_t) + crypto_box_MACBYTES;
    return WFB_OK;
}

wfb_status wfb_tx::send_block_fragment(size_t packet_size)
{
    wblock_hdr_t *block_hdr = (wblock_hdr_t*)ciphertext;
    long long unsigned int ciphertext_len;

    block_hdr->packet_type = WFB_PACKET_DATA;
    block_hdr->data_nonce = htobe64(((block_idx & BLOCK_IDX_MASK) << 8) + fragment_idx);

    // encrypted payload
    if (crypto_aead_chacha20poly1305_encrypt(ciphertext + sizeof(wblock_hdr_t), &ciphertext_len,
                                             block[fragment_idx], packet_size,
                                             (uint8_t*)block_hdr, sizeof(wblock_hdr_t),
                                             NULL, (uint8_t*)(&(block_hdr->data_nonce)), session_key) < 0)
    {
        return WFB_ERR_CRYPTO;
    }

    sink.send_frame(ciphertext, sizeof(wblock_hdr_t) + ciphertext_len);
    return WFB_OK;
}

wfb_status wfb_tx::send_session_key(void)
{
    if (fec_p == NULL)
    {
        return WFB_ERR_NOT_READY;
    }
    sink.send_frame(session_packet, session_packet_size);
    return WFB_OK;
}

wfb_status wfb_tx::send(const uint8_t *buf, size_t size, uint64_t now_us)
{
    if (fec_p == NULL)
    {
        return WFB_ERR_NOT_READY;
    }
    if (size > config.max_payload)
    {
        return WFB_ERR_TOO_BIG;
    }

    if (now_us >= next_announce_us)
    {
        // Session packet interval is not in fixed grid because
        // we yield session packets only if there are data packets
        send_session_key();
        next_announce_us = now_us + SESSION_KEY_ANNOUNCE_MSEC * 1000ULL;
    }

    bool sent;
    return send_packet(buf, size, 0, &sent);
}

wfb_status wfb_tx::close_block(void)
{
    if (fec_p == NULL)
    {
        return WFB_ERR_NOT_READY;
    }

    // FEC-only packets until the block is complete; send_packet refuses them once it is
    bool sent = true;
    while (sent)
    {
        wfb_status rc = send_packet(NULL, 0, WFB_PACKET_FEC_ONLY, &sent);
        if (rc != WFB_OK)
        {
            return rc;
        }
    }
    return WFB_OK;
}

wfb_status wfb_tx::send_packet(const uint8_t *buf, size_t size, uint8_t flags, bool *sent)
{
    *sent = false;

    // FEC-only packets are only for closing already opened blocks
    if (fragment_idx == 0 && (flags & WFB_PACKET_FEC_ONLY))
    {
        return WFB_OK;
    }

    wpacket_hdr_t *packet_hdr = (wpacket_hdr_t*)block[fragment_idx];

    packet_hdr->flags = flags;
    packet_hdr->packet_size = htobe16(size);

    if(size > 0)
    {
        memcpy(block[fragment_idx] + sizeof(wpacket_hdr_t), buf, size);
    }

    memset(block[fragment_idx] + sizeof(wpacket_hdr_t) + size, '\0', max_fec_payload - (sizeof(wpacket_hdr_t) + size));

    wfb_status rc = send_block_fragment(sizeof(wpacket_hdr_t) + size);
    if (rc != WFB_OK)
    {
        return rc;
    }
    *sent = true;
    max_packet_size = max_packet_size > sizeof(wpacket_hdr_t) + size ? max_packet_size : sizeof(wpacket_hdr_t) + size;
    fragment_idx += 1;

    if (fragment_idx < config.fec_k)  return WFB_OK;

    if (fec_encode_simd(fec_p, (const uint8_t**)block, block + config.fec_k, ZFEX_ROUND_UP_SIMD(max_packet_size)) != ZFEX_SC_OK)
    {
        return WFB_ERR_CONFIG;
    }

    while (fragment_idx < config.fec_n)
    {
        rc = send_block_fragment(max_packet_size);
        if (rc != WFB_OK)
        {
            return rc;
        }
        fragment_idx += 1;
    }
    block_idx += 1;
    fragment_idx = 0;
    max_packet_size = 0;

    // Generate new session key after MAX_BLOCK_IDX blocks
    if (block_idx > MAX_BLOCK_IDX)
    {
        rc = init_session();
        if (rc != WFB_OK)
        {
            return rc;
        }
        for(int i = 0; i < config.fec_n - config.fec_k + 1; i++)
        {
            send_session_key();
        }
    }

    return WFB_OK;
}
