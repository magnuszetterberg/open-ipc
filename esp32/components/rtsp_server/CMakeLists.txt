# rtsp_server: RTSP (RFC 2326) for the base station's streams. Plain BSD sockets, no ESP-IDF calls, so the same
# sources build as an ESP-IDF component (lwIP) and, through ../../host, for Linux.
set(srcs rtsp_server.cpp)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${srcs} INCLUDE_DIRS include REQUIRES lwip)
else()
    add_library(rtsp_server STATIC ${srcs})
    target_include_directories(rtsp_server PUBLIC include)
endif()
