// TFLM C++ wrapper; C callers use include/wake_model.h.
#include "wake_model.h"

#include <cstring>
#include <new>

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
}

static const char *TAG = "wake";

static tflite::MicroMutableOpResolver<20> s_resolver;
static uint8_t *s_var_arena;
static tflite::MicroAllocator *s_var_alloc;
static tflite::MicroResourceVariables *s_mrv;
static tflite::MicroInterpreter *s_interp;
static const tflite::Model *s_model;
static uint8_t *s_model_copy;
static int s_stride;
static size_t s_arena_used;
static size_t s_model_bytes;
static bool s_ops_ready;

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

extern "C" esp_err_t wake_model_init(uint8_t *arena, size_t arena_size) {
    if (arena == NULL || arena_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_model_bytes = hey_jarvis_tflite_len;
    const uint8_t *raw = hey_jarvis_tflite;
    const uint8_t *model_bytes = raw;
    if (((uintptr_t)raw & 15u) != 0) {
        s_model_copy = (uint8_t *)heap_caps_aligned_alloc(
            16, s_model_bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (s_model_copy == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(s_model_copy, raw, s_model_bytes);
        model_bytes = s_model_copy;
        ESP_LOGW(TAG, "copied model %uB to 16-byte aligned buffer",
                 (unsigned)s_model_bytes);
    }

    s_model = tflite::GetModel(model_bytes);
    if (s_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "schema %u != %d", s_model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_FAIL;
    }

    if (!register_ops()) {
        ESP_LOGE(TAG, "op resolver failed");
        return ESP_FAIL;
    }

    s_var_arena = (uint8_t *)heap_caps_aligned_alloc(
        16, WAKE_VAR_ARENA_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_var_arena == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_var_alloc = tflite::MicroAllocator::Create(s_var_arena, WAKE_VAR_ARENA_SIZE);
    if (s_var_alloc == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_mrv = tflite::MicroResourceVariables::Create(s_var_alloc, 20);
    if (s_mrv == NULL) {
        return ESP_ERR_NO_MEM;
    }

    alignas(tflite::MicroInterpreter) static uint8_t interp_store[sizeof(tflite::MicroInterpreter)];
    s_interp = new (static_cast<void *>(interp_store)) tflite::MicroInterpreter(
        s_model, s_resolver, arena, arena_size, s_mrv);

    if (s_interp->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed (arena %u)", (unsigned)arena_size);
        return ESP_ERR_NO_MEM;
    }

    TfLiteTensor *in = s_interp->input(0);
    TfLiteTensor *out = s_interp->output(0);
    if (in == NULL || in->dims == NULL || in->dims->size != 3 ||
        in->dims->data[0] != 1 || in->dims->data[2] != WAKE_FEATURE_SIZE ||
        in->type != kTfLiteInt8) {
        ESP_LOGE(TAG, "unexpected input tensor");
        return ESP_FAIL;
    }
    if (out == NULL || out->type != kTfLiteUInt8) {
        ESP_LOGE(TAG, "unexpected output tensor");
        return ESP_FAIL;
    }
    if (in->dims->data[1] <= 0 || in->dims->data[1] > WAKE_MAX_STRIDE) {
        ESP_LOGE(TAG, "stride %d out of range", in->dims->data[1]);
        return ESP_FAIL;
    }

    s_stride = in->dims->data[1];
    s_arena_used = s_interp->arena_used_bytes();
    ESP_LOGI(TAG, "hey_jarvis model=%uB stride=%d arena_used=%u/%u in=%dx%dx%d",
             (unsigned)s_model_bytes, s_stride, (unsigned)s_arena_used,
             (unsigned)arena_size, in->dims->data[0], in->dims->data[1],
             in->dims->data[2]);
    return ESP_OK;
}

extern "C" int wake_model_stride(void) {
    return s_stride;
}

extern "C" esp_err_t wake_model_invoke(const int8_t *feat, uint8_t *prob) {
    if (s_interp == NULL || feat == NULL || prob == NULL || s_stride <= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    TfLiteTensor *in = s_interp->input(0);
    memcpy(in->data.int8, feat, (size_t)s_stride * WAKE_FEATURE_SIZE);
    if (s_interp->Invoke() != kTfLiteOk) {
        return ESP_FAIL;
    }
    *prob = s_interp->output(0)->data.uint8[0];
    return ESP_OK;
}

extern "C" esp_err_t wake_model_reset(void) {
    if (s_interp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_interp->Reset() != kTfLiteOk) {
        return ESP_FAIL;
    }
    if (s_mrv != NULL) {
        (void)s_mrv->ResetAll();
    }
    return ESP_OK;
}

extern "C" size_t wake_model_arena_used(void) {
    return s_arena_used;
}

extern "C" size_t wake_model_bytes(void) {
    return s_model_bytes;
}
