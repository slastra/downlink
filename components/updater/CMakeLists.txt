idf_component_register(
    SRCS "updater.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_https_ota esp_http_client app_update esp_partition esp_timer mbedtls nvs_flash esp_app_format espressif__cjson
)
