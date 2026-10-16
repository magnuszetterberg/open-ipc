# rtp_jpeg: JPEG frames as RTP packets per RFC 2435 (core layer, R4, R7). Plain C++, no ESP-IDF headers,
# so the same sources build as an ESP-IDF component, for the RW612 (Zephyr) and, through ../../host, for Linux.
set(srcs rtp_jpeg.cpp jpeg_tables.cpp)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${srcs} INCLUDE_DIRS include)
elseif(COMMAND zephyr_library_named)
    # The radar firmware (esp32/radar, Zephyr on the RW612)
    zephyr_library_named(rtp_jpeg)
    zephyr_library_sources(${srcs})
    zephyr_include_directories(include)
else()
    add_library(rtp_jpeg STATIC ${srcs})
    target_include_directories(rtp_jpeg PUBLIC include)
endif()
