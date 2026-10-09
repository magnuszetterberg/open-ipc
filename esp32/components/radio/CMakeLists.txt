# Raw 802.11 transmit on the ESP32 (platform layer, R4): the only place wfb-ng frames meet ESP-IDF's WiFi.
idf_component_register(SRCS radio.cpp INCLUDE_DIRS include
                       REQUIRES wfb_core esp_wifi esp_event nvs_flash esp_timer)
