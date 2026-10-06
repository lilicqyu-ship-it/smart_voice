/*
 * smart_voice - LVGL UI: status panel, waveform, chat bubbles
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include "app_priv.h"

void app_ui_init(void);

/* Thread-safe setters (they take the LVGL lock themselves). */
void app_ui_set_state(app_state_t st);
void app_ui_set_wifi(bool connected, const char *ip);
void app_ui_user_msg(const char *text);
void app_ui_assistant_begin(void);
void app_ui_assistant_append(const char *text);
void app_ui_assistant_end(void);
void app_ui_clear_chat(void);
