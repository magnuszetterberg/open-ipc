// The 802.11 header around each packet: the bytes wfb_rx's capture filter checks, and the
// sequence number stepping as wfb_tx steps it.
#include <stdint.h>
#include <string.h>

#include "check.hpp"
#include "wfb_80211.hpp"

int main()
{
    const uint32_t channel_id = (0x123456 << 8) + 7;  // link 0x123456, radio_port 7
    wfb_80211_framer framer(channel_id);
    const uint8_t packet[] = {1, 2, 3, 4, 5};
    uint8_t frame[64];

    size_t size = framer.frame(frame, sizeof(frame), packet, sizeof(packet));
    CHECK(size == WFB_80211_HEADER_SIZE + sizeof(packet));
    CHECK(frame[0] == 0x08 && frame[1] == 0x01);  // data frame, to DS

    // wfb_rx: "ether[0x0a:2]==0x5742 && ether[0x0c:4] == channel_id" (rx.cpp)
    CHECK(frame[0x0a] == 0x57 && frame[0x0b] == 0x42);
    uint32_t id = (uint32_t)frame[0x0c] << 24 | frame[0x0d] << 16 | frame[0x0e] << 8 | frame[0x0f];
    CHECK(id == channel_id);
    CHECK(memcmp(frame + 4, "\xff\xff\xff\xff\xff\xff", 6) == 0);  // to broadcast
    CHECK(memcmp(frame + 16, frame + 10, 6) == 0);                  // third address = second
    CHECK(memcmp(frame + WFB_80211_HEADER_SIZE, packet, sizeof(packet)) == 0);

    // Sequence number: the upper 12 bits of bytes 22-23, one step per frame.
    CHECK(frame[22] == 0 && frame[23] == 0);
    framer.frame(frame, sizeof(frame), packet, sizeof(packet));
    CHECK(frame[22] == 0x10 && frame[23] == 0);

    CHECK(framer.frame(frame, WFB_80211_HEADER_SIZE + 4, packet, sizeof(packet)) == 0);  // too small

    // Unframing what the framer made: the channel and the packet come back
    size = framer.frame(frame, sizeof(frame), packet, sizeof(packet));
    uint32_t got_channel = 0;
    const uint8_t *got = nullptr;
    size_t got_size = 0;
    CHECK(wfb_80211_unframe(frame, size, &got_channel, &got, &got_size));
    CHECK(got_channel == channel_id && got_size == sizeof(packet) && memcmp(got, packet, sizeof(packet)) == 0);

    // Not ours: another frame type, another address, or nothing after the header
    uint8_t other[64];
    memcpy(other, frame, size);
    other[0] = 0x80;  // beacon
    CHECK(!wfb_80211_unframe(other, size, &got_channel, &got, &got_size));
    memcpy(other, frame, size);
    other[0x0b] = 0x43;
    CHECK(!wfb_80211_unframe(other, size, &got_channel, &got, &got_size));
    CHECK(!wfb_80211_unframe(frame, WFB_80211_HEADER_SIZE, &got_channel, &got, &got_size));
    return check_result("test_80211");
}
