# The link key, embedded at build time (R9): CONFIG_WFB_KEY_FILE names it, relative to the repo root.
# The repo's test keys are the defaults. Your own pair in keys/own/ (./ipc keys, never committed) replaces
# them; real keys elsewhere are pointed to here.
idf_component_register(SRCS keys.cpp INCLUDE_DIRS include)

if(CONFIG_WFB_KEY_FILE)
    include(${CMAKE_CURRENT_LIST_DIR}/key_file.cmake)
    wfb_key_file(key_file ${CONFIG_WFB_KEY_FILE})
    target_add_binary_data(${COMPONENT_LIB} ${key_file} BINARY RENAME_TO wfb_key)
endif()
