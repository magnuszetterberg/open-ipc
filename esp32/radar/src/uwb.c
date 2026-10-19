// The SR250 UWB radar over SPI (platform layer, R4). See uwb.h.
//
// The host interface, as the SR250 uses it on SPI:
//   - Every SPI transfer starts with a direction byte, 0x00 when the host writes and 0xFF when it reads;
//     what the SR250 sends follows it.
//   - The SR250 raises IRQ while it has a message. The host then reads the 4-byte UCI header in one transfer
//     and the payload, whose length the header gives, in a second.
//   - SPI without DMA (prj.conf): Zephyr v4.4.2's flexcomm SPI driver drops the first received byte of a
//     transfer with DMA, the direction byte's slot, so reads come back one byte short and shifted.
//   - After a reset the boot loader (HDLL) sends one ready message, 8 bytes in its own framing, then starts
//     the firmware, which speaks UCI.
#include "uwb.h"

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#define SR250 DT_NODELABEL(sr250)

static const struct spi_dt_spec spi = SPI_DT_SPEC_GET(SR250, SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB);
static const struct gpio_dt_spec irq = GPIO_DT_SPEC_GET(SR250, irq_gpios);
static const struct gpio_dt_spec reset = GPIO_DT_SPEC_GET(SR250, reset_gpios);

static struct gpio_callback irq_cb;
static K_SEM_DEFINE(irq_sem, 0, 1);

static void irq_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    k_sem_give(&irq_sem);
}

// Waits for IRQ high: true when the SR250 has a message
static bool wait_irq(int timeout_ms)
{
    int64_t end = k_uptime_get() + timeout_ms;
    while (gpio_pin_get_dt(&irq) != 1)
    {
        int64_t left = end - k_uptime_get();
        if (left <= 0)
        {
            return false;
        }
        k_sem_take(&irq_sem, K_MSEC(MIN(left, 10)));
    }
    return true;
}

// One read transfer: the direction byte, then n bytes from the SR250 into out
static int read_bytes(uint8_t *out, size_t n)
{
    // The same length both ways
    static uint8_t tx[1 + UWB_MAX_MESSAGE], rx[1 + UWB_MAX_MESSAGE];
    if (n > UWB_MAX_MESSAGE)
    {
        return -EMSGSIZE;
    }
    memset(tx, 0xFF, 1 + n);   // the read direction byte, and MOSI held high while the SR250 sends
    memset(rx, 0xAA, 1 + n);   // an rx the driver never wrote shows as AA, not as zeros from the SR250
    struct spi_buf tx_buf = {.buf = tx, .len = 1 + n};
    struct spi_buf rx_buf = {.buf = rx, .len = 1 + n};
    struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
    struct spi_buf_set rx_set = {.buffers = &rx_buf, .count = 1};
    int rc = spi_transceive_dt(&spi, &tx_set, &rx_set);
    if (rc == 0)
    {
        memcpy(out, rx + 1, n);
    }
    return rc;
}


int uwb_start(void)
{
    if (!spi_is_ready_dt(&spi) || !gpio_is_ready_dt(&irq) || !gpio_is_ready_dt(&reset))
    {
        printk("uwb: SPI or GPIO not ready\n");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&irq, GPIO_INPUT);
    gpio_init_callback(&irq_cb, irq_handler, BIT(irq.pin));
    gpio_add_callback(irq.port, &irq_cb);
    gpio_pin_interrupt_configure_dt(&irq, GPIO_INT_EDGE_TO_ACTIVE);

    // Reset twice, as NXP's host code does (its board reset, then its reset into UCI): RSTN low for 10 ms
    // (reset-gpios is active low, so "1" holds it in reset). The first boot loader message isn't read.
    gpio_pin_configure_dt(&reset, GPIO_OUTPUT_ACTIVE);
    k_msleep(10);
    gpio_pin_set_dt(&reset, 0);
    k_msleep(20);
    gpio_pin_set_dt(&reset, 1);
    k_msleep(10);
    gpio_pin_set_dt(&reset, 0);
    k_msleep(10);    // as NXP's host code waits before reading the boot loader

    // The boot loader's ready message: a 2-byte HDLL header (length in its low 13 bits), payload, 2-byte CRC
    if (!wait_irq(1000))
    {
        printk("uwb: no ready message from the SR250 after reset (IRQ stays low): is the shield seated, and SPI "
               "routed to the header?\n");
        return -ETIMEDOUT;
    }
    k_usleep(500);
    uint8_t hdr[2], body[16];
    if (read_bytes(hdr, sizeof(hdr)) != 0)
    {
        return -EIO;
    }
    size_t len = ((hdr[0] & 0x1F) << 8) | hdr[1];
    if (len + 2 > sizeof(body) || read_bytes(body, len + 2) != 0)
    {
        printk("uwb: unexpected boot message header %02x %02x\n", hdr[0], hdr[1]);
        return -EIO;
    }
    printk("uwb: SR250 boot loader ready (%02x %02x", hdr[0], hdr[1]);
    for (size_t i = 0; i < len + 2; i++)
    {
        printk(" %02x", body[i]);
    }
    printk(")\n");

    // The firmware starts, then says it is ready: DEVICE_STATUS_NTF (0x60 0x01) with state 1
    // The firmware starts, then says it is ready: DEVICE_STATUS_NTF (0x60 0x01) with state 1
    k_msleep(150);
    uint8_t status[8];
    int n = uwb_receive(status, sizeof(status), 1000);
    if (n == 5 && status[0] == 0x60 && status[1] == 0x01 && status[4] == 0x01)
    {
        printk("uwb: SR250 ready (UCI)\n");
        return 0;
    }
    printk("uwb: SR250 didn't say it is ready (%d bytes:", n);
    for (int i = 0; i < n; i++)
    {
        printk(" %02x", status[i]);
    }
    printk(")\n");
    return -EIO;
}

int uwb_send(const uint8_t *packet, size_t size)
{
    static uint8_t tx[1 + UWB_MAX_MESSAGE];
    if (size > UWB_MAX_MESSAGE)
    {
        return -EMSGSIZE;
    }
    tx[0] = 0x00;  // the host writes
    memcpy(tx + 1, packet, size);
    struct spi_buf tx_buf = {.buf = tx, .len = 1 + size};
    struct spi_buf_set tx_set = {.buffers = &tx_buf, .count = 1};
    k_msleep(2);   // the SR250 recovering from the last transfer, as NXP's host code waits
    int rc = spi_write_dt(&spi, &tx_set);
    k_busy_wait(80);
    return rc;
}

int uwb_receive(uint8_t *buf, size_t size, int timeout_ms)
{
    if (size < 4)
    {
        return -EINVAL;
    }
    if (!wait_irq(timeout_ms))
    {
        return 0;
    }
    k_busy_wait(500);
    int rc = read_bytes(buf, 4);
    if (rc != 0)
    {
        return rc;
    }
    // Payload length: data packets (message type 0) and extended control messages (bit 7 of byte 1) carry 16
    // bits in bytes 2 and 3; other control messages 8 bits in byte 3
    size_t len;
    if ((buf[0] & 0xE0) == 0 || (buf[1] & 0x80))
    {
        len = buf[2] | (buf[3] << 8);
    }
    else
    {
        len = buf[3];
    }
    if (4 + len > size)
    {
        return -EMSGSIZE;
    }
    if (len > 0)
    {
        rc = read_bytes(buf + 4, len);
        if (rc != 0)
        {
            return rc;
        }
    }
    return (int)(4 + len);
}
