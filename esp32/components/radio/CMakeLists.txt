# Raw 802.11 transmit and receive on the ESP32 (platform layer, R4): where wfb-ng frames meet ESP-IDF's WiFi.
idf_component_register(SRCS radio.cpp INCLUDE_DIRS include
                       REQUIRES wfb_core esp_wifi esp_event nvs_flash esp_timer heap freertos)
