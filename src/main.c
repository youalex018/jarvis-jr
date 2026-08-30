#include <stdio.h>
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "audio_ingest.h"

static const char *TAG = "boot";

void app_main(void)
{
    printf("--- Edge AI Pipeline Booting ---\n");

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("CPU Cores: %d\n", chip_info.cores);

    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    printf("External PSRAM: %d MB\n", (int)(psram_size / (1024 * 1024)));

    ESP_ERROR_CHECK(audio_ingest_start());
    ESP_LOGI(TAG, "ingest running; app_main returning");
}
