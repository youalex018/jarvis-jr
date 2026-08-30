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

`upload_port` / `monitor_port` are **unset** on purpose.

| What `pio device list` shows | Meaning |
|---|---|
| `BTHENUM\...` Standard Serial over Bluetooth (COM4/COM5 here) | **Not** the ESP32. Do not flash these. |
| `VID_10C4` / CP2102 | Classic ESP32 DevKit USB-UART |
| `VID_303A` | ESP32-S3 native USB-C (the Nano) |

Until a **data** USB-C cable is plugged into the Nano, you will only see Bluetooth COMs. Charge-only cables do not enumerate.

After USB-C enumerates:

1. `pio device list` — pick the `VID_303A` port
2. Set `upload_port` and `monitor_port` in `platformio.ini`

---

## sdkconfig

Chip-specific options are not all in `sdkconfig.defaults`:

| File | Used when |
|---|---|
| `sdkconfig.defaults` | Every target (tickless idle) |
| `sdkconfig.defaults.esp32s3` | S3-Nano: 16 MB flash, octal PSRAM, USB Serial/JTAG console |
| `sdkconfig.defaults.esp32` | D0WD: 4 MB flash, no PSRAM |

After changing these, rebuild from scratch:

```powershell
pio run -t fullclean
pio run
```

Expect `CPU Cores: 2` and **non-zero** `External PSRAM` on the Nano.

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

Expect `CPU Cores: 2` and non-zero PSRAM on the Nano. Ingest logs about once a second with `mean_sq` and `voiced` (including silence) so you can set `VAD_MEAN_SQ_MIN`.
