#pragma once

#include "esp_err.h"

#define PM_MIN_FREQ_MHZ 40  // XTAL; APB_MIN when no PM locks are held

esp_err_t power_init(void);
void power_dump(void);  // locks + mode stats (needs CONFIG_PM_PROFILING)
