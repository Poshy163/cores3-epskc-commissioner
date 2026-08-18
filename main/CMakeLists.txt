idf_component_register(
    SRCS "main.c" "meshcop.c" "ui.c" "discover.c" "qrscan.c" "thread.c" "power.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif nvs_flash console mbedtls esp_timer lwip
             openthread driver vfs
)
