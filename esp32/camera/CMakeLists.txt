cmake_minimum_required(VERSION 3.22)

# Shared components (R6): every firmware finds ../components/*.
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(camera)
