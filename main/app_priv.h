/*
 * smart_voice - shared application types
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

/* Board audio runs at one fixed format because the ESP32-S3 I2S TX and RX
 * channels share the same bit clock / word select pins on this board. */
#define APP_AUDIO_SAMPLE_RATE   16000
#define APP_AUDIO_CHANNELS      2
#define APP_AUDIO_BITS          16

typedef enum {
    APP_STATE_BOOT = 0,
    APP_STATE_IDLE,         /* AFE armed, waiting for wake word */
    APP_STATE_LISTENING,    /* recording user speech */
    APP_STATE_THINKING,     /* ASR / LLM in progress */
    APP_STATE_SPEAKING,     /* TTS playback */
    APP_STATE_ERROR,
} app_state_t;

typedef enum {
    APP_EVT_WAKE = 0,       /* wake word detected (or PTT pressed) */
    APP_EVT_RECORD_DONE,    /* arg0: recorded sample count (mono, 16 kHz) */
    APP_EVT_RECORD_ABORT,   /* no usable speech captured */
    APP_EVT_TOUCH,          /* user tapped the screen */
    APP_EVT_AI_DONE,        /* arg0: ai_converse() result */
} app_event_id_t;

typedef struct {
    app_event_id_t id;
    int32_t arg0;
} app_event_t;

extern QueueHandle_t g_app_queue;
extern volatile app_state_t g_app_state;
extern volatile bool g_ai_abort;

static inline void app_post_event(app_event_id_t id, int32_t arg0)
{
    app_event_t e = { .id = id, .arg0 = arg0 };
    xQueueSend(g_app_queue, &e, 0);
}

const char *app_state_name(app_state_t st);
