#pragma once

#include "esp_err.h"

#define AUDIO_SAMPLE_RATE_HZ 16000
#define AUDIO_DMA_DESC_NUM 2
#define AUDIO_DMA_FRAME_NUM  512   // 512 / 16000 = 32 ms per buffer
#define VAD_MEAN_SQ_MIN 1ULL


esp_err_t audio_ingest_start(void);
