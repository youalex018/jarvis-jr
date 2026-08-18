#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"

void app_main(void) {
    printf("--- Edge AI Pipeline Booting ---\n");
    
    // Check CPU Cores
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    printf("CPU Cores: %d\n", chip_info.cores);
    
    // Check External PSRAM
    size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    printf("External PSRAM: %d MB\n", psram_size / (1024 * 1024));
    
    // Main idle loop
    while(1) {
        printf("RTOS Tick: System Nominal\n");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}