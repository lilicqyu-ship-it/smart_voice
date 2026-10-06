/*
 * smart_voice - offline fixed-command recognition and Chinese TTS
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

enum {
    APP_LOCAL_CMD_RED = 0,
    APP_LOCAL_CMD_GREEN,
    APP_LOCAL_CMD_BLUE,
    APP_LOCAL_CMD_WHITE,
    APP_LOCAL_CMD_YELLOW,
    APP_LOCAL_CMD_PURPLE,
    APP_LOCAL_CMD_CYAN,
    APP_LOCAL_CMD_LIGHT_OFF,
    APP_LOCAL_CMD_CJDH_ON,
    APP_LOCAL_CMD_CJDH_OFF,
};

/* Initialize MultiNet6 and the small on-device Chinese TTS voice. */
esp_err_t app_local_start(void);

/* Feed enhanced 16 kHz mono samples while the user is speaking.
 * Returns true when a command was detected and an APP_EVT_LOCAL_COMMAND was
 * posted to the application state task. */
bool app_local_feed(const int16_t *samples, int count);

/* Speak a short local confirmation through the board speaker. */
esp_err_t app_local_speak(const char *text);
