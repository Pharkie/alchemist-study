# CLAUDE.md — Alchemist Study

Firmware for the **ESP32-C3 Super Mini** that powers a 3D-printed alchemist's-study
model. Reed switches sense which potion bottles are seated in fixed homes; an OLED
names the resulting potion in one of two game universes (Skyrim / Baldur's Gate 3).
A rotary encoder is the "stirring" control and a passive buzzer provides audio.

**Build order:** the bench firmware (potion logic) is built **WiFi-free first**, so
it's a clean one-shot deliverable that works without anyone entering credentials.
**Connectivity is a deliberate later stage** (see backlog) — OTA reflashing of the
potion tables, a tiny status page, and Home Assistant integration (e.g. announce
"Philter of the Phantom brewed" as an HA event / trigger a scene).

WiFi capability is **not removed** — the radio is the C3's whole point over an
ATmega, and the WiFi library is built into the core (free, on pioarduino too). What
gets deleted is the *template's WiFi implementation*, not the capability. Nothing in
the bench firmware should architect WiFi out (it won't — the pin map doesn't clash).

## Build / flash / monitor

```sh
pio run                       # build default env (c3-prod)
pio run -e c3-dev -t upload   # flash dev build (verbose logs)
pio device monitor -b 115200  # serial monitor over USB-CDC
```

USB-CDC serial is enabled (`ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`),
so the board enumerates as a serial port over its native USB.

## Platform — IMPORTANT

Target **arduino-esp32 3.x** via the **pioarduino** platform fork, pinned to a
specific release:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.39/platform-espressif32.zip
```

(`55.03.39` = arduino-esp32 core 3.3.9 / ESP-IDF 5.5.4.)

Why not the stock `espressif32@6.x` platform? That's the *official* platform but it's
stuck on arduino-esp32 **2.x**, where `tone()`/`noTone()` and the `ESP32Encoder`
pull-resistor API behave differently. The platform move is required for the buzzer
API regardless of WiFi — pioarduino 3.x gives us `tone()` **and** WiFi. Do not
silently bump this pin.

## Hardware & pin map (ESP32-C3 Super Mini, native USB)

| Function                 | GPIO  | Notes                                              |
|--------------------------|-------|----------------------------------------------------|
| Reed slot 1 (bit0)       | 3     | `INPUT_PULLUP`; magnet pulls LOW = present         |
| Reed slot 2 (bit1)       | 4     | `INPUT_PULLUP`                                      |
| Reed slot 3 (bit2)       | 10    | `INPUT_PULLUP`                                      |
| OLED SDA (I²C)           | 5     | SSD1306 128×64, white                              |
| OLED SCL (I²C)           | 6     |                                                    |
| Buzzer (passive)         | 1     | `tone()` / `noTone()`                              |
| Encoder A                | 0     | Safe on C3 — GPIO0 is NOT a strapping pin here     |
| Encoder B                | 7     |                                                    |
| Encoder SW (push)        | 20    | `INPUT_PULLUP`                                      |

**Combo bit mapping:** `bit0=slot1, bit1=slot2, bit2=slot3` → combo value `1..7`
(`0` = empty base = idle).

## Libraries

- **U8g2** (`olikraus/U8g2@^2.36.18`) — display, full-buffer:
  `U8G2_SSD1306_128X64_NONAME_F_HW_I2C`.
- **Preferences** — built into the core; used for NVS persistence of the universe.

### Encoder — no library (important)

`madhephaestus/ESP32Encoder` does **NOT** work on the ESP32-C3: it relies on the
PCNT pulse-counter peripheral, which the C3 lacks (S2/S3/C6 and classic ESP32 have
it; C3/C2 don't). The library even emits `#warning PCNT not supported on this SoC`
and fails to link. So the encoder is decoded in software with a tiny GPIO-interrupt
quadrature routine in `main.cpp` (both channels on `CHANGE`, a 16-entry transition
table → ±1, internal pullups). `g_encoderCount` is the running signed position.

## Architecture — single `src/main.cpp`, clean state machine

States: **IDLE → IDENTIFY → STIRRING → REVEAL**

- **IDLE** — empty base. "Place ingredients" + "Press for settings". No realm
  shown here; a press opens **Settings** (where the realm lives).
- **IDENTIFY** — ≥1 bottle seated. **Features** the ingredient name(s) in an
  elegant serif with sparkles (a single ingredient large, two/three as a flanked
  list); no realm header. A subtle "turn to stir" cue.
- **STIRRING** — encoder is turning. A **power bar** fills as you stir, getting
  **harder the fuller it gets** (the **Stir Level** setting — Easy/Medium/Hard —
  sets how steeply it slows toward the right), with a swirling vortex + rising
  trill (~320→1100 Hz). **Pause and the bar drains gradually (~20%/sec)** rather
  than resetting; once empty and idle a while (~2.5 s) it drifts back to IDENTIFY.
  When the bar fills, the whole screen becomes a framed **"Press to create"**
  call-to-action (held until a press or a real combo change).
- **REVEAL** — one of **three random full-screen animations** (starburst /
  rising bubbles / expanding rings) reveals the potion name (wrapped to two lines
  if wide) over a short ascending jingle. After ~3 s it auto-returns to IDLE
  ("Place ingredients"), ready for the next brew.

### Interaction rules

- **Combo latching (reliability):** IDLE and IDENTIFY follow the bottles live.
  But once a brew is underway (STIRRING or REVEAL) the combo is **latched** —
  pulling a magnet away (fully or partially, deliberately or via a flaky reed)
  is **ignored**, so the swirl and the revealed potion survive. A brew only
  restarts on a genuinely new arrangement: a bottle **added** to the set, or the
  base cleared to **empty and then refilled**. When a stir fizzles out (knob
  idle) it resyncs to whatever is actually on the base.
- **Short press**:
  - In IDENTIFY/STIRRING = mix & reveal — but **stirring is REQUIRED first**: a
    press before enough stir progress does nothing (a low "not yet" buzz).
  - On IDLE = open **Settings**.
  - In SETTINGS = activate the highlighted item.
- **Long press (≥600 ms)** = leave a menu (SETTINGS → idle, Hardware Test →
  Settings). It no longer toggles the realm.

### Settings menu (`ST_SETTINGS`) — a reusable mini-menu

Open with a press on the idle screen. **Turn** to move between items; **press**
to start editing a value, **turn** to change it (live), **press** to confirm;
**long-press** cancels an edit, or (when not editing) leaves the menu. Actions
(Hardware Test, Exit) run on press. Items are a data table (`kMenu` in
`main.cpp`) of `{label, kind, get/set, …}`, so adding one is a single row.

- **Realm** — 7 universes: Skyrim, Baldur's Gate 3, The Witcher 3, World of
  Warcraft, Zelda, Minecraft, Ultima VII (persisted; replaces the old long-press)
- **Mute** — Off / On (gates all audio: chime, trill, jingles, error beep)
- **Bright** — 1–5 (OLED contrast via `setContrast`)
- **Stir Level** — Easy / Medium / Hard (fill difficulty curve)
- **Hardware Test** — opens a built-in live diagnostic (`ST_DIAG`): reed/button
  boxes + encoder count; long-press to return. (Same idea as the `c3-hwcheck`
  build, but in-firmware so it needs no reflash.)
- **Firmware** — shows the version string
- **Exit** — back to idle

Mute and Brightness persist to NVS alongside the universe.

## Potion lookup — index by combo `1..7` (`0` = idle)

Slots per universe: bit0=slot1, bit1=slot2, bit2=slot3.

**Skyrim** — Blue Mountain Flower / Deathbell / Nightshade

| Combo | Ingredients              | Potion                       |
|-------|--------------------------|------------------------------|
| 001   | Flower                   | Potion of Minor Healing      |
| 010   | Deathbell                | Damage Health Poison         |
| 100   | Nightshade               | Fortify Destruction          |
| 011   | Flower + Deathbell       | Lingering Damage Poison      |
| 101   | Flower + Nightshade      | Potion of Regeneration       |
| 110   | Deathbell + Nightshade   | Deadly Paralysis Poison      |
| 111   | all                      | Philter of the Phantom       |

**Baldur's Gate 3** — Salts of Mugwort / Bullywug Crayfish Tail / Rogue's Morsel

| Combo | Ingredients              | Potion                          |
|-------|--------------------------|---------------------------------|
| 001   | Mugwort                  | Potion of Healing               |
| 010   | Crayfish                 | Elixir of Vigilance             |
| 100   | Morsel                   | Potion of Speed                 |
| 011   | Mugwort + Crayfish       | Elixir of the Colossus          |
| 101   | Mugwort + Morsel         | Potion of Invisibility          |
| 110   | Crayfish + Morsel        | Elixir of Heroism               |
| 111   | all                      | Elixir of Universal Resistance  |

**The Witcher 3** — Celandine / Drowner Brain / Dwarven Spirit

| Combo | Ingredients                  | Potion                     |
|-------|------------------------------|----------------------------|
| 001   | Celandine                    | White Honey                |
| 010   | Drowner Brain                | Black Blood                |
| 100   | Dwarven Spirit               | Tawny Owl                  |
| 011   | Celandine + Drowner Brain    | Full Moon                  |
| 101   | Celandine + Dwarven Spirit   | White Raffard's Decoction  |
| 110   | Drowner Brain + Dwarven Spirit | Ekimmara Decoction       |
| 111   | all                          | Swallow                    |

**World of Warcraft** — Peacebloom / Silverleaf / Earthroot

| Combo | Ingredients              | Potion                     |
|-------|--------------------------|----------------------------|
| 001   | Peacebloom               | Minor Rejuvenation Potion  |
| 010   | Silverleaf               | Elixir of Minor Defense    |
| 100   | Earthroot                | Elixir of Minor Fortitude  |
| 011   | Peacebloom + Silverleaf  | Minor Healing Potion       |
| 101   | Peacebloom + Earthroot   | Weak Troll's Blood Potion  |
| 110   | Silverleaf + Earthroot   | Elixir of Lion's Strength  |
| 111   | all                      | Flask of the Titans        |

**Zelda (classic)** — Mushroom / Bottled Fairy / Lon Lon Milk

| Combo | Ingredients              | Potion          |
|-------|--------------------------|-----------------|
| 001   | Mushroom                 | Green Potion    |
| 010   | Bottled Fairy            | Red Potion      |
| 100   | Lon Lon Milk             | Lon Lon Milk    |
| 011   | Mushroom + Fairy         | Blue Potion     |
| 101   | Mushroom + Milk          | Elixir Soup     |
| 110   | Fairy + Milk             | Life Potion     |
| 111   | all                      | Chateau Romani  |

**Minecraft** — Nether Wart / Glistering Melon Slice / Blaze Powder

| Combo | Ingredients              | Potion                     |
|-------|--------------------------|----------------------------|
| 001   | Nether Wart              | Awkward Potion             |
| 010   | Glistering Melon Slice   | Potion of Healing          |
| 100   | Blaze Powder             | Potion of Strength         |
| 011   | Wart + Melon             | Potion of Regeneration     |
| 101   | Wart + Blaze Powder      | Potion of Swiftness        |
| 110   | Melon + Blaze Powder     | Potion of Fire Resistance  |
| 111   | all                      | Potion of Harming          |

**Ultima VII** — Garlic / Ginseng / Spider Silk

| Combo | Ingredients              | Potion        |
|-------|--------------------------|---------------|
| 001   | Garlic                   | Cure Poison   |
| 010   | Ginseng                  | Mana Potion   |
| 100   | Spider Silk              | Sleep         |
| 011   | Garlic + Ginseng         | Awaken        |
| 101   | Garlic + Spider Silk     | Protection    |
| 110   | Ginseng + Spider Silk    | Invisibility  |
| 111   | all                      | Heal          |

## Conventions

- Keep everything in `src/main.cpp` with the state machine clean and readable.
- Add brief comments explaining the pin choices and the core-3.x dependency.
- Non-blocking loop: debounce reeds and button by time, no long `delay()`s that
  would stall stir audio/animation.
