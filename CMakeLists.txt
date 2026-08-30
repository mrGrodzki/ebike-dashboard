# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
if(DEFINED ENV{EBIKE_PROJECT_VER})
    set(PROJECT_VER "$ENV{EBIKE_PROJECT_VER}")
else()
    set(PROJECT_VER "0.10.0")
endif()
project(ebike_dashboard)
