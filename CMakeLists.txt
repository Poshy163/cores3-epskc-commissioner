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
