/*
 * smart_voice - WiFi station + SNTP
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>

void app_wifi_start(void);
bool app_wifi_is_connected(void);
const char *app_wifi_ip(void);
