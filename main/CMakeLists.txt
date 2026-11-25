idf_component_register(
    SRCS "main.c" "matter.cpp" "matter_battery.cpp" "matter_temperature.cpp" "meshcop.c" "ui.c" "discover.c" "qrscan.c" "thread.c" "power.c" "power_log.c" "log_ring.c" "settings.c" "otbr_rest.c" "remote_admin.c" "device_status.c" "rcp_update.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif nvs_flash console mbedtls esp_timer lwip esp_pm esp_mm
             openthread vfs esp_driver_gpio esp_driver_uart esp_driver_i2c
             esp_driver_tsens esp_timer esp_http_server spiffs
             espressif__esp_matter espressif__esp_rcp_update bt
    LDFRAGMENTS "linker.lf"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

# ESP-IDF's MeshCoP publisher registers its own TXT callback, with
# mn=BorderRouter, each time it adds _meshcop._udp. Route every registration
# through thread.c so the firmware's callback stays installed.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=otBorderAgentSetMeshCoPServiceChangedCallback"
    "-u __wrap_otBorderAgentSetMeshCoPServiceChangedCallback")

# The Matter SDK starts Bluetooth at every boot, even on a device that is
# already paired and has no use for it. matter.cpp gates that one call on
# Settings > Matter > Bluetooth.
if(CONFIG_ENABLE_CHIPOBLE)
    target_link_libraries(${COMPONENT_LIB} INTERFACE
        "-Wl,--wrap=nimble_port_init"
        "-u __wrap_nimble_port_init")
endif()

# CHIP makes one station connect of its own while Matter starts, racing the
# firmware's Wi-Fi supervisor. matter.cpp skips just that call.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_wifi_connect"
    "-u __wrap_esp_wifi_connect")

# The top-level CMakeLists.txt derives the version from git (or version.txt)
# and hands it to ESP-IDF as PROJECT_VER, so the app descriptor that Matter
# reports as SoftwareVersionString and the FW_VERSION the console prints are
# always the same string.
idf_build_get_property(FW_VERSION PROJECT_VER)
target_compile_definitions(${COMPONENT_LIB} PRIVATE FW_VERSION="${FW_VERSION}")
