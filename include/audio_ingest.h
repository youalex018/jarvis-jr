#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_SAMPLE_RATE_HZ 16000
#define AUDIO_DMA_DESC_NUM 2
#define AUDIO_DMA_FRAME_NUM 512   // 512 / 16000 = 32 ms per buffer

#define VAD_RATIO_K 3             // voice if ac_mean_sq > K * noise floor
#define VAD_BOOT_BLOCKS 16        // ~0.5 s after start: learn noise
#define VAD_HANGOVER_BLOCKS 8     // ~256 ms extra voiced queue after speech

typedef struct {
    uint32_t blocks;         // total DMA blocks processed
    uint32_t overrun;        // ISR saw previous block unconsumed
    uint32_t proc_last_us;   // ingest block processing time
    uint32_t proc_max_us;
    uint64_t proc_total_us;  // for avg = total / blocks
    uint32_t period_last_us; // wake-to-wake - expect ~32000
    uint32_t period_max_us;
    uint32_t period_min_us;
    uint64_t noise;
    int voiced;
    int led_on;              // listen window (DSP), not VAD
    uint32_t stack_hwm;      // bytes remaining
} audio_ingest_stats_t;

esp_err_t audio_ingest_start(void);
void audio_ingest_get_stats(audio_ingest_stats_t *out);
void audio_ingest_reset_stats(void); // zero max/min/overrun/totals; keep noise + VAD
void audio_ingest_set_log(bool on);  // 1 Hz ESP_LOGI on/off
