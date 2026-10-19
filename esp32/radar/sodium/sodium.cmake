# libsodium for the radar firmware: the pinned release (zephyr-setup.sh fetches it into LIBSODIUM_DIR) as a
# Zephyr library. ESP-IDF has a component for it and Linux a package; Zephyr has neither. Every portable source
# is compiled and the linker keeps what wfb_core uses; the hardware random generator replaces the OS one.
if(NOT DEFINED ENV{LIBSODIUM_DIR} OR NOT EXISTS $ENV{LIBSODIUM_DIR}/src/libsodium)
    message(FATAL_ERROR "libsodium not found (LIBSODIUM_DIR): run esp32/zephyr-setup.sh, and build through esp32/zephyr.sh")
endif()
set(sodium_src $ENV{LIBSODIUM_DIR}/src/libsodium)

# version.h is made by libsodium's configure, which isn't run here
file(STRINGS $ENV{LIBSODIUM_DIR}/configure.ac sodium_ac REGEX "^(AC_INIT|SODIUM_LIBRARY_VERSION_M)")
string(REGEX MATCH "AC_INIT\\(\\[libsodium\\],\\[([0-9.]+)\\]" _ "${sodium_ac}")
set(VERSION ${CMAKE_MATCH_1})
string(REGEX MATCH "SODIUM_LIBRARY_VERSION_MAJOR=([0-9]+)" _ "${sodium_ac}")
set(SODIUM_LIBRARY_VERSION_MAJOR ${CMAKE_MATCH_1})
string(REGEX MATCH "SODIUM_LIBRARY_VERSION_MINOR=([0-9]+)" _ "${sodium_ac}")
set(SODIUM_LIBRARY_VERSION_MINOR ${CMAKE_MATCH_1})
set(SODIUM_LIBRARY_MINIMAL_DEF "")
set(sodium_gen ${CMAKE_CURRENT_BINARY_DIR}/sodium_include)
# It sits in a directory of its own, so it names export.h by its place in libsodium's include directory
file(READ ${sodium_src}/include/sodium/version.h.in version_in)
string(REPLACE "#include \"export.h\"" "#include <sodium/export.h>" version_in "${version_in}")
string(CONFIGURE "${version_in}" version_h @ONLY)
file(GENERATE OUTPUT ${sodium_gen}/sodium/version.h CONTENT "${version_h}")

# The OS random sources and x86 assembly don't apply; SIMD files compile to nothing on Arm by their own #ifs.
file(GLOB_RECURSE sodium_srcs ${sodium_src}/*.c)
list(FILTER sodium_srcs EXCLUDE REGEX "/randombytes/(internal|sysrandom)/")

zephyr_library_named(sodium)
zephyr_library_sources(${sodium_srcs} ${CMAKE_CURRENT_LIST_DIR}/randombytes_zephyr.c)
zephyr_include_directories(${sodium_gen} ${sodium_src}/include)
# randombytes_internal.h here, ahead of libsodium's own, names the Zephyr random source as the default
zephyr_library_include_directories(${CMAKE_CURRENT_LIST_DIR} ${sodium_gen}/sodium ${sodium_src}/include/sodium)
zephyr_library_compile_definitions(CONFIGURED NATIVE_LITTLE_ENDIAN HAVE_WEAK_SYMBOLS __STDC_LIMIT_MACROS
                                   __STDC_CONSTANT_MACROS __STDC_WANT_LIB_EXT1__=0)
set_source_files_properties(${sodium_src}/randombytes/randombytes.c PROPERTIES COMPILE_DEFINITIONS
                            RANDOMBYTES_DEFAULT_IMPLEMENTATION)
# third-party code: its warnings aren't ours to fix
zephyr_library_compile_options(-w)
