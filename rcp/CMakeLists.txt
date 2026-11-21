# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# The S3 reflashes the H2 when this firmware's version string changes. ESP-IDF's
# OpenThread component builds that string when CMake configures. Two parts are
# pinned here so that CI and local builds of the same sources match:
#
# - The timestamp comes from SOURCE_DATE_EPOCH. The build scripts also pass
#   rcp/'s last commit time as the RCP_SOURCE_DATE_EPOCH cache entry: a new
#   value makes idf.py reconfigure an existing build, so a commit to rcp/
#   always gives a new version, and the cached value keeps the version stable
#   when the build reconfigures for any other reason.
# - ESP-IDF names its own commit and OpenThread's with `git rev-parse --short`,
#   whose length depends on the clone: ESP-IDF's CI image gave
#   051e4c4eb20-43cc05a9b where a local install gave 051e4c4e-43cc05a. A fixed
#   core.abbrev, added to any git configuration already in the environment,
#   gives every clone the same length.
if(DEFINED RCP_SOURCE_DATE_EPOCH)
    set(ENV{SOURCE_DATE_EPOCH} "${RCP_SOURCE_DATE_EPOCH}")
endif()
set(git_config_index "$ENV{GIT_CONFIG_COUNT}")
if(NOT git_config_index MATCHES "^[0-9]+$")
    set(git_config_index 0)
endif()
set(ENV{GIT_CONFIG_KEY_${git_config_index}} "core.abbrev")
set(ENV{GIT_CONFIG_VALUE_${git_config_index}} "12")
math(EXPR git_config_count "${git_config_index} + 1")
set(ENV{GIT_CONFIG_COUNT} "${git_config_count}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(esp_ot_rcp)
