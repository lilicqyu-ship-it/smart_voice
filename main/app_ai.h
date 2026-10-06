/*
 * smart_voice - AI cloud pipeline: ASR -> LLM (SSE stream) -> TTS
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* ai_converse() was interrupted by the user (screen tap / button). */
#define AI_ERR_ABORTED      ((esp_err_t)0x3f0)
#define AI_ERR_NO_SPEECH    ((esp_err_t)0x3f1)
#define AI_ERR_LOCAL_HANDLED ((esp_err_t)0x3f2)

typedef struct {
    /* UI notifications, all called from the caller's task */
    void (*on_asr_text)(const char *text);      /* recognized user speech */
    void (*on_llm_start)(void);                 /* LLM request begins */
    void (*on_llm_text)(const char *chunk);     /* streamed LLM delta */
    void (*on_speaking)(void);                  /* a TTS sentence starts */
    bool (*aborted)(void);                      /* return true to interrupt */
    bool (*on_local_command)(const char *text); /* handled without LLM */
} ai_callbacks_t;

esp_err_t ai_converse(const int16_t *pcm, int samples, const ai_callbacks_t *cb,
                      char *out_text, size_t out_size);

/* Forget previous conversation turns. */
void ai_reset_history(void);

/* Synthesize a short local response, e.g. for hardware commands. */
esp_err_t ai_speak_text(const char *text);
