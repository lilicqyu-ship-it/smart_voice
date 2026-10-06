/*
 * smart_voice - ESP-SR audio front end: wake word + VAD + recorder
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t app_afe_start(void);

/* Recorded speech (mono 16 kHz s16le), valid after APP_EVT_RECORD_DONE. */
extern int16_t *g_rec_buf;
extern int g_rec_samples;

/* Live mic diagnostics (updated every AFE fetch, ~64 ms). */
extern volatile int g_mic_rms;   /* RMS of the fetched mono audio */
extern volatile int g_mic_vad;   /* 1 while VAD reports speech */

/* Force a listening round as if the wake word had been spoken (button). */
void app_afe_manual_wake(void);

/* Cancel an ongoing recording (e.g. user tapped while listening). */
void app_afe_cancel(void);
