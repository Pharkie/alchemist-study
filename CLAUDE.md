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
| Reed slot 1 (bit0)       | 1     | `INPUT_PULLUP`; magnet pulls LOW = present         |
| Reed slot 2 (bit1)       | 3     | `INPUT_PULLUP`                                      |
| Reed slot 3 (bit2)       | 4     | `INPUT_PULLUP`                                      |
| OLED SDA (I²C)           | 5     | SSD1306 128×64, white                              |
| OLED SCL (I²C)           | 6     |                                                    |
| Buzzer (passive)         | 7     | `tone()` / `noTone()`                              |
| Encoder A (CLK)          | 10    |                                                    |
| Encoder B (DT)           | 20    | UART0-RX by default; free (console is USB-CDC)     |
| Encoder SW (push)        | 21    | `INPUT_PULLUP`; UART0-TX by default, free likewise |

**Combo bit mapping:** `bit0=slot1, bit1=slot2, bit2=slot3` → combo value `1..7`
(`0` = empty base = idle).

The canonical pin map in code is **`src/pins.h`** — shared by the firmware and
both standalone diagnostics, so the wiring can't drift between programs.

## Libraries

- **U8g2** (`olikraus/U8g2@^2.36.18`) — display, full-buffer:
  `U8G2_SSD1306_128X64_NONAME_F_HW_I2C`.
- **Preferences** — built into the core; used for NVS persistence of the universe.

### Encoder — no library (important)

`madhephaestus/ESP32Encoder` does **NOT** work on the ESP32-C3: it relies on the
PCNT pulse-counter peripheral, which the C3 lacks (S2/S3/C6 and classic ESP32 have
it; C3/C2 don't). The library even emits `#warning PCNT not supported on this SoC`
and fails to link. So the encoder is decoded in software with a tiny GPIO-interrupt
quadrature routine in `src/quadrature.h` (both channels on `CHANGE`, a 16-entry
transition table → ±1, internal pullups; shared by `main.cpp` and `hwcheck.cpp`).
`g_encoderCount` is the running signed position.

## Architecture — single `src/main.cpp`, clean state machine

States: **IDLE → IDENTIFY → STIRRING (→ RITUAL) → REVEAL**, plus **STORY**

The **brewing mechanic scales with the "act"** — the number of bottles seated
(design + shelved ideas in [docs/MINIGAMES.md](docs/MINIGAMES.md)): act 1 (one
bottle) is the classic stir, act 2 (two) is the **"align the essences"**
phase-matching game, act 3 (all three = the master potion) adds the **Grand
Brew ritual** after the stir. Story brews get the same act mechanics — the
story's 1 → 2 → 3 bottle escalation walks the player up all three.

- **IDLE** — empty base. A **home carousel** (Place ingredients / Story Mode /
  Settings), sitting side by side and **wrapping at both ends** (left from the
  first panel arrives at the last). Turning the encoder **slides smoothly**
  between panels (eased pixel scroll along the shortest way round — both
  panels visible mid-slide, including across the wrap seam), with page dots
  and edge chevrons as a fixed overlay. Press activates the panel in view:
  Place = nothing yet (low "nope" buzz), Story Mode = the adventure (STORY),
  Settings = the menu. Panels are **table-driven** (`kHomePanels` — one
  renderer + press action per row, order matching the `HomePanel` enum;
  adding a mode is one enum entry + one row). The Story panel's d20 tumbles
  lazily in 3D. No realm shown here; it lives in Settings. Exiting
  Settings/Story back to idle lands on that panel.
- **STORY** — the adventure (Skyrim, three-act Peryite arc; ALL THREE ACTS
  built, playable end to end). Act 1: road/Jarl choice → rat battle with
  forced healing brew → The Bannered Mare (gold economy, lute, campfire
  sleep). Act 2 "The Steward's Goblet": Danica diagnoses the festering bite
  → granary → recipe spoken → confront/watch → brew Lingering Damage Poison
  (011) → feast use-choice → the steward dies naming Peryite. Act 3 "The
  Cauldron": brew the Philter (111) → sneak scene (invisible: only your
  footprints appear among the Afflicted) → cauldron use-choice → counter-brew
  is act 1's healing flower (001) → realm saved, Jarl bookend. Brews escalate
  1 → 2 → 3 bottles then back to 1. Built on a **data-driven story
  engine**: a story is a table of `StoryNode`s (card / **speak** / choice /
  tune / scene / **brew** / battle / end) linked by successor indices, so
  acts and universes are pure data (`kStorySkyrim`). `N_BREW` runs the brew
  machinery outside battle (wrong potion named, retried at leisure, no
  backing out). **Recipe rule:** every act's target recipe is SPOKEN in
  dialogue before the brew and repeated as the brew-screen hint.
  **Build gates (don't bypass them):** contracts that used to be prose are
  enforced by PlatformIO pre-scripts — a violation FAILS the build naming
  the offender. `tools/check_story_text.py`: authored story text
  (card/speak bodies, titles, brew hints, battle intros) must fit each
  renderer's real wrap budget — drawWrapped truncates silently on-device;
  budgets live at the top of the script, keep them in step if a render
  call site changes. `tools/check_invariants.py`: story-graph integrity
  (successors in range, all nodes reachable, brew combos 1..7), the
  battle-maths rule (enemy HP > 14), kStirCap > kStirDecay, the pin map
  vs its doc copies (CLAUDE.md / CIRCUIT.md / HARDWARE_TEST.md), the d20
  tables and every multi-word display string shared with
  tools/screens_preview.py (twin drift), and menu/potion names appearing
  in CLAUDE.md. When adding content, run `pio run` early — the gates give
  precise errors. `N_SPEAK` renders a character: a procedural **emblem**
  on the left (the node's `art` fn — `drawJarl` is a bobbing, glinting crown;
  emblems beat literal faces at 30 px), speech bubble with tail on the right,
  speaker name beneath.
  Choices set bits in a generic flag set (`s_flags`) that later nodes read
  back — e.g. `SF_JARL` earns the brew-screen hint but gives the rat first
  strike (**fold-back branching**, no bespoke per-story state) — and can
  carry per-option HP effects (`healA`/`healB`, the inn's +5/+7).
  Battles are parameterised by a `BattleDef` (bar label, intro line, sprite
  fn, enemy HP, bite damage, demanded brew combo, brew title, hint + its
  flag, first-strike flag) — the rat sprite is procedural (`drawRat`) and
  referenced from its def; new universes bring their own. Card bodies must
  fit the frame (~22 chars/line in 5x8, max 4 lines). KO routes to a card
  that loops back into the battle node, which re-arms itself.
  **UI conventions:** every decision uses the **choice spinner** at the
  bottom of the display (`drawChoiceLine`: `< option >`, turn to cycle, press
  to select; options too wide for one line wrap onto two) — never stacked
  menus over the scene. In battle
  both **HP bars stack at the top** (player above enemy — the key info),
  animated via `easeHP` (~20 HP/s drain/refill, numbers ticking along); the
  enemy idles below; messages share the bottom lines. The attack roll is a
  **full-screen cutaway** (`renderBattleRoll`): the 3D icosahedron d20
  (`drawD20Tumbling` — golden-ratio vertices, projected-area back-face
  culling, weak perspective; numbers drawn ON the faces via `kD20FaceNum`,
  rotated so the landing face carries the roll) tumbles along a table line,
  zooms to face-on, holds the number; rotation is continuous throughout. A
  landed hit — either side's — **blink-inverts the whole screen** (XOR box).
  NOTE: U8g2 line coords are **unsigned** — anything that can leave the
  screen must go through `drawLineClipped` or it smears wrapped lines across
  the buffer.
  **Battle rules:** player dice are ALWAYS honest (never fudged) → damage
  4–7. The forcing is authored **enemy behaviour**: the `CRIT_ON_BITE`th bite
  drops the player to `CRIT_HP` and numbs the sword arm (attacking numb is
  fatal). Enemy HP must exceed 2× max attack damage (14) so the crit lands
  before the enemy can die, and stay low enough that honest post-heal rolls
  finish within a turn or two. Brewing consumes a turn (the enemy bites while
  you're at the cauldron); a wrong potion is named and wasted — usually fatal
  at `CRIT_HP`.
  **Brew handoff:** the real IDENTIFY/STIRRING machinery runs with
  `s_storyBrew` up (custom prompt screen replaces IDENTIFY; STIRRING
  unchanged; `enterCombo(0)` waits instead of idling; the finished stir —
  or, with all three bottles, the finished RITUAL — routes to
  `storyBrewResolve` instead of REVEAL; **press with nothing
  seated backs out** to the fight, no turn consumed). Act mechanics apply
  inside story brews too (two bottles = align, three = ritual). Story forces
  `g_universe` to Skyrim while active and restores it on exit (NVS
  untouched). **Exiting a quest:** 15 s without input raises the pause
  spinner ("Quest on" / "Quit") over any story screen — the one deliberate
  quit path (a bare long-press proved undiscoverable and was dropped).
  Mid-brew/mid-ritual a long-press still abandons, as a hidden hatch — the
  pause menu can't reach the shared brew screens.
- **IDENTIFY** — ≥1 bottle seated. **Features** the ingredient name(s) in an
  elegant serif with sparkles (a single ingredient large, two/three stacked);
  each name carries a small **ordinal caption** above it ("First/Second/Third
  ingredient", by order seated) so it's clear ingredients **stack**, not replace.
  No realm header. A subtle "turn to stir" cue.
- **STIRRING** — encoder is turning.
  - *Acts 1 & 3:* a **power bar** fills as you stir, with a swirling vortex +
    rising trill (~320→1100 Hz). The add rate is **capped**, so spinning faster
    can't rush it — each **Difficulty** (Easy/Medium/Hard) has a guaranteed
    **minimum fill time** (~3 / 5 / 10 s; see [docs/TUNING.md](docs/TUNING.md)).
    An escalating **caption** climbs with the bar (random brew-themed lines per
    band at <50 / <75 / <90 / ≥90 %).
  - *Act 2 — "align the essences":* the knob steers your solid wave into phase
    with a drifting ghost wave; the bar fills **only while aligned** and drains
    while not. The drift keeps a persistent heading (occasionally reversing),
    **speeds up and the tolerance tightens as the bar fills** — a parked knob
    loses. Aligned, the wave doubles up ("glows") and sparkles; the trill
    is the hot/cold aid (closer = higher and steadier, far = low and warbling).
    Tolerance / drift / fill rate scale with Difficulty (`kAlign*`/`ALIGN_*`).
  - In both, the bar **always drains** (faster on harder levels); pause and it
    bleeds down, and once empty it waits a ~3 s grace before drifting back to
    IDENTIFY (turning the knob counts as activity, so a slow hunt can't fizzle).
    When the bar fills: acts 1–2 show a framed **"Press to create"**
    call-to-action (held until a press or a real combo change); act 3 goes
    straight into the ritual.
- **RITUAL** (act 3 only) — the **Grand Brew**: the brew speaks a growing
  incantation of glyphs (**turn right / turn left / press**, each with its own
  note so it can be memorised by ear) and you repeat it back over three verses
  (Simon-style prefixes of one sequence, lengths 2/3/4). A wrong or stalled
  answer **replays the verse** — never punishes; completing the incantation
  goes **straight to REVEAL** (no extra press) — or to `storyBrewResolve`
  inside a story brew. Long-press abandons (to the base, or to the story's
  carousel exit mid-story).
- **REVEAL** — one of **three random full-screen animations** (radar sweep /
  rising liquid / expanding rings) reveals the potion name (wrapped to two lines
  if wide) over a short ascending jingle. After ~3 s it **resyncs to the base**:
  back to IDLE if the bottles were cleared, or straight to IDENTIFY if they're
  still seated (so you can re-brew without reseating anything).

### Interaction rules

- **Combo latching (reliability):** IDLE and IDENTIFY follow the bottles live.
  But once a brew is underway (STIRRING, RITUAL or REVEAL) the combo is
  **latched** — pulling a magnet away (fully or partially, deliberately or via
  a flaky reed) is **ignored**, so the swirl, the ritual and the revealed
  potion survive. A brew only restarts on a genuinely new arrangement: a bottle
  **added** to the set, or the base cleared to **empty and then refilled**.
  When a stir fizzles out (knob idle) it resyncs to whatever is actually on
  the base.
- **Short press**:
  - In IDENTIFY/STIRRING = mix & reveal — but **stirring is REQUIRED first**: a
    press before enough stir progress does nothing (a low "not yet" buzz).
  - In RITUAL = a "press" answer to the incantation (or skips the intro card).
  - On IDLE = activate the carousel panel in view (Story Mode / Settings;
    the Place panel just gives the "not yet" buzz).
  - In STORY = advance a card / select the spinner option (incl. the idle
    pause menu's "Quest on" / "Quit").
  - In SETTINGS = activate the highlighted item.
- **Long press (≥600 ms)** = leave a menu (SETTINGS → idle, Hardware Test →
  Settings) or **abandon the ritual** (resync to the base). In STORY it does
  nothing — quitting is the idle pause menu's job. It no longer toggles the
  realm.

### Settings menu (`ST_SETTINGS`) — a reusable mini-menu

Open with a press on the carousel's Settings panel. **Turn** to move between items; **press**
to start editing a value, **turn** to change it (applied live), **press** to
confirm — the NVS write happens on confirm; **long-press** cancels an edit and
**reverts** the value, or (when not editing) leaves the menu (resyncing to
whatever bottles are on the base). Actions (Hardware Test, Exit) run on press.
Items are a data table (`kMenu` in `main.cpp`) of `{label, kind, get/set/persist,
…}`, so adding one is a single row.

- **Realm** — 7 universes: Skyrim, Baldur's Gate 3, The Witcher 3, World of
  Warcraft, Zelda, Minecraft, Ultima VII (persisted; replaces the old long-press)
- **Volume** — 0–5 (0 = mute; gates all audio). Loudness is the buzzer's LEDC
  duty cycle (`kVolDuty`, `buzzTone()` — main.cpp drives LEDC directly, not
  `tone()`), applied live while editing with a feedback blip
- **Bright** — 1–5 (OLED contrast via `setContrast`)
- **Difficulty** — Easy / Medium / Hard (fill difficulty curve; NVS key and
  code identifiers still say `stir`/`g_stirLevelIdx`)
- **Sleep** — screen-blank timeout: 1m / 10m / 60m / 12h (no "Never" — an
  always-on OLED burns in). The OLED powers
  down (`setPowerSave`) after that long with no input and wakes on any encoder /
  reed / button activity. A waking **turn or press is swallowed** so it doesn't
  also act; a **reed change still registers** (seating a bottle should count).
- **Hardware Test** — opens a built-in live diagnostic (`ST_DIAG`): reed/button
  boxes + encoder count; long-press to return. (Same idea as the `c3-hwcheck`
  build, but in-firmware so it needs no reflash.)
- **Firmware** — shows the version string
- **Exit** — back to idle

Realm, Volume, Brightness, Difficulty and Sleep all persist to NVS (written when
an edit is confirmed, not per step).

On boot, an animated ~3 s splash ("Welcome to the Alchemist's Study", growing
title + rising sparkle-chime, silent at Volume 0) plays before the idle screen.

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

## Previewing display art off-device

`tools/oledsim.py` replicates the U8g2 primitive subset main.cpp uses (same
integer rounding, y-down coords, draw-color semantics incl. XOR, plus an
approximate 5×7 text font that runs ~15% wide) on a 128×64 buffer and writes
an upscaled multi-frame PNG — so screens can be SEEN and iterated before
flashing. Write a sketch script that imports it, transliterate the C++
drawing function (keep them line-for-line twins), render several `now`
timestamps, look at the PNG, iterate, then port back to C++. Lessons already
learned this way: rotation aliases 1-bit art to mush at icon sizes (draw
upright, animate with bob/glint instead), and emblems read better than
literal faces.

**The shared screen contact sheet.** `tools/screens_preview.py` holds
transliterations of every screen renderer (update it when screens change);
`tools/make_screen_sheet.py` builds them into a captioned HTML review page.
It is published as a Claude artifact — ALWAYS redeploy to the same URL so
the shared page stays current:
`https://claude.ai/code/artifact/4974b679-8055-4225-9491-1919771370a0`

## Conventions

- **State machine: follow [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).** One
  transition point (`enterState`), time measured as `now - g_stateMs`, sequences
  as re-stamped sub-phases, `update`/`render` split, latched inputs + grace
  timers. This is the standard — don't reintroduce independent timers or
  instantaneous-input control flow.
- Keep the app in `src/main.cpp` with the state machine clean and readable. The
  shared pin map (`src/pins.h`) and encoder decoder (`src/quadrature.h`) are the
  only split-out pieces — they exist so the diagnostics can't drift from the
  firmware; don't grow them into a module system.
- Add brief comments explaining the pin choices and the core-3.x dependency.
- Non-blocking loop: debounce reeds and button by time, no long `delay()`s that
  would stall stir audio/animation.
