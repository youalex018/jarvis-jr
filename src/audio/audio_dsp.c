// DSP task: microfrontend + hey_jarvis, then optional light on/off in a listen window
#include "audio_dsp.h"
#include "audio_ingest.h"
#include "board.h"
#include "net.h"
#include "wake_model.h"

#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "sdkconfig.h"
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

typedef struct {
    int8_t ring[WAKE_MAX_STRIDE * WAKE_FEATURE_SIZE];
    int ring_n;
    uint8_t window[WAKE_SLIDING_WINDOW];
    size_t win_i;
    int16_t ignore;
    uint8_t prob_last;
} det_t;

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
static uint32_t s_det_jarvis;
static uint32_t s_det_on;
static uint32_t s_det_off;
static uint32_t s_listen_timeouts;
static uint32_t s_resets;
static uint32_t s_arena_used;
static uint32_t s_prob_jarvis;
static uint32_t s_prob_on;
static uint32_t s_prob_off;

static struct FrontendState s_frontend;
static det_t s_det[WAKE_SLOT_COUNT];
static int64_t s_last_block_us;
static volatile bool s_listening;
static int64_t s_listen_deadline_us;
static int s_led_on;
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_cpu_lock;
#endif

static void led_set(int on) {
    s_led_on = on ? 1 : 0;
    gpio_set_level(LED_GPIO, s_led_on);
}

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

static void reset_slot(wake_slot_t slot, int16_t ignore) {
    det_t *d = &s_det[slot];
    memset(d->ring, 0, sizeof(d->ring));
    d->ring_n = 0;
    memset(d->window, 0, sizeof(d->window));
    d->win_i = 0;
    d->ignore = ignore;
    d->prob_last = 0;
    if (wake_model_ready(slot)) {
        (void)wake_model_reset(slot);
    }
}

static uint8_t slot_cutoff(wake_slot_t slot) {
    return (slot == WAKE_SLOT_JARVIS) ? WAKE_PROB_CUTOFF : WAKE_CMD_PROB_CUTOFF;
}

static size_t slot_window_n(wake_slot_t slot) {
    return (slot == WAKE_SLOT_JARVIS) ? WAKE_SLIDING_WINDOW : WAKE_CMD_SLIDING_WINDOW;
}

static bool window_hit(const det_t *d, wake_slot_t slot) {
    const size_t n = slot_window_n(slot);
    uint32_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += d->window[i];
    }
    return sum >= ((uint32_t)slot_cutoff(slot) * n);
}

static void enter_listen(int64_t now_us) {
    s_listening = true;
    s_listen_deadline_us = now_us + WAKE_LISTEN_US;
    led_set(1);
    reset_slot(WAKE_SLOT_LIGHT_ON, -WAKE_WARMUP_SLICES);
    reset_slot(WAKE_SLOT_LIGHT_OFF, -WAKE_WARMUP_SLICES);
    ESP_LOGI(TAG, "listen start %d ms", (int)(WAKE_LISTEN_US / 1000));
}

static void exit_listen(const char *why) {
    s_listening = false;
    s_listen_deadline_us = 0;
    led_set(0);
    reset_slot(WAKE_SLOT_JARVIS, -WAKE_COOLDOWN_SLICES);
    ESP_LOGI(TAG, "listen end %s", why);
}

static bool feed_slot(wake_slot_t slot, const int8_t feat[WAKE_FEATURE_SIZE]) {
    if (!wake_model_ready(slot)) {
        return false;
    }

    const int stride = wake_model_stride(slot);
    if (stride <= 0 || stride > WAKE_MAX_STRIDE) {
        return false;
    }

    det_t *d = &s_det[slot];
    memcpy(&d->ring[d->ring_n * WAKE_FEATURE_SIZE], feat, WAKE_FEATURE_SIZE);
    d->ring_n++;

    uint8_t last_prob = d->prob_last;
    bool hit = false;
    uint32_t infer_us = 0;

    if (d->ring_n >= stride) {
        uint8_t prob = 0;
        const int64_t t0 = esp_timer_get_time();
        const esp_err_t err = wake_model_invoke(slot, d->ring, &prob);
        infer_us = (uint32_t)(esp_timer_get_time() - t0);
        d->ring_n = 0;

        if (err == ESP_OK) {
            const size_t win_n = slot_window_n(slot);
            d->win_i++;
            if (d->win_i == win_n) {
                d->win_i = 0;
            }
            d->window[d->win_i] = prob;
            d->prob_last = prob;
            last_prob = prob;
            if (d->ignore >= 0 && window_hit(d, slot)) {
                hit = true;
                memset(d->window, 0, sizeof(d->window));
                d->ignore = -WAKE_COOLDOWN_SLICES;
            }
        }

        uint32_t *prob_stat = &s_prob_jarvis;
        if (slot == WAKE_SLOT_LIGHT_ON) {
            prob_stat = &s_prob_on;
        } else if (slot == WAKE_SLOT_LIGHT_OFF) {
            prob_stat = &s_prob_off;
        }

        taskENTER_CRITICAL(&s_lock);
        s_infers++;
        s_infer_last_us = infer_us;
        if (infer_us > s_infer_max_us) {
            s_infer_max_us = infer_us;
        }
        if (err == ESP_OK) {
            *prob_stat = prob;
        }
        taskEXIT_CRITICAL(&s_lock);
    }

    if (last_prob < slot_cutoff(slot) && d->ignore < 0) {
        d->ignore++;
    }
    return hit;
}

static void process_feature(const int8_t feat[WAKE_FEATURE_SIZE], int64_t now_us) {
    taskENTER_CRITICAL(&s_lock);
    s_slices++;
    taskEXIT_CRITICAL(&s_lock);

    if (s_listening) {
        if (feed_slot(WAKE_SLOT_LIGHT_ON, feat)) {
            uint32_t n;
            taskENTER_CRITICAL(&s_lock);
            s_det_on++;
            n = s_det_on;
            taskEXIT_CRITICAL(&s_lock);
            net_set_light(true);
            ESP_LOGI(TAG, "light on det=%" PRIu32 " prob=%u", n,
                     (unsigned)s_det[WAKE_SLOT_LIGHT_ON].prob_last);
            exit_listen("light_on");
            return;
        }
        if (feed_slot(WAKE_SLOT_LIGHT_OFF, feat)) {
            uint32_t n;
            taskENTER_CRITICAL(&s_lock);
            s_det_off++;
            n = s_det_off;
            taskEXIT_CRITICAL(&s_lock);
            net_set_light(false);
            ESP_LOGI(TAG, "light off det=%" PRIu32 " prob=%u", n,
                     (unsigned)s_det[WAKE_SLOT_LIGHT_OFF].prob_last);
            exit_listen("light_off");
            return;
        }
        return;
    }

    if (feed_slot(WAKE_SLOT_JARVIS, feat)) {
        uint32_t n;
        taskENTER_CRITICAL(&s_lock);
        s_det_jarvis++;
        n = s_det_jarvis;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "hey jarvis det=%" PRIu32 " prob=%u", n,
                 (unsigned)s_det[WAKE_SLOT_JARVIS].prob_last);
        enter_listen(now_us);
    }
}

static void dsp_task(void *arg) {
    (void)arg;
    audio_block_t *block;

    while (1) {
        if (xQueueReceive(s_filled, &block, portMAX_DELAY) != pdTRUE) {
            continue;
        }
#if CONFIG_PM_ENABLE
        (void)esp_pm_lock_acquire(s_cpu_lock);
#endif

        const int64_t t0 = esp_timer_get_time();
        if (s_listening) {
            if (t0 >= s_listen_deadline_us) {
                taskENTER_CRITICAL(&s_lock);
                s_listen_timeouts++;
                taskEXIT_CRITICAL(&s_lock);
                exit_listen("timeout");
            }
        } else if (s_last_block_us != 0 && (t0 - s_last_block_us) > WAKE_GAP_US) {
            reset_slot(WAKE_SLOT_JARVIS, -WAKE_WARMUP_SLICES);
            FrontendReset(&s_frontend);
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
            process_feature(feat, t0);
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
#if CONFIG_PM_ENABLE
        (void)esp_pm_lock_release(s_cpu_lock);
#endif
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

    esp_err_t err = wake_model_init();
    if (err != ESP_OK) {
        return err;
    }
    s_arena_used = (uint32_t)wake_model_arena_used(WAKE_SLOT_JARVIS);
    reset_slot(WAKE_SLOT_JARVIS, -WAKE_WARMUP_SLICES);

#if CONFIG_PM_ENABLE
    err = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "dsp", &s_cpu_lock);
    if (err != ESP_OK) {
        return err;
    }
#endif

    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    led_set(0);

    if (!frontend_init()) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "jarvis arena_used=%u model=%uB queue len=%u listen_ms=%d "
             "cut_j=%u/%u cut_cmd=%u/%u pcm_gain=%d",
             (unsigned)s_arena_used, (unsigned)wake_model_bytes(WAKE_SLOT_JARVIS),
             (unsigned)AUDIO_DSP_QUEUE_LEN, (int)(WAKE_LISTEN_US / 1000),
             (unsigned)WAKE_PROB_CUTOFF, (unsigned)WAKE_SLIDING_WINDOW,
             (unsigned)WAKE_CMD_PROB_CUTOFF, (unsigned)WAKE_CMD_SLIDING_WINDOW,
             AUDIO_PCM_GAIN);

    BaseType_t ok = xTaskCreatePinnedToCore(dsp_task, "dsp", DSP_TASK_STACK,
                                            NULL, DSP_TASK_PRIO, &s_dsp_task,
                                            DSP_TASK_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
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
        int32_t s = (dma_samples[i] >> 16) * AUDIO_PCM_GAIN;
        if (s > INT16_MAX) {
            s = INT16_MAX;
        } else if (s < INT16_MIN) {
            s = INT16_MIN;
        }
        block->pcm[i] = (int16_t)s;
    }

    if (xQueueSend(s_filled, &block, 0) != pdTRUE) {
        s_drops++;
        (void)xQueueSend(s_free, &block, 0);
        return false;
    }
    return true;
}

bool audio_dsp_listening(void) {
    return s_listening;
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
    const int64_t now = esp_timer_get_time();
    uint32_t left_ms = 0;
    if (s_listening && s_listen_deadline_us > now) {
        left_ms = (uint32_t)((s_listen_deadline_us - now) / 1000);
    }
    taskENTER_CRITICAL(&s_lock);
    out->blocks = s_blocks;
    out->proc_last_us = s_proc_last_us;
    out->proc_max_us = s_proc_max_us;
    out->slices = s_slices;
    out->infers = s_infers;
    out->infer_last_us = s_infer_last_us;
    out->infer_max_us = s_infer_max_us;
    out->det_jarvis = s_det_jarvis;
    out->det_on = s_det_on;
    out->det_off = s_det_off;
    out->listen_timeouts = s_listen_timeouts;
    out->prob_jarvis = s_prob_jarvis;
    out->prob_on = s_prob_on;
    out->prob_off = s_prob_off;
    out->resets = s_resets;
    out->arena_used = s_arena_used;
    taskEXIT_CRITICAL(&s_lock);
    out->listening = s_listening ? 1 : 0;
    out->listen_left_ms = left_ms;
    out->led = (uint32_t)s_led_on;
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
    s_det_jarvis = 0;
    s_det_on = 0;
    s_det_off = 0;
    s_listen_timeouts = 0;
    s_prob_jarvis = 0;
    s_prob_on = 0;
    s_prob_off = 0;
    s_resets = 0;
    taskEXIT_CRITICAL(&s_lock);
}
