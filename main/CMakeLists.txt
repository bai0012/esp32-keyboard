idf_component_register(
    SRCS
        "main.c"
        "buzzer.c"
        "macropad_hid.c"
        "touch_slider.c"
        "oled.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_pcnt esp_driver_touch_sens nvs_flash esp_wifi esp_event esp_netif led_strip
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(KEYMAP_CONFIG_YAML "${CMAKE_CURRENT_LIST_DIR}/../config/keymap_config.yaml")
    set(KEYMAP_CONFIG_GENERATOR "${CMAKE_CURRENT_LIST_DIR}/../tools/generate_keymap_header.py")
    set(KEYMAP_CONFIG_HEADER "${CMAKE_CURRENT_LIST_DIR}/keymap_config.h")

    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${KEYMAP_CONFIG_YAML}"
        "${KEYMAP_CONFIG_GENERATOR}"
    )

    if(DEFINED PYTHON)
        set(KEYMAP_PYTHON "${PYTHON}")
    else()
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        set(KEYMAP_PYTHON "${Python3_EXECUTABLE}")
    endif()

    add_custom_command(
        OUTPUT "${KEYMAP_CONFIG_HEADER}"
        COMMAND "${KEYMAP_PYTHON}" "${KEYMAP_CONFIG_GENERATOR}" --in "${KEYMAP_CONFIG_YAML}" --out "${KEYMAP_CONFIG_HEADER}"
        DEPENDS "${KEYMAP_CONFIG_YAML}" "${KEYMAP_CONFIG_GENERATOR}"
        COMMENT "Generating keymap_config.h from YAML"
        VERBATIM
    )

    add_custom_target(generate_keymap_config_header DEPENDS "${KEYMAP_CONFIG_HEADER}")
    add_dependencies(${COMPONENT_LIB} generate_keymap_config_header)
endif()
