#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_SAMPLE_RATE_HZ 16000
#define AUDIO_DMA_DESC_NUM 2
#define AUDIO_DMA_FRAME_NUM 512   // 512 / 16000 = 32 ms per buffer

#define VAD_RATIO_K 3             // voice if ac_mean_sq > K * noise floor
#define VAD_BOOT_SKIP_BLOCKS 16   // ~0.5 s: ignore I2S clock-start transient before learning
#define VAD_BOOT_BLOCKS 16        // ~0.5 s after that: learn noise
#define VAD_HANGOVER_BLOCKS 8     // ~256 ms extra voiced queue after speech

#define DOZE_AFTER_MS 30000       // quiet time before first doze
#define DOZE_SLEEP_MS 500         // I2S off; chip may light-sleep
#define DOZE_FLUSH_BLOCKS 2       // drop first DMA after clocks restart
#define DOZE_DISCARD_BLOCKS 12    // time-based ignore of clock-start AC
#define DOZE_SETTLED_K 2          // ac below this * pre-doze floor => restart tail gone
#define DOZE_SETTLE_QUIET 4       // near-floor blocks (hysteresis, not strict consecutive)
#define DOZE_SETTLE_MAX 48        // give up this cycle after ~1.5 s
#define DOZE_PROBE_BLOCKS 32      // VAD after floor (~1 s)
#define DOZE_VOICE_BLOCKS 6       // in-band hits within one probe window to leave doze (quiet room ~2, speech 7-9)
#define DOZE_ARM_BLOCKS 12        // awake VAD hits in the last 32 blocks (~1 s) to reset the 30 s clock
#define DOZE_PROBE_RATIO_K 3      // vs pre-doze floor; must be > DOZE_SETTLED_K
#define DOZE_GLITCH_K 16          // above this * floor is DMA junk, not speech

typedef struct {
    uint32_t blocks;         // total DMA blocks processed
    uint32_t overrun;        // ISR saw previous block unconsumed
    uint32_t proc_last_us;   // ingest block processing time
    uint32_t proc_max_us;
    uint64_t proc_total_us;  // for avg = total / blocks
    uint32_t period_last_us; // wake-to-wake - expect ~32000
    uint32_t period_max_us;
    uint32_t period_min_us;
    uint64_t noise;
    int voiced;
    int led_on;              // listen window (DSP), not VAD
    int doze_en;             // pm doze on|off
    int dozing;              // I2S stopped; duty-cycling light sleep
    uint32_t quiet_ms;       // since last sustained voice (doze clock)
    uint32_t hang;           // VAD hangover blocks left (DSP submit, not doze)
    uint32_t doze_cycles;    // times ingest entered doze
    uint32_t doze_probes;    // VAD blocks processed while probing
    uint32_t doze_wakes;     // probe heard voice and returned to awake
    uint32_t stack_hwm;      // bytes remaining
} audio_ingest_stats_t;

esp_err_t audio_ingest_start(void);
void audio_ingest_get_stats(audio_ingest_stats_t *out);
void audio_ingest_reset_stats(void); // zero max/min/overrun/totals; keep noise + VAD + dozing
void audio_ingest_set_log(bool on);  // 1 Hz ESP_LOGI on/off
void audio_ingest_set_doze(bool on); // idle I2S-off / light-sleep duty cycle
