# Radar camera firmware for the FRDM-RW612: a Zephyr project (R6) sharing the core with the ESP32 firmwares.
# Build it with esp32/build.sh radar (Zephyr from esp32/zephyr-setup.sh).
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radar C CXX)

# libsodium, then the core (R4), which links it
include(sodium/sodium.cmake)
add_subdirectory(../components/wfb_core wfb_core)

# The link key (R9), as a C array the keys component includes
include(../components/keys/key_file.cmake)
wfb_key_file(key_file ${CONFIG_WFB_KEY_FILE})
set(key_inc ${CMAKE_CURRENT_BINARY_DIR}/generated/wfb_key.inc)
generate_inc_file_for_target(app ${key_file} ${key_inc})

target_sources(app PRIVATE src/main.cpp src/radio.cpp src/radio_nxp.c src/radar.cpp src/uwb.c ../components/keys/keys.cpp)
target_include_directories(app PRIVATE ../components/keys/include)
# Zephyr links only its own tree's libraries by itself
target_link_libraries(app PRIVATE wfb_core sodium)
set_source_files_properties(../components/keys/keys.cpp PROPERTIES COMPILE_DEFINITIONS WFB_KEY_INC="${key_inc}")
