cmake_minimum_required(VERSION 3.22)

# Shared components (R6): every firmware finds ../components/*.
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../components)
# Build only what main requires, so a shared component joins when main asks for it (wfb_core: #7).
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(camera)
