// wfb_rx fed by wfb_tx: payloads come back in order, FEC covers up to n - k lost packets per block,
// and packets for another key or channel are rejected.
#include <stdint.h>
#include <string.h>

#include <functional>
#include <string>
#include <vector>

#include <sodium.h>

#include "check.hpp"
#include "keys.hpp"
#include "wfb_rx.hpp"
#include "wfb_tx.hpp"

struct payload_list : wfb_payload_sink
{
    std::vector<std::string> payloads;
    void send_payload(const uint8_t *buf, size_t size) override
    {
        payloads.emplace_back((const char *)buf, size);
    }
};

// Passes the transmitter's frames to the receiver, except those drop() picks.
// drop() sees the data/FEC frame number (session frames aren't counted, and never dropped).
struct air : wfb_sink
{
    wfb_rx &rx;
    std::function<bool(int)> drop;
    int data_frames = 0;
    air(wfb_rx &rx, std::function<bool(int)> drop) : rx(rx), drop(drop) {}
    void send_frame(const uint8_t *buf, size_t size) override
    {
        if (buf[0] == 1 && drop(data_frames++))
        {
            return;
        }
        rx.process_packet(buf, size);
    }
};

static uint8_t drone_key[64], gs_key[64];
static const size_t max_payload = 1448;  // wfb_tx's limit with 1476-byte packets (test_tx checks it)

// Payload i: varying sizes, up to the largest the transmitter takes.
static std::string make_payload(int i)
{
    std::string payload = "payload " + std::to_string(i) + std::string(i * 37 % max_payload, 'x');
    payload.resize(payload.size() < max_payload ? payload.size() : max_payload);
    return payload;
}

static std::vector<std::string> expected(int count)
{
    std::vector<std::string> v;
    for (int i = 0; i < count; i++)
    {
        v.push_back(make_payload(i));
    }
    return v;
}

// Send `count` payloads through tx -> air -> rx. Returns what rx delivered.
static std::vector<std::string> run(int count, std::function<bool(int)> drop, wfb_rx_stats *stats = nullptr,
                                    uint32_t tx_channel = 0, const uint8_t *rx_key = gs_key, int ring_size = 8)
{
    payload_list out;
    wfb_rx rx(out);
    wfb_rx_config rx_config;
    rx_config.ring_size = ring_size;
    CHECK(rx.init(rx_key, 64, rx_config) == WFB_OK);

    air link(rx, drop);
    wfb_tx tx(link);
    wfb_tx_config tx_config;
    tx_config.channel_id = tx_channel;
    CHECK(tx.init(drone_key, sizeof(drone_key), tx_config) == WFB_OK);
    CHECK(tx.max_payload() == max_payload);

    for (int i = 0; i < count; i++)
    {
        std::string payload = make_payload(i);
        CHECK(tx.send((const uint8_t *)payload.data(), payload.size(), (uint64_t)i * 1000) == WFB_OK);
    }
    CHECK(tx.close_block() == WFB_OK);
    if (stats)
    {
        *stats = rx.stats;
    }
    return out.payloads;
}

int main()
{
    CHECK(read_key("drone.key", drone_key, sizeof(drone_key)));
    CHECK(read_key("gs.key", gs_key, sizeof(gs_key)));
    const int count = 100;  // 12.5 blocks of 8, so the last one is closed by close_block()

    // No loss: everything, in order.
    wfb_rx_stats stats;
    CHECK(run(count, [](int) { return false; }, &stats) == expected(count));
    CHECK(stats.count_p_fec_recovered == 0 && stats.count_p_lost == 0 && stats.count_p_dec_err == 0);

    // Lose 4 of every 12 frames (n - k): FEC recovers all of it.
    CHECK(run(count, [](int f) { return f % 12 < 4; }, &stats) == expected(count));
    CHECK(stats.count_p_fec_recovered > 0 && stats.count_p_lost == 0);

    // Lose 4 frames per block at other positions, including FEC frames.
    CHECK(run(count, [](int f) { return f % 12 == 1 || f % 12 == 6 || f % 12 == 9 || f % 12 == 11; }) ==
          expected(count));

    // Lose 5 of every 12: blocks can't be recovered, and the receiver counts the loss.
    std::vector<std::string> got = run(count, [](int f) { return f % 12 < 5; }, &stats);
    CHECK(got.size() < (size_t)count);
    CHECK(stats.count_p_lost > 0);

    // Another channel, or the wrong key: the session is refused and nothing comes out.
    CHECK(run(count, [](int) { return false; }, &stats, (0 << 8) + 1).empty());
    CHECK(stats.count_p_dec_err > 0 && stats.count_p_outgoing == 0);
    // (drone.key itself would open it: crypto_box derives the same shared key in both directions.)
    uint8_t other_key[64], other_public[32];
    crypto_box_keypair(other_public, other_key);         // a different ground station's secret key...
    memcpy(other_key + 32, gs_key + 32, 32);              // ...next to our drone's public key
    CHECK(run(count, [](int) { return false; }, &stats, 0, other_key).empty());
    CHECK(stats.count_p_dec_err > 0 && stats.count_p_outgoing == 0);

    // A ring of one block (what fits three cameras on an ESP32 without PSRAM): on a radio that delivers
    // in order, FEC still recovers up to n - k lost frames per block.
    CHECK(run(count, [](int) { return false; }, &stats, 0, gs_key, 1) == expected(count));
    CHECK(run(count, [](int f) { return f % 12 < 4; }, &stats, 0, gs_key, 1) == expected(count));
    CHECK(stats.count_p_fec_recovered > 0 && stats.count_p_lost == 0 && stats.count_p_override == 0);
    CHECK(run(count, [](int f) { return f % 12 == 1 || f % 12 == 6 || f % 12 == 9 || f % 12 == 11; }, &stats, 0,
              gs_key, 1) == expected(count));

    // Telemetry's stream: FEC 1/2, so each message stands alone and survives losing either of its two
    // frames. Lose the data frame of even messages and the FEC frame of odd ones: all arrive, in order.
    {
        payload_list telemetry_out;
        wfb_rx rx(telemetry_out);
        wfb_rx_config rx_config;
        rx_config.channel_id = 0x10;
        CHECK(rx.init(gs_key, 64, rx_config) == WFB_OK);
        air link(rx, [](int f) { return (f / 2) % 2 == 0 ? f % 2 == 0 : f % 2 == 1; });
        wfb_tx tx(link);
        wfb_tx_config tx_config;
        tx_config.fec_k = 1;
        tx_config.fec_n = 2;
        tx_config.channel_id = 0x10;
        CHECK(tx.init(drone_key, sizeof(drone_key), tx_config) == WFB_OK);
        std::vector<std::string> sent;
        for (int i = 0; i < 20; i++)
        {
            sent.push_back("{\"cam\":0,\"seq\":" + std::to_string(i) + ",\"uptime_s\":" + std::to_string(i) + "}");
            CHECK(tx.send((const uint8_t *)sent.back().data(), sent.back().size(), (uint64_t)i * 1000000) == WFB_OK);
        }
        CHECK(telemetry_out.payloads == sent);
        CHECK(rx.stats.count_p_fec_recovered == 10 && rx.stats.count_p_lost == 0);
    }

    return check_result("test_rx");
}
