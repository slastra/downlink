idf_component_register(
    SRCS "uplink.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__mqtt espressif__cjson esp_event esp_netif nvs_flash esp_timer esp_app_format esp_system esp_hw_support
)
