idf_component_register(SRCS "main.c" "bt_a2dp.c" "creeper_audio.c" "button.c" "leds.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash esp_driver_gpio esp_timer)
