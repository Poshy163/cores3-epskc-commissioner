cmake_minimum_required(VERSION 3.16)

# One version string for the whole image. The console `status` line, the About
# page, the app descriptor and the Matter Basic Information
# SoftwareVersionString all report it: the short git revision, with "*" when
# the working tree has uncommitted changes. The Docker build copies the sources
# without .git and drops a version.txt beside them instead.
execute_process(
    COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE FW_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(FW_VERSION)
    execute_process(
        COMMAND git diff --quiet
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        RESULT_VARIABLE FW_DIRTY ERROR_QUIET)
    if(FW_DIRTY)
        set(FW_VERSION "${FW_VERSION}*")
    endif()
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/version.txt)
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/version.txt FW_VERSION)
    string(STRIP "${FW_VERSION}" FW_VERSION)
else()
    set(FW_VERSION "unknown")
endif()
set(PROJECT_VER "${FW_VERSION}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(epskc_commissioner)
