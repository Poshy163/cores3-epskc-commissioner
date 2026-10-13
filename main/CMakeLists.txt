idf_component_register(
    SRCS "main.c" "matter.cpp" "matter_battery.cpp" "meshcop.c" "ui.c" "discover.c" "qrscan.c" "thread.c" "power.c" "power_log.c" "log_ring.c" "settings.c" "otbr_rest.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif nvs_flash console mbedtls esp_timer lwip esp_pm esp_mm
             openthread driver vfs esp_driver_tsens esp_timer esp_http_server
             espressif__esp_matter
    LDFRAGMENTS "linker.lf"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

# The top-level CMakeLists.txt derives the version from git (or version.txt)
# and hands it to ESP-IDF as PROJECT_VER, so the app descriptor that Matter
# reports as SoftwareVersionString and the FW_VERSION the console prints are
# always the same string.
idf_build_get_property(FW_VERSION PROJECT_VER)
target_compile_definitions(${COMPONENT_LIB} PRIVATE FW_VERSION="${FW_VERSION}")
