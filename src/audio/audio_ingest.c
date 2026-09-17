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
static uint32_t s_boot_skip = VAD_BOOT_SKIP_BLOCKS; // clock-start transient; do not learn from it
static uint32_t s_boot_left = VAD_BOOT_BLOCKS;
static uint32_t s_hang_left;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_ingest_stats_t s_stats;
static int64_t s_prev_wake;
static int64_t s_last_voice_us;
static uint64_t s_probe_noise;
static bool s_log_enabled = true;
static volatile bool s_doze_enabled = true;
static volatile bool s_doze_now;

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

static bool wait_dma_block(const int32_t **samples, size_t *n) {
    if (xSemaphoreTake(s_dma_sem, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const int32_t *p = s_block.samples;
    const size_t nbytes = s_block.nbytes;
    s_block.samples = NULL;
    if (p == NULL || nbytes < sizeof(int32_t)) {
        return false;
    }
    *samples = p;
    *n = nbytes / sizeof(int32_t);
    return true;
}

static void dma_clear_pending(void) {
    s_block.samples = NULL;
    while (xSemaphoreTake(s_dma_sem, 0) == pdTRUE) {
    }
}

static bool i2s_stop_for_doze(void) {
    const esp_err_t err = i2s_channel_disable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s disable %s", esp_err_to_name(err));
        return false;
    }
    dma_clear_pending();
    return true;
}

static bool i2s_start_for_probe(void) {
    const esp_err_t err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "i2s enable %s", esp_err_to_name(err));
        return false;
    }
    s_prev_wake = 0;
    return true;
}

static uint64_t block_ac_mean_sq(const int32_t *samples, size_t n,
                                int32_t *dc_out, int32_t *min_out, int32_t *max_out) {
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
    const int32_t dc = (int32_t)(sum / (int64_t)n);
    uint64_t acc_ac = 0;
    for (size_t i = 0; i < n; i++) {
        const int64_t d = (int64_t)(samples[i] >> 8) - dc;
        acc_ac += (uint64_t)(d * d);
    }
    if (dc_out != NULL) {
        *dc_out = dc;
    }
    if (min_out != NULL) {
        *min_out = min_s;
    }
    if (max_out != NULL) {
        *max_out = max_s;
    }
    return acc_ac / n;
}

static int vad_against(uint64_t ac_mean_sq, uint64_t noise, int ratio_k) {
    if (ratio_k <= 0 || noise == 0) {
        return 0;
    }
    if (noise > (UINT64_MAX / (uint64_t)ratio_k)) {
        return 0;
    }
    return ac_mean_sq > (noise * (uint64_t)ratio_k);
}

// VAD + optional DSP submit + ingest stats. Returns 1 if this block is voiced.
static int process_block(const int32_t *samples, size_t n) {
    const int64_t t0 = esp_timer_get_time();
    int32_t min_s;
    int32_t max_s;
    int32_t dc;
    const uint64_t ac_mean_sq = block_ac_mean_sq(samples, n, &dc, &min_s, &max_s);

    int voiced;
    if (s_boot_skip > 0) {
        // Same restart energy doze sees on every re-enable. Learning the
        // floor from it made VAD deaf (floor ~300x too high).
        s_boot_skip--;
        voiced = 0;
    } else if (s_boot_left > 0) {
        s_boot_left--;
        voiced = 0;
        s_noise = (s_noise == 0) ? ac_mean_sq : (s_noise * 3 + ac_mean_sq) / 4; // learn noise floor
        if (s_boot_left == 0) {
            s_last_voice_us = t0; // start quiet timer after boot learn
            ESP_LOGI(TAG, "VAD floor learned=%" PRIu64, s_noise);
        }
    } else if (vad_against(ac_mean_sq, s_noise, VAD_GLITCH_K)) {
        voiced = 0; // DMA junk: neither voice nor a floor sample
    } else {
        voiced = vad_against(ac_mean_sq, s_noise, VAD_RATIO_K);
        if (!voiced) {
            s_noise = (s_noise * 19 + ac_mean_sq) / 20;
        }
    }

    // Only a wake-word/listen event resets the doze clock. Magnitude VAD
    // also fires on unnoticed room noise and I2S tails, so it is not a
    // reliable signal that the user interacted with the device.
    const bool listening = audio_dsp_listening();
    if (listening) {
        s_last_voice_us = t0;
    }

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

    uint32_t quiet_ms = 0;
    if (s_last_voice_us != 0 && t0 >= s_last_voice_us) {
        quiet_ms = (uint32_t)((t0 - s_last_voice_us) / 1000);
    }

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
    s_stats.doze_en = s_doze_enabled ? 1 : 0;
    s_stats.quiet_ms = quiet_ms;
    s_stats.hang = s_hang_left;
    proc_max_us = s_stats.proc_max_us;
    period_max_us = s_stats.period_max_us;
    taskEXIT_CRITICAL(&s_lock);

    const uint32_t count = ++s_buffers;
    if (s_log_enabled && (count % LOG_EVERY_BUFFERS) == 0) {
        ESP_LOGI(TAG,
                 "buf=%" PRIu32 " frames=%u dc=%" PRId32
                 " ac=%" PRIu64 " noise=%" PRIu64 " voiced=%d listen=%d"
                 " hang=%" PRIu32 " doze_en=%d quiet_ms=%" PRIu32
                 " min=%" PRId32 " max=%" PRId32 " ovf=%" PRIu32
                 " hwm_in=%u hwm_dsp=%u qdepth=%u qdrop=%" PRIu32
                 " proc_us=%" PRIu32 "/%" PRIu32
                 " period_us=%" PRIu32 "/%" PRIu32,
                 count, (unsigned)n, dc, ac_mean_sq, s_noise, voiced, (int)listening,
                 s_hang_left, (int)s_doze_enabled, quiet_ms,
                 min_s, max_s, s_overrun,
                 (unsigned)uxTaskGetStackHighWaterMark(s_ingest_task),
                 (unsigned)uxTaskGetStackHighWaterMark(audio_dsp_task()),
                 (unsigned)audio_dsp_queue_waiting(),
                 audio_dsp_drops(),
                 proc_us, proc_max_us,
                 period_us, period_max_us);
    }
    return voiced;
}

static void run_doze(void) {
    if (!i2s_stop_for_doze()) {
        return;
    }
    s_probe_noise = s_noise;
    s_hang_left = 0;
    taskENTER_CRITICAL(&s_lock);
    s_stats.dozing = 1;
    s_stats.doze_cycles++;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "doze enter floor=%" PRIu64, s_probe_noise);

    while (s_doze_enabled) {
        vTaskDelay(pdMS_TO_TICKS(DOZE_SLEEP_MS));
        if (!s_doze_enabled) {
            (void)i2s_start_for_probe();
            break;
        }
        if (!i2s_start_for_probe()) {
            continue;
        }

        uint64_t last_ac = 0;
        for (uint32_t i = 0; i < DOZE_FLUSH_BLOCKS; i++) {
            const int32_t *samples;
            size_t n;
            if (!wait_dma_block(&samples, &n)) {
                continue;
            }
        }

        // Drop only the first invalid DMA blocks, then listen. A timed
        // discard can remove "Hey" and leave only "Jarvis" for the model.
        // The model rejects the restart tail; only extreme junk is skipped.
        audio_dsp_begin_probe();
        audio_dsp_stats_t dsp0;
        audio_dsp_get_stats(&dsp0);
        const uint32_t inf0 = dsp0.infers;
        const uint32_t sl0 = dsp0.slices;
        int heard = 0;
        uint32_t hits = 0;
        uint32_t seen = 0;
        uint32_t dsp_blocks = 0;
        uint32_t skipped = 0;
        uint64_t max_ac = 0;
        for (uint32_t i = 0; i < DOZE_PROBE_MAX_BLOCKS; i++) {
            if (audio_dsp_listening()) {
                heard = 1;
                break;
            }
            const int32_t *samples;
            size_t n;
            if (!wait_dma_block(&samples, &n)) {
                continue;
            }
            seen++;
            last_ac = block_ac_mean_sq(samples, n, NULL, NULL, NULL);
            if (last_ac > max_ac) {
                max_ac = last_ac;
            }
            taskENTER_CRITICAL(&s_lock);
            s_stats.doze_probes++;
            taskEXIT_CRITICAL(&s_lock);
            if (vad_against(last_ac, s_probe_noise, DOZE_PROBE_GLITCH_K)) {
                skipped++;
                continue; // extreme DMA junk, well above observed loud speech
            }
            if (vad_against(last_ac, s_probe_noise, DOZE_PROBE_RATIO_K)) {
                hits++;
            }
            if (audio_dsp_try_submit(samples, n, last_ac)) {
                dsp_blocks++;
                taskENTER_CRITICAL(&s_lock);
                s_stats.doze_dsp_blocks++;
                taskEXIT_CRITICAL(&s_lock);
            }
        }

        // Ingest is higher prio than DSP on the same core. wait_dma_block
        // can return immediately on a pending ping-pong buffer, so yield
        // until DSP is idle or listening.
        for (uint32_t i = 0; i < 40 && !heard; i++) {
            if (audio_dsp_listening()) {
                heard = 1;
                break;
            }
            if (audio_dsp_idle()) {
                break;
            }
            vTaskDelay(1);
        }
        if (!heard && audio_dsp_listening()) {
            heard = 1;
        }

        audio_dsp_stats_t dsp_st;
        audio_dsp_get_stats(&dsp_st);
        ESP_LOGI(TAG, "doze probe ac=%" PRIu64 " max=%" PRIu64 " floor=%" PRIu64
                 " hits=%" PRIu32 "/%" PRIu32 " dsp=%" PRIu32 " skip=%" PRIu32
                 " p_j=%" PRIu32 " p_max=%" PRIu32 " inf=%" PRIu32
                 " sl=%" PRIu32 " wake=%d",
                 last_ac, max_ac, s_probe_noise, hits, seen, dsp_blocks,
                 skipped, dsp_st.prob_jarvis, dsp_st.prob_jarvis_max,
                 dsp_st.infers - inf0, dsp_st.slices - sl0, heard);
        if (heard) {
            s_last_voice_us = esp_timer_get_time();
            taskENTER_CRITICAL(&s_lock);
            s_stats.doze_wakes++;
            s_stats.dozing = 0;
            taskEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "doze exit wake");
            return; // I2S left enabled
        }
        if (!s_doze_enabled) {
            break; // I2S left enabled
        }
        if (!i2s_stop_for_doze()) {
            taskENTER_CRITICAL(&s_lock);
            s_stats.dozing = 0;
            taskEXIT_CRITICAL(&s_lock);
            return;
        }
    }

    taskENTER_CRITICAL(&s_lock);
    s_stats.dozing = 0;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "doze exit disabled");
}

static void ingest_task(void *arg) {
    (void)arg;

    while (1) {
        const int32_t *samples;
        size_t n;
        if (!wait_dma_block(&samples, &n)) {
            continue;
        }
        (void)process_block(samples, n);

        if (s_boot_left == 0 && s_last_voice_us != 0 && s_doze_enabled &&
            !audio_dsp_listening() &&
            (s_doze_now ||
             (esp_timer_get_time() - s_last_voice_us) >
                 ((int64_t)DOZE_AFTER_MS * 1000))) {
            s_doze_now = false;
            run_doze();
        }
    }
}

void audio_ingest_get_stats(audio_ingest_stats_t *out) {
    if (out == NULL) {
        return;
    }
    int64_t last_voice_us;
    taskENTER_CRITICAL(&s_lock);
    *out = s_stats;
    out->overrun = s_overrun;
    last_voice_us = s_last_voice_us;
    out->doze_en = s_doze_enabled ? 1 : 0;
    out->hang = s_hang_left;
    taskEXIT_CRITICAL(&s_lock);
    out->stack_hwm = (uint32_t)uxTaskGetStackHighWaterMark(s_ingest_task);
    out->quiet_ms = 0;
    if (last_voice_us != 0) {
        const int64_t q = esp_timer_get_time() - last_voice_us;
        if (q > 0) {
            out->quiet_ms = (uint32_t)(q / 1000);
        }
    }
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
    s_stats.doze_cycles = 0;
    s_stats.doze_probes = 0;
    s_stats.doze_dsp_blocks = 0;
    s_stats.doze_wakes = 0;
    taskEXIT_CRITICAL(&s_lock);
}

void audio_ingest_set_log(bool on) {
    s_log_enabled = on;
}

void audio_ingest_set_doze(bool on) {
    int dozing;
    taskENTER_CRITICAL(&s_lock);
    dozing = s_stats.dozing;
    taskEXIT_CRITICAL(&s_lock);
    s_doze_enabled = on;
    // The CLI command is also an explicit request to enter doze on the
    // next eligible ingest block; do not falsify the quiet_ms timestamp.
    s_doze_now = on && !dozing;
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
    ESP_LOGI(TAG, "doze after=%d ms sleep=%d ms flush=%d probe=%d k=%d glitch=%d",
             DOZE_AFTER_MS, DOZE_SLEEP_MS, DOZE_FLUSH_BLOCKS,
             DOZE_PROBE_MAX_BLOCKS, DOZE_PROBE_RATIO_K,
             DOZE_PROBE_GLITCH_K);
    return ESP_OK;
}
