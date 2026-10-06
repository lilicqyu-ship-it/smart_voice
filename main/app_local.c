/*
 * smart_voice - offline fixed-command recognition and Chinese TTS
 *
 * MultiNet recognizes a small, explicit command vocabulary locally.  The
 * command IDs are intentionally stable because main.c maps them to hardware
 * actions.  No Wi-Fi, HTTP, ASR service, or LLM is used on this path.
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_local.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "model_path.h"
#include "esp_tts.h"
#include "esp_tts_voice_xiaole.h"

#include "app_priv.h"
#include "board_audio.h"

static const char *TAG = "local";

static const esp_mn_iface_t *s_mn;
static model_iface_data_t *s_mn_data;
static srmodel_list_t *s_sr_models;
static int16_t *s_mn_buf;
static int s_mn_buf_count;
static esp_tts_handle_t s_tts;
static esp_tts_voice_t *s_voice;
static const void *s_voice_data;
static esp_partition_mmap_handle_t s_voice_mmap;

/* MultiNet6 uses pinyin/grapheme tokens.  Variants share an ID so the user
 * can say either a natural phrase or the short color name. */
static const struct {
    int id;
    const char *phrase;
} s_commands[] = {
    { APP_LOCAL_CMD_RED,        "da kai hong deng" },
    { APP_LOCAL_CMD_RED,        "hong deng" },
    { APP_LOCAL_CMD_GREEN,      "da kai lv deng" },
    { APP_LOCAL_CMD_GREEN,      "lv deng" },
    { APP_LOCAL_CMD_BLUE,       "da kai lan deng" },
    { APP_LOCAL_CMD_BLUE,       "lan deng" },
    { APP_LOCAL_CMD_WHITE,      "da kai bai deng" },
    { APP_LOCAL_CMD_WHITE,      "bai deng" },
    { APP_LOCAL_CMD_YELLOW,     "da kai huang deng" },
    { APP_LOCAL_CMD_YELLOW,     "huang deng" },
    { APP_LOCAL_CMD_PURPLE,     "da kai zi deng" },
    { APP_LOCAL_CMD_PURPLE,     "zi deng" },
    { APP_LOCAL_CMD_CYAN,       "da kai qing deng" },
    { APP_LOCAL_CMD_CYAN,       "qing deng" },
    { APP_LOCAL_CMD_LIGHT_OFF,  "guan bi deng" },
    { APP_LOCAL_CMD_LIGHT_OFF,  "guan deng" },
    { APP_LOCAL_CMD_CJDH_ON,    "da kai she bei" },
    { APP_LOCAL_CMD_CJDH_OFF,   "guan bi she bei" },
};

esp_err_t app_local_start(void)
{
    s_sr_models = esp_srmodel_init("model");
    if (!s_sr_models) {
        ESP_LOGE(TAG, "cannot load model partition");
        return ESP_ERR_NOT_FOUND;
    }

    char *mn_name = esp_srmodel_filter(s_sr_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) {
        ESP_LOGE(TAG, "no Chinese MultiNet model in model partition");
        return ESP_ERR_NOT_FOUND;
    }

    s_mn = esp_mn_handle_from_name(mn_name);
    if (!s_mn) {
        ESP_LOGE(TAG, "MultiNet model handle not found: %s", mn_name);
        return ESP_ERR_NOT_FOUND;
    }

    s_mn_data = s_mn->create(mn_name, 6000);
    if (!s_mn_data) {
        ESP_LOGE(TAG, "MultiNet6 create failed");
        return ESP_ERR_NO_MEM;
    }

    /* The PSRAM/flash mode leaves more internal SRAM for the display and
     * codec while retaining good command latency on the S3. */
    if (s_mn->switch_loader_mode) {
        s_mn->switch_loader_mode(s_mn_data, ESP_MN_LOAD_FROM_PSRAM_FLASH);
    }

    ESP_RETURN_ON_ERROR(esp_mn_commands_alloc(s_mn, s_mn_data), TAG,
                        "command list allocation failed");
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        esp_err_t err = esp_mn_commands_add(s_commands[i].id, s_commands[i].phrase);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "skip command id=%d phrase=%s: %s",
                     s_commands[i].id, s_commands[i].phrase, esp_err_to_name(err));
        }
    }
    esp_mn_error_t *errors = esp_mn_commands_update();
    if (errors) {
        ESP_LOGW(TAG, "some local command phrases were rejected: %d", errors->num);
    }
    esp_mn_active_commands_print();

    int chunk = s_mn->get_samp_chunksize(s_mn_data);
    if (chunk <= 0) {
        ESP_LOGE(TAG, "invalid MultiNet sample chunk: %d", chunk);
        return ESP_FAIL;
    }
    s_mn_buf = heap_caps_malloc((size_t)chunk * sizeof(int16_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_mn_buf) {
        return ESP_ERR_NO_MEM;
    }

    /* esp_tts_voice_xiaole is a template: its syllable tables are in the
     * firmware, while the AMR-WB syllable data lives in voice_data. */
    const esp_partition_t *voice_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "voice_data");
    if (!voice_part) {
        ESP_LOGE(TAG, "voice_data partition is missing");
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_partition_mmap(voice_part, 0, voice_part->size,
                                       ESP_PARTITION_MMAP_DATA,
                                       &s_voice_data, &s_voice_mmap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "voice_data mmap failed: %s", esp_err_to_name(err));
        return err;
    }
    s_voice = esp_tts_voice_set_init(&esp_tts_voice_xiaole,
                                     (void *)s_voice_data);
    if (!s_voice) {
        ESP_LOGE(TAG, "xiaole voice data init failed");
        return ESP_ERR_NO_MEM;
    }
    s_tts = esp_tts_create(s_voice);
    if (!s_tts) {
        ESP_LOGE(TAG, "local Chinese TTS create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "offline voice ready: model=%s rate=%d Hz chunk=%d, tts=local",
             mn_name, s_mn->get_samp_rate(s_mn_data), chunk);
    return ESP_OK;
}

bool app_local_feed(const int16_t *samples, int count)
{
    if (!s_mn || !s_mn_data || !s_mn_buf || !samples || count <= 0) {
        return false;
    }

    int chunk = s_mn->get_samp_chunksize(s_mn_data);
    bool detected = false;
    while (count > 0) {
        int copy = chunk - s_mn_buf_count;
        if (copy > count) {
            copy = count;
        }
        memcpy(s_mn_buf + s_mn_buf_count, samples, (size_t)copy * sizeof(int16_t));
        s_mn_buf_count += copy;
        samples += copy;
        count -= copy;

        if (s_mn_buf_count == chunk) {
            esp_mn_state_t state = s_mn->detect(s_mn_data, s_mn_buf);
            s_mn_buf_count = 0;
            if (state == ESP_MN_STATE_DETECTED) {
                esp_mn_results_t *result = s_mn->get_results(s_mn_data);
                if (result && result->num > 0) {
                    ESP_LOGI(TAG, "offline command id=%d phrase=%s prob=%.2f",
                             result->command_id[0], result->string, result->prob[0]);
                    app_post_event(APP_EVT_LOCAL_COMMAND, result->command_id[0]);
                    detected = true;
                }
                s_mn->clean(s_mn_data);
                break;
            }
        }
    }
    return detected;
}

esp_err_t app_local_speak(const char *text)
{
    if (!s_tts || !text || !text[0]) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_tts_parse_chinese(s_tts, text)) {
        ESP_LOGW(TAG, "local TTS cannot parse: %s", text);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ESP_OK;
    while (true) {
        int len = 0;
        short *pcm = esp_tts_stream_play(s_tts, &len, 3);
        if (!pcm || len <= 0) {
            break;
        }
        ret = board_audio_play_mono(pcm, (size_t)len, &g_ai_abort);
        if (ret != ESP_OK) {
            break;
        }
    }
    esp_tts_stream_reset(s_tts);
    return ret;
}
