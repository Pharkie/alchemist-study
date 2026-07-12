# The Alchemist's Study

**Brew potions with real bottles. No screen-tapping — magnets, a brass-feel
knob, and a tiny glowing study on your shelf.**

<!-- GIF: hero demo — seat bottles, stir, reveal. ~10s loop.
<p align="center"><img src="docs/assets/alchemist-demo.gif" alt="Brewing a potion" width="70%"></p>
-->
*(demo GIF coming soon)*

A 3D-printed alchemist's study with three potion bottles that really *do*
something: seat them in their homes and hidden reed switches sense which are
present; turn the stirring knob and a vortex swirls on the little OLED; and
the potion you brewed is revealed by name — drawn from **seven RPG realms**
(Skyrim, Baldur's Gate 3, The Witcher 3, World of Warcraft, Zelda, Minecraft,
Ultima VII). A passive buzzer scores everything from the stir-trill to the
reveal jingle.

Runs fully offline. No app, no account, no WiFi setup — power it and brew.

### Reasons to love it

- **Physical play** — real bottles with magnets in their bases are the
  controller; one, two or all three change what you brew
- **Three mini-games, rising ceremony** — one bottle is a classic stir; two
  becomes *align the essences* (steer your wave into phase with a drifting
  one, guided by ear); all three earns the *Grand Brew* — a Simon-style
  incantation you answer with turns and presses, learned by sound
- **A story mode** — a three-evening Skyrim adventure with choices, an honest
  3D-rendered d20, a battle that teaches you to brew under pressure, and a
  poisoning plot to unravel. Every playthrough opens by asking your skill:
  *Apprentice, Graduate or Professor?*
- **Seven realms** — each with three in-universe ingredients and seven potions
- **Shelf-friendly** — screen sleeps on a timer, volume 0–5 (0 = silent),
  everything remembered across power-off

## How it plays

1. **Place ingredients** — seat 1–3 bottles; the screen names them as they stack
2. **Stir** — turn the knob; each bottle count is its own mini-game
3. **Brew** — a full bar (and, for the master potion, a completed incantation)
   reveals your potion with a full-screen animation
4. **Story Mode** — pick it from the home carousel and play the adventure;
   15 s idle offers "Quest on / Quit" so you can always leave cleanly
5. **Settings** — realm, volume, brightness, screen sleep, and a built-in
   hardware test

Three bottles give seven combinations (3 singles + 3 pairs + 1 triple) —
seven potions per realm, forty-nine names to discover.

## What you need

Rough cost is pocket-money territory — every electronic part is a common
hobbyist module.

| Part | Qty | Notes | AliExpress | Amazon |
|---|---|---|---|---|
| ESP32-C3 Super Mini | 1 | The brain; native USB-C | *link tbc* | *link tbc* |
| 0.96" SSD1306 OLED, 128×64, I²C | 1 | White pixels look best | *link tbc* | *link tbc* |
| KY-040 rotary encoder module | 1 | With push-switch and knob | *link tbc* | *link tbc* |
| Reed switch (normally open) | 3 | Glass, ~14 mm | *link tbc* | *link tbc* |
| Passive piezo buzzer | 1 | Must be **passive** — active ones can't play tunes | *link tbc* | *link tbc* |
| Neodymium disc magnets | 3 | ~6×3 mm, one per bottle base | *link tbc* | *link tbc* |
| Hook-up wire / Dupont jumpers | — | | *link tbc* | *link tbc* |
| USB-C cable + 5 V supply | 1 | Any phone charger | | |
| 3D-printed model | 1 | Print files: *coming soon* | | |

## Build it

The full guide — wiring diagram, printing, assembly, flashing — lives in
**[docs/ASSEMBLY.md](docs/ASSEMBLY.md)**.

The short version: print the study, push the reed switches into their slots
under the bottle homes, glue a magnet into each bottle base, wire the modules
to the ESP32-C3 per the diagram, and flash the firmware with one command.

## Flash the firmware

With [PlatformIO](https://platformio.org/) (Core ≥ 6.1.19) installed:

```sh
pio run -e c3-prod -t upload    # build + flash over USB-C
```

That's it — the study boots with a welcome chime. If something doesn't
respond, flash the built-in wiring doctor instead
(`pio run -e c3-hwcheck -t upload`) and follow
[docs/HARDWARE_TEST.md](docs/HARDWARE_TEST.md).

## The realms

**Skyrim · Baldur's Gate 3 · The Witcher 3 · World of Warcraft · Zelda ·
Minecraft · Ultima VII** — three ingredients and seven potions each, all
period-correct to their games. Full tables in [CLAUDE.md](CLAUDE.md).

## For tinkerers

Built with PlatformIO + Arduino (arduino-esp32 **3.x** via the pioarduino
platform, pinned). The interesting bits:

- Single-file state machine with strict timing conventions
  ([docs/ARCHITECTURE.md](docs/ARCHITECTURE.md))
- Data-driven story engine — an adventure is a table of nodes; new realms
  bring their own scripts ([CLAUDE.md](CLAUDE.md))
- Feel tunables documented with the maths ([docs/TUNING.md](docs/TUNING.md)),
  mini-game design notes ([docs/MINIGAMES.md](docs/MINIGAMES.md))
- **Build gates**: story text that would overflow the screen, a broken story
  graph, doc/tuning drift, or a stale screen-preview twin *fails the build*
  (`tools/check_story_text.py`, `tools/check_invariants.py`)
- An off-device screen simulator renders every UI screen to PNG for review
  (`tools/screens_preview.py`)

| Env | Purpose |
|---|---|
| `c3-prod` | optimised release build (default) |
| `c3-dev` | verbose logs + bench shortcuts (fake bottles from the Place panel) |
| `c3-hwcheck` | standalone wiring diagnostic |

`FW_VERSION` in `src/main.cpp` tracks the
[GitHub releases](https://github.com/Pharkie/alchemist-study/releases).
Connectivity (WiFi provisioning, OTA, Home Assistant) is a planned later
stage — see [BACKLOG.md](BACKLOG.md).

## LLM Generated, Human Reviewed

This code was generated with [Claude Code](https://claude.com/claude-code)
(Anthropic), primarily on Claude Opus 4.8 and Claude Fable 5. Development was
overseen by the human author with attention to reliability and security.
Architectural decisions, configuration choices, and development sessions were
closely planned, directed, and verified by the human author throughout —
including hardware bring-up and on-device testing beyond the LLM. Still, the
code has had limited manual review; please make your own checks and use it at
your own risk.

## License

Free for noncommercial use under the **PolyForm Noncommercial License 1.0.0**.
**Commercial use is not free — a commercial license is required.** See
[LICENSE](LICENSE) for full terms.

© 2026 Adam Knowles. All rights reserved.
