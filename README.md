# Jarvis-Jr — build, flash, serial

ESP-IDF firmware via PlatformIO. Run these from `C:\Users\alexj\PlatformIO\Jarvis-Jr` in a **new** PowerShell. Active env today: `esp32-s3-devkitc-1` (Waveshare ESP32-S3-Nano).

If `pio` is not found:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" --version
```

Use that full path in place of `pio` below, or restart the terminal after installing PlatformIO.

---

## Everyday commands

| What | Command |
|---|---|
| Toolchain alive | `pio --version` |
| Which COM port Windows gave the board | `pio device list` |
| Compile only (no cable) | `pio run` |
| Compile and flash | `pio run -t upload` |
| Serial logs (115200) | `pio device monitor` |
| Flash, then open serial | `pio run -t upload -t monitor` |
| Wipe the build cache | `pio run -t clean` |
| Rebuild from scratch | `pio run -t fullclean` then `pio run` |

Leave the monitor with **Ctrl+C**. While it is open, uploads often fail (the port is busy).

---

## COM port

`platformio.ini` currently pins `upload_port` and `monitor_port` to **COM3**. That was the old ESP32-D0WD CP2102 dongle.

The S3-Nano uses **native USB-C** (Espressif `VID_303A`), not CP2102. After you plug in a **data** USB-C cable:

1. `pio device list`
2. Put that COM number into `upload_port` and `monitor_port` in `platformio.ini`

Charge-only cables will not enumerate a serial port.

---

## If upload fails (download / bootloader mode)

**ESP32-S3-Nano**

1. Hold **BOOT** (sometimes labeled B1).
2. Tap **RST**, or plug the USB-C in while holding BOOT.
3. Release BOOT.
4. `pio run -t upload`

**Classic ESP32 DevKit (if you turn `esp32dev` back on)**

Hold **BOOT**, tap **RESET**, release **BOOT**, then upload.

---

## What “debug” is on this desk

There is no logic analyzer. Use:

- `pio device monitor` — boot banner, `ingest:` lines, `ovf`, min/max
- On-board LED later (`LED_GPIO` in `include/board.h`)
- Rebuild with extra `ESP_LOGI` in the ingest task if you need a number

GDB (`pio debug`) needs a debug session in Cursor/VS Code. The S3 has built-in USB JTAG; it is not required for bring-up. Prefer serial logs until ingest is proven.

---

## INMP441 pins (S3-Nano build)

| Mic | GPIO | Header |
|---|---|---|
| SCK (BCLK) | 10 | D7 |
| WS | 17 | D8 |
| SD | 21 | D10 |
| VDD | — | 3V3 |
| GND, L/R | — | GND |

Classic ESP32 pins (26 / 25 / 33) apply only if you compile the `esp32dev` env.

---

## Monitor tips

```powershell
pio device monitor --baud 115200
pio device monitor --filter time
```

Expect `CPU Cores: 2`. On this S3-Nano, PSRAM should be non-zero **after** the env is configured for octal PSRAM; the current `sdkconfig.defaults` is still the old 4 MB / no-PSRAM DevKit file, so do not treat `External PSRAM: 0 MB` as a silicon defect until that config is split.
