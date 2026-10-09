# Linux build of the core components and their tests (design: Testing). Run through ../test.sh.
cmake_minimum_required(VERSION 3.22)
project(esp32_host C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
add_compile_options(-Wall "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>")  # the ESP32's limits (Coding conventions)

add_subdirectory(../components/wfb_core wfb_core)

enable_testing()
function(host_test name)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

host_test(test_fec wfb_core)
