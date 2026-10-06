/*
 * smart_voice - WiFi station + SNTP
 * SPDX-License-Identifier: MIT
 */
#include "app_wifi.h"

#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "app_priv.h"
#include "app_ui.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT  BIT0

static EventGroupHandle_t s_wifi_events;
static esp_timer_handle_t s_retry_timer;
static bool s_sntp_started;
static char s_ip[16];

bool app_wifi_is_connected(void)
{
    return s_wifi_events && (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT);
}

const char *app_wifi_ip(void)
{
    return s_ip;
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    ESP_LOGI(TAG, "got ip: %s", s_ip);

    /* TLS certificate checks want a roughly correct clock. */
    if (!s_sntp_started) {
        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
        esp_err_t sntp_ret = esp_netif_sntp_init(&sntp_cfg);
        if (sntp_ret == ESP_OK) {
            s_sntp_started = true;
        } else {
            ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(sntp_ret));
        }
    }
    setenv("TZ", "CST-8", 1);
    tzset();

    app_ui_set_wifi(true, s_ip);
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (!app_wifi_is_connected()) {
        esp_wifi_connect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        app_ui_set_wifi(false, NULL);
        ESP_LOGW(TAG, "disconnected, retry scheduled");
        if (s_retry_timer && !esp_timer_is_active(s_retry_timer)) {
            esp_timer_start_once(s_retry_timer, 3000000);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        on_got_ip(arg, base, id, data);
    }
}

void app_wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();

    const esp_timer_create_args_t retry_timer_args = {
        .callback = reconnect_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_timer_args, &s_retry_timer));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wifi_config = { 0 };
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_SMART_VOICE_WIFI_SSID,
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_SMART_VOICE_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi connecting to \"%s\"", CONFIG_SMART_VOICE_WIFI_SSID);
}
