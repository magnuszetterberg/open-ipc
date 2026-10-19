// Stands in for libsodium's own randombytes_internal.h (found first on the include path, see sodium.cmake):
// the RW612's hardware random generator is libsodium's default, as esp_random() is on the ESP32.
#pragma once

#include "export.h"
#include "randombytes.h"

SODIUM_EXPORT
extern const struct randombytes_implementation randombytes_zephyr_implementation;

#undef RANDOMBYTES_DEFAULT_IMPLEMENTATION
#define RANDOMBYTES_DEFAULT_IMPLEMENTATION &randombytes_zephyr_implementation
