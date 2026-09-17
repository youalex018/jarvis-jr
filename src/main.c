// Boot: start PM, DSP, ingest, CLI, and net.
#include <stdio.h>
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "audio_dsp.h"
#include "audio_ingest.h"
#include "cli.h"
#include "net.h"
#include "power.h"

static const char *TAG = "boot";

void app_main(void) {
    printf("--- Edge AI Pipeline Booting ---\n");

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("CPU Cores: %d\n", chip_info.cores);

    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    printf("External PSRAM: %d MB\n", (int)(psram_size / (1024 * 1024)));

    ESP_ERROR_CHECK(power_init());
    ESP_ERROR_CHECK(audio_dsp_start());
    ESP_ERROR_CHECK(audio_ingest_start());
    ESP_ERROR_CHECK(cli_start());
    ESP_ERROR_CHECK(net_start());
    ESP_LOGI(TAG, "pm + dsp + ingest + cli + net running; app_main returning");
}
