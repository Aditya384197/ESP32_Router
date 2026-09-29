idf_component_register(
    SRCS "esp32_nat_router.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_timer esp_wifi esp_netif esp_event nvs_flash esp_http_server lwip
)
target_compile_options(__idf_main PRIVATE
    -Wno-error=format-truncation
    -Wno-error=stringop-truncation
    -Wno-error=maybe-uninitialized)
