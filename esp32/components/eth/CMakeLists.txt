# Wired Ethernet on the Olimex ESP32-POE, and UDP out (platform layer, R4).
idf_component_register(SRCS eth.cpp INCLUDE_DIRS include
                       REQUIRES wfb_core esp_eth esp_netif esp_event lwip esp_driver_gpio espressif__lan87xx)
