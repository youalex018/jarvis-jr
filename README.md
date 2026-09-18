# Jarvis Jr - Edge AI Voice Activated Light

Every smart speaker on the market can turn on a lamp. Every one of them does it by shipping your voice to a data centre first. I wanted to know how much of that trip was actually necessary, so I built one that never leaves the desk.

Jarvis Jr is a voice-controlled lamp that fits in your hand. Say **Hey Jarvis**, then **light on** or **light off**, and a WiZ bulb across the room does what you said. The part that matters is what doesn't happen in between. There is no cloud, no phone app, and no account.

The microphone feeds an ESP32-S3, a microcontroller with a few hundred kilobytes of RAM, and that chip alone decides whether it just heard its name. A small neural net listens for the wake word around the clock. Two even smaller ones wake up only for the three seconds after it, long enough to catch the command, and the board's LED stays lit so you know it's listening. When it decides, it sends one UDP packet to the bulb over your own Wi-Fi and goes back to sleep. If you were too slow, you say the name again. If you'd rather type, a serial console CLI can flip the bulb too.

Doing all of this on a chip that costs a few dollars, without a cloud to lean on, is the fun part. It has to be quiet enough to stay put during a conversation, quick enough to catch a single "Hey Jarvis", and frugal enough to spend most of its life asleep. The wiring, the models, the power tricks, and the ways this can go wrong are in [docs/firmware.md](docs/firmware.md).

## Hardware

- [Waveshare ESP32-S3-Nano](https://www.waveshare.com/wiki/ESP32-S3-Nano)
- [INMP441](https://www.invensense.com/products/digital/inmp441/) microphone
- WiZ color bulb on the same LAN

Mic pinout and the LED pin are in [docs/firmware.md](docs/firmware.md#microphone-wiring).

The Nano is **USB-C only** (no battery). Unplugging the PC cable, or moving that same cable to a wall brick, cuts 5 V and resets the chip. `pm` `light_sleep_counts` cannot be read that way. Keep 5 V on the header (or a powered hub) **then** unplug USB-C from the PC. Full procedure: [Power and light sleep](docs/firmware.md#power-and-light-sleep).

## Setup

You need [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation.html) (`pio`). This project is ESP-IDF, not Arduino.

```text
git clone --recurse-submodules <this-repo>
cd Jarvis-Jr
pio run -j 2
pio run -j 2 -t upload
pio device monitor
```

Always pass `-j 2` if your computer OOMs. After the board boots, set Wi-Fi and the bulb (SSID must not contain spaces):

```text
wifi <ssid> <pass>
wiz <ip>
```

The board reboots after `wifi` so the station can join. Then `wiz on` / `wiz off` check the lamp without a wake word.

A factory Waveshare image uses Arduino USB and cannot be flashed until you put the chip in ROM download once. COM ports, that jumper sequence, Windows serial settings, and everything else: **[docs/firmware.md](docs/firmware.md)**.
