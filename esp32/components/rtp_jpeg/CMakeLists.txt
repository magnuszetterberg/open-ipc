# rtp_jpeg: JPEG frames as RTP packets per RFC 2435 (core layer, R4, R7). Plain C++, no ESP-IDF headers,
# so the same sources build as an ESP-IDF component and, through ../../host, for Linux.
set(srcs rtp_jpeg.cpp jpeg_tables.cpp)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${srcs} INCLUDE_DIRS include)
else()
    add_library(rtp_jpeg STATIC ${srcs})
    target_include_directories(rtp_jpeg PUBLIC include)
endif()
