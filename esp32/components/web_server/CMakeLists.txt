# web_server: the base station's web view over HTTP (docs/DESIGN.md, M7). Plain BSD sockets, no ESP-IDF calls,
# so the same sources build as an ESP-IDF component (lwIP) and, through ../../host, for Linux.
set(srcs web_server.cpp)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${srcs} INCLUDE_DIRS include REQUIRES lwip)
else()
    add_library(web_server STATIC ${srcs})
    target_include_directories(web_server PUBLIC include)
endif()
