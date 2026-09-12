#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// C API around the hey_jarvis INT8 streaming model (TFLM).
#define WAKE_FEATURE_SIZE 40
#define WAKE_MAX_STRIDE 16
#define WAKE_VAR_ARENA_SIZE 1024
#define WAKE_ARENA_SIZE 49152
#define WAKE_ARENA_IN_PSRAM 0     // 1 = PSRAM; internal SRAM is the default
#define WAKE_SLIDING_WINDOW 5
#define WAKE_PROB_CUTOFF 247      // 0.97 * 255
#define WAKE_WARMUP_SLICES 20     // VAD-gated; ESPHome uses 100 on a raw stream
#define WAKE_COOLDOWN_SLICES 100
#define WAKE_GAP_US 1000000

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wake_model_init(uint8_t *arena, size_t arena_size);
int wake_model_stride(void);
esp_err_t wake_model_invoke(const int8_t *feat, uint8_t *prob);
esp_err_t wake_model_reset(void);
size_t wake_model_arena_used(void);
size_t wake_model_bytes(void);

#ifdef __cplusplus
}
#endif
