#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// C API around INT8 streaming microWakeWord models (TFLM).
#define WAKE_FEATURE_SIZE 40
#define WAKE_MAX_STRIDE 16
#define WAKE_VAR_ARENA_SIZE 1024
#define WAKE_ARENA_SIZE 49152
#define WAKE_ARENA_IN_PSRAM 0     // 1 = PSRAM for hey_jarvis; internal is default
#define WAKE_SLIDING_WINDOW 5
#define WAKE_CMD_SLIDING_WINDOW 3 // shorter phrases than Hey Jarvis
#define WAKE_PROB_CUTOFF 230      // 0.90 * 255; official hey_jarvis is 0.97
#define WAKE_CMD_PROB_CUTOFF 204  // 0.80 * 255; listen window only
#define WAKE_WARMUP_SLICES 20     // VAD-gated; ESPHome uses 100 on a raw stream
#define WAKE_COOLDOWN_SLICES 100
#define WAKE_GAP_US 1000000
#define WAKE_LISTEN_US 3000000    // command window after Hey Jarvis

typedef enum {
    WAKE_SLOT_JARVIS = 0,
    WAKE_SLOT_LIGHT_ON,
    WAKE_SLOT_LIGHT_OFF,
    WAKE_SLOT_COUNT
} wake_slot_t;

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wake_model_init(void);
bool wake_model_ready(wake_slot_t slot);
int wake_model_stride(wake_slot_t slot);
esp_err_t wake_model_invoke(wake_slot_t slot, const int8_t *feat, uint8_t *prob);
esp_err_t wake_model_reset(wake_slot_t slot);
size_t wake_model_arena_used(wake_slot_t slot);
size_t wake_model_bytes(wake_slot_t slot);

#ifdef __cplusplus
}
#endif
