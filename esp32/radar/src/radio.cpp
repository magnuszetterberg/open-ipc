// Raw 802.11 transmit on the RW612 (platform layer, R4): the C++ side. See radio.hpp and radio_nxp.c.
#include "radio.hpp"

#include <errno.h>

#include "radio_nxp.h"

int radio_start(const radio_config &config)
{
    return radio_nxp_start(config.channel, config.mcs);
}

size_t radio_max_packet()
{
    return radio_nxp_max_body();
}

radio_tx::radio_tx(uint32_t channel_id) : framer(channel_id)
{
}

void radio_tx::send_frame(const uint8_t *buf, size_t size)
{
    size_t frame_size = framer.frame(frame, sizeof(frame), buf, size);
    if (frame_size == 0)
    {
        dropped += 1;
        last_error = -EMSGSIZE;
        return;
    }
    int rc = radio_nxp_send(frame, frame + WFB_80211_HEADER_SIZE, size);
    if (rc != 0)
    {
        dropped += 1;
        last_error = rc;
        return;
    }
    frames += 1;
    bytes += frame_size;
}
