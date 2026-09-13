// Ingest audio from I2S microphone (INMP441) at 16 kHz, 32-bit Philips format, mono left channel
#include "audio_ingest.h"
#include "audio_dsp.h"
#include "board.h"

#include <inttypes.h>
#include <stdint.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"

static const char *TAG = "ingest";

#define INGEST_TASK_STACK 4096
#define INGEST_TASK_PRIO 10          // high - above DSP and net; Wi-Fi driver is higher
#define INGEST_TASK_CORE 1           // APP_CPU; PRO_CPU hosts Wi-Fi / net
#define LOG_EVERY_BUFFERS 32          // ~1 s at 16 kHz / 512 frames

typedef struct {
    const int32_t *samples;
    size_t nbytes;
} dma_block_t; // DMA block descriptor passed from ISR to ingest task

static i2s_chan_handle_t s_rx;
static TaskHandle_t s_ingest_task;
static SemaphoreHandle_t s_dma_sem;
static volatile dma_block_t s_block;
static volatile uint32_t s_overrun;
static volatile uint32_t s_buffers;
static uint64_t s_noise;
static uint32_t s_boot_left = VAD_BOOT_BLOCKS;
static uint32_t s_hang_left;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_ingest_stats_t s_stats;
static int64_t s_prev_wake;
static bool s_log_enabled = true;

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

        const int64_t t0 = esp_timer_get_time();
        const int32_t *samples = s_block.samples;
        size_t nbytes = s_block.nbytes;
        s_block.samples = NULL;

        if (samples == NULL || nbytes < sizeof(int32_t)) {
            continue;
        }

        const size_t n = nbytes / sizeof(int32_t);
        int32_t min_s = INT32_MAX;
        int32_t max_s = INT32_MIN;
        int64_t sum = 0;

        for (size_t i = 0; i < n; i++) {
            const int32_t s = samples[i] >> 8;
            sum += s;
            if (s < min_s) {
                min_s = s;
            }
            if (s > max_s) {
                max_s = s;
            }
        }
        // Compute DC offset (mean) and AC power (mean squared zero-mean samples)
        const int32_t dc = (int32_t)(sum / (int64_t)n);

        uint64_t acc_ac = 0;
        for (size_t i = 0; i < n; i++) {
            const int64_t d = (int64_t)(samples[i] >> 8) - dc;
            acc_ac += (uint64_t)(d * d); // accumulate squared AC samples
        }
        const uint64_t ac_mean_sq = acc_ac / n;

        int voiced;
        if (s_boot_left > 0) {
            s_boot_left--;
            voiced = 0;
            s_noise = (s_noise == 0) ? ac_mean_sq : (s_noise * 3 + ac_mean_sq) / 4; // learn noise floor
        } else {
            uint64_t thresh = UINT64_MAX;
            if (s_noise <= (UINT64_MAX / VAD_RATIO_K)) {
                thresh = s_noise * VAD_RATIO_K; // voice if ac_mean_sq > K * noise floor
            }
            voiced = (ac_mean_sq > thresh);
            if (!voiced) {
                s_noise = (s_noise * 19 + ac_mean_sq) / 20;
            }
        }

        const bool listening = audio_dsp_listening();
        if (voiced) {
            s_hang_left = VAD_HANGOVER_BLOCKS;
        }
        if (listening || voiced || s_hang_left > 0) {
            (void)audio_dsp_try_submit(samples, n, ac_mean_sq);
            if (!voiced && !listening) {
                s_hang_left--;
            }
        }

        const int64_t t1 = esp_timer_get_time();
        const uint32_t proc_us = (uint32_t)(t1 - t0);
        uint32_t period_us = 0;
        if (s_prev_wake != 0) {
            period_us = (uint32_t)(t0 - s_prev_wake);
        }
        s_prev_wake = t0;

        uint32_t proc_max_us;
        uint32_t period_max_us;
        taskENTER_CRITICAL(&s_lock);
        s_stats.blocks++;
        s_stats.proc_last_us = proc_us;
        if (proc_us > s_stats.proc_max_us) {
            s_stats.proc_max_us = proc_us;
        }
        s_stats.proc_total_us += proc_us;
        if (period_us != 0) {
            s_stats.period_last_us = period_us;
            if (period_us > s_stats.period_max_us) {
                s_stats.period_max_us = period_us;
            }
            if (s_stats.period_min_us == 0 || period_us < s_stats.period_min_us) {
                s_stats.period_min_us = period_us;
            }
        }
        s_stats.noise = s_noise;
        s_stats.voiced = voiced;
        s_stats.led_on = listening ? 1 : 0;
        proc_max_us = s_stats.proc_max_us;
        period_max_us = s_stats.period_max_us;
        taskEXIT_CRITICAL(&s_lock);

        const uint32_t count = ++s_buffers;
        if (s_log_enabled && (count % LOG_EVERY_BUFFERS) == 0) {
            ESP_LOGI(TAG,
                     "buf=%" PRIu32 " frames=%u dc=%" PRId32
                     " ac=%" PRIu64 " noise=%" PRIu64 " voiced=%d listen=%d"
                     " min=%" PRId32 " max=%" PRId32 " ovf=%" PRIu32
                     " hwm_in=%u hwm_dsp=%u qdepth=%u qdrop=%" PRIu32
                     " proc_us=%" PRIu32 "/%" PRIu32
                     " period_us=%" PRIu32 "/%" PRIu32,
                     count, (unsigned)n, dc, ac_mean_sq, s_noise, voiced, (int)listening,
                     min_s, max_s, s_overrun,
                     (unsigned)uxTaskGetStackHighWaterMark(s_ingest_task),
                     (unsigned)uxTaskGetStackHighWaterMark(audio_dsp_task()),
                     (unsigned)audio_dsp_queue_waiting(),
                     audio_dsp_drops(),
                     proc_us, proc_max_us,
                     period_us, period_max_us);
        }
    }
}

void audio_ingest_get_stats(audio_ingest_stats_t *out) {
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_stats;
    out->overrun = s_overrun;
    taskEXIT_CRITICAL(&s_lock);
    out->stack_hwm = (uint32_t)uxTaskGetStackHighWaterMark(s_ingest_task);
}

void audio_ingest_reset_stats(void) {
    // ISR may increment s_overrun on this or the other core during the store.
    // A rare lost count is acceptable for a debug counter
    taskENTER_CRITICAL(&s_lock);
    s_overrun = 0;
    s_stats.blocks = 0;
    s_stats.overrun = 0;
    s_stats.proc_last_us = 0;
    s_stats.proc_max_us = 0;
    s_stats.proc_total_us = 0;
    s_stats.period_last_us = 0;
    s_stats.period_max_us = 0;
    s_stats.period_min_us = 0;
    taskEXIT_CRITICAL(&s_lock);
}

void audio_ingest_set_log(bool on) {
    s_log_enabled = on;
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
                                            NULL, INGEST_TASK_PRIO, &s_ingest_task,
                                            INGEST_TASK_CORE); // high priority to avoid DMA overrun
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
    ESP_LOGI(TAG, "VAD: ac > %d*noise, hangover=%d blocks (~32 ms each); idle gate only",
             VAD_RATIO_K, VAD_HANGOVER_BLOCKS);
    ESP_LOGI(TAG, "LED GPIO %d on during Hey Jarvis listen window", (int)LED_GPIO);
    return ESP_OK;
}
