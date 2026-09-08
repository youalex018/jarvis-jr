// DSP task implementation
#include "audio_dsp.h"
#include "audio_ingest.h"

#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"

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

static void dsp_task(void *arg) {
    (void)arg;
    audio_block_t *block;

    while (1) {
        if (xQueueReceive(s_filled, &block, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const int64_t t0 = esp_timer_get_time();
        ESP_LOGD(TAG, "rx n=%" PRIu32 " ac=%" PRIu64, block->n, block->ac_mean_sq);
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

    BaseType_t ok = xTaskCreatePinnedToCore(dsp_task, "dsp", DSP_TASK_STACK,
                                            NULL, DSP_TASK_PRIO, &s_dsp_task,
                                            DSP_TASK_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "dsp stub: voiced-only queue len=%u", (unsigned)AUDIO_DSP_QUEUE_LEN);
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
    taskEXIT_CRITICAL(&s_lock);
    out->drops = s_drops;
    out->depth = (uint32_t)audio_dsp_queue_waiting();
    out->stack_hwm = (uint32_t)uxTaskGetStackHighWaterMark(s_dsp_task);
}

void audio_dsp_reset_stats(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_blocks = 0;
    s_proc_last_us = 0;
    s_proc_max_us = 0;
    s_drops = 0;
    taskEXIT_CRITICAL(&s_lock);
}
