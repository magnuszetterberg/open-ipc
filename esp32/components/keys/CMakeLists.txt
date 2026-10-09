# The link key, embedded at build time (R9): CONFIG_WFB_KEY_FILE names it, relative to the repo root.
# The repo's test keys are the defaults; real keys stay out of git and are pointed to here.
idf_component_register(SRCS keys.cpp INCLUDE_DIRS include)

if(CONFIG_WFB_KEY_FILE)
    set(key_file ${CMAKE_CURRENT_LIST_DIR}/../../../${CONFIG_WFB_KEY_FILE})
    if(NOT EXISTS ${key_file})
        message(FATAL_ERROR "key file not found: ${key_file} (CONFIG_WFB_KEY_FILE)")
    endif()
    target_add_binary_data(${COMPONENT_LIB} ${key_file} BINARY RENAME_TO wfb_key)
endif()
