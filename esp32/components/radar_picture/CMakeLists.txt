# radar_picture: what the radar finds, drawn, and the picture as JPEG (core layer, R4). Plain C++, so it
# builds for the RW612 (Zephyr, esp32/radar), as an ESP-IDF component, and, through ../../host, for Linux.
# The JPEG headers and tables come from rtp_jpeg, so its JPEGs are exactly what RFC 2435 carries.
set(srcs radar_detector.cpp radar_draw.cpp radar_font.cpp jpeg_encode.cpp)

if(ESP_PLATFORM)
    # ESP-IDF finds every component in esp32/components; no ESP32 firmware uses this one yet
    idf_component_register(SRCS ${srcs} INCLUDE_DIRS include REQUIRES rtp_jpeg)
elseif(COMMAND zephyr_library_named)
    zephyr_library_named(radar_picture)
    zephyr_library_sources(${srcs})
    zephyr_include_directories(include)
    zephyr_library_link_libraries(rtp_jpeg)
else()
    add_library(radar_picture STATIC ${srcs})
    target_include_directories(radar_picture PUBLIC include)
    target_link_libraries(radar_picture PUBLIC rtp_jpeg)
endif()
