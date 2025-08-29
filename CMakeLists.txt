# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Include additional local components
set(EXTRA_COMPONENT_DIRS "./button")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Project name will be set based on target chip
if(DEFINED ENV{IDF_TARGET})
    if($ENV{IDF_TARGET} STREQUAL "esp32")
        project(esp32-ntrip-duo)
    elseif($ENV{IDF_TARGET} STREQUAL "esp32c3")
        project(esp32c3-ntrip-duo)
    elseif($ENV{IDF_TARGET} STREQUAL "esp32s3")
        project(esp32s3-ntrip-duo)
    elseif($ENV{IDF_TARGET} STREQUAL "esp32c6")
        project(esp32c6-ntrip-duo)
    else()
        project(esp32-ntrip-duo)
    endif()
else()
    project(esp32-ntrip-duo)
endif()
