idf_component_register(SRCS "main.c" "bt_a2dp.c" "creeper_audio.c" "ui.cpp"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash esp_timer)
