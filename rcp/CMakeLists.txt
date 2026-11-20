# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# The S3 reflashes the H2 when this firmware's version string changes. ESP-IDF's
# OpenThread component fixes the string's timestamp when CMake configures, from
# SOURCE_DATE_EPOCH. The build scripts also pass rcp/'s last commit time as this
# cache entry: a new value makes idf.py reconfigure an existing build, so a
# commit to rcp/ always gives a new version, and the cached value keeps the
# version stable when the build reconfigures for any other reason.
if(DEFINED RCP_SOURCE_DATE_EPOCH)
    set(ENV{SOURCE_DATE_EPOCH} "${RCP_SOURCE_DATE_EPOCH}")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(esp_ot_rcp)
