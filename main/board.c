/*
 * smart_voice - board helpers: WS2812 status LED, BOOT button, PA control
 * SPDX-License-Identifier: MIT
 */
#include "board.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_io_expander.h"
#include "bsp/esp-bsp.h"
#include "led_strip.h"

#include "app_priv.h"

static const char *TAG = "board";

/* TCA9554 P0 drives the NS4150 power amplifier enable line. */
#define BOARD_PA_EXPANDER_PIN   IO_EXPANDER_PIN_NUM_0
#define BOARD_RGB_GREEN_PIN     IO_EXPANDER_PIN_NUM_1
#define BOARD_RGB_RED_PIN       IO_EXPANDER_PIN_NUM_2
#define BOARD_RGB_BLUE_PIN      IO_EXPANDER_PIN_NUM_3

#define BOARD_LED_GPIO          GPIO_NUM_4
/* P4/EX_IO4 is the first free TCA9554 pin in the supplied schematic.  P0 is
 * the amplifier enable and P1/P2/P3 are reserved for the external RGB LED. */
#define BOARD_CJDH11B_EXPANDER_PIN IO_EXPANDER_PIN_NUM_4

static esp_io_expander_handle_t s_expander;
static led_strip_handle_t s_led;
static bool s_pa_configured;
static bool s_rgb_configured;
static bool s_cjdh11b_configured;

esp_err_t board_pa_enable(bool enable)
{
    if (s_expander == NULL) {
        s_expander = bsp_io_expander_init();
        ESP_RETURN_ON_FALSE(s_expander != NULL, ESP_FAIL, TAG, "io expander init failed");
    }
    if (!s_pa_configured) {
        ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_expander, BOARD_PA_EXPANDER_PIN,
                                                    IO_EXPANDER_OUTPUT),
                            TAG, "PA pin direction failed");
        s_pa_configured = true;
    }
    return esp_io_expander_set_level(s_expander, BOARD_PA_EXPANDER_PIN, enable);
}

esp_err_t board_rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_expander == NULL) {
        s_expander = bsp_io_expander_init();
        ESP_RETURN_ON_FALSE(s_expander != NULL, ESP_FAIL, TAG, "io expander init failed");
    }
    if (!s_rgb_configured) {
        uint32_t rgb_mask = BOARD_RGB_GREEN_PIN | BOARD_RGB_RED_PIN | BOARD_RGB_BLUE_PIN;
        ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_expander, rgb_mask,
                                                    IO_EXPANDER_OUTPUT),
                            TAG, "RGB pins direction failed");
        s_rgb_configured = true;
    }

    /* The expander API takes one level for the whole pin mask, so set each
     * channel separately. Common-cathode output: non-zero means on. */
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, BOARD_RGB_GREEN_PIN,
                                                  g > 0), TAG, "green level failed");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, BOARD_RGB_RED_PIN,
                                                  r > 0), TAG, "red level failed");
    return esp_io_expander_set_level(s_expander, BOARD_RGB_BLUE_PIN, b > 0);
}

esp_err_t board_cjdh11b_set(bool enable)
{
    if (s_expander == NULL) {
        s_expander = bsp_io_expander_init();
        ESP_RETURN_ON_FALSE(s_expander != NULL, ESP_FAIL, TAG, "io expander init failed");
    }
    if (!s_cjdh11b_configured) {
        ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_expander,
                                                    BOARD_CJDH11B_EXPANDER_PIN,
                                                    IO_EXPANDER_OUTPUT),
                            TAG, "CJDH11B P4 direction failed");
        s_cjdh11b_configured = true;
    }
    return esp_io_expander_set_level(s_expander, BOARD_CJDH11B_EXPANDER_PIN,
                                     enable ? 1 : 0);
}

esp_err_t board_led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (s_led == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (r == 0 && g == 0 && b == 0) {
        return led_strip_clear(s_led);
    }
    return led_strip_set_pixel(s_led, 0, r, g, b) == ESP_OK
           ? led_strip_refresh(s_led) : ESP_FAIL;
}

/* Status LED patterns follow the global app state. */
static void led_task(void *arg)
{
    int frame = 0;
    while (true) {
        app_state_t st = g_app_state;
        int breath = (int)(110.0 * (0.5 - 0.5 * cosf(2 * (float)M_PI * frame / 60.0f)));
        frame = (frame + 1) % 60;

        switch (st) {
        case APP_STATE_IDLE:
            board_led_rgb(breath / 6, breath / 4, breath / 4);   /* dim cyan breath */
            break;
        case APP_STATE_LISTENING:
            board_led_rgb(0, (uint8_t)(60 + breath), 10);        /* green pulse */
            break;
        case APP_STATE_THINKING:
            board_led_rgb(frame < 30 ? 10 : 60, 10, frame < 30 ? 80 : 10); /* blue blink */
            break;
        case APP_STATE_SPEAKING:
            board_led_rgb(0, breath / 2, (uint8_t)(40 + breath / 2)); /* cyan breath */
            break;
        case APP_STATE_ERROR:
            board_led_rgb(frame < 30 ? 120 : 0, 0, 0);           /* red blink */
            break;
        default:
            board_led_rgb(10, 10, 40);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(33));
    }
}

void board_led_task_start(void)
{
    xTaskCreatePinnedToCoreWithCaps(led_task, "led", 3072, NULL, 2, NULL, 0,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void button_cb(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    app_post_event(APP_EVT_TOUCH, 0);   /* treat button as "tap" */
}

esp_err_t board_init(void)
{
    /* WS2812 on GPIO4, GRB order, RMT backend */
    led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led),
                        TAG, "led strip init failed");
    led_strip_clear(s_led);

    /* BOOT button (GPIO0, active low) doubles as an interrupt button. */
    button_handle_t btns[1] = { NULL };
    ESP_RETURN_ON_ERROR(bsp_iot_button_create(btns, NULL, 1), TAG, "button init failed");
    ESP_RETURN_ON_ERROR(iot_button_register_cb(btns[0], BUTTON_PRESS_DOWN, NULL, button_cb, NULL),
                        TAG, "button cb failed");

    board_led_task_start();
    return ESP_OK;
}
