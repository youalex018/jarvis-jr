#pragma once

#include "esp_err.h"

#define CLI_TASK_STACK 4096
#define CLI_TASK_PRIO 2         // debug; below ingest, DSP, and net
#define CLI_TASK_CORE 0         // PRO_CPU; APP_CPU stays audio
#define CLI_LINE_MAX 128

esp_err_t cli_start(void);
