idf_component_register(
    SRCS
        "main.c"
        "buzzer.c"
        "macropad_hid.c"
        "oled_clock.c"
        "touch_slider.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_pcnt esp_driver_touch_sens nvs_flash esp_wifi esp_event esp_netif led_strip
)
