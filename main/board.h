/*
 * smart_voice - board helpers: WS2812 status LED, BOOT button, PA control
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t board_init(void);

/* Power amplifier sits behind the TCA9554 IO expander (P0). */
esp_err_t board_pa_enable(bool enable);

/* External RGB LED on TCA9554: P1=G, P2=R, P3=B. */
esp_err_t board_rgb_set(uint8_t r, uint8_t g, uint8_t b);

/* CJDH11B control input is wired to ESP32 GPIO5. */
esp_err_t board_cjdh11b_set(bool enable);

/* Set the on-board WS2812 (GPIO4). */
esp_err_t board_led_rgb(uint8_t r, uint8_t g, uint8_t b);

/* Start the status LED breathing task (reads g_app_state). */
void board_led_task_start(void);
