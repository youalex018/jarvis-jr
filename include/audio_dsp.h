#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_DSP_QUEUE_LEN 4   // ~128 ms of voiced 32 ms blocks
#define DSP_TASK_STACK 3072
#define DSP_TASK_PRIO 5         // medium — below ingest, above future net
#define DSP_TASK_CORE 1         // APP_CPU; PRO_CPU stays free for Wi-Fi

esp_err_t audio_dsp_start(void);

// Copy a DMA block to int16 and enqueue. Never blocks. False = dropped.
bool audio_dsp_try_submit(const int32_t *dma_samples, size_t n, uint64_t ac_mean_sq);

uint32_t audio_dsp_drops(void);
UBaseType_t audio_dsp_queue_waiting(void);
TaskHandle_t audio_dsp_task(void);
