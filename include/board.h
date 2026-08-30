#pragma once

#include "sdkconfig.h"
#include "driver/gpio.h"

/*
 * INMP441:
 *   VDD -> 3V3
 *   GND -> GND
 *   L/R -> GND  (left channel)
 *   SCK -> I2S_BCLK_GPIO
 *   WS  -> I2S_WS_GPIO
 *   SD  -> I2S_SD_GPIO
 *
 * ESP32-S3 has no GPIO 22–25. Do not reuse the classic DevKit map.
 * S3-Nano / Arduino Nano ESP32 header: D7=10, D8=17, D10=21.
 */
#if CONFIG_IDF_TARGET_ESP32S3
#define I2S_BCLK_GPIO GPIO_NUM_10
#define I2S_WS_GPIO GPIO_NUM_17
#define I2S_SD_GPIO GPIO_NUM_21
#define LED_GPIO GPIO_NUM_48
#else
#define I2S_BCLK_GPIO GPIO_NUM_26
#define I2S_WS_GPIO GPIO_NUM_25
#define I2S_SD_GPIO GPIO_NUM_33
#define LED_GPIO GPIO_NUM_2
#endif
