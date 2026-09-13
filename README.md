# Jarvis-Jr - Edge AI Voice Activated Light Controller

ESP-IDF firmware via PlatformIO. Run these in a **new** PowerShell. Active env: `esp32-s3-nano` (`board = arduino_nano_esp32` — Waveshare ESP32-S3-Nano / Arduino Nano ESP32).

On-device **"Hey Jarvis"** (microWakeWord v2 INT8) opens a 3 s listen window and lights GPIO 48. **"light on"** / **"light off"** (optional `.tflite`s) set a local WiZ bulb over UDP. The LED stays on until a command is recognized or the window expires. No cloud.

After clone: `git submodule update --init --recursive`. The factory app partition is **4 MB** (`partitions.csv`); TFLM does not fit the default 1 MB app. If the compile runs out of RAM, use `pio run -j 2`.

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
| Compile only (no cable) | `pio run` (`pio run -j 2` if TFLM OOMs) |
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
| `sdkconfig.defaults.esp32s3` | 16 MB flash, octal PSRAM, USB Serial/JTAG, custom `partitions.csv` |

PlatformIO generates `sdkconfig.esp32-s3-nano` locally; it is gitignored. After changing the `.defaults` files: `pio run -t fullclean` then `pio run`.

Expect `CPU Cores: 2` and **non-zero** `External PSRAM` on the Nano.

---

## Train `light on` / `light off` (Google Colab)

Checked-in `models/light_on.tflite` and `models/light_off.tflite` are the Colab-trained pair used on the desk. Firmware embeds them when those exact names exist. Retrain on **Colab with a GPU**, not this PC, if you want new weights:

1. Open [Google Colab](https://colab.research.google.com/). Runtime → Change runtime type → **T4 GPU**.
2. File → Upload notebook → `train/colab_light_on_off.ipynb` from this repo.
3. Runtime → Run all. After the install cell: **Runtime → Restart session**, then run from **Config** downward.
4. Download `light_on.tflite` and `light_off.tflite`. Copy both into `models/` (exact names).
5. Close the serial monitor, then `pio run -j 2` and `pio run -t upload`.

Boot must log `light_on` and `light_off`, not `cmd models: none`. CMake only sees new `.tflite` files at configure time; if they were added after a prior build, `pio run -t fullclean` then `pio run -j 2`. Then: Hey Jarvis → LED on → “light on” / “light off” within 3 s → bulb + LED off.

Desk-checked knobs (normal voice): `AUDIO_PCM_GAIN` 4, `VAD_RATIO_K` 3, Jarvis cutoff 230 / window 5, command cutoff 204 / window 3. See `include/wake_model.h`, `include/audio_dsp.h`, `include/audio_ingest.h`.

---

## First-party, generated, submodule, vendored

| Path | What it is |
|---|---|
| `src/`, `include/` | First-party C |
| `components/wake_model/` (`wake_model.cc`, `gen_model_c.py`) | First-party C++ TFLM wrapper |
| `models/hey_jarvis.tflite` | Checked-in wake model (source of truth) |
| `models/light_on.tflite`, `models/light_off.tflite` | Optional command models; omit to build listen-window-only |
| `train/colab_light_on_off.ipynb` | Google Colab notebook to train those two models |
| `hey_jarvis_model.c`, `light_on_model.c`, `light_off_model.c` | **Generated** at CMake configure from the `.tflite`s; gitignored |
| `components/esp-tflite-micro` | **Git submodule** (`espressif/esp-tflite-micro`, pin `99f49e1` in `.gitmodules`) |
| `components/tflite_microfrontend/` | **Vendored** TFLM frontend (not a submodule). See `ORIGIN.txt` |
| `managed_components/`, `dependencies.lock` | **Generated** IDF Component Manager fetch of `esp-nn`; gitignored |
| `.pio/`, `sdkconfig.esp32-s3-nano` | **Generated** PlatformIO/IDF build; gitignored |

---

## If upload fails (ROM download)

Jumper **B1 to GND**, tap **RST**, remove the jumper (RGB purple), then flash the `303A:1001` COM from `pio device list`.

---

## What “debug” is on this desk

- `pio device monitor` — boot banner, `ingest:` lines, `hey jarvis` / `listen start|end`, `ovf`, min/max
- On-board LED (`LED_GPIO` in `include/board.h`) — D13 / GPIO 48, **on during the listen window only**
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
