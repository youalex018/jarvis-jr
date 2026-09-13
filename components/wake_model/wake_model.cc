// TFLM C++ wrapper; C callers use include/wake_model.h.
#include "wake_model.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

extern "C" {
extern const uint8_t hey_jarvis_tflite[];
extern const size_t hey_jarvis_tflite_len;
#if WAKE_HAS_LIGHT_ON
extern const uint8_t light_on_tflite[];
extern const size_t light_on_tflite_len;
#endif
#if WAKE_HAS_LIGHT_OFF
extern const uint8_t light_off_tflite[];
extern const size_t light_off_tflite_len;
#endif
}

static const char *TAG = "wake";

typedef struct {
    tflite::MicroInterpreter *interp;
    tflite::MicroAllocator *var_alloc;
    tflite::MicroResourceVariables *mrv;
    const tflite::Model *model;
    uint8_t *arena;
    uint8_t *var_arena;
    uint8_t *model_copy;
    int stride;
    size_t arena_used;
    size_t model_bytes;
    bool ready;
    bool arena_psram;
} wake_slot_state_t;

static tflite::MicroMutableOpResolver<20> s_resolver;
static bool s_ops_ready;
static wake_slot_state_t s_slot[WAKE_SLOT_COUNT];

static const char *slot_name(wake_slot_t slot) {
    switch (slot) {
    case WAKE_SLOT_JARVIS:
        return "hey_jarvis";
    case WAKE_SLOT_LIGHT_ON:
        return "light_on";
    case WAKE_SLOT_LIGHT_OFF:
        return "light_off";
    default:
        return "?";
    }
}

static bool slot_ok(wake_slot_t slot) {
    return slot >= 0 && slot < WAKE_SLOT_COUNT;
}

static bool register_ops(void) {
    if (s_ops_ready) {
        return true;
    }
    if (s_resolver.AddCallOnce() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddVarHandle() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddReshape() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddReadVariable() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddStridedSlice() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddConcatenation() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddAssignVariable() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddConv2D() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddMul() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddAdd() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddMean() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddFullyConnected() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddLogistic() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddQuantize() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddDepthwiseConv2D() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddAveragePool2D() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddMaxPool2D() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddPad() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddPack() != kTfLiteOk) {
        return false;
    }
    if (s_resolver.AddSplitV() != kTfLiteOk) {
        return false;
    }
    s_ops_ready = true;
    return true;
}

static uint8_t *alloc_arena(wake_slot_t slot, bool *psram) {
    *psram = false;
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t spiram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (slot == WAKE_SLOT_JARVIS) {
#if WAKE_ARENA_IN_PSRAM
        uint8_t *p = (uint8_t *)heap_caps_aligned_alloc(16, WAKE_ARENA_SIZE, spiram);
        if (p != NULL) {
            *psram = true;
            return p;
        }
#endif
        return (uint8_t *)heap_caps_aligned_alloc(16, WAKE_ARENA_SIZE, internal);
    }
    uint8_t *p = (uint8_t *)heap_caps_aligned_alloc(16, WAKE_ARENA_SIZE, internal);
    if (p != NULL) {
        return p;
    }
    p = (uint8_t *)heap_caps_aligned_alloc(16, WAKE_ARENA_SIZE, spiram);
    if (p != NULL) {
        *psram = true;
    }
    return p;
}

// One function-static interpreter per slot (same pattern as TFLM examples).
static tflite::MicroInterpreter *make_interpreter(
    wake_slot_t slot, const tflite::Model *model, uint8_t *arena,
    tflite::MicroResourceVariables *mrv) {
    switch (slot) {
    case WAKE_SLOT_JARVIS: {
        static tflite::MicroInterpreter interp(
            model, s_resolver, arena, WAKE_ARENA_SIZE, mrv);
        return &interp;
    }
    case WAKE_SLOT_LIGHT_ON: {
        static tflite::MicroInterpreter interp(
            model, s_resolver, arena, WAKE_ARENA_SIZE, mrv);
        return &interp;
    }
    case WAKE_SLOT_LIGHT_OFF: {
        static tflite::MicroInterpreter interp(
            model, s_resolver, arena, WAKE_ARENA_SIZE, mrv);
        return &interp;
    }
    default:
        return NULL;
    }
}

static esp_err_t init_slot(wake_slot_t slot, const uint8_t *raw, size_t raw_len) {
    wake_slot_state_t *st = &s_slot[slot];
    memset(st, 0, sizeof(*st));
    if (raw == NULL || raw_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    st->model_bytes = raw_len;
    const uint8_t *model_bytes = raw;
    if (((uintptr_t)raw & 15u) != 0) {
        st->model_copy = (uint8_t *)heap_caps_aligned_alloc(
            16, raw_len, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (st->model_copy == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(st->model_copy, raw, raw_len);
        model_bytes = st->model_copy;
        ESP_LOGW(TAG, "%s copied model %uB to 16-byte aligned buffer",
                 slot_name(slot), (unsigned)raw_len);
    }

    st->model = tflite::GetModel(model_bytes);
    if (st->model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "%s schema %u != %d", slot_name(slot), st->model->version(),
                 TFLITE_SCHEMA_VERSION);
        return ESP_FAIL;
    }

    st->arena = alloc_arena(slot, &st->arena_psram);
    if (st->arena == NULL) {
        ESP_LOGE(TAG, "%s arena alloc %u failed", slot_name(slot),
                 (unsigned)WAKE_ARENA_SIZE);
        return ESP_ERR_NO_MEM;
    }

    st->var_arena = (uint8_t *)heap_caps_aligned_alloc(
        16, WAKE_VAR_ARENA_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (st->var_arena == NULL) {
        return ESP_ERR_NO_MEM;
    }
    st->var_alloc = tflite::MicroAllocator::Create(st->var_arena, WAKE_VAR_ARENA_SIZE);
    if (st->var_alloc == NULL) {
        return ESP_ERR_NO_MEM;
    }
    st->mrv = tflite::MicroResourceVariables::Create(st->var_alloc, 20);
    if (st->mrv == NULL) {
        return ESP_ERR_NO_MEM;
    }

    st->interp = make_interpreter(slot, st->model, st->arena, st->mrv);
    if (st->interp == NULL) {
        ESP_LOGE(TAG, "%s MicroInterpreter init failed", slot_name(slot));
        return ESP_FAIL;
    }

    if (st->interp->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "%s AllocateTensors failed (arena %u)", slot_name(slot),
                 (unsigned)WAKE_ARENA_SIZE);
        st->interp = NULL;
        return ESP_ERR_NO_MEM;
    }

    TfLiteTensor *in = st->interp->input(0);
    TfLiteTensor *out = st->interp->output(0);
    if (in == NULL || in->dims == NULL || in->dims->size != 3 ||
        in->dims->data[0] != 1 || in->dims->data[2] != WAKE_FEATURE_SIZE ||
        in->type != kTfLiteInt8) {
        ESP_LOGE(TAG, "%s unexpected input tensor", slot_name(slot));
        return ESP_FAIL;
    }
    if (out == NULL || out->type != kTfLiteUInt8) {
        ESP_LOGE(TAG, "%s unexpected output tensor", slot_name(slot));
        return ESP_FAIL;
    }
    if (in->dims->data[1] <= 0 || in->dims->data[1] > WAKE_MAX_STRIDE) {
        ESP_LOGE(TAG, "%s stride %d out of range", slot_name(slot), in->dims->data[1]);
        return ESP_FAIL;
    }

    st->stride = in->dims->data[1];
    st->arena_used = st->interp->arena_used_bytes();
    st->ready = true;
    ESP_LOGI(TAG, "%s model=%uB stride=%d arena_used=%u/%u %s in=%dx%dx%d",
             slot_name(slot), (unsigned)st->model_bytes, st->stride,
             (unsigned)st->arena_used, (unsigned)WAKE_ARENA_SIZE,
             st->arena_psram ? "psram" : "internal",
             in->dims->data[0], in->dims->data[1], in->dims->data[2]);
    return ESP_OK;
}

extern "C" esp_err_t wake_model_init(void) {
    if (!register_ops()) {
        ESP_LOGE(TAG, "op resolver failed");
        return ESP_FAIL;
    }

    esp_err_t err = init_slot(WAKE_SLOT_JARVIS, hey_jarvis_tflite, hey_jarvis_tflite_len);
    if (err != ESP_OK) {
        return err;
    }

#if WAKE_HAS_LIGHT_ON
    if (init_slot(WAKE_SLOT_LIGHT_ON, light_on_tflite, light_on_tflite_len) != ESP_OK) {
        ESP_LOGW(TAG, "light_on init failed; commands disabled for this slot");
        s_slot[WAKE_SLOT_LIGHT_ON].ready = false;
    }
#endif
#if WAKE_HAS_LIGHT_OFF
    if (init_slot(WAKE_SLOT_LIGHT_OFF, light_off_tflite, light_off_tflite_len) != ESP_OK) {
        ESP_LOGW(TAG, "light_off init failed; commands disabled for this slot");
        s_slot[WAKE_SLOT_LIGHT_OFF].ready = false;
    }
#endif

    if (!s_slot[WAKE_SLOT_LIGHT_ON].ready && !s_slot[WAKE_SLOT_LIGHT_OFF].ready) {
        ESP_LOGI(TAG, "cmd models: none");
    }
    return ESP_OK;
}

extern "C" bool wake_model_ready(wake_slot_t slot) {
    return slot_ok(slot) && s_slot[slot].ready;
}

extern "C" int wake_model_stride(wake_slot_t slot) {
    if (!wake_model_ready(slot)) {
        return 0;
    }
    return s_slot[slot].stride;
}

extern "C" esp_err_t wake_model_invoke(wake_slot_t slot, const int8_t *feat, uint8_t *prob) {
    if (!wake_model_ready(slot) || feat == NULL || prob == NULL || s_slot[slot].stride <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    TfLiteTensor *in = s_slot[slot].interp->input(0);
    memcpy(in->data.int8, feat, (size_t)s_slot[slot].stride * WAKE_FEATURE_SIZE);
    if (s_slot[slot].interp->Invoke() != kTfLiteOk) {
        return ESP_FAIL;
    }
    *prob = s_slot[slot].interp->output(0)->data.uint8[0];
    return ESP_OK;
}

extern "C" esp_err_t wake_model_reset(wake_slot_t slot) {
    if (!wake_model_ready(slot)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_slot[slot].interp->Reset() != kTfLiteOk) {
        return ESP_FAIL;
    }
    if (s_slot[slot].mrv != NULL) {
        (void)s_slot[slot].mrv->ResetAll();
    }
    return ESP_OK;
}

extern "C" size_t wake_model_arena_used(wake_slot_t slot) {
    if (!wake_model_ready(slot)) {
        return 0;
    }
    return s_slot[slot].arena_used;
}

extern "C" size_t wake_model_bytes(wake_slot_t slot) {
    if (!wake_model_ready(slot)) {
        return 0;
    }
    return s_slot[slot].model_bytes;
}
