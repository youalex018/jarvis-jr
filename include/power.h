#pragma once

#include "esp_err.h"

#define PM_MIN_FREQ_MHZ 40  // XTAL; APB_MIN when no PM locks are held

// USB host holds ESP_PM_NO_LIGHT_SLEEP.

esp_err_t power_init(void);
void power_dump(void);  // locks + mode stats (needs CONFIG_PM_PROFILING)
