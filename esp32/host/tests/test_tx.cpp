// wfb_tx with the repo's test keys: which packets come out, in what order, and whether the ground
// side's key opens them (session with gs.key, data with the session key).
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "check.hpp"
#include "wfb_tx.hpp"
#include "wifibroadcast.hpp"

struct list_sink : wfb_sink
{
    std::vector<std::vector<uint8_t>> frames;
    void send_frame(const uint8_t *buf, size_t size) override
    {
        frames.emplace_back(buf, buf + size);
    }
};

static bool read_key(const char *name, uint8_t *key, size_t size)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", KEYS_DIR, name);
    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
    {
        return false;
    }
    bool ok = fread(key, size, 1, fp) == 1;
    fclose(fp);
    return ok;
}

static uint64_t nonce_of(const std::vector<uint8_t> &frame)
{
    return be64toh(((const wblock_hdr_t *)frame.data())->data_nonce);
}

int main()
{
    uint8_t drone_key[64], gs_key[64];
    CHECK(read_key("drone.key", drone_key, sizeof(drone_key)));
    CHECK(read_key("gs.key", gs_key, sizeof(gs_key)));

    list_sink sink;
    wfb_tx tx(sink);
    wfb_tx_config config;
    config.channel_id = (0 << 8) + 0;  // link 0, port 0: what link.sh's wfb_rx listens for
    CHECK(tx.init(drone_key, sizeof(drone_key), config) == WFB_OK);
    CHECK(tx.max_payload() == 1500 - 24 - sizeof(wblock_hdr_t) - 16 - sizeof(wpacket_hdr_t));

    // One full block: the session announcement, 8 data packets, 4 FEC packets.
    const uint8_t payload[] = "frame 0";
    for (int i = 0; i < 8; i++)
    {
        CHECK(tx.send(payload, sizeof(payload), 0) == WFB_OK);
    }
    CHECK(sink.frames.size() == 1 + 12);
    CHECK(sink.frames[0][0] == WFB_PACKET_SESSION);
    for (size_t i = 1; i < sink.frames.size(); i++)
    {
        CHECK(sink.frames[i][0] == WFB_PACKET_DATA);
        CHECK(nonce_of(sink.frames[i]) == (0ULL << 8) + (i - 1));  // block 0, fragments 0..11
    }

    // The ground key opens the session packet, and it carries our settings.
    const std::vector<uint8_t> &session = sink.frames[0];
    wsession_data_t session_data;
    CHECK(session.size() == sizeof(wsession_hdr_t) + sizeof(session_data) + crypto_box_MACBYTES);
    CHECK(crypto_box_open_easy((uint8_t *)&session_data, session.data() + sizeof(wsession_hdr_t),
                               session.size() - sizeof(wsession_hdr_t),
                               ((const wsession_hdr_t *)session.data())->session_nonce, gs_key + 32, gs_key) == 0);
    CHECK(be32toh(session_data.channel_id) == config.channel_id);
    CHECK(session_data.fec_type == WFB_FEC_VDM_RS && session_data.k == 8 && session_data.n == 12);

    // The session key opens a data packet, and the payload comes back.
    const std::vector<uint8_t> &data = sink.frames[1];
    uint8_t plain[WFB_MAX_PACKET];
    unsigned long long plain_len = 0;
    CHECK(crypto_aead_chacha20poly1305_decrypt(plain, &plain_len, NULL, data.data() + sizeof(wblock_hdr_t),
                                               data.size() - sizeof(wblock_hdr_t), data.data(), sizeof(wblock_hdr_t),
                                               (const uint8_t *)&((const wblock_hdr_t *)data.data())->data_nonce,
                                               session_data.session_key) == 0);
    CHECK(plain_len == sizeof(wpacket_hdr_t) + sizeof(payload));
    CHECK(memcmp(plain + sizeof(wpacket_hdr_t), payload, sizeof(payload)) == 0);

    // Half a second later: no new announcement. Closing the block pads it with FEC-only packets.
    sink.frames.clear();
    CHECK(tx.send(payload, sizeof(payload), 500000) == WFB_OK);
    CHECK(sink.frames.size() == 1);
    CHECK(tx.close_block() == WFB_OK);
    CHECK(sink.frames.size() == 12);
    CHECK(nonce_of(sink.frames.back()) == (1ULL << 8) + 11);  // block 1, last fragment
    CHECK(tx.close_block() == WFB_OK);                      // nothing open: nothing sent
    CHECK(sink.frames.size() == 12);

    // A second after the first announcement, the next payload announces again.
    sink.frames.clear();
    CHECK(tx.send(payload, sizeof(payload), 1000000) == WFB_OK);
    CHECK(sink.frames.size() == 2 && sink.frames[0][0] == WFB_PACKET_SESSION);

    // Limits.
    static uint8_t big[WFB_MAX_PACKET];
    CHECK(tx.send(big, tx.max_payload(), 1000000) == WFB_OK);
    CHECK(sink.frames.back().size() <= WFB_MAX_PACKET);
    CHECK(tx.send(big, tx.max_payload() + 1, 1000000) == WFB_ERR_TOO_BIG);
    wfb_tx unready(sink);
    CHECK(unready.send(payload, sizeof(payload), 0) == WFB_ERR_NOT_READY);
    CHECK(unready.init(drone_key, 10, config) == WFB_ERR_CONFIG);

    return check_result("test_tx");
}
