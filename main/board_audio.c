/*
 * smart_voice - audio path: ES7210 mic capture + ES8311 speaker playback
 *
 * The ESP32-S3-LCD-EV-Board wires one I2S controller to both codecs:
 *   ES8311 (DAC  -> speaker, I2C 0x18/8-bit 0x30)
 *   ES7210 (ADC <- dual mic,  I2C 0x41/8-bit 0x82)
 * TX and RX share BCLK/WS, so both run 16 kHz / stereo / 16-bit.
 *
 * SPDX-License-Identifier: MIT
 */
#include "board_audio.h"

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2s_std.h"
#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#include "app_priv.h"
#include "board.h"

static const char *TAG = "audio";

/* I2S pins, see ESP32-S3-LCD-EV-Board user guide (v1.5) */
#define BOARD_I2S_MCLK  GPIO_NUM_5
#define BOARD_I2S_BCLK  GPIO_NUM_16
#define BOARD_I2S_WS    GPIO_NUM_7
#define BOARD_I2S_DOUT  GPIO_NUM_6   /* -> ES8311 */
#define BOARD_I2S_DIN   GPIO_NUM_15  /* <- ES7210 */

/* 8-bit I2C addresses (esp_codec_dev shifts right by one internally) */
#define BOARD_ES8310_ADDR   ES8311_CODEC_DEFAULT_ADDR   /* 0x30 -> 7-bit 0x18 */
#define BOARD_ES7210_ADDR   0x82                        /* 0x82 -> 7-bit 0x41 (board strap) */

static i2s_chan_handle_t s_tx_chan;
static i2s_chan_handle_t s_rx_chan;
static const audio_codec_data_if_t *s_data_if;
static esp_codec_dev_handle_t s_play_dev;
static esp_codec_dev_handle_t s_rec_dev;
static bool s_pa_on;

esp_codec_dev_handle_t board_audio_rec_dev(void) { return s_rec_dev; }
esp_codec_dev_handle_t board_audio_play_dev(void) { return s_play_dev; }

esp_err_t board_audio_init(void)
{
    /* I2S: one controller, duplex (TX + RX share the clock pins) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan), TAG, "i2s new channel");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(APP_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK,
            .bclk = BOARD_I2S_BCLK,
            .ws = BOARD_I2S_WS,
            .dout = BOARD_I2S_DOUT,
            .din = BOARD_I2S_DIN,
            .invert_flags.mclk_inv = false,
            .invert_flags.bclk_inv = false,
            .invert_flags.ws_inv = false,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg), TAG, "init tx");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_chan, &std_cfg), TAG, "init rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "enable tx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_chan), TAG, "enable rx");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .tx_handle = s_tx_chan,
        .rx_handle = s_rx_chan,
    };
    s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_data_if != NULL, ESP_FAIL, TAG, "new i2s data if");

    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(i2c_bus != NULL, ESP_FAIL, TAG, "i2c bus not ready (call BSP display init first)");

    /* ---- ES8311: speaker path ---- */
    audio_codec_i2c_cfg_t i2c_cfg_spk = {
        .port = BSP_I2C_NUM,
        .addr = BOARD_ES8310_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *spk_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg_spk);
    ESP_RETURN_ON_FALSE(spk_ctrl != NULL, ESP_FAIL, TAG, "es8311 ctrl if");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = spk_ctrl,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = GPIO_NUM_NC,          /* PA is behind the TCA9554 expander */
        .use_mclk = true,
        .master_mode = false,
    };
    const audio_codec_if_t *spk_codec = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(spk_codec != NULL, ESP_FAIL, TAG, "es8311 codec if");

    esp_codec_dev_cfg_t play_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = spk_codec,
        .data_if = s_data_if,
    };
    s_play_dev = esp_codec_dev_new(&play_cfg);
    ESP_RETURN_ON_FALSE(s_play_dev != NULL, ESP_FAIL, TAG, "play dev");

    /* ---- ES7210: dual microphone path ---- */
    audio_codec_i2c_cfg_t i2c_cfg_mic = {
        .port = BSP_I2C_NUM,
        .addr = BOARD_ES7210_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *mic_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg_mic);
    ESP_RETURN_ON_FALSE(mic_ctrl != NULL, ESP_FAIL, TAG, "es7210 ctrl if");

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = mic_ctrl,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2,   /* 2 mics, no TDM */
    };
    const audio_codec_if_t *mic_codec = es7210_codec_new(&es7210_cfg);
    ESP_RETURN_ON_FALSE(mic_codec != NULL, ESP_FAIL, TAG, "es7210 codec if");

    esp_codec_dev_cfg_t rec_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = mic_codec,
        .data_if = s_data_if,
    };
    s_rec_dev = esp_codec_dev_new(&rec_cfg);
    ESP_RETURN_ON_FALSE(s_rec_dev != NULL, ESP_FAIL, TAG, "rec dev");

    /* Same format for both directions - they share one bit clock. */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate = APP_AUDIO_SAMPLE_RATE,
        .channel = APP_AUDIO_CHANNELS,
        .bits_per_sample = APP_AUDIO_BITS,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_rec_dev, &fs), TAG, "open rec dev");
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_play_dev, &fs), TAG, "open play dev");

    esp_codec_dev_set_out_vol(s_play_dev, CONFIG_SMART_VOICE_SPEAKER_VOLUME);
    const uint32_t mic_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0);
    esp_codec_dev_set_in_channel_gain(s_rec_dev, mic_mask, (float)CONFIG_SMART_VOICE_MIC_GAIN_DB);
    esp_codec_dev_set_in_channel_gain(s_rec_dev, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
                                      (float)CONFIG_SMART_VOICE_MIC_GAIN_DB);
    esp_codec_dev_set_out_mute(s_play_dev, true);

    ESP_LOGI(TAG, "audio ready: %d Hz %d ch %d bit", APP_AUDIO_SAMPLE_RATE, APP_AUDIO_CHANNELS, APP_AUDIO_BITS);
    return ESP_OK;
}

esp_err_t board_audio_read(int16_t *buf, size_t samples_per_ch, size_t timeout_ms)
{
    (void)timeout_ms;   /* esp_codec_dev_read blocks until the buffer is filled */
    if (s_rec_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t bytes = samples_per_ch * APP_AUDIO_CHANNELS * sizeof(int16_t);
    return esp_codec_dev_read(s_rec_dev, buf, bytes);
}

esp_err_t board_audio_set_volume(int volume)
{
    if (s_play_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_codec_dev_set_out_vol(s_play_dev, volume);
}

static void pa_set(bool on)
{
    if (s_pa_on == on) {
        return;
    }
    board_pa_enable(on);
    s_pa_on = on;
}

esp_err_t board_audio_play_mono(const int16_t *mono, size_t samples, volatile bool *abort)
{
    if (s_play_dev == NULL || mono == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples == 0) {
        return ESP_OK;
    }

    /* Stereo staging buffer, written to the codec in small chunks so that a
     * screen tap (or the button) can interrupt long playback. */
    static int16_t stereo[4 * APP_AUDIO_SAMPLE_RATE / 1000 * APP_AUDIO_CHANNELS]; /* 64 ms */
    const size_t chunk_frames = sizeof(stereo) / sizeof(stereo[0]) / APP_AUDIO_CHANNELS;

    esp_codec_dev_set_out_mute(s_play_dev, false);
    pa_set(true);
    vTaskDelay(pdMS_TO_TICKS(20));   /* let the amp settle to avoid a pop */

    size_t done = 0;
    esp_err_t ret = ESP_OK;
    while (done < samples) {
        if (abort && *abort) {
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        size_t n = samples - done;
        if (n > chunk_frames) {
            n = chunk_frames;
        }
        for (size_t i = 0; i < n; i++) {
            stereo[2 * i] = mono[done + i];
            stereo[2 * i + 1] = mono[done + i];
        }
        ret = esp_codec_dev_write(s_play_dev, stereo, n * APP_AUDIO_CHANNELS * sizeof(int16_t));
        if (ret != ESP_OK) {
            break;
        }
        done += n;
    }

    vTaskDelay(pdMS_TO_TICKS(120));  /* let the last DMA frame drain before cutting the amp */
    esp_codec_dev_set_out_mute(s_play_dev, true);
    pa_set(false);
    return ret;
}

esp_err_t board_audio_beep(void)
{
    enum { BEEP_MS = 180 };
    static int16_t beep[BEEP_MS * APP_AUDIO_SAMPLE_RATE / 1000];
    for (size_t i = 0; i < sizeof(beep) / sizeof(beep[0]); i++) {
        float t = (float)i / APP_AUDIO_SAMPLE_RATE;
        float f = 880.0f + 500.0f * t * (1000.0f / BEEP_MS);
        float env = (i < 40) ? (float)i / 40.0f
                             : (i > sizeof(beep) / sizeof(beep[0]) - 40)
                               ? (float)(sizeof(beep) / sizeof(beep[0]) - i) / 40.0f : 1.0f;
        beep[i] = (int16_t)(12000.0f * env * sinf(2.0f * (float)M_PI * f * t));
    }
    volatile bool no_abort = false;
    return board_audio_play_mono(beep, sizeof(beep) / sizeof(beep[0]), &no_abort);
}
