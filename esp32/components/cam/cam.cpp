#include "cam.hpp"

#include "esp_log.h"

static const char *TAG = "cam";

esp_err_t cam_start(const cam_config &config)
{
    camera_config_t c = {};
    // AI-Thinker ESP32-CAM pin map
    c.pin_pwdn = 32;
    c.pin_reset = -1;
    c.pin_xclk = 0;
    c.pin_sccb_sda = 26;
    c.pin_sccb_scl = 27;
    c.pin_d7 = 35;
    c.pin_d6 = 34;
    c.pin_d5 = 39;
    c.pin_d4 = 36;
    c.pin_d3 = 21;
    c.pin_d2 = 19;
    c.pin_d1 = 18;
    c.pin_d0 = 5;
    c.pin_vsync = 25;
    c.pin_href = 23;
    c.pin_pclk = 22;
    c.xclk_freq_hz = 20000000;
    c.ledc_timer = LEDC_TIMER_0;
    c.ledc_channel = LEDC_CHANNEL_0;

    c.pixel_format = PIXFORMAT_JPEG;
    c.frame_size = config.frame_size;
    c.jpeg_quality = config.jpeg_quality;
    c.fb_count = 2;                      // one being filled while the other is sent
    c.fb_location = CAMERA_FB_IN_PSRAM;  // a VGA JPEG is tens of KB: too big for internal RAM
    c.grab_mode = CAMERA_GRAB_LATEST;    // newest frame; older ones are dropped whole

    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "camera init: %s", esp_err_to_name(err));
        return err;
    }
    sensor_t *s = esp_camera_sensor_get();
    ESP_LOGI(TAG, "sensor PID 0x%02x, frame size %d, quality %d", s->id.PID, config.frame_size, config.jpeg_quality);
    return ESP_OK;
}

camera_fb_t *cam_capture(void)
{
    return esp_camera_fb_get();
}

void cam_release(camera_fb_t *frame)
{
    esp_camera_fb_return(frame);
}
