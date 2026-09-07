#pragma once

#include "esp_err.h"

#define AUDIO_SAMPLE_RATE_HZ 16000
#define AUDIO_DMA_DESC_NUM 2
#define AUDIO_DMA_FRAME_NUM 512   // 512 / 16000 = 32 ms per buffer

#define VAD_RATIO_K 6      /* voice if ac_mean_sq > K * noise floor */
#define VAD_BOOT_BLOCKS 16     /* ~0.5 s after start: learn noise, LED off */
#define VAD_ON_BLOCKS 3      /* ~96 ms consecutive voice before LED on */
#define VAD_OFF_BLOCKS 4      /* ~128 ms consecutive quiet before LED off */


esp_err_t audio_ingest_start(void);
