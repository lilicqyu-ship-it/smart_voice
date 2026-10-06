/*
 * smart_voice - AI cloud pipeline: ASR -> LLM (SSE stream) -> TTS
 *
 * Talks to any OpenAI-compatible service. Defaults target SiliconFlow,
 * which offers all three legs under one API key:
 *   ASR  POST /audio/transcriptions   (multipart wav)
 *   LLM  POST /chat/completions       (SSE streaming)
 *   TTS  POST /audio/speech           (wav download)
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_ai.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#include "app_priv.h"
#include "board_audio.h"

static const char *TAG = "ai";

#define BASE_URL         CONFIG_SMART_VOICE_AI_BASE_URL
#define AUTH_HEADER_VAL  s_auth
#define AI_HTTP_TIMEOUT_MS 45000

/* ------------------------------------------------------------------ */
/* conversation history                                                */
/* ------------------------------------------------------------------ */

#define AI_HIST_MAX_MSGS   8
#define AI_HIST_MAX_CHARS  1200

static char *s_hist_role[AI_HIST_MAX_MSGS];
static char *s_hist_text[AI_HIST_MAX_MSGS];
static int s_hist_count;
static char s_auth[128];

static bool text_is_blank(const char *text)
{
    if (!text) return true;
    while (*text) {
        unsigned char c = (unsigned char)*text++;
        if (c > 0x20 && c != 0x7f) return false;
    }
    return true;
}

static void hist_push(const char *role, const char *text)
{
    if (s_hist_count == AI_HIST_MAX_MSGS) {
        heap_caps_free(s_hist_role[0]);
        heap_caps_free(s_hist_text[0]);
        memmove(&s_hist_role[0], &s_hist_role[1], (AI_HIST_MAX_MSGS - 1) * sizeof(char *));
        memmove(&s_hist_text[0], &s_hist_text[1], (AI_HIST_MAX_MSGS - 1) * sizeof(char *));
        s_hist_count--;
    }
    size_t len = strnlen(text, AI_HIST_MAX_CHARS);
    char *copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if (copy) {
        char *role_copy = heap_caps_malloc(strlen(role) + 1, MALLOC_CAP_SPIRAM);
        if (!role_copy) {
            heap_caps_free(copy);
            return;
        }
        memcpy(copy, text, len);
        copy[len] = 0;
        strcpy(role_copy, role);
        s_hist_role[s_hist_count] = role_copy;
        s_hist_text[s_hist_count] = copy;
        s_hist_count++;
    }
}

void ai_reset_history(void)
{
    for (int i = 0; i < s_hist_count; i++) {
        heap_caps_free(s_hist_role[i]);
        heap_caps_free(s_hist_text[i]);
    }
    s_hist_count = 0;
}

/* ------------------------------------------------------------------ */
/* HTTP helpers                                                        */
/* ------------------------------------------------------------------ */

static esp_http_client_handle_t http_post(const char *url, const char *content_type)
{
    snprintf(s_auth, sizeof(s_auth), "Bearer %s", CONFIG_SMART_VOICE_AI_API_KEY);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = AI_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .user_agent = "smart-voice/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return NULL;
    }
    esp_http_client_set_header(client, "Authorization", AUTH_HEADER_VAL);
    esp_http_client_set_header(client, "Content-Type", content_type);
    return client;
}

/* esp_http_client_write() may accept only part of a large PSRAM buffer.  A
 * multipart upload must drain the complete buffer or the server waits for
 * bytes that will never arrive and eventually reports a timeout. */
static esp_err_t http_write_all(esp_http_client_handle_t client,
                                const void *data, size_t len)
{
    const char *p = (const char *)data;
    while (len > 0) {
        int n = esp_http_client_write(client, p, (int)len);
        if (n <= 0) {
            ESP_LOGE(TAG, "http write failed: n=%d errno=%d",
                     n, esp_http_client_get_errno(client));
            return ESP_FAIL;
        }
        p += n;
        len -= (size_t)n;
    }
    return ESP_OK;
}

static bool http_fetch_headers_ok(esp_http_client_handle_t client, const char *stage)
{
    int64_t content_length = esp_http_client_fetch_headers(client);
    if (content_length < 0) {
        ESP_LOGE(TAG, "%s: fetch headers failed errno=%d", stage,
                 esp_http_client_get_errno(client));
        return false;
    }
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        char error_body[256];
        int n = esp_http_client_read(client, error_body, sizeof(error_body) - 1);
        if (n > 0) {
            error_body[n] = 0;
            ESP_LOGE(TAG, "%s: http %d body=%s", stage, status, error_body);
        } else {
            ESP_LOGE(TAG, "%s: http %d", stage, status);
        }
        return false;
    }
    return true;
}

static void http_close(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

/* ------------------------------------------------------------------ */
/* ASR                                                                 */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* little-endian helpers (avoid unaligned access on packed headers)    */
/* ------------------------------------------------------------------ */

static inline void put_le16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static inline uint16_t get_le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t get_le32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int wav_header_put(uint8_t *h, int rate, int channels, int bits, int data_bytes)
{
    int byte_rate = rate * channels * bits / 8;
    int block_align = channels * bits / 8;
    memcpy(h, "RIFF", 4);           /* 0  */
    put_le32(h + 4, 36 + data_bytes);
    memcpy(h + 8, "WAVE", 4);       /* 8  */
    memcpy(h + 12, "fmt ", 4);      /* 12 */
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);            /* PCM */
    put_le16(h + 22, channels);
    put_le32(h + 24, rate);
    put_le32(h + 28, byte_rate);
    put_le16(h + 32, block_align);
    put_le16(h + 34, bits);
    memcpy(h + 36, "data", 4);      /* 36 */
    put_le32(h + 40, data_bytes);
    return 44;
}

#define ASR_BOUNDARY "----smartvoice7f3a"

static esp_err_t ai_asr(const int16_t *pcm, int samples, const ai_callbacks_t *cb,
                        char *out, size_t out_size)
{
    char head[512], tail[64], ctype[64];
    int head_len = snprintf(head, sizeof(head),
        "--" ASR_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\n"
        "%s\r\n"
        "--" ASR_BOUNDARY "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        CONFIG_SMART_VOICE_ASR_MODEL);
    int tail_len = snprintf(tail, sizeof(tail), "\r\n--" ASR_BOUNDARY "--\r\n");
    int wav_bytes = samples * (int)sizeof(int16_t);
    int total = head_len + 44 + wav_bytes + tail_len;

    char url[192];
    snprintf(url, sizeof(url), "%s/audio/transcriptions", BASE_URL);
    snprintf(ctype, sizeof(ctype), "multipart/form-data; boundary=" ASR_BOUNDARY);
    esp_http_client_handle_t client = http_post(url, ctype);
    ESP_RETURN_ON_FALSE(client != NULL, ESP_FAIL, TAG, "http init");

    uint8_t wav_hdr[44];
    wav_header_put(wav_hdr, APP_AUDIO_SAMPLE_RATE, 1, 16, wav_bytes);

    esp_err_t ret = ESP_FAIL;
    do {
        if (cb && cb->aborted && cb->aborted()) {
            ret = AI_ERR_ABORTED;
            break;
        }
        if (esp_http_client_open(client, total) != ESP_OK) {
            ESP_LOGE(TAG, "asr: connect failed");
            break;
        }
        if (http_write_all(client, head, head_len) != ESP_OK ||
            http_write_all(client, wav_hdr, 44) != ESP_OK ||
            (cb && cb->aborted && cb->aborted()) ||
            http_write_all(client, pcm, (size_t)wav_bytes) != ESP_OK ||
            http_write_all(client, tail, tail_len) != ESP_OK) {
            if (cb && cb->aborted && cb->aborted()) {
                ret = AI_ERR_ABORTED;
            }
            ESP_LOGE(TAG, "asr: upload failed");
            break;
        }
        if (!http_fetch_headers_ok(client, "asr")) break;

        static char resp[2048];
        int n = esp_http_client_read(client, resp, sizeof(resp) - 1);
        if (n <= 0) {
            ESP_LOGE(TAG, "asr: empty response");
            break;
        }
        if (cb && cb->aborted && cb->aborted()) {
            ret = AI_ERR_ABORTED;
            break;
        }
        resp[n] = 0;
        cJSON *root = cJSON_Parse(resp);
        if (!root) {
            ESP_LOGE(TAG, "asr: bad json");
            break;
        }
        const cJSON *text = cJSON_GetObjectItem(root, "text");
        strlcpy(out, text && cJSON_IsString(text) ? text->valuestring : "", out_size);
        cJSON_Delete(root);
        ret = ESP_OK;
        ESP_LOGI(TAG, "asr: \"%s\"", out);
    } while (0);

    http_close(client);
    return ret;
}

/* ------------------------------------------------------------------ */
/* TTS                                                                 */
/* ------------------------------------------------------------------ */

/* Linear-interpolation resampler to 16 kHz mono. */
static int resample_to_16k_mono(const int16_t *in, int n_in, int rate, int channels,
                                int16_t *out, int out_max)
{
    if (rate == APP_AUDIO_SAMPLE_RATE) {
        int n = n_in / channels;
        if (n > out_max) n = out_max;
        if (channels == 1) {
            memcpy(out, in, n * sizeof(int16_t));
        } else {
            for (int i = 0; i < n; i++) {
                out[i] = (in[channels * i] + in[channels * i + 1]) / 2;
            }
        }
        return n;
    }

    double step = (double)rate / APP_AUDIO_SAMPLE_RATE;
    int n_out = (int)(n_in / channels / step) + 1;
    if (n_out > out_max) n_out = out_max;
    for (int i = 0; i < n_out; i++) {
        double src = i * step;
        int idx = (int)src;
        double frac = src - idx;
        int base = idx * channels;
        if (base + channels >= n_in) {
            n_out = i;
            break;
        }
        int s0 = (channels == 1) ? in[base] : (in[base] + in[base + 1]) / 2;
        int s1 = (channels == 1) ? in[base + channels] : (in[base + channels] + in[base + channels + 1]) / 2;
        out[i] = (int16_t)(s0 + (s1 - s0) * frac);
    }
    return n_out;
}

/* Download one TTS sentence as WAV and play it. */
static esp_err_t ai_tts_play(const char *text, const ai_callbacks_t *cb)
{
    char url[192];
    snprintf(url, sizeof(url), "%s/audio/speech", BASE_URL);

    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", CONFIG_SMART_VOICE_TTS_MODEL);
    cJSON_AddStringToObject(body, "input", text);
    cJSON_AddStringToObject(body, "voice", CONFIG_SMART_VOICE_TTS_VOICE);
    cJSON_AddStringToObject(body, "response_format", "wav");
    cJSON_AddNumberToObject(body, "sample_rate", APP_AUDIO_SAMPLE_RATE);
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_handle_t client = http_post(url, "application/json");
    esp_err_t ret = ESP_FAIL;
    do {
        if (!client) break;
        int len = (int)strlen(payload);
        if (esp_http_client_open(client, len) != ESP_OK) break;
        if (http_write_all(client, payload, (size_t)len) != ESP_OK) break;
        if (!http_fetch_headers_ok(client, "tts")) break;

        /* Parse the WAV container while streaming. */
        int rate = 0, channels = 0, bits = 16;
        uint8_t hdr[16];
        int got = esp_http_client_read(client, (char *)hdr, 12);
        if (got != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
            ESP_LOGE(TAG, "tts: not a wav stream");
            break;
        }
        bool have_data = false;
        while (!have_data) {
            got = esp_http_client_read(client, (char *)hdr, 8);
            if (got != 8) break;
            uint32_t chunk_size = get_le32(hdr + 4);
            if (!memcmp(hdr, "fmt ", 4)) {
                got = esp_http_client_read(client, (char *)hdr, 16);
                if (got != 16) break;
                channels = get_le16(hdr + 2);
                rate = get_le32(hdr + 4);
                bits = get_le16(hdr + 14);
                if (chunk_size > 16) {
                    uint32_t skip = chunk_size - 16;
                    while (skip) {
                        int k = esp_http_client_read(client, (char *)hdr,
                                                     skip > sizeof(hdr) ? sizeof(hdr) : skip);
                        if (k <= 0) break;
                        skip -= k;
                    }
                }
            } else if (!memcmp(hdr, "data", 4)) {
                have_data = true;
            } else {
                uint32_t skip = chunk_size;
                while (skip) {
                    int k = esp_http_client_read(client, (char *)hdr,
                                                 skip > sizeof(hdr) ? sizeof(hdr) : skip);
                    if (k <= 0) break;
                    skip -= k;
                }
            }
        }
        if (!have_data || rate == 0 || channels == 0) {
            ESP_LOGE(TAG, "tts: wav header incomplete");
            break;
        }
        ESP_LOGI(TAG, "tts: %d Hz %d ch %d bit", rate, channels, bits);

        /* Download the whole sentence (TTS sentences are short), then play it
         * in one go so the PA is not toggled mid-sentence. Buffers are sized
         * to keep the PSRAM budget sane alongside XIP code + 3 framebuffers:
         * 320k elements cover ~10 s at 16 kHz mono; longer pending text is
         * force-split by sentence_feed() at 100 chars. */
        static int16_t *pcm_in;     /* source samples */
        static int16_t *pcm_out;    /* 16 kHz mono samples */
        if (!pcm_in) {
            pcm_in = heap_caps_malloc(320000 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            pcm_out = heap_caps_malloc(160000 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        }
        if (!pcm_in || !pcm_out) {
            ESP_LOGE(TAG, "tts: no mem");
            break;
        }

        /* cap at ~10 s of audio regardless of source rate */
        size_t in_max = (size_t)rate * 10 * channels;    /* frames (10 s) */
        if (in_max > 320000 / (channels > 0 ? channels : 1)) {
            in_max = 320000 / channels;
        }
        size_t got_samples = 0;
        ret = ESP_OK;
        while (ret == ESP_OK) {
            if (cb && cb->aborted && cb->aborted()) {
                ret = AI_ERR_ABORTED;
                break;
            }
            size_t want = in_max - got_samples;
            if (want == 0) break;
            int n = esp_http_client_read(client, (char *)(pcm_in + got_samples),
                                         (int)(want * sizeof(int16_t)));
            if (n < 0) { ret = ESP_FAIL; break; }
            if (n == 0) break;   /* end of stream */
            got_samples += n / sizeof(int16_t);
        }

        if (ret == ESP_OK && got_samples > 0) {
            int mono = resample_to_16k_mono(pcm_in, (int)got_samples, rate, channels,
                                            pcm_out, 160000);
            ESP_LOGI(TAG, "tts: %d -> %d samples", (int)got_samples, mono);
            if (mono > 0) {
                esp_err_t w = board_audio_play_mono(pcm_out, mono, &g_ai_abort);
                if (w != ESP_OK) ret = w;
            }
        }
    } while (0);

    if (payload) cJSON_free(payload);
    if (client) http_close(client);
    return ret;
}

esp_err_t ai_speak_text(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    return ai_tts_play(text, NULL);
}

/* ------------------------------------------------------------------ */
/* LLM (SSE stream)                                                    */
/* ------------------------------------------------------------------ */

/* Sentence terminators used to split LLM output into TTS chunks. */
static bool is_sentence_end(const char *s, int *len)
{
    *len = 1;
    unsigned char c = (unsigned char)s[0];
    size_t remaining = strlen(s);
    if (c >= 0xF0) {
        if (remaining >= 4) *len = 4;
        return remaining >= 4;                       /* rare CJK ext */
    }
    if (c == 0xE3) {                                  /* 。．？！etc (U+3002, U+FF01, U+FF1F, U+FF1B) */
        if (remaining < 3) return false;
        if (s[1] == (char)0x80 && (s[2] == (char)0x82 || s[2] == (char)0x8E ||
                                   s[2] == (char)0x9F || s[2] == (char)0x9B)) {
            *len = 3;
            return true;
        }
        return false;
    }
    if (c == 0xEF) {                                  /* ！？；(fullwidth) */
        if (remaining < 3) return false;
        if (s[1] == (char)0xBC && (s[2] == (char)0x81 || s[2] == (char)0x9F || s[2] == (char)0x9B)) {
            *len = 3;
            return true;
        }
        return false;
    }
    if (c == '!' || c == '?' || c == ';' || c == '\n') {
        return true;
    }
    if (c == '.') {
        /* don't split numbers like 3.14 */
        if (remaining > 1 && s[1] >= '0' && s[1] <= '9') return false;
        return true;
    }
    return false;
}

typedef struct {
    char *buf;              /* pending sentence text (PSRAM) */
    int len;
    int cap;
} sentence_buf_t;

static void sentence_flush(sentence_buf_t *sb, const ai_callbacks_t *cb, bool *spoke)
{
    while (sb->len && sb->buf[sb->len - 1] == ' ') sb->len--;   /* trailing space */
    sb->buf[sb->len] = 0;
    if (sb->len == 0) return;

    if (cb && cb->on_speaking) cb->on_speaking();
    *spoke = true;
    esp_err_t err = ai_tts_play(sb->buf, cb);
    if (err != ESP_OK && err != AI_ERR_ABORTED) {
        ESP_LOGW(TAG, "tts failed: %s", esp_err_to_name(err));
    }
    sb->len = 0;
    sb->buf[0] = 0;
}

static void sentence_feed(sentence_buf_t *sb, const char *text, const ai_callbacks_t *cb, bool *spoke)
{
    while (*text) {
        /* force-split long clauses so one TTS request stays inside the
         * ~10 s download cap (spoken CJK is roughly 4 chars/s) */
        if (sb->len + 8 >= sb->cap || sb->len >= 300) {
            sentence_flush(sb, cb, spoke);
            if (cb && cb->aborted && cb->aborted()) return;
        }
        int L = 1;
        bool end = is_sentence_end(text, &L);
        memcpy(sb->buf + sb->len, text, L);
        sb->len += L;
        if (sb->len + 1 < sb->cap) sb->buf[sb->len] = 0;
        text += L;
        if (end) {
            sentence_flush(sb, cb, spoke);
            if (cb && cb->aborted && cb->aborted()) return;
        }
    }
}

static esp_err_t ai_llm_stream(const ai_callbacks_t *cb, char *out_text, size_t out_size)
{
    char url[192];
    snprintf(url, sizeof(url), "%s/chat/completions", BASE_URL);

    cJSON *body = cJSON_CreateObject();
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(body, "model", CONFIG_SMART_VOICE_LLM_MODEL);
    cJSON_AddBoolToObject(body, "stream", true);
    /* Voice replies should be deterministic and short enough to speak. */
    cJSON_AddNumberToObject(body, "temperature", 0.2);
    cJSON_AddNumberToObject(body, "top_p", 0.85);
    cJSON_AddNumberToObject(body, "max_tokens", 320);
    cJSON *messages = cJSON_AddArrayToObject(body, "messages");
    if (!messages) {
        cJSON_Delete(body);
        return ESP_ERR_NO_MEM;
    }
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content", CONFIG_SMART_VOICE_SYSTEM_PROMPT);
    cJSON_AddItemToArray(messages, sys);
    for (int i = 0; i < s_hist_count; i++) {   /* includes the new user turn */
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", s_hist_role[i]);
        cJSON_AddStringToObject(m, "content", s_hist_text[i]);
        cJSON_AddItemToArray(messages, m);
    }

    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    ESP_RETURN_ON_FALSE(payload != NULL, ESP_ERR_NO_MEM, TAG, "no mem");

    esp_http_client_handle_t client = http_post(url, "application/json");
    esp_err_t ret = ESP_FAIL;
    do {
        if (!client) break;
        int len = (int)strlen(payload);
        if (esp_http_client_open(client, len) != ESP_OK) break;
        if (http_write_all(client, payload, (size_t)len) != ESP_OK) break;
        if (!http_fetch_headers_ok(client, "llm")) break;

        if (cb && cb->on_llm_start) cb->on_llm_start();

        static char read_buf[2048];
        static char line_buf[4096];
        int line_pos = 0;

        char *resp = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
        /* 512 B pending-sentence buffer: force-splits at ~100 chars so a long
         * clause still fits the 10 s TTS download cap. */
        sentence_buf_t sb = { .buf = heap_caps_malloc(512, MALLOC_CAP_SPIRAM), .len = 0, .cap = 512 };
        if (!resp || !sb.buf) {
            heap_caps_free(resp);
            heap_caps_free(sb.buf);
            ret = ESP_ERR_NO_MEM;
            break;
        }
        size_t resp_len = 0;
        bool spoke = false;
        bool done = false;
        ret = ESP_OK;

        while (!done && ret == ESP_OK) {
            if (cb && cb->aborted && cb->aborted()) { ret = AI_ERR_ABORTED; break; }
            int n = esp_http_client_read(client, read_buf, sizeof(read_buf));
            if (n < 0) { ret = ESP_FAIL; break; }
            if (n == 0) break;   /* stream closed */
            for (int i = 0; i < n; i++) {
                char c = read_buf[i];
                if (c == '\n') {
                    line_buf[line_pos] = 0;
                    if (!strncmp(line_buf, "data:", 5)) {
                        const char *data = line_buf + 5;
                        while (*data == ' ') data++;
                        if (!strcmp(data, "[DONE]")) {
                            done = true;
                        } else {
                            cJSON *root = cJSON_Parse(data);
                            if (root) {
                                const cJSON *ch = cJSON_GetObjectItem(root, "choices");
                                const cJSON *delta = (ch && cJSON_GetArraySize(ch) > 0)
                                    ? cJSON_GetObjectItem(cJSON_GetArrayItem(ch, 0), "delta") : NULL;
                                const cJSON *content = delta
                                    ? cJSON_GetObjectItem(delta, "content") : NULL;
                                if (content && cJSON_IsString(content) && content->valuestring[0]) {
                                    const char *t = content->valuestring;
                                    size_t tl = strlen(t);
                                    if (resp_len + tl < 16383) {
                                        memcpy(resp + resp_len, t, tl);
                                        resp_len += tl;
                                        resp[resp_len] = 0;
                                    }
                                    if (cb && cb->on_llm_text) cb->on_llm_text(t);
                                    sentence_feed(&sb, t, cb, &spoke);
                                }
                                cJSON_Delete(root);
                            }
                        }
                    }
                    line_pos = 0;
                } else if (line_pos < (int)sizeof(line_buf) - 1) {
                    line_buf[line_pos++] = c;
                }
            }
        }
        sentence_flush(&sb, cb, &spoke);   /* trailing text without terminator */

        (void)spoke;
        strlcpy(out_text, resp_len ? resp : "", out_size);
        heap_caps_free(resp);
        heap_caps_free(sb.buf);
        if (ret == ESP_OK && out_text[0]) {
            hist_push("assistant", out_text);
        }
    } while (0);

    if (payload) cJSON_free(payload);
    if (client) http_close(client);
    return ret;
}

/* ------------------------------------------------------------------ */
/* one full round                                                      */
/* ------------------------------------------------------------------ */

esp_err_t ai_converse(const int16_t *pcm, int samples, const ai_callbacks_t *cb,
                      char *out_text, size_t out_size)
{
    if (out_size) out_text[0] = 0;
    if (pcm == NULL || samples <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char *user_text = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!user_text) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ai_asr(pcm, samples, cb, user_text, 2048);
    if (ret != ESP_OK) {
        heap_caps_free(user_text);
        return ret;
    }
    if (cb && cb->aborted && cb->aborted()) {
        heap_caps_free(user_text);
        return AI_ERR_ABORTED;
    }
    if (text_is_blank(user_text)) {
        heap_caps_free(user_text);
        return AI_ERR_NO_SPEECH;
    }
    if (cb && cb->on_asr_text) cb->on_asr_text(user_text);
    bool local_handled = cb && cb->on_local_command && cb->on_local_command(user_text);
    hist_push("user", user_text);
    heap_caps_free(user_text);

    if (local_handled) {
        return AI_ERR_LOCAL_HANDLED;
    }

    ret = ai_llm_stream(cb, out_text, out_size);
    if (ret != ESP_OK) {
        /* keep the failed user turn out of context */
        if (s_hist_count > 0) {
            heap_caps_free(s_hist_role[s_hist_count - 1]);
            heap_caps_free(s_hist_text[s_hist_count - 1]);
            s_hist_count--;
        }
    }
    return ret;
}
