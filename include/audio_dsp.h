#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_DSP_QUEUE_LEN 8   // ~256 ms; listen window submits every 32 ms block
#define AUDIO_PCM_GAIN 4        // saturating multiply after I2S >>16
#define DSP_TASK_STACK 8192
#define DSP_TASK_PRIO 5         // medium - below ingest, above net
#define DSP_TASK_CORE 1         // APP_CPU; PRO_CPU stays free for Wi-Fi

esp_err_t audio_dsp_start(void);

// Copy a DMA block to int16 and enqueue. Never blocks. False = dropped.
bool audio_dsp_try_submit(const int32_t *dma_samples, size_t n, uint64_t ac_mean_sq);

// True while Hey Jarvis listen window is open (ingest submits every block).
bool audio_dsp_listening(void);

uint32_t audio_dsp_drops(void);
UBaseType_t audio_dsp_queue_waiting(void);
TaskHandle_t audio_dsp_task(void);

typedef struct {
    uint32_t blocks;         // blocks consumed
    uint32_t drops;
    uint32_t depth;          // filled queue now
    uint32_t proc_last_us;
    uint32_t proc_max_us;
    uint32_t stack_hwm;
    uint32_t slices;
    uint32_t infers;
    uint32_t infer_last_us;
    uint32_t infer_max_us;
    uint32_t listening;
    uint32_t listen_left_ms;
    uint32_t led;
    uint32_t det_jarvis;
    uint32_t det_on;
    uint32_t det_off;
    uint32_t listen_timeouts;
    uint32_t prob_jarvis;
    uint32_t prob_on;
    uint32_t prob_off;
    uint32_t resets;
    uint32_t arena_used;
} audio_dsp_stats_t;

void audio_dsp_get_stats(audio_dsp_stats_t *out);
void audio_dsp_reset_stats(void);
