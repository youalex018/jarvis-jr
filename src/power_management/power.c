// Power management: DFS + automatic light sleep (when no PM locks are held).
// A PC USB host holds NO_LIGHT_SLEEP.
#include "power.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_pm.h"
#include "sdkconfig.h"

static const char *TAG = "power";

esp_err_t power_init(void) {
#if CONFIG_PM_ENABLE
    const esp_pm_config_t cfg = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = PM_MIN_FREQ_MHZ,
        .light_sleep_enable = true,
    };
    esp_err_t err = esp_pm_configure(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_pm_configure failed %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "cpu_max=%d cpu_min=%d light_sleep=1",
             cfg.max_freq_mhz, cfg.min_freq_mhz);
    return ESP_OK;
#else
    ESP_LOGW(TAG, "CONFIG_PM_ENABLE is off");
    return ESP_OK;
#endif
}

void power_dump(void) {
#if CONFIG_PM_ENABLE
    (void)esp_pm_dump_locks(stdout);
    fflush(stdout);
#else
    printf("pm disabled\r\n");
#endif
}
