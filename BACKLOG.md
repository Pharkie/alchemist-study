# Backlog — Alchemist Study

Phased plan. Check items off as completed. See `CLAUDE.md` for the full spec,
pin map, and potion tables.

> **Status:** Phases 0–6 shipped as **v0.1** (the bench firmware). Items whose
> behaviour later evolved carry a *(superseded: …)* note. GPIO numbers quoted
> in the phase notes are the original bench wiring; the final pin map lives in
> `src/pins.h` (and the CLAUDE.md table). Next up: Stage 2 (connectivity),
> below.

## Phase 0 — Project setup

Two independent moves here — keep them separate:

- [x] **Repoint platform (required for `tone()`, unrelated to WiFi):** moved
      `platformio.ini` off `espressif32@6.9.0` (dead on core 2.x) to the pioarduino
      3.x platform (`55.03.39`). `monitor_speed = 115200`, build flags
      `-D ARDUINO_USB_MODE=1 -D ARDUINO_USB_CDC_ON_BOOT=1`. (Needs PlatformIO Core
      ≥6.1.19.)
- [x] **Strip the template's baggage (not WiFi capability):** removed ArduinoJson,
      LittleFS wiring, `secrets.h`/`config/`, the custom partition CSV, and the WiFi
      glue from `src/main.cpp`. The core WiFi library stays available for the
      connectivity stage — we just don't use it yet.
- [x] Add `lib_deps`: `olikraus/U8g2`. (No encoder lib — `ESP32Encoder` needs
      the PCNT peripheral the C3 lacks; encoder is decoded via GPIO interrupts in
      `main.cpp`.)
- [x] Confirm a clean `pio run` (compile only) before writing app logic.
- [x] Update `README.md` to describe the real device (dropped WiFi quick-start;
      noted connectivity is a planned later stage).

## Phase 1 — Inputs & sensing

- [x] Configure reed pins (3, 4, 10) as `INPUT_PULLUP`; read combo `0..7`
      with bit mapping `bit0=slot1, bit1=slot2, bit2=slot3` (magnet = LOW = present).
- [x] Debounce reed reads (time-based) so a settling bottle doesn't chatter.
- [x] Use the GPIO-interrupt quadrature decoder (A=0, B=7, internal pullups)
      already scaffolded in Phase 0; read `g_encoderCount` delta per loop for
      stir progress + swirl angle. *(now lives in `src/quadrature.h`, shared
      with the diagnostics.)*
- [x] Configure button (SW=20, `INPUT_PULLUP`); detect short press vs
      long press (≥600 ms) with debounce.

## Phase 2 — State machine skeleton

- [x] Implement `IDLE / IDENTIFY / STIRRING / REVEAL` enum + transitions.
- [x] Combo change at any time → IDENTIFY (or IDLE when combo == 0).
- [x] REVEAL is dismissed **only** by a combo change. *(superseded: the reveal
      now times out on its own — ~1.3 s animation + ~3 s name — then resyncs to
      whatever is on the base; an added bottle also restarts it.)*
- [x] Wire transitions: bottle seated → IDENTIFY; encoder turning → STIRRING;
      knob still → decay back to IDENTIFY; short press (gated on stir) → REVEAL.

## Phase 3 — Display (U8g2)

- [x] Init `U8G2_SSD1306_128X64_NONAME_F_HW_I2C` on SDA=5 / SCL=6, full-buffer.
- [x] IDLE screen: "Place ingredients" + realm-toggle hint.
- [x] IDENTIFY screen: list present ingredient name(s) + "turn to stir".
- [x] STIRRING swirl: ring of dim dots, one bright dot orbiting; angle follows
      the encoder; intensity/decay tracks stir progress.
- [x] REVEAL: potion name, word-wrap to two lines if wider than 128px.

## Phase 4 — Audio (buzzer, GPIO7)

- [x] Stir brewing trill: pitch rises with stir progress (~320 → ~1100 Hz),
      decays back down when the knob is still.
- [x] REVEAL success jingle: short ascending notes.
- [x] Universe-toggle: distinct two-note beep.

## Phase 5 — Potion data & universes

- [x] Encode both lookup tables (Skyrim / BG3), indexed by combo `1..7`.
      *(grown since: seven realms — see CLAUDE.md for all tables.)*
- [x] Encode per-universe ingredient slot names for the IDENTIFY screen.
- [x] Long press → toggle universe; persist to NVS via `Preferences`; restore
      on boot. *(superseded: the realm moved into the Settings menu; long-press
      now leaves menus / cancels edits.)*

## Phase 6 — Polish & verify

- [x] Tune stir feel: progress accumulation, decay rate, gating threshold for
      reveal, pitch curve.
- [x] Verify non-blocking loop (audio/animation stay smooth; no stalls).
- [x] On-hardware pass: all 7 combos × both universes, reveal wrapping,
      toggle persists across reboot.
- [x] Comment pass: pin choices + core-3.x dependency rationale in `main.cpp`.

---

## Stage 1.5 — Brewing acts (mini-games)

The brewing mechanic now scales with the **act** = bottles seated
(design + shelved ideas in `docs/MINIGAMES.md`, tuning in `docs/TUNING.md`):

- [x] Act 1 (one bottle): classic capped-add stir (unchanged).
- [x] Act 2 (two bottles): **"align the essences"** — steer your wave into
      phase with a drifting ghost wave; the bar fills only while aligned, and
      the trill doubles as the hot/cold aid.
- [x] Act 3 (all three = master potion): the **Grand Brew ritual** — a
      Simon-style incantation of turns/presses (3 verses, lengths 2/3/4)
      after the stir; finishing goes straight to the reveal.
- [ ] Future candidates (see `docs/MINIGAMES.md`): attunement dial
      ("perfect brew" finisher), essence catcher easter egg, alembic balance,
      wheel of fate, per-realm ritual flavour.

---

## Stage 2 — Connectivity (deferred, deliberate)

Only start after the bench firmware works end-to-end on hardware. The radio is the
C3's reason for existing; bolting this on later is easy, debugging the swirl through
a provisioning layer is not. Build it so the device still works fully offline.

- [ ] WiFi connect with non-blocking credential handling (not hardcoded
      `secrets.h`) — e.g. captive-portal / runtime provisioning to NVS.
- [ ] OTA firmware update (reflash potion tables without pulling the model off the
      shelf to find the USB port).
- [ ] Tiny status web page (current universe, seated combo, last potion).
- [ ] Home Assistant integration: announce brews as HA events / trigger scenes
      (e.g. "Philter of the Phantom brewed").
