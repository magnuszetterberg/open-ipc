# Linux build of the core components and their tests (design: Testing). Run through ../test.sh.
cmake_minimum_required(VERSION 3.22)
project(esp32_host C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
add_compile_options(-Wall "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")  # the ESP32's limits (Coding conventions)

add_subdirectory(../components/wfb_core wfb_core)
add_subdirectory(../components/rtp_jpeg rtp_jpeg)

enable_testing()
function(host_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ARGN})
    target_compile_definitions(${name} PRIVATE KEYS_DIR="${CMAKE_CURRENT_LIST_DIR}/../../keys")
    add_test(NAME ${name} COMMAND ${name})
endfunction()

host_test(test_fec wfb_core)
host_test(test_tx wfb_core)
host_test(test_rx wfb_core)
host_test(test_80211 wfb_core)

# Runs the real wfb_tx and wfb_rx from the submodule; test.sh builds them first.
host_test(test_wfb_ng wfb_core)
target_compile_definitions(test_wfb_ng PRIVATE WFB_NG_DIR="${CMAKE_CURRENT_LIST_DIR}/../../wfb-ng")

host_test(test_rtp_jpeg rtp_jpeg)
target_compile_definitions(test_rtp_jpeg PRIVATE DATA_DIR="${CMAKE_CURRENT_LIST_DIR}/tests/data")
