/*
 * smart_voice - AI voice assistant for ESP32-S3-LCD-EV-Board
 *
 * Wake word ("你好小智") -> record -> cloud ASR -> LLM (streamed) -> TTS.
 * Tap the screen (or press BOOT) to talk / interrupt playback.
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "nvs_flash.h"

#include "app_priv.h"
#include "board.h"
#include "board_audio.h"
#include "app_afe.h"
#include "app_wifi.h"
#include "app_ai.h"
#include "app_ui.h"

static const char *TAG = "main";

#define AI_TASK_STACK_SIZE  12288
#define STATE_TASK_STACK_SIZE 8192

QueueHandle_t g_app_queue;
volatile app_state_t g_app_state = APP_STATE_BOOT;
volatile bool g_ai_abort;

const char *app_state_name(app_state_t st)
{
    switch (st) {
    case APP_STATE_BOOT:      return "boot";
    case APP_STATE_IDLE:      return "idle";
    case APP_STATE_LISTENING: return "listening";
    case APP_STATE_THINKING:  return "thinking";
    case APP_STATE_SPEAKING:  return "speaking";
    case APP_STATE_ERROR:     return "error";
    default:                  return "?";
    }
}

static void set_state(app_state_t st)
{
    g_app_state = st;
    app_ui_set_state(st);
    ESP_LOGI(TAG, "state -> %s", app_state_name(st));
}

static void show_error(const char *msg)
{
    app_ui_assistant_begin();
    app_ui_assistant_append(msg);
    app_ui_assistant_end();
    vTaskDelay(pdMS_TO_TICKS(2500));
}

static void on_asr_text(const char *text)
{
    app_ui_user_msg(text);
    set_state(APP_STATE_THINKING);
}

static void on_llm_start(void)
{
    app_ui_assistant_begin();
}

static void on_llm_text(const char *chunk)
{
    app_ui_assistant_append(chunk);
}

static void on_speaking(void)
{
    set_state(APP_STATE_SPEAKING);
}

static bool on_local_command(const char *text)
{
    uint8_t r = 0, g = 0, b = 0;
    const char *reply = NULL;
    bool cjdh11b = strstr(text, "CJDH") || strstr(text, "cjdh") ||
                   strstr(text, "11B") || strstr(text, "11b") ||
                   strstr(text, "5脚") || strstr(text, "五脚");
    bool has_red = strstr(text, "红") != NULL;
    bool has_green = strstr(text, "绿") != NULL;
    bool has_blue = strstr(text, "蓝") != NULL;

    if (cjdh11b && (strstr(text, "关") || strstr(text, "停") || strstr(text, "关闭"))) {
        if (board_cjdh11b_set(false) != ESP_OK) {
            reply = "CJDH11B 控制失败，请检查 GPIO5 连接";
        } else {
            reply = "好的，已关闭 CJDH11B，GPIO5 输出低电平";
        }
    } else if (cjdh11b && (strstr(text, "开") || strstr(text, "启") || strstr(text, "打开"))) {
        if (board_cjdh11b_set(true) != ESP_OK) {
            reply = "CJDH11B 控制失败，请检查 GPIO5 连接";
        } else {
            reply = "好的，已打开 CJDH11B，GPIO5 输出高电平";
        }
    } else if (strstr(text, "关灯") || strstr(text, "关闭灯") || strstr(text, "熄灭")) {
        reply = "好的，已关闭灯光";
    } else if (strstr(text, "白") || strstr(text, "打开灯") || strstr(text, "开灯")) {
        r = 255;
        g = 255;
        b = 255;
        reply = "好的，已打开白灯";
    } else if (strstr(text, "黄")) {
        r = 255;
        g = 255;
        reply = "好的，已切换为黄灯";
    } else if (strstr(text, "紫")) {
        r = 255;
        b = 255;
        reply = "好的，已切换为紫灯";
    } else if (strstr(text, "青")) {
        g = 255;
        b = 255;
        reply = "好的，已切换为青灯";
    } else if (has_red || has_green || has_blue) {
        r = has_red ? 255 : 0;
        g = has_green ? 255 : 0;
        b = has_blue ? 255 : 0;
        if (has_red && has_green && has_blue) {
            reply = "好的，已打开白灯";
        } else if (has_red && has_green) {
            reply = "好的，已切换为黄灯";
        } else if (has_red && has_blue) {
            reply = "好的，已切换为紫灯";
        } else if (has_green && has_blue) {
            reply = "好的，已切换为青灯";
        } else if (has_red) {
            reply = "好的，已打开红灯";
        } else if (has_green) {
            reply = "好的，已打开绿灯";
        } else {
            reply = "好的，已打开蓝灯";
        }
    } else {
        return false;
    }

    if (board_rgb_set(r, g, b) != ESP_OK) {
        reply = "灯光控制失败，请检查 RGB 灯连接";
    }
    app_ui_assistant_begin();
    app_ui_assistant_append(reply);
    app_ui_assistant_end();
    set_state(APP_STATE_SPEAKING);
    esp_err_t err = ai_speak_text(reply);
    if (err != ESP_OK && err != AI_ERR_ABORTED) {
        ESP_LOGW(TAG, "local command TTS failed: %s", esp_err_to_name(err));
    }
    return true;
}

static bool ai_aborted(void);

static const ai_callbacks_t s_ai_cbs = {
    .on_asr_text = on_asr_text,
    .on_llm_start = on_llm_start,
    .on_llm_text = on_llm_text,
    .on_speaking = on_speaking,
    .aborted = ai_aborted,
    .on_local_command = on_local_command,
};

static QueueHandle_t s_ai_jobs;
static int16_t *s_ai_pcm;
static volatile bool s_ai_running;
static int s_pending_samples;

static bool ai_aborted(void)
{
    return g_ai_abort;
}

static esp_err_t ai_round(const int16_t *pcm, int samples)
{
    static char *final_text;   /* PSRAM */
    if (!final_text) {
        final_text = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    }
    if (!final_text) {
        return ESP_ERR_NO_MEM;
    }
    final_text[0] = 0;
    g_ai_abort = false;

    esp_err_t err = ai_converse(pcm, samples, &s_ai_cbs,
                                final_text, 8192);
    app_ui_assistant_end();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "assistant: %s", final_text);
    } else if (err == AI_ERR_ABORTED) {
        ESP_LOGW(TAG, "interrupted");
    } else if (err == AI_ERR_NO_SPEECH) {
        show_error("没有听清，请再说一遍");
    } else if (err == AI_ERR_LOCAL_HANDLED) {
        ESP_LOGI(TAG, "local hardware command handled");
    } else if (!app_wifi_is_connected()) {
        show_error("网络未连接，请检查 WiFi 配置");
    } else {
        ESP_LOGE(TAG, "ai round failed: %s", esp_err_to_name(err));
        show_error("AI 服务暂不可用，请稍后再试");
    }
    return err;
}

static void ai_task(void *arg)
{
    (void)arg;
    int samples;
    while (xQueueReceive(s_ai_jobs, &samples, portMAX_DELAY) == pdTRUE) {
        esp_err_t err = ai_round(s_ai_pcm, samples);
        app_post_event(APP_EVT_AI_DONE, (int32_t)err);
    }
}

static bool start_ai_job(int samples)
{
    size_t max_samples = CONFIG_SMART_VOICE_RECORD_MAX_MS * APP_AUDIO_SAMPLE_RATE / 1000;
    if (!s_ai_pcm || s_ai_running || !g_rec_buf || samples <= 0) {
        return false;
    }
    if ((size_t)samples > max_samples) {
        samples = (int)max_samples;
    }
    memcpy(s_ai_pcm, g_rec_buf, (size_t)samples * sizeof(int16_t));
    s_ai_running = true;
    g_ai_abort = false;
    if (xQueueSend(s_ai_jobs, &samples, 0) != pdTRUE) {
        s_ai_running = false;
        return false;
    }
    set_state(APP_STATE_THINKING);
    return true;
}

static void state_task(void *arg)
{
    (void)arg;
    while (true) {
        app_event_t ev;
        if (xQueueReceive(g_app_queue, &ev, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (ev.id) {
        case APP_EVT_WAKE:
            if (g_app_state == APP_STATE_IDLE) {
                set_state(APP_STATE_LISTENING);
                board_audio_beep();
            }
            break;

        case APP_EVT_RECORD_DONE:
            if (g_app_state == APP_STATE_LISTENING) {
                if (s_ai_running) {
                    /* The previous cloud round is being cancelled.  Keep the
                     * fresh recording in g_rec_buf and dispatch it after the
                     * worker acknowledges the cancellation. */
                    s_pending_samples = ev.arg0;
                } else if (!start_ai_job(ev.arg0)) {
                    set_state(APP_STATE_IDLE);
                    show_error("语音任务暂时繁忙，请稍后再试");
                }
            }
            break;

        case APP_EVT_RECORD_ABORT:
            if (g_app_state == APP_STATE_LISTENING) {
                s_pending_samples = 0;
                set_state(APP_STATE_IDLE);
            }
            break;

        case APP_EVT_AI_DONE:
            s_ai_running = false;
            if (g_app_state == APP_STATE_LISTENING) {
                if (s_pending_samples > 0) {
                    int pending = s_pending_samples;
                    s_pending_samples = 0;
                    if (!start_ai_job(pending)) {
                        set_state(APP_STATE_IDLE);
                    }
                }
            } else if (g_app_state == APP_STATE_THINKING ||
                       g_app_state == APP_STATE_SPEAKING) {
                set_state(APP_STATE_IDLE);
            }
            break;

        case APP_EVT_TOUCH:
            switch (g_app_state) {
            case APP_STATE_IDLE:            /* start a round manually */
                set_state(APP_STATE_LISTENING);
                app_afe_manual_wake();
                break;
            case APP_STATE_LISTENING:       /* cancel recording */
                app_afe_cancel();
                break;
            case APP_STATE_THINKING:
            case APP_STATE_SPEAKING:        /* barge-in */
                g_ai_abort = true;
                s_pending_samples = 0;
                set_state(APP_STATE_LISTENING);
                app_afe_manual_wake();
                break;
            default:
                break;
            }
            break;
        }
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    g_app_queue = xQueueCreate(16, sizeof(app_event_t));

    ESP_ERROR_CHECK(board_init());

    /* Display first: it also brings up the I2C bus the codecs sit on. */
    app_ui_init();
    ESP_ERROR_CHECK(board_rgb_set(0, 0, 0));
    ESP_ERROR_CHECK(board_audio_init());

    app_wifi_start();

    ret = app_afe_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio front-end init failed: %s", esp_err_to_name(ret));
        app_ui_set_state(APP_STATE_ERROR);
        return;
    }

    size_t max_samples = CONFIG_SMART_VOICE_RECORD_MAX_MS * APP_AUDIO_SAMPLE_RATE / 1000;
    s_ai_pcm = heap_caps_malloc(max_samples * sizeof(int16_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ai_pcm) {
        ESP_LOGE(TAG, "AI audio snapshot allocation failed");
        app_ui_set_state(APP_STATE_ERROR);
        return;
    }
    s_ai_jobs = xQueueCreate(1, sizeof(int));
    if (!s_ai_jobs) {
        ESP_LOGE(TAG, "AI job queue creation failed");
        app_ui_set_state(APP_STATE_ERROR);
        return;
    }

    BaseType_t ai_ret = xTaskCreatePinnedToCoreWithCaps(
        ai_task, "ai_worker", AI_TASK_STACK_SIZE, NULL, 3, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ai_ret != pdPASS) {
        ESP_LOGE(TAG, "AI worker creation failed");
        app_ui_set_state(APP_STATE_ERROR);
        return;
    }

    BaseType_t state_ret = xTaskCreatePinnedToCoreWithCaps(
        state_task, "state", STATE_TASK_STACK_SIZE, NULL, 4, NULL, 1,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (state_ret != pdPASS) {
        ESP_LOGE(TAG, "state task creation failed; refusing to enter reboot loop");
        app_ui_set_state(APP_STATE_ERROR);
        return;
    }

    set_state(APP_STATE_IDLE);
    ESP_LOGI(TAG, "smart_voice ready - say the wake word");
}
