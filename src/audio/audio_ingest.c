// Ingest audio from I2S microphone (INMP441) at 16 kHz, 32-bit Philips format, mono left channel
#include "audio_ingest.h"
#include "board.h"

#include <inttypes.h>
#include <stdint.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"

static const char *TAG = "ingest";

#define INGEST_TASK_STACK 4096
#define INGEST_TASK_PRIO 10          // high - Wi-Fi stays higher when added
#define INGEST_TASK_CORE 1           // APP_CPU - PRO_CPU hosts Wi-Fi later
#define LOG_EVERY_BUFFERS 32          // ~1 s at 16 kHz / 512 frames

typedef struct {
    const int32_t *samples;
    size_t nbytes;
} dma_block_t;

static i2s_chan_handle_t s_rx;
static SemaphoreHandle_t s_dma_sem;
static volatile dma_block_t s_block;
static volatile uint32_t s_overrun;
static volatile uint32_t s_buffers;

static bool IRAM_ATTR on_recv(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx) {
    (void)handle;
    (void)user_ctx;
    BaseType_t woken = pdFALSE;

    if (s_block.samples != NULL) {
        s_overrun++;
    }
    s_block.samples = (const int32_t *)event->dma_buf;
    s_block.nbytes = event->size;
    xSemaphoreGiveFromISR(s_dma_sem, &woken);
    return woken == pdTRUE;
}

static void ingest_task(void *arg) {
    (void)arg;

    while (1) {
        if (xSemaphoreTake(s_dma_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        const int32_t *samples = s_block.samples;
        size_t nbytes = s_block.nbytes;
        s_block.samples = NULL;

        if (samples == NULL || nbytes < sizeof(int32_t)) {
            continue;
        }

        const size_t n = nbytes / sizeof(int32_t);
        int32_t min_s = INT32_MAX;
        int32_t max_s = INT32_MIN;

        uint64_t acc = 0;
        for (size_t i = 0; i < n; i++) {
            // INMP441: 24-bit left-justified in a 32-bit I2S slot
            const int32_t s = samples[i] >> 8;
            acc += (int64_t)s * (int64_t)s;

            if (s < min_s) {
                min_s = s;
            }
            if (s > max_s) {
                max_s = s;
            }
        }
        uint64_t mean_sq = acc / n;
        int voiced = (mean_sq >= VAD_MEAN_SQ_MIN);

        if (!voiced) {
            continue;
        }

        const uint32_t count = ++s_buffers;
        if ((count % LOG_EVERY_BUFFERS) == 0) {
            ESP_LOGI(TAG,
                     "buf=%" PRIu32 " frames=%u min=%" PRId32 " max=%" PRId32
                     " ovf=%" PRIu32 " first=%08" PRIx32,
                     count, (unsigned)n, min_s, max_s, s_overrun,
                     (uint32_t)samples[0]);
        }
    }
}

esp_err_t audio_ingest_start(void) {
    s_dma_sem = xSemaphoreCreateBinary();
    if (s_dma_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = AUDIO_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = AUDIO_DMA_FRAME_NUM;

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK_GPIO,
            .ws = I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_SD_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    // L/R tied to GND → left slot. Default mono mask is already LEFT on ESP32
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }

    i2s_event_callbacks_t cbs = {
        .on_recv = on_recv,
        .on_recv_q_ovf = NULL,   // we never i2s_channel_read(); ignore SW queue
        .on_sent = NULL,
        .on_send_q_ovf = NULL,
    };
    err = i2s_channel_register_event_callback(s_rx, &cbs, NULL);
    if (err != ESP_OK) {
        return err;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(ingest_task, "ingest", INGEST_TASK_STACK,
                                            NULL, INGEST_TASK_PRIO, NULL, INGEST_TASK_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "I2S RX 16 kHz Philips 32-bit, ping-pong DMA (%u x %u frames)",
             AUDIO_DMA_DESC_NUM, AUDIO_DMA_FRAME_NUM);
    ESP_LOGI(TAG, "INMP441  BCLK=%d WS=%d SD=%d  (L/R=GND, VDD=3V3)",
             (int)I2S_BCLK_GPIO, (int)I2S_WS_GPIO, (int)I2S_SD_GPIO);
    return ESP_OK;
}
