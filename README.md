# Jarvis-Jr — build, flash, serial

ESP-IDF firmware via PlatformIO. Run these in a **new** PowerShell. Active env: `esp32-s3-nano` (`board = arduino_nano_esp32` — Waveshare ESP32-S3-Nano / Arduino Nano ESP32).

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

| VID:PID | What it is | Use with esptool? |
|---|---|---|
| `2341:0070` | Arduino CDC (factory). Was COM9. | **No** |
| `303A:1001` | ESP32 ROM download | **Yes — first IDF flash** |
| `303A:4001` | This firmware’s USB Serial/JTAG | Monitor / later flashes |
| `BTHENUM` | Bluetooth | Never |

There is no BOOT button. **B1** (GPIO0) is on the **3V3 / VUSB** side of the header.

**First IDF flash (Arduino factory image):**

1. Unplug/replug so USB is healthy again. Close the monitor.
2. Jumper **B1 to GND**. RGB should go **green**.
3. Tap **RST** while the jumper is on.
4. **Remove the jumper**. RGB should stay **purple**.
5. `pio device list` — pick **`303A:1001`** (new COM, not 2341).
6. Set `upload_port` / `monitor_port` in `platformio.ini` to that COM.
7. `pio run -t upload` immediately.
8. Tap **RST** once more so the new app starts.

Do not double-tap RST (Arduino DFU / green fade). Do not flash `2341:0070`.

---

## sdkconfig

| File | Role |
|---|---|
| `sdkconfig.defaults` | Tickless idle |
| `sdkconfig.defaults.esp32s3` | 16 MB flash, octal PSRAM, USB Serial/JTAG |

PlatformIO generates `sdkconfig.esp32-s3-nano` locally; it is gitignored. After changing the `.defaults` files: `pio run -t fullclean` then `pio run`.

Expect `CPU Cores: 2` and **non-zero** `External PSRAM` on the Nano.

---

## If upload fails (ROM download)

Jumper **B1 to GND**, tap **RST**, remove the jumper (RGB purple), then flash the `303A:1001` COM from `pio device list`.

---

## What “debug” is on this desk

There is no logic analyzer. Use:

- `pio device monitor` — boot banner, `ingest:` lines, `ovf`, min/max
- On-board LED (`LED_GPIO` in `include/board.h`) — D13 / GPIO 48
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

---

## Monitor tips

```powershell
pio device monitor --baud 115200
pio device monitor --filter time
```

Expect `CPU Cores: 2` and non-zero PSRAM on the Nano. Ingest logs about once a second with `dc`, `ac`, `noise`, and `voiced`. Stay quiet for ~0.5 s after boot so the noise floor can learn.
