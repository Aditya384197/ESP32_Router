idf_component_register(
    SRCS "esp32_nat_router.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES driver esp_wifi esp_netif nvs_flash esp_http_server app_update
)
target_compile_options(__idf_main PRIVATE -O2)
