#pragma once

#include "driver/gpio.h"

/*
 * Suggested INMP441 map on classic ESP32 (avoid strapping 0/2/12/15,
 * flash 6–11, and UART0 GPIO1/GPIO3). Confirm against the DevKit silkscreen
 * before soldering.
 *
 * INMP441:
 *   VDD -> 3V3
 *   GND -> GND
 *   L/R -> GND  (left channel)
 *   SCK -> I2S_BCLK_GPIO
 *   WS  -> I2S_WS_GPIO
 *   SD  -> I2S_SD_GPIO
 */
#define I2S_BCLK_GPIO   GPIO_NUM_26
#define I2S_WS_GPIO     GPIO_NUM_25
#define I2S_SD_GPIO     GPIO_NUM_33
