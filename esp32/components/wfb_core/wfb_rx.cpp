// Copyright (C) 2017 - 2026 Vasily Evseenko <svpcom@p2ptech.org>
// Adaptation for the ESP32: Copyright (C) 2026 Magnus Zetterberg
//
// This program is free software; you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation; version 3.
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details: wfb-ng/LICENSE.txt, or <https://www.gnu.org/licenses/>.
//
// Adapted from wfb-ng/src/rx.cpp (class Aggregator) at submodule commit
// 59adeac09a35f416b396f06da7963dedc8fa3920 (R3). See wfb_rx.hpp for what changed and why.
#include "wfb_rx.hpp"

#include <stdlib.h>
#include <string.h>

#include "wifibroadcast.hpp"

static inline int modN(int x, int base)
{
    return (base + (x % base)) % base;
}

wfb_rx::wfb_rx(wfb_payload_sink &sink) : \
    sink(sink), config(), max_fec_payload(0), max_payload_size(0),
    fec_p(NULL), fec_k(-1), fec_n(-1), session_hash{}, seq(0), rx_ring(NULL), rx_ring_front(0), rx_ring_alloc(0),
    last_known_block((uint64_t)-1), epoch(0), rx_secretkey{}, tx_publickey{}, session_key{},
    decrypted(NULL), session_tmp(NULL), fec_index(NULL), fec_in_blocks(NULL), fec_out_blocks(NULL)
{
    static_assert(sizeof(rx_secretkey) == crypto_box_SECRETKEYBYTES, "gs.key layout");
    static_assert(sizeof(tx_publickey) == crypto_box_PUBLICKEYBYTES, "gs.key layout");
    static_assert(sizeof(session_key) == crypto_aead_chacha20poly1305_KEYBYTES, "session key size");
    static_assert(sizeof(session_hash) == crypto_generichash_BYTES, "session hash size");
}

wfb_rx::~wfb_rx()
{
    if (fec_p != NULL)
    {
        deinit_fec();
    }
    if (rx_ring != NULL)
    {
        for(int ring_idx = 0; ring_idx < config.ring_size; ring_idx++)
        {
            if (rx_ring[ring_idx].fragments != NULL)
            {
                for(int i=0; i < config.max_fec_n; i++)
                {
                    free(rx_ring[ring_idx].fragments[i]);
                }
            }
            free(rx_ring[ring_idx].fragments);
            free(rx_ring[ring_idx].fragment_map);
        }
        free(rx_ring);
    }
    free(decrypted);
    free(session_tmp);
    free(fec_index);
    free(fec_in_blocks);
    free(fec_out_blocks);
    sodium_memzero(rx_secretkey, sizeof(rx_secretkey));
    sodium_memzero(session_key, sizeof(session_key));
}

wfb_status wfb_rx::init(const uint8_t *keypair, size_t keypair_size, const wfb_rx_config &cfg)
{
    if (rx_ring != NULL)
    {
        return WFB_ERR_CONFIG;  // init() runs once
    }
    if (keypair_size != sizeof(rx_secretkey) + sizeof(tx_publickey))
    {
        return WFB_ERR_CONFIG;
    }
    if (cfg.max_fec_n < 1 || cfg.ring_size < 1)
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
    epoch = config.epoch;
    max_fec_payload = config.max_packet - sizeof(wblock_hdr_t) - crypto_aead_chacha20poly1305_ABYTES;
    max_payload_size = max_fec_payload - sizeof(wpacket_hdr_t);

    memcpy(rx_secretkey, keypair, sizeof(rx_secretkey));
    memcpy(tx_publickey, keypair + sizeof(rx_secretkey), sizeof(tx_publickey));

    decrypted = (uint8_t *)malloc(max_fec_payload);
    session_tmp = (uint8_t *)malloc(config.max_packet);
    fec_index = (unsigned *)calloc(config.max_fec_n, sizeof(*fec_index));
    fec_in_blocks = (uint8_t **)calloc(config.max_fec_n, sizeof(*fec_in_blocks));
    fec_out_blocks = (uint8_t **)calloc(config.max_fec_n, sizeof(*fec_out_blocks));
    rx_ring = (rx_ring_item_t *)calloc(config.ring_size, sizeof(*rx_ring));
    if (decrypted == NULL || session_tmp == NULL || fec_index == NULL || fec_in_blocks == NULL ||
        fec_out_blocks == NULL || rx_ring == NULL)
    {
        return WFB_ERR_NO_MEMORY;
    }

    // ring_size * max_fec_n buffers of about 1.5 KB each: the receiver's main memory cost
    for(int ring_idx = 0; ring_idx < config.ring_size; ring_idx++)
    {
        rx_ring[ring_idx].fragments = (uint8_t **)calloc(config.max_fec_n, sizeof(uint8_t *));
        rx_ring[ring_idx].fragment_map = (size_t *)calloc(config.max_fec_n, sizeof(size_t));
        if (rx_ring[ring_idx].fragments == NULL || rx_ring[ring_idx].fragment_map == NULL)
        {
            return WFB_ERR_NO_MEMORY;
        }
        for(int i=0; i < config.max_fec_n; i++)
        {
            if (posix_memalign((void**)&rx_ring[ring_idx].fragments[i], ZFEX_SIMD_ALIGNMENT, ZFEX_ROUND_UP_SIMD(max_fec_payload)) != 0)
            {
                return WFB_ERR_NO_MEMORY;
            }
        }
    }
    return WFB_OK;
}

wfb_status wfb_rx::init_fec(int k, int n)
{
    if (fec_new(k, n, &fec_p) != ZFEX_SC_OK)
    {
        fec_p = NULL;
        return WFB_ERR_NO_MEMORY;
    }

    fec_k = k;
    fec_n = n;

    rx_ring_front = 0;
    rx_ring_alloc = 0;
    last_known_block = (uint64_t)-1;
    seq = 0;

    for(int ring_idx = 0; ring_idx < config.ring_size; ring_idx++)
    {
        rx_ring[ring_idx].block_idx = 0;
        rx_ring[ring_idx].fragment_to_send_idx = 0;
        rx_ring[ring_idx].has_fragments = 0;
        memset(rx_ring[ring_idx].fragment_map, '\0', config.max_fec_n * sizeof(size_t));
    }
    return WFB_OK;
}

void wfb_rx::deinit_fec(void)
{
    fec_free(fec_p);
    fec_p = NULL;
    fec_k = -1;
    fec_n = -1;
}

int wfb_rx::rx_ring_push(void)
{
    if(rx_ring_alloc < config.ring_size)
    {
        int idx = modN(rx_ring_front + rx_ring_alloc, config.ring_size);
        rx_ring_alloc += 1;
        return idx;
    }

    /*
      Ring overflow. This means that there are more unfinished blocks than ring size
      Possible solutions:
      1. Increase ring size. Do this if you have large variance of packet travel time throught WiFi card or network stack.
         Some cards can do this due to packet reordering inside, diffent chipset and/or firmware or your RX hosts have different CPU power.
      2. Reduce packet injection speed or try to unify RX hardware.
    */

    stats.count_p_override += 1;

    for(int f_idx=rx_ring[rx_ring_front].fragment_to_send_idx; f_idx < fec_k; f_idx++)
    {
        if(rx_ring[rx_ring_front].fragment_map[f_idx])
        {
            send_packet(rx_ring_front, f_idx);
        }
    }

    // override last item in ring
    int ring_idx = rx_ring_front;
    rx_ring_front = modN(rx_ring_front + 1, config.ring_size);
    return ring_idx;
}

int wfb_rx::get_block_ring_idx(uint64_t block_idx)
{
    // check if block is already in the ring
    for(int i = rx_ring_front, c = rx_ring_alloc; c > 0; i = modN(i + 1, config.ring_size), c--)
    {
        if (rx_ring[i].block_idx == block_idx) return i;
    }

    // check if block is already known and not in the ring then it is already processed
    if (last_known_block != (uint64_t)-1 && block_idx <= last_known_block)
    {
        return -1;
    }

    uint64_t ahead = last_known_block != (uint64_t)-1 ? block_idx - last_known_block : 1;
    int new_blocks = (int)(ahead < (uint64_t)config.ring_size ? ahead : (uint64_t)config.ring_size);

    last_known_block = block_idx;
    int ring_idx = -1;

    for(int i = 0; i < new_blocks; i++)
    {
        ring_idx = rx_ring_push();
        rx_ring[ring_idx].block_idx = block_idx + i + 1 - new_blocks;
        rx_ring[ring_idx].fragment_to_send_idx = 0;
        rx_ring[ring_idx].has_fragments = 0;
        memset(rx_ring[ring_idx].fragment_map, '\0', fec_n * sizeof(size_t));
    }
    return ring_idx;
}

void wfb_rx::process_packet(const uint8_t *buf, size_t size)
{
    uint8_t new_session_hash[sizeof(session_hash)];

    wsession_data_t* new_session_data = NULL;

    if (rx_ring == NULL)
    {
        return;  // init() hasn't succeeded
    }

    stats.count_p_all += 1;
    stats.count_b_all += size;

    if(size == 0) return;

    if (size > config.max_packet)
    {
        stats.count_p_bad += 1;
        return;
    }

    switch(buf[0])
    {
    case WFB_PACKET_DATA:
        if(size < sizeof(wblock_hdr_t) + crypto_aead_chacha20poly1305_ABYTES + sizeof(wpacket_hdr_t))
        {
            stats.count_p_bad += 1;
            return;
        }

        if(fec_p == NULL)
        {
            // No session yet, session_key is still all zeros
            stats.count_p_dec_err += 1;
            return;
        }
        break;

    case WFB_PACKET_SESSION:
        new_session_data = (wsession_data_t*)session_tmp;

        if(size < sizeof(wsession_hdr_t) + sizeof(wsession_data_t) + crypto_box_MACBYTES)
        {
            stats.count_p_bad += 1;
            return;
        }

        if(crypto_generichash(new_session_hash,
                              sizeof(new_session_hash),
                              buf + sizeof(wsession_hdr_t),
                              size - sizeof(wsession_hdr_t),
                              ((wsession_hdr_t*)buf)->session_nonce,
                              sizeof(((wsession_hdr_t*)buf)->session_nonce)) != 0)
        {
            stats.count_p_bad += 1;
            return;
        }

        if (memcmp(session_hash, new_session_hash, sizeof(session_hash)) == 0)
        {
            // Session is equal to current so we can ignore it
            stats.count_p_session += 1;
            return;
        }

        if(crypto_box_open_easy((uint8_t*)session_tmp,
                                buf + sizeof(wsession_hdr_t),
                                size - sizeof(wsession_hdr_t),
                                ((wsession_hdr_t*)buf)->session_nonce,
                                tx_publickey, rx_secretkey) != 0)
        {
            stats.count_p_dec_err += 1;
            return;
        }

        if (be64toh(new_session_data->epoch) < epoch)
        {
            stats.count_p_dec_err += 1;
            return;
        }

        if (be32toh(new_session_data->channel_id) != config.channel_id)
        {
            stats.count_p_dec_err += 1;
            return;
        }

        if (new_session_data->fec_type != WFB_FEC_VDM_RS)
        {
            stats.count_p_dec_err += 1;
            return;
        }

        // n above max_fec_n: our buffers are sized for max_fec_n (wfb_rx allocates per session instead)
        if (new_session_data->n < 1 || new_session_data->n > config.max_fec_n)
        {
            stats.count_p_dec_err += 1;
            return;
        }

        if (new_session_data->k < 1 || new_session_data->k > new_session_data->n)
        {
            stats.count_p_dec_err += 1;
            return;
        }

        stats.count_p_session += 1;

        if (memcmp(session_key, new_session_data->session_key, sizeof(session_key)) != 0)
        {
            epoch = be64toh(new_session_data->epoch);
            memcpy(session_key, new_session_data->session_key, sizeof(session_key));

            if (fec_p != NULL)
            {
                deinit_fec();
            }

            if (init_fec(new_session_data->k, new_session_data->n) != WFB_OK)
            {
                sodium_memzero(session_key, sizeof(session_key));
                return;
            }
        }

        // Cache already processed session
        memcpy(session_hash, new_session_hash, sizeof(session_hash));
        sodium_memzero(session_tmp, size);

        return;

    default:
        stats.count_p_bad += 1;
        return;
    }

    unsigned long long decrypted_len;
    wblock_hdr_t *block_hdr = (wblock_hdr_t*)buf;

    if (crypto_aead_chacha20poly1305_decrypt(decrypted, &decrypted_len,
                                             NULL,
                                             buf + sizeof(wblock_hdr_t), size - sizeof(wblock_hdr_t),
                                             buf,
                                             sizeof(wblock_hdr_t),
                                             (uint8_t*)(&(block_hdr->data_nonce)), session_key) != 0)
    {
        stats.count_p_dec_err += 1;
        return;
    }

    stats.count_p_data += 1;

    if (decrypted_len < sizeof(wpacket_hdr_t) || decrypted_len > max_fec_payload)
    {
        stats.count_p_bad += 1;
        return;
    }

    uint64_t block_idx = be64toh(block_hdr->data_nonce) >> 8;
    uint8_t fragment_idx = (uint8_t)(be64toh(block_hdr->data_nonce) & 0xff);

    // Should never happend due to generating new session key on tx side
    if (block_idx > MAX_BLOCK_IDX)
    {
        stats.count_p_bad += 1;
        return;
    }

    if (fragment_idx >= fec_n)
    {
        stats.count_p_bad += 1;
        return;
    }

    int ring_idx = get_block_ring_idx(block_idx);

    //ignore already processed blocks
    if (ring_idx < 0) return;

    rx_ring_item_t *p = &rx_ring[ring_idx];

    //ignore already processed fragments
    if (p->fragment_map[fragment_idx]) return;

    memset(p->fragments[fragment_idx], '\0', max_fec_payload);
    memcpy(p->fragments[fragment_idx], decrypted, decrypted_len);

    p->fragment_map[fragment_idx] = decrypted_len;
    p->has_fragments += 1;

    // Check if we use current (oldest) block
    // then we can optimize and don't wait for all K fragments
    // and send packets if there are no gaps in fragments from the beginning of this block
    if(ring_idx == rx_ring_front)
    {
        // check if there are any packets without gaps
        // and send them immediately
        while(p->fragment_to_send_idx < fec_k && p->fragment_map[p->fragment_to_send_idx])
        {
            send_packet(ring_idx, p->fragment_to_send_idx);
            p->fragment_to_send_idx += 1;
        }

        // remove block if all K elements (without gaps) were sent
        if(p->fragment_to_send_idx == fec_k)
        {
            rx_ring_front = modN(rx_ring_front + 1, config.ring_size);
            rx_ring_alloc -= 1;
            return;
        }
    }

    // Check that this block has K elements (with gaps) and can be recovered via FEC
    if(p->fragment_to_send_idx < fec_k && p->has_fragments == fec_k)
    {
        // send all queued packets in all unfinished blocks before current
        // and then remove that blocks
        int nrm = modN(ring_idx - rx_ring_front, config.ring_size);

        while(nrm > 0)
        {
            for(int f_idx=rx_ring[rx_ring_front].fragment_to_send_idx; f_idx < fec_k; f_idx++)
            {
                if(rx_ring[rx_ring_front].fragment_map[f_idx])
                {
                    send_packet(rx_ring_front, f_idx);
                }
            }
            rx_ring_front = modN(rx_ring_front + 1, config.ring_size);
            rx_ring_alloc -= 1;
            nrm -= 1;
        }

        // Search for missed data fragments and apply FEC only if needed
        for(int f_idx=p->fragment_to_send_idx; f_idx < fec_k; f_idx++)
        {
            if(! p->fragment_map[f_idx])
            {
                uint32_t fec_count = 0;

                //Recover missed fragments using FEC
                apply_fec(ring_idx);

                // Count total number of recovered fragments
                for(; f_idx < fec_k; f_idx++)
                {
                    if(! p->fragment_map[f_idx])
                    {
                        fec_count += 1;
                    }
                }

                stats.count_p_fec_recovered += fec_count;
                break;
            }
        }

        while(p->fragment_to_send_idx < fec_k)
        {
            send_packet(ring_idx, p->fragment_to_send_idx);
            p->fragment_to_send_idx += 1;
        }

        // remove block
        rx_ring_front = modN(rx_ring_front + 1, config.ring_size);
        rx_ring_alloc -= 1;
    }
}

void wfb_rx::send_packet(int ring_idx, int fragment_idx)
{
    wpacket_hdr_t* packet_hdr = (wpacket_hdr_t*)(rx_ring[ring_idx].fragments[fragment_idx]);
    uint8_t *payload = (rx_ring[ring_idx].fragments[fragment_idx]) + sizeof(wpacket_hdr_t);
    uint8_t flags = packet_hdr->flags;
    uint16_t packet_size = be16toh(packet_hdr->packet_size);
    uint64_t packet_seq = rx_ring[ring_idx].block_idx * fec_k + fragment_idx;

    if (packet_seq > seq + 1 && seq > 0)
    {
        stats.count_p_lost += (uint32_t)(packet_seq - seq - 1);
    }

    seq = packet_seq;

    if(packet_size > max_payload_size)
    {
        stats.count_p_bad += 1;
    }
    else if(!(flags & WFB_PACKET_FEC_ONLY))
    {
        sink.send_payload(payload, packet_size);
        stats.count_p_outgoing += 1;
        stats.count_b_outgoing += packet_size;
    }
}

void wfb_rx::apply_fec(int ring_idx)
{
    unsigned *index = fec_index;
    uint8_t **in_blocks = fec_in_blocks;
    uint8_t **out_blocks = fec_out_blocks;
    int j = fec_k;
    int ob_idx = 0;
    size_t max_packet_size = 0;

    for(int i=0; i < fec_k; i++)
    {
        if(rx_ring[ring_idx].fragment_map[i])
        {
            in_blocks[i] = rx_ring[ring_idx].fragments[i];
            index[i] = i;
        }
        else
        {
            while(j < fec_n && ! rx_ring[ring_idx].fragment_map[j])
            {
                j++;
            }

            if (j >= fec_n)
            {
                return;  // an assert in rx.cpp: callers only get here with k fragments in the block
            }

            // FEC packets always have max size between packets in block
            max_packet_size = max_packet_size > rx_ring[ring_idx].fragment_map[j] ? max_packet_size : rx_ring[ring_idx].fragment_map[j];
            in_blocks[i] = rx_ring[ring_idx].fragments[j];
            out_blocks[ob_idx++] = rx_ring[ring_idx].fragments[i];
            index[i] = j++;
        }
    }

    fec_decode_simd(fec_p, (const uint8_t**)in_blocks, out_blocks, index, ZFEX_ROUND_UP_SIMD(max_packet_size));
}
