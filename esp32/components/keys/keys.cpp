#include "keys.hpp"

#ifdef WFB_KEY_INC
// Zephyr (the radar firmware): the key file as a generated C array
static const uint8_t wfb_key[] = {
#include WFB_KEY_INC
};

const uint8_t *keys_link_key(size_t *size)
{
    *size = sizeof(wfb_key);
    return wfb_key;
}
#else
// ESP-IDF: target_add_binary_data's symbols
extern const uint8_t wfb_key_start[] asm("_binary_wfb_key_start");
extern const uint8_t wfb_key_end[] asm("_binary_wfb_key_end");

const uint8_t *keys_link_key(size_t *size)
{
    *size = wfb_key_end - wfb_key_start;
    return wfb_key_start;
}
#endif
