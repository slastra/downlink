idf_component_register(
    SRCS "main.c" "settings.c" "stream.c" "ogg_sniff.c" "player.cpp" "audio_out.c" "drift.c" "mixer.c" "announce.cpp"
    INCLUDE_DIRS "."
    REQUIRES netlink provision led uplink espressif__cjson nvs_flash esp_timer esp_http_client esp_driver_i2s esp_driver_gpio mbedtls
             esp_psram esphome__micro-opus
)
