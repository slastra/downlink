idf_component_register(
    SRCS "provision.c" "dns_hijack.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event esp_http_server nvs_flash esp_timer netlink lwip
)
