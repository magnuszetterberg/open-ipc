# Which key file a firmware embeds (R9). Shared by this ESP-IDF component and the radar firmware (Zephyr).
# wfb_key_file(<out> <configured>): <configured> is CONFIG_WFB_KEY_FILE, relative to the repo root. The repo's
# test keys are the defaults; your own pair in keys/own/ (./ipc keys, never committed) replaces them, and real
# keys elsewhere are named directly.
function(wfb_key_file out configured)
    set(repo ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../..)
    set(key_file ${repo}/${configured})
    # CONFIGURE_DEPENDS: keys/own appearing or going away re-runs this on the next build
    file(GLOB own_keys CONFIGURE_DEPENDS ${repo}/keys/own/*.key)
    get_filename_component(key_name ${configured} NAME)
    if(configured MATCHES "^keys/(drone|gs)\\.key$" AND EXISTS ${repo}/keys/own/${key_name})
        set(key_file ${repo}/keys/own/${key_name})
    endif()
    if(NOT EXISTS ${key_file})
        message(FATAL_ERROR "key file not found: ${key_file} (CONFIG_WFB_KEY_FILE)")
    endif()
    message(STATUS "link key: ${key_file}")
    set(${out} ${key_file} PARENT_SCOPE)
endfunction()
