/*
 * smart_voice - ESP-SR audio front end: wake word + VAD + recorder
 *
 * Two tasks (same pattern as esp-skainet examples):
 *   feed  - reads raw mic frames from I2S and pushes them into the AFE
 *   fetch - pulls cleaned mono audio from the AFE, watches for the wake
 *           word and records speech between wake and end-of-silence
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_afe.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"

#include "app_priv.h"
#include "board_audio.h"
#include "app_local.h"

static const char *TAG = "afe";

#define REC_MIN_MS      300
#define SKIP_AFTER_WAKE_MS  300   /* swallow the wake chime picked up by the mic */
#define WAKE_COOLDOWN_MS    800   /* ignore wake words right after TTS playback */

int16_t *g_rec_buf;
int g_rec_samples;

volatile int g_mic_rms;
volatile int g_mic_vad;

static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;

typedef enum {
    REC_ST_MONITOR = 0,   /* waiting for the wake word */
    REC_ST_RECORDING,     /* recording speech after wake */
} rec_mode_t;

static volatile rec_mode_t s_mode;
static volatile bool s_manual_wake_pending;

static TaskHandle_t s_feed_task;
static TaskHandle_t s_fetch_task;

void app_afe_manual_wake(void)
{
    g_rec_samples = 0;
    s_manual_wake_pending = true;
    s_mode = REC_ST_RECORDING;
}

void app_afe_cancel(void)
{
    if (s_mode == REC_ST_RECORDING) {
        s_manual_wake_pending = false;
        s_mode = REC_ST_MONITOR;
        app_post_event(APP_EVT_RECORD_ABORT, 0);
    }
}

static void feed_task(void *arg)
{
    (void)arg;
    int chunk = s_afe_handle->get_feed_chunksize(s_afe_data);
    int nch = s_afe_handle->get_feed_channel_num(s_afe_data);
    int16_t *buf = heap_caps_malloc(chunk * nch * sizeof(int16_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(buf);
    ESP_LOGI(TAG, "feed task started: chunk=%d samples, %d ch", chunk, nch);

    int read_errors = 0;
    while (true) {
        esp_err_t err = board_audio_read(buf, chunk, portMAX_DELAY);
        if (err != ESP_OK) {
            if (read_errors < 10 || (read_errors % 100) == 0) {
                ESP_LOGE(TAG, "mic read failed (%d): %s", read_errors + 1,
                         esp_err_to_name(err));
            }
            read_errors++;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        read_errors = 0;
        s_afe_handle->feed(s_afe_data, buf);
    }
}

static void fetch_task(void *arg)
{
    (void)arg;
    bool speech_seen = false;
    int silence_ms = 0;
    int listen_ms = 0;
    int skip_ms = 0;
    int idle_ms = 0;

    /* real per-fetch duration (esp-sr uses 64 ms chunks on S3, not 16 ms) */
    int chunk_ms = s_afe_handle->get_fetch_chunksize(s_afe_data) * 1000
                   / APP_AUDIO_SAMPLE_RATE;
    int dbg_ticks = 0;

    while (true) {
        afe_fetch_result_t *res = s_afe_handle->fetch_with_delay(s_afe_data, pdMS_TO_TICKS(500));
        if (!res || res->ret_value != ESP_OK) {
            continue;
        }

        /* mic level diagnostics (RMS of the fetched mono stream) */
        int n = res->data_size / (int)sizeof(int16_t);
        int64_t acc = 0;
        for (int i = 0; i < n; i++) {
            acc += (int32_t)res->data[i] * res->data[i];
        }
        g_mic_rms = n ? (int)(sqrtf((float)acc / n)) : 0;
        g_mic_vad = (res->vad_state == VAD_SPEECH);

#if CONFIG_SMART_VOICE_DEBUG_MIC
        if (++dbg_ticks >= 15) {   /* ~1 s at 64 ms chunks */
            dbg_ticks = 0;
            ESP_LOGI(TAG, "mic rms=%5d vad=%d wn_state=%d", g_mic_rms, g_mic_vad,
                     res->wakeup_state);
        }
#endif

        switch (s_mode) {
        case REC_ST_MONITOR:
            listen_ms = 0;
            if (g_app_state != APP_STATE_IDLE) {
                idle_ms = 0;
                break;
            }
            idle_ms += chunk_ms;
            if (idle_ms < WAKE_COOLDOWN_MS) {
                break;
            }
            if (res->wakeup_state == WAKENET_DETECTED) {
                g_rec_samples = 0;
                speech_seen = false;
                silence_ms = 0;
                skip_ms = SKIP_AFTER_WAKE_MS;
                s_mode = REC_ST_RECORDING;
                app_post_event(APP_EVT_WAKE, 0);
            }
            break;

        case REC_ST_RECORDING: {
            if (s_manual_wake_pending) {
                /* Reset local VAD bookkeeping as well as the shared sample
                 * count.  Without this, a manual tap after a previous round
                 * could inherit stale silence/speech state and end early. */
                s_manual_wake_pending = false;
                speech_seen = false;
                silence_ms = 0;
                listen_ms = 0;
                skip_ms = 0;
            }
            listen_ms += chunk_ms;
            if (skip_ms > 0) {
                skip_ms -= chunk_ms;
                break;
            }

            int samples = res->data_size / sizeof(int16_t);
            if (app_local_feed(res->data, samples)) {
                /* MultiNet has already posted the command event.  Stop this
                 * recording immediately so the next wake starts cleanly. */
                s_mode = REC_ST_MONITOR;
                g_rec_samples = 0;
                break;
            }

            size_t max_samples = CONFIG_SMART_VOICE_RECORD_MAX_MS * APP_AUDIO_SAMPLE_RATE / 1000;
            int copy = samples;
            if (g_rec_samples + copy > (int)max_samples) {
                copy = (int)max_samples - g_rec_samples;
            }
            if (copy > 0 && g_rec_buf) {
                memcpy(g_rec_buf + g_rec_samples, res->data, copy * sizeof(int16_t));
                g_rec_samples += copy;
            }

            if (res->vad_state == VAD_SPEECH) {
                speech_seen = true;
                silence_ms = 0;
            } else if (speech_seen) {
                silence_ms += chunk_ms;
            }

            bool too_long = g_rec_samples >= (int)max_samples;
            bool long_silence = speech_seen && silence_ms >= CONFIG_SMART_VOICE_SILENCE_MS;
            bool no_speech = !speech_seen && listen_ms >= CONFIG_SMART_VOICE_WAKE_TIMEOUT_MS;

            if (too_long || long_silence || no_speech) {
                s_mode = REC_ST_MONITOR;
                if (speech_seen && g_rec_samples >= REC_MIN_MS * APP_AUDIO_SAMPLE_RATE / 1000) {
                    app_post_event(APP_EVT_RECORD_DONE, g_rec_samples);
                } else {
                    app_post_event(APP_EVT_RECORD_ABORT, 0);
                }
            }
            break;
        }
        }
    }
}

esp_err_t app_afe_start(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(models != NULL, ESP_FAIL, TAG, "no 'model' partition / srmodels not flashed");

    afe_config_t *cfg = afe_config_init("MM", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_FAIL, TAG, "afe config init failed");
    cfg->aec_init = false;      /* no AEC loopback wiring used in this firmware */
    cfg->vad_init = true;
    cfg->wakenet_init = true;
    /* Keep the large AFE working buffers in PSRAM.  The display, WiFi and
     * TLS stacks all need scarce internal SRAM at runtime; the AFE remains
     * real-time because its feed buffer and task stacks are explicitly kept
     * DMA/PSRAM-safe below. */
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    ESP_LOGI(TAG, "wakenet: %s", cfg->wakenet_model_name ? cfg->wakenet_model_name : "(none)");

    s_afe_handle = esp_afe_handle_from_config(cfg);
    ESP_RETURN_ON_FALSE(s_afe_handle != NULL, ESP_FAIL, TAG, "afe handle");
    s_afe_data = s_afe_handle->create_from_config(cfg);
    ESP_RETURN_ON_FALSE(s_afe_data != NULL, ESP_FAIL, TAG, "afe create");

    g_rec_buf = heap_caps_malloc(CONFIG_SMART_VOICE_RECORD_MAX_MS * APP_AUDIO_SAMPLE_RATE / 1000
                                 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(g_rec_buf != NULL, ESP_ERR_NO_MEM, TAG, "rec buffer");

    const uint32_t task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    BaseType_t feed_ret = xTaskCreatePinnedToCoreWithCaps(feed_task, "afe_feed", 6144,
                                                          NULL, 5, &s_feed_task, 0,
                                                          task_stack_caps);
    BaseType_t fetch_ret = xTaskCreatePinnedToCoreWithCaps(fetch_task, "afe_fetch", 6144,
                                                           NULL, 5, &s_fetch_task, 1,
                                                           task_stack_caps);
    ESP_LOGI(TAG, "AFE tasks: feed=%s fetch=%s internal_free=%u psram_free=%u",
             feed_ret == pdPASS ? "ok" : "FAIL",
             fetch_ret == pdPASS ? "ok" : "FAIL",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_RETURN_ON_FALSE(feed_ret == pdPASS && fetch_ret == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "AFE task creation failed");
    return ESP_OK;
}
