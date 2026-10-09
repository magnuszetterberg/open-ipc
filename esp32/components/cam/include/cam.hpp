// OV2640 JPEG capture on the AI-Thinker ESP32-CAM (platform layer, R4).
#pragma once

#include "esp_camera.h"
#include "esp_err.h"

struct cam_config
{
    framesize_t frame_size = FRAMESIZE_VGA;  // 640x480 to start (protocol table)
    int jpeg_quality = 12;                   // 0-63, lower is better quality and bigger frames
};

// Starts the camera. Frame buffers live in PSRAM, and a capture always returns the newest frame:
// when the pipeline falls behind, older frames are dropped whole (Coding conventions).
esp_err_t cam_start(const cam_config &config);

// The newest frame, or NULL on timeout. Give it back with cam_release() as soon as it is sent.
camera_fb_t *cam_capture(void);
void cam_release(camera_fb_t *frame);
