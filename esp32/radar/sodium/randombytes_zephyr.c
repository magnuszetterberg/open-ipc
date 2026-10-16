// libsodium's random source on Zephyr (platform layer, R4): the chip's entropy device, the RW612's TRNG.
// wfb-ng's session keys come from here, so a failing source stops the firmware instead of sending weak keys.
#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "randombytes_internal.h"

static const struct device *const entropy = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));

static const char *zephyr_name(void)
{
    return "zephyr-entropy";
}

static void zephyr_buf(void *const buf, const size_t size)
{
    uint8_t *p = buf;
    size_t left = size;
    while (left > 0)
    {
        uint16_t n = left > UINT16_MAX ? UINT16_MAX : (uint16_t)left;
        if (!device_is_ready(entropy) || entropy_get_entropy(entropy, p, n) != 0)
        {
            printk("randombytes: the entropy device failed; stopping\n");
            k_panic();
        }
        p += n;
        left -= n;
    }
}

static uint32_t zephyr_random(void)
{
    uint32_t r;
    zephyr_buf(&r, sizeof(r));
    return r;
}

const struct randombytes_implementation randombytes_zephyr_implementation = {
    .implementation_name = zephyr_name,
    .random = zephyr_random,
    .stir = NULL,
    .uniform = NULL,
    .buf = zephyr_buf,
    .close = NULL,
};
