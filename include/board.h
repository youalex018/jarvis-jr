#pragma once

#include "driver/gpio.h"

// INMP441 on Waveshare ESP32-S3-Nano / Arduino Nano ESP32:
//   VDD -> 3V3
//   GND -> GND
//   L/R -> GND (left channel)
//   SCK -> D7 (GPIO 10)
//   WS  -> D8 (GPIO 17)
//   SD  -> D10 (GPIO 21)
#define I2S_BCLK_GPIO GPIO_NUM_10
#define I2S_WS_GPIO GPIO_NUM_17
#define I2S_SD_GPIO GPIO_NUM_21
#define LED_GPIO GPIO_NUM_48
