cmake_minimum_required(VERSION 3.22)

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
        COMMAND git diff --quiet HEAD
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

# ESP-Matter 1.6.1's legacy command dispatcher retains lighting callbacks even
# when those clusters are disabled. Its unconditional lighting codegen sources
# use Scenes helpers that the managed build omits when Scenes is disabled.
# Supply its dynamic-endpoint adapters and core helpers without adding a Scenes
# cluster/endpoint or editing the managed package. Remove this with the SDK upgrade
# that fixes the dispatcher's cluster selection.
if(CONFIG_ESP_MATTER_ENABLE_DATA_MODEL AND
   NOT CONFIG_ESP_MATTER_ENABLE_GENERATED_DATA_MODEL AND
   NOT CONFIG_SUPPORT_SCENES_CLUSTER)
    idf_component_get_property(matter_dir espressif__esp_matter COMPONENT_DIR)
    file(STRINGS "${matter_dir}/idf_component.yml" matter_version_line
         REGEX "^version:" LIMIT_COUNT 1)
    if(matter_version_line MATCHES "^version:[ \t]*[\"']?1[.]6[.]1[\"']?[ \t]*$")
        idf_component_get_property(matter_lib espressif__esp_matter COMPONENT_LIB)
        set(matter_scenes_dir "${matter_dir}/connectedhomeip/connectedhomeip/src/app/clusters/scenes-server")
        set(matter_scenes_provider_dir "${matter_dir}/components/esp_matter/data_model_provider/clusters/scenes_management")
        target_sources(${matter_lib} PRIVATE
            "${matter_scenes_provider_dir}/attribute_value_pair_validator.cpp"
            "${matter_scenes_provider_dir}/integration.cpp"
            "${matter_scenes_dir}/ExtensionFieldSetsImpl.cpp"
            "${matter_scenes_dir}/SceneHandlerImpl.cpp"
            "${matter_scenes_dir}/ScenesManagementCluster.cpp"
            "${matter_scenes_dir}/SceneTableImpl.cpp")
    endif()
endif()

# Pack the ESP32-H2 radio firmware (rcp/, built separately for esp32h2) into
# the rcp_fw SPIFFS partition. main/rcp_update.c flashes it to the H2 when the
# bundled version changes. esp_rcp_update's own packing (CONFIG_AUTO_UPDATE_RCP)
# needs an absolute RCP path in sdkconfig and an update flow that restarts while
# OpenThread is running, so this project uses only its image format and API.
set(RCP_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/rcp" CACHE PATH
    "Build directory of rcp/ whose image is packed into the rcp_fw partition")
set(rcp_inputs
    "${RCP_BUILD_DIR}/rcp_version"
    "${RCP_BUILD_DIR}/flash_args"
    "${RCP_BUILD_DIR}/bootloader/bootloader.bin"
    "${RCP_BUILD_DIR}/partition_table/partition-table.bin"
    "${RCP_BUILD_DIR}/esp_ot_rcp.bin")
foreach(rcp_input IN LISTS rcp_inputs)
    if(NOT EXISTS "${rcp_input}")
        message(FATAL_ERROR
            "Radio firmware not built: ${rcp_input} is missing. Build rcp/ first, "
            "for example `idf.py -C rcp -B build/rcp -D SDKCONFIG=<abs>/build/rcp/sdkconfig build` "
            "(build-native.ps1, build.sh and CI do this), or pass -DRCP_BUILD_DIR=<dir>.")
    endif()
endforeach()
idf_component_get_property(rcp_update_dir espressif__esp_rcp_update COMPONENT_DIR)
idf_build_get_property(python PYTHON)
set(rcp_image_dir "${CMAKE_BINARY_DIR}/rcp_fw_image")
add_custom_command(
    OUTPUT "${rcp_image_dir}/ot_rcp_0/rcp_image"
    COMMAND ${python} "${rcp_update_dir}/create_ota_image.py"
            --rcp-build-dir "${RCP_BUILD_DIR}"
            --target-file "${rcp_image_dir}/ot_rcp_0/rcp_image"
    DEPENDS ${rcp_inputs}
    COMMENT "Packing the ESP32-H2 radio firmware into rcp_fw"
    VERBATIM)
add_custom_target(rcp_image DEPENDS "${rcp_image_dir}/ot_rcp_0/rcp_image")
spiffs_create_partition_image(rcp_fw "${rcp_image_dir}" FLASH_IN_PROJECT DEPENDS rcp_image)
