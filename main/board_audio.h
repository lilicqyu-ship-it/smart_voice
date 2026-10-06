/*
 * smart_voice - audio path: ES7210 mic capture + ES8311 speaker playback
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_codec_dev.h"

esp_err_t board_audio_init(void);

/* Recording device (ES7210, stereo 16 kHz 16-bit). */
esp_codec_dev_handle_t board_audio_rec_dev(void);

/* Playback device (ES8311, stereo 16 kHz 16-bit). */
esp_codec_dev_handle_t board_audio_play_dev(void);

/* Read raw mic frames (interleaved stereo s16le). Blocking. */
esp_err_t board_audio_read(int16_t *buf, size_t samples_per_ch, size_t timeout_ms);

/* Play mono 16 kHz s16le samples on the speaker (blocking, abort-aware). */
esp_err_t board_audio_play_mono(const int16_t *mono, size_t samples, volatile bool *abort);

/* Short wake-up chime. */
esp_err_t board_audio_beep(void);

esp_err_t board_audio_set_volume(int volume);
