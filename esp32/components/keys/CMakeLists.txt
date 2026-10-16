# The link key, embedded at build time (R9): CONFIG_WFB_KEY_FILE names it, relative to the repo root.
# The repo's test keys are the defaults. Your own pair in keys/own/ (./ipc keys, never committed) replaces
# them; real keys elsewhere are pointed to here.
idf_component_register(SRCS keys.cpp INCLUDE_DIRS include)

if(CONFIG_WFB_KEY_FILE)
    set(repo ${CMAKE_CURRENT_LIST_DIR}/../../..)
    set(key_file ${repo}/${CONFIG_WFB_KEY_FILE})
    # CONFIGURE_DEPENDS: keys/own appearing or going away re-runs this on the next build
    file(GLOB own_keys CONFIGURE_DEPENDS ${repo}/keys/own/*.key)
    get_filename_component(key_name ${CONFIG_WFB_KEY_FILE} NAME)
    if(CONFIG_WFB_KEY_FILE MATCHES "^keys/(drone|gs)\\.key$" AND EXISTS ${repo}/keys/own/${key_name})
        set(key_file ${repo}/keys/own/${key_name})
    endif()
    if(NOT EXISTS ${key_file})
        message(FATAL_ERROR "key file not found: ${key_file} (CONFIG_WFB_KEY_FILE)")
    endif()
    message(STATUS "link key: ${key_file}")
    target_add_binary_data(${COMPONENT_LIB} ${key_file} BINARY RENAME_TO wfb_key)
endif()
