// DSP task: microfrontend + hey_jarvis INT8 wake word on voiced blocks
#include "audio_dsp.h"
#include "audio_ingest.h"
#include "net.h"
#include "wake_model.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"

static const char *TAG = "dsp";

typedef struct {
    uint32_t n;
    uint64_t ac_mean_sq;
    int16_t pcm[AUDIO_DMA_FRAME_NUM];
} audio_block_t;

static audio_block_t s_pool[AUDIO_DSP_QUEUE_LEN];
static QueueHandle_t s_free;
static QueueHandle_t s_filled;
static TaskHandle_t s_dsp_task;
static uint32_t s_drops;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_blocks;
static uint32_t s_proc_last_us;
static uint32_t s_proc_max_us;
static uint32_t s_slices;
static uint32_t s_infers;
static uint32_t s_infer_last_us;
static uint32_t s_infer_max_us;
static uint32_t s_prob_last;
static uint32_t s_detections;
static uint32_t s_resets;
static uint32_t s_arena_used;

static struct FrontendState s_frontend;
static int8_t s_feat_ring[WAKE_MAX_STRIDE * WAKE_FEATURE_SIZE];
static int s_ring_n;
static uint8_t s_window[WAKE_SLIDING_WINDOW];
static size_t s_win_i;
static int16_t s_ignore;
static int64_t s_last_block_us;
static uint8_t *s_arena;

#if WAKE_ARENA_IN_PSRAM
#define WAKE_ARENA_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define WAKE_ARENA_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

static void quantize_feature(const uint16_t *in, size_t n, int8_t *out) {
    for (size_t i = 0; i < n; i++) {
        int32_t value = (((int32_t)in[i] * 256) + 333) / 666;
        value += INT8_MIN;
        if (value < INT8_MIN) {
            value = INT8_MIN;
        }
        if (value > INT8_MAX) {
            value = INT8_MAX;
        }
        out[i] = (int8_t)value;
    }
}

static void reset_detector(int16_t ignore) {
    memset(s_feat_ring, 0, sizeof(s_feat_ring));
    s_ring_n = 0;
    memset(s_window, 0, sizeof(s_window));
    s_win_i = 0;
    s_ignore = ignore;
    (void)wake_model_reset();
    FrontendReset(&s_frontend);
}

static bool window_hit(void) {
    uint32_t sum = 0;
    for (size_t i = 0; i < WAKE_SLIDING_WINDOW; i++) {
        sum += s_window[i];
    }
    return sum > ((uint32_t)WAKE_PROB_CUTOFF * WAKE_SLIDING_WINDOW);
}

static void process_feature(const int8_t feat[WAKE_FEATURE_SIZE]) {
    const int stride = wake_model_stride();
    if (stride <= 0 || stride > WAKE_MAX_STRIDE) {
        return;
    }

    memcpy(&s_feat_ring[s_ring_n * WAKE_FEATURE_SIZE], feat, WAKE_FEATURE_SIZE);
    s_ring_n++;

    uint8_t last_prob;
    taskENTER_CRITICAL(&s_lock);
    s_slices++;
    last_prob = (uint8_t)s_prob_last;
    taskEXIT_CRITICAL(&s_lock);

    if (s_ring_n >= stride) {
        uint8_t prob = 0;
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = wake_model_invoke(s_feat_ring, &prob);
        const uint32_t infer_us = (uint32_t)(esp_timer_get_time() - t0);
        s_ring_n = 0;

        bool hit = false;
        if (err == ESP_OK) {
            s_win_i++;
            if (s_win_i == WAKE_SLIDING_WINDOW) {
                s_win_i = 0;
            }
            s_window[s_win_i] = prob;
            last_prob = prob;
            if (s_ignore >= 0 && window_hit()) {
                hit = true;
                memset(s_window, 0, sizeof(s_window));
                s_ignore = -WAKE_COOLDOWN_SLICES;
            }
        }

        uint32_t detections = 0;
        taskENTER_CRITICAL(&s_lock);
        s_infers++;
        s_infer_last_us = infer_us;
        if (infer_us > s_infer_max_us) {
            s_infer_max_us = infer_us;
        }
        if (err == ESP_OK) {
            s_prob_last = prob;
            if (hit) {
                s_detections++;
            }
        }
        detections = s_detections;
        taskEXIT_CRITICAL(&s_lock);

        if (hit) {
            const bool on = net_toggle_light();
            ESP_LOGI(TAG, "hey jarvis det=%" PRIu32 " prob=%u light=%s infer_us=%" PRIu32,
                     detections, (unsigned)prob, on ? "on" : "off", infer_us);
        }
    }

    if (last_prob < WAKE_PROB_CUTOFF && s_ignore < 0) {
        s_ignore++;
    }
}

static void dsp_task(void *arg) {
    (void)arg;
    audio_block_t *block;

    while (1) {
        if (xQueueReceive(s_filled, &block, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const int64_t t0 = esp_timer_get_time();
        if (s_last_block_us != 0 && (t0 - s_last_block_us) > WAKE_GAP_US) {
            reset_detector(-WAKE_WARMUP_SLICES);
            taskENTER_CRITICAL(&s_lock);
            s_resets++;
            taskEXIT_CRITICAL(&s_lock);
            ESP_LOGD(TAG, "gap reset %" PRId64 " us", t0 - s_last_block_us);
        }
        s_last_block_us = t0;

        const int16_t *pcm = block->pcm;
        size_t left = block->n;
        while (left > 0) {
            size_t consumed = 0;
            struct FrontendOutput out =
                FrontendProcessSamples(&s_frontend, pcm, left, &consumed);
            if (consumed == 0) {
                break;
            }
            pcm += consumed;
            left -= consumed;
            if (out.size == 0) {
                continue;
            }
            int8_t feat[WAKE_FEATURE_SIZE];
            const size_t n = (out.size < WAKE_FEATURE_SIZE) ? out.size : WAKE_FEATURE_SIZE;
            quantize_feature(out.values, n, feat);
            if (n < WAKE_FEATURE_SIZE) {
                memset(&feat[n], INT8_MIN, WAKE_FEATURE_SIZE - n);
            }
            process_feature(feat);
        }

        const uint32_t proc_us = (uint32_t)(esp_timer_get_time() - t0);

        taskENTER_CRITICAL(&s_lock);
        s_blocks++;
        s_proc_last_us = proc_us;
        if (proc_us > s_proc_max_us) {
            s_proc_max_us = proc_us;
        }
        taskEXIT_CRITICAL(&s_lock);

        (void)xQueueSend(s_free, &block, portMAX_DELAY);
    }
}

static bool frontend_init(void) {
    struct FrontendConfig cfg;
    FrontendFillConfigWithDefaults(&cfg);
    cfg.window.size_ms = 30;
    cfg.window.step_size_ms = 10;
    cfg.filterbank.num_channels = WAKE_FEATURE_SIZE;
    cfg.filterbank.lower_band_limit = 125.0f;
    cfg.filterbank.upper_band_limit = 7500.0f;
    cfg.noise_reduction.smoothing_bits = 10;
    cfg.noise_reduction.even_smoothing = 0.025f;
    cfg.noise_reduction.odd_smoothing = 0.06f;
    cfg.noise_reduction.min_signal_remaining = 0.05f;
    cfg.pcan_gain_control.enable_pcan = 1;
    cfg.pcan_gain_control.strength = 0.95f;
    cfg.pcan_gain_control.offset = 80.0f;
    cfg.pcan_gain_control.gain_bits = 21;
    cfg.log_scale.enable_log = 1;
    cfg.log_scale.scale_shift = 6;
    if (!FrontendPopulateState(&cfg, &s_frontend, AUDIO_SAMPLE_RATE_HZ)) {
        ESP_LOGE(TAG, "frontend populate failed");
        return false;
    }
    return true;
}

esp_err_t audio_dsp_start(void) {
    s_free = xQueueCreate(AUDIO_DSP_QUEUE_LEN, sizeof(audio_block_t *));
    s_filled = xQueueCreate(AUDIO_DSP_QUEUE_LEN, sizeof(audio_block_t *));
    if (s_free == NULL || s_filled == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < AUDIO_DSP_QUEUE_LEN; i++) {
        audio_block_t *p = &s_pool[i];
        if (xQueueSend(s_free, &p, 0) != pdTRUE) {
            return ESP_ERR_NO_MEM;
        }
    }

    const size_t heap_before = heap_caps_get_free_size(WAKE_ARENA_CAPS);
    s_arena = (uint8_t *)heap_caps_aligned_alloc(16, WAKE_ARENA_SIZE, WAKE_ARENA_CAPS);
    if (s_arena == NULL) {
        ESP_LOGE(TAG, "arena alloc %u failed caps=0x%x", (unsigned)WAKE_ARENA_SIZE,
                 (unsigned)WAKE_ARENA_CAPS);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = wake_model_init(s_arena, WAKE_ARENA_SIZE);
    if (err != ESP_OK) {
        return err;
    }
    s_arena_used = (uint32_t)wake_model_arena_used();
    s_ignore = -WAKE_WARMUP_SLICES;

    if (!frontend_init()) {
        return ESP_FAIL;
    }

    const size_t heap_after = heap_caps_get_free_size(WAKE_ARENA_CAPS);
    ESP_LOGI(TAG, "arena %s size=%u used=%u heap_delta=%d model=%uB",
             WAKE_ARENA_IN_PSRAM ? "psram" : "internal",
             (unsigned)WAKE_ARENA_SIZE, (unsigned)s_arena_used,
             (int)heap_before - (int)heap_after,
             (unsigned)wake_model_bytes());

    BaseType_t ok = xTaskCreatePinnedToCore(dsp_task, "dsp", DSP_TASK_STACK,
                                            NULL, DSP_TASK_PRIO, &s_dsp_task,
                                            DSP_TASK_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "wake hey_jarvis queue len=%u", (unsigned)AUDIO_DSP_QUEUE_LEN);
    return ESP_OK;
}

bool audio_dsp_try_submit(const int32_t *dma_samples, size_t n, uint64_t ac_mean_sq) {
    if (s_free == NULL || dma_samples == NULL || n == 0) {
        return false;
    }

    audio_block_t *block;
    if (xQueueReceive(s_free, &block, 0) != pdTRUE) {
        s_drops++;
        return false;
    }

    if (n > AUDIO_DMA_FRAME_NUM) {
        n = AUDIO_DMA_FRAME_NUM;
    }
    block->n = (uint32_t)n;
    block->ac_mean_sq = ac_mean_sq;
    for (size_t i = 0; i < n; i++) {
        const int32_t s = dma_samples[i] >> 8;
        block->pcm[i] = (int16_t)(s >> 8);
    }

    if (xQueueSend(s_filled, &block, 0) != pdTRUE) {
        s_drops++;
        (void)xQueueSend(s_free, &block, 0);
        return false;
    }
    return true;
}

uint32_t audio_dsp_drops(void) {
    return s_drops;
}

UBaseType_t audio_dsp_queue_waiting(void) {
    if (s_filled == NULL) {
        return 0;
    }
    return uxQueueMessagesWaiting(s_filled);
}

TaskHandle_t audio_dsp_task(void) {
    return s_dsp_task;
}

void audio_dsp_get_stats(audio_dsp_stats_t *out) {
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    out->blocks = s_blocks;
    out->proc_last_us = s_proc_last_us;
    out->proc_max_us = s_proc_max_us;
    out->slices = s_slices;
    out->infers = s_infers;
    out->infer_last_us = s_infer_last_us;
    out->infer_max_us = s_infer_max_us;
    out->prob_last = s_prob_last;
    out->detections = s_detections;
    out->resets = s_resets;
    out->arena_used = s_arena_used;
    taskEXIT_CRITICAL(&s_lock);
    out->drops = s_drops;
    out->depth = (uint32_t)audio_dsp_queue_waiting();
    out->stack_hwm = (uint32_t)uxTaskGetStackHighWaterMark(s_dsp_task);
}

void audio_dsp_reset_stats(void) {
    taskENTER_CRITICAL(&s_lock);
    s_blocks = 0;
    s_proc_last_us = 0;
    s_proc_max_us = 0;
    s_drops = 0;
    s_slices = 0;
    s_infers = 0;
    s_infer_last_us = 0;
    s_infer_max_us = 0;
    s_prob_last = 0;
    s_detections = 0;
    s_resets = 0;
    taskEXIT_CRITICAL(&s_lock);
}
