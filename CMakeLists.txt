cmake_minimum_required(VERSION 3.16)

# Credentials stay out of git. sdkconfig.defaults holds everything shareable;
# sdkconfig.defaults.local holds this bench's WiFi PSK and is ignored.
# Listing a file that does not exist is a hard error, so probe. Release
# images (tools/release, -DDL_RELEASE=1) are public and never take it.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
endif()
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local" AND NOT DL_RELEASE)
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(downlink)
