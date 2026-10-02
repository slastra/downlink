idf_component_register(
    SRCS "main.c" "settings.c" "stream.c" "ogg_sniff.c" "player.cpp" "audio_out.c" "drift.c" "mixer.c" "announce.cpp" "crash.c"
    INCLUDE_DIRS "."
    REQUIRES netlink provision led uplink updater esp_app_format espressif__cjson nvs_flash esp_timer esp_http_client esp_driver_i2s esp_driver_gpio mbedtls
             esp_psram esphome__micro-opus spi_flash esp_system
)

# crash.c records why a crash happened before IDF's handler resets the chip.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
