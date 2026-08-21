idf_component_register(
    SRCS "main.c" "meshcop.c" "ui.c" "discover.c" "qrscan.c" "thread.c" "power.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif nvs_flash console mbedtls esp_timer lwip
             openthread driver vfs
)

# Bake the git revision into the binary so the board can say which build it
# runs. Falls back to a version.txt dropped next to the sources (the Docker
# build copies sources into a volume without .git) and then to "unknown".
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
elseif(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../version.txt)
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../version.txt FW_VERSION)
    string(STRIP "${FW_VERSION}" FW_VERSION)
else()
    set(FW_VERSION "unknown")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE FW_VERSION="${FW_VERSION}")
