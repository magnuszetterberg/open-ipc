# wfb_core: wfb-ng's protocol, encryption and FEC (core layer, R4). Plain C/C++ with no ESP-IDF
# headers, so the same sources build as an ESP-IDF component, for the RW612 (Zephyr) and, through ../../host,
# for Linux.

# The submodule is read-only (R2): its files are compiled and included from where they are.
set(wfb_ng ${CMAKE_CURRENT_LIST_DIR}/../../../wfb-ng/src)
set(srcs wfb_core.cpp wfb_tx.cpp wfb_rx.cpp wfb_80211.cpp ${wfb_ng}/zfex.c)

# zfex options as wfb-ng's Makefile sets them; the SIMD ones switch themselves off on chips without it.
set(zfex_defs ZFEX_UNROLL_ADDMUL_SIMD=8 ZFEX_USE_INTEL_SSSE3 ZFEX_USE_ARM_NEON ZFEX_INLINE_ADDMUL ZFEX_INLINE_ADDMUL_SIMD)

if(ESP_PLATFORM)
    # esp_compat: stand-ins for Linux headers wifibroadcast.hpp includes (esp_compat/README.md)
    idf_component_register(SRCS ${srcs} INCLUDE_DIRS include ${wfb_ng} PRIV_INCLUDE_DIRS esp_compat
                           REQUIRES espressif__libsodium lwip)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${zfex_defs})
elseif(COMMAND zephyr_library_named)
    # The radar firmware (esp32/radar, Zephyr on the RW612): zephyr_compat stands in for Linux headers, as
    # esp_compat does on the ESP32; libsodium is built there (sodium.cmake)
    zephyr_library_named(wfb_core)
    zephyr_library_sources(${srcs})
    zephyr_include_directories(include ${wfb_ng})
    zephyr_library_include_directories(zephyr_compat)
    # POSIX level, as on Linux: picolibc declares posix_memalign() and utime.h's time_t only then
    zephyr_library_compile_definitions(${zfex_defs} _POSIX_C_SOURCE=200809L)
    zephyr_library_link_libraries(sodium)
    # Zephyr defines KERNEL for its own code; zfex.c uses the name for a macro of its own
    set_source_files_properties(${wfb_ng}/zfex.c PROPERTIES COMPILE_OPTIONS -UKERNEL)
else()
    add_library(wfb_core STATIC ${srcs})
    target_include_directories(wfb_core PUBLIC include ${wfb_ng})
    target_compile_definitions(wfb_core PRIVATE ${zfex_defs})
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(sodium REQUIRED IMPORTED_TARGET libsodium)
    target_link_libraries(wfb_core PUBLIC PkgConfig::sodium)
endif()
