// The link key embedded at build time (R9).
#pragma once
#include <stddef.h>
#include <stdint.h>

// The 64 bytes of the key file named by CONFIG_WFB_KEY_FILE.
const uint8_t *keys_link_key(size_t *size);
