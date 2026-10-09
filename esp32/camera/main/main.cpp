// Camera firmware (app layer, R4). M0: only proves the toolchain builds and flashes.
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_psram.h"

static const char *TAG = "camera";

extern "C" void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "ESP32 rev %d.%d, %d cores, PSRAM %u bytes",
             chip.revision / 100, chip.revision % 100, chip.cores, (unsigned)esp_psram_get_size());
}
