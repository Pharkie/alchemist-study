# Alchemist Study

Firmware for an **ESP32-C3 Super Mini** — the brain of a 3D-printed alchemist's
study. Magnetic potion bottles drop into fixed homes; **reed switches** sense
which are present, and an **OLED** names the potion you've brewed — across **seven
RPG universes**. A rotary encoder is the "stir" control and a passive buzzer
scores the brewing.

Built with PlatformIO + Arduino (arduino-esp32 **3.x** via the pioarduino platform).
Runs fully offline — no WiFi, no accounts, no configuration.

## How it plays

1. **Place ingredients** — seat 1–3 bottles; the screen features the ingredient name(s).
2. **Stir** — turn the knob. A power bar fills (with diminishing returns toward the
   top), a vortex swirls, and a trill rises in pitch. Pause and it drains gently.
3. **Brew** — once the bar is full, press the knob: one of three random full-screen
   animations reveals the potion, with a success jingle.
4. **Settings** — press on the idle screen: **Realm**, **Mute**, **Brightness**,
   **Stir Level** (Easy/Medium/Hard), **Hardware Test**, **Firmware**. Turn to move,
   press to edit a value, turn to change, press to confirm. Settings persist (NVS).

Three bottles give seven combinations (3 singles + 3 pairs + 1 triple) → seven
potions per realm.

## Realms

**Skyrim · Baldur's Gate 3 · The Witcher 3 · World of Warcraft · Zelda ·
Minecraft · Ultima VII** — each with three in-universe ingredients and seven
potions. Full tables in [CLAUDE.md](CLAUDE.md).

## Hardware

- **Board:** ESP32-C3 Super Mini (native USB-CDC serial).
- **Display:** SSD1306 128×64 OLED (I²C).
- **Input:** 3× reed switch, rotary encoder with push button.
- **Audio:** passive piezo buzzer (a *passive* one — active buzzers can't play the pitched audio).

| Function | GPIO | Notes |
|---|---|---|
| Reed slot 1 / 2 / 3 | 3 / 4 / 10 | `INPUT_PULLUP`; magnet pulls LOW |
| OLED SDA / SCL | 5 / 6 | I²C @ 400 kHz |
| Buzzer | 1 | `tone()` (needs core 3.x) |
| Encoder A / B / SW | 0 / 7 / 20 | SW `INPUT_PULLUP` |

The encoder is decoded with a GPIO-interrupt routine in firmware — the ESP32-C3
has no PCNT peripheral, so the usual `ESP32Encoder` library can't be used. See
[CLAUDE.md](CLAUDE.md) for the full pin map and the platform rationale.

## Build & flash

Requires **PlatformIO Core ≥ 6.1.19** (the pinned pioarduino platform needs it).

```sh
pio run                       # build (default env: c3-prod)
pio run -e c3-dev -t upload   # flash dev build (verbose logs)
pio device monitor -b 115200  # serial monitor over USB-CDC
```

| Env | Builds | Use |
|---|---|---|
| `c3-prod` | `main.cpp` (optimised) | release build |
| `c3-dev` | `main.cpp` (verbose, exception decoder) | day-to-day flashing |
| `c3-hwcheck` | `hwcheck.cpp` | wiring / component diagnostics |

The real firmware **self-checks on boot**: a rising chime, a pin-map report, and
if the OLED is missing it runs headless (error beep + serial warning) rather than
failing silently.

## Hardware check

A standalone wiring diagnostic — beeps the buzzer, scans for the OLED, and shows a
live ON/off status screen for every input while logging both edges to serial:

```sh
pio run -e c3-hwcheck -t upload
pio device monitor -b 115200
```

Full guide + troubleshooting: **[docs/HARDWARE_TEST.md](docs/HARDWARE_TEST.md)**.
The same diagnostic is also reachable in-firmware via **Settings → Hardware Test**.

## Repo layout

| Path | Purpose |
|---|---|
| `src/main.cpp` | The firmware: state machine, realms, stir/brew, menu, animations |
| `src/hwcheck.cpp` | Standalone hardware diagnostic (`c3-hwcheck`) |
| `platformio.ini` | Build environments |
| `CLAUDE.md` | Full spec: pin map, platform rationale, all realm tables |
| `docs/HARDWARE_TEST.md` | Running the hardware checker + troubleshooting |
| `docs/TUNING.md` | Tunable feel constants (stir curve, audio, timing) |
| `BACKLOG.md` | Phased build plan |

## Versioning

`FW_VERSION` in `src/main.cpp` tracks the git tag / GitHub release. Current: **v0.1**.

## Roadmap

1. **Bench firmware (offline)** — the potion experience, standalone. ✅ (v0.1)
2. **Connectivity (later)** — runtime WiFi provisioning, OTA updates, a status
   page, and Home Assistant integration (e.g. announce a brew as an event). The
   C3's radio stays available; it's added once the magic works on the bench. See
   [BACKLOG.md](BACKLOG.md).

## License

Free for noncommercial use under the **PolyForm Noncommercial License 1.0.0**.
**Commercial use is not free — a commercial license is required.** See
[LICENSE](LICENSE) for full terms.

© 2025 Adam Knowles. All rights reserved.

## LLM Generated, Human Reviewed

This code was generated with [Claude Code](https://claude.com/claude-code)
(Anthropic), primarily on Claude Opus 4.8. Development was overseen by the human
author with attention to reliability and security. Architectural decisions,
configuration choices, and development sessions were closely planned, directed,
and verified by the human author throughout — including hardware bring-up and
on-device testing beyond the LLM. Still, the code has had limited manual review;
please make your own checks and use it at your own risk.
