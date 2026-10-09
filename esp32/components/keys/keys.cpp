#include "keys.hpp"

extern const uint8_t wfb_key_start[] asm("_binary_wfb_key_start");
extern const uint8_t wfb_key_end[] asm("_binary_wfb_key_end");

const uint8_t *keys_link_key(size_t *size)
{
    *size = wfb_key_end - wfb_key_start;
    return wfb_key_start;
}
