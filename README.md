# Jarvis Jr

On-device voice control for a lamp. Say **Hey Jarvis**, then **light on** or **light off**. The phrase is recognized on an ESP32-S3. The bulb is switched over the local network. There is no cloud, no phone assistant, and no audio leaving the desk.

## What you say

1. **Hey Jarvis** — the board wakes up and the on-board LED turns on.
2. Within about three seconds, **light on** or **light off**.
3. The LED turns off. A WiZ bulb on the same LAN switches if Wi-Fi is configured.

If you miss the window, say **Hey Jarvis** again. A serial command can still toggle the bulb without a wake word.

## Hardware

| Piece | Role |
|---|---|
| [Waveshare ESP32-S3-Nano](https://www.waveshare.com/wiki/ESP32-S3-Nano) | Dual-core MCU, 8 MB PSRAM, 16 MB flash, USB-C |
| [INMP441](https://www.invensense.com/products/digital/inmp441/) | I2S MEMS microphone |
| WiZ color bulb | Local JSON/UDP on port 38899 (no WiZ cloud) |
| User LED | GPIO 48 / D13, on only during the listen window |

The microphone wiring is in the [firmware notes](docs/firmware.md#microphone-wiring).

## How it works

Audio never leaves the chip. DMA fills 32 ms microphone blocks. A cheap energy check (voice activity detection) decides whether to run the neural net. A streaming INT8 **Hey Jarvis** model opens the listen window. Two smaller command models run only in that window and send `setPilot` to the bulb.

After a stretch of silence the firmware stops the I2S clock so the chip can light-sleep, then briefly listens again. That is a power trade-off: the first wake word after a long quiet period can land in a sleep gap and need a repeat.

```mermaid
flowchart LR
  mic[Microphone] --> dma[I2S DMA]
  dma --> vad[Voice gate]
  vad --> wake[Hey Jarvis]
  wake --> cmd["light on / light off"]
  cmd --> bulb[WiZ bulb]
```

Stack: **C**, ESP32-S3, ESP-IDF, FreeRTOS, **TFLite Micro**, I2S/DMA.

## Status

Working on the S3-Nano: wake word, listen window, command models, local WiZ control, USB CLI, and idle doze / light sleep. Current draw is not published until it is measured. Wi-Fi credentials and the bulb IP live in on-chip flash (NVS), never in this repository.

## Build and flash

PlatformIO + ESP-IDF, not the Arduino framework. After clone:

```text
git submodule update --init --recursive
pio run -j 2
```

The Waveshare Nano ships with an Arduino USB stack that **cannot** be flashed by esptool. The first IDF install needs a one-time bootloader entry. Serial monitor settings on Windows must leave RTS/DTR off, or the COM port dies.

Step-by-step clone, COM ports, wiring, CLI, power-management, and training: **[docs/firmware.md](docs/firmware.md)**.

## License and models

| Item | Notes |
|---|---|
| `models/hey_jarvis.tflite` | Official microWakeWord v2 “Hey Jarvis” (Kevin Ahrendt / ESPHome), [Apache 2.0](models/LICENSE) |
| `models/light_on.tflite`, `models/light_off.tflite` | Custom command models. Background training audio has mixed licenses — see [models/NOTICE](models/NOTICE) |
| `components/esp-tflite-micro` | Espressif TFLite Micro submodule, Apache 2.0 |
| `components/tflite_microfrontend/` | Vendored TFLM frontend, Apache 2.0 ([ORIGIN.txt](components/tflite_microfrontend/ORIGIN.txt)) |

First-party firmware in `src/`, `include/`, and `components/wake_model/` has no root license file yet. Add one before treating this as a formal open-source release.
