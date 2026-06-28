# Alchemist Study

Firmware for an **ESP32-C3 Super Mini** — the brain of a 3D-printed alchemist's
study. Reed switches sense which potion bottles are seated in their homes, and an
SSD1306 OLED names the resulting potion in one of two game universes (Skyrim /
Baldur's Gate 3). A rotary encoder is the "stir" control; a passive buzzer scores
the brewing.

Built with PlatformIO + Arduino (arduino-esp32 **3.x** via the pioarduino platform).

## Quick start

```sh
pio run                       # build (default env: c3-prod)
pio run -e c3-dev -t upload   # flash dev build (verbose logs)
pio device monitor -b 115200  # serial monitor over USB-CDC
```

> Requires **PlatformIO Core ≥ 6.1.19** (the pinned pioarduino platform needs it).

No configuration or credentials are needed — the bench firmware runs fully offline.

## Layout

| Path                | Purpose                                              |
|---------------------|------------------------------------------------------|
| `platformio.ini`    | Build envs: `c3-prod` (default), `c3-dev`            |
| `src/main.cpp`      | Single-file firmware: state machine, potions, audio  |
| `CLAUDE.md`         | Full spec: pin map, platform rationale, potion tables |
| `BACKLOG.md`        | Phased build plan                                    |
| `lib/` `include/`   | Project-private libs / shared headers                |
| `test/`             | Unity tests                                          |

## Hardware

- **Target:** ESP32-C3 Super Mini, native USB (USB-CDC serial).
- **Platform:** pioarduino `55.03.39` (core 3.3.9) — required for `tone()` and the
  software encoder decode. See `CLAUDE.md` for the full pin map and the why.
- **Encoder:** decoded via GPIO interrupts in firmware (the C3 has no PCNT
  peripheral, so `ESP32Encoder` can't be used).

## Roadmap

1. **Bench firmware (WiFi-free)** — the potion logic, built to work standalone.
2. **Connectivity (later)** — runtime WiFi provisioning, OTA, status page, Home
   Assistant integration. The C3's radio stays available; we just add it after the
   magic works on the bench. See `BACKLOG.md`.
