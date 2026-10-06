# Main component sources
set(MAIN_SRCS
    "main.c"
    "board.c"
    "board_audio.c"
    "app_afe.c"
    "app_wifi.c"
    "app_ai.c"
    "app_ui.c"
    "fonts/font_cjk_20.c"
)

idf_component_register(
    SRCS ${MAIN_SRCS}
    INCLUDE_DIRS "." "fonts"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_http_client
        esp-tls
        mbedtls
        esp_timer
        cjson
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp-sr
        esp_codec_dev
        esp32_s3_lcd_ev_board
        led_strip
        button
)

# Generated font file includes "lvgl.h" directly
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)
