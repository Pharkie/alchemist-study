# Backlog — Alchemist Study

Phased plan. Check items off as completed. See `CLAUDE.md` for the full spec,
pin map, and potion tables.

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

- [ ] Configure reed pins (3, 4, 10) as `INPUT_PULLUP`; read combo `0..7`
      with bit mapping `bit0=slot1, bit1=slot2, bit2=slot3` (magnet = LOW = present).
- [ ] Debounce reed reads (time-based) so a settling bottle doesn't chatter.
- [ ] Use the `main.cpp` GPIO-interrupt quadrature decoder (A=0, B=7, internal
      pullups) already scaffolded in Phase 0; read `g_encoderCount` delta per loop
      for stir progress + swirl angle.
- [ ] Configure button (SW=20, `INPUT_PULLUP`); detect short press vs
      long press (≥600 ms) with debounce.

## Phase 2 — State machine skeleton

- [ ] Implement `IDLE / IDENTIFY / STIRRING / REVEAL` enum + transitions.
- [ ] Combo change at any time → IDENTIFY (or IDLE when combo == 0).
- [ ] REVEAL is dismissed **only** by a combo change.
- [ ] Wire transitions: bottle seated → IDENTIFY; encoder turning → STIRRING;
      knob still → decay back to IDENTIFY; short press (gated on stir) → REVEAL.

## Phase 3 — Display (U8g2)

- [ ] Init `U8G2_SSD1306_128X64_NONAME_F_HW_I2C` on SDA=5 / SCL=6, full-buffer.
- [ ] IDLE screen: "Place ingredients" + realm-toggle hint.
- [ ] IDENTIFY screen: list present ingredient name(s) + "turn to stir".
- [ ] STIRRING swirl: ring of dim dots, one bright dot orbiting; angle follows
      the encoder; intensity/decay tracks stir progress.
- [ ] REVEAL: potion name, word-wrap to two lines if wider than 128px.

## Phase 4 — Audio (buzzer, GPIO1)

- [ ] Stir brewing trill: pitch rises with stir progress (~320 → ~1100 Hz),
      decays back down when the knob is still.
- [ ] REVEAL success jingle: short ascending notes.
- [ ] Universe-toggle: distinct two-note beep.

## Phase 5 — Potion data & universes

- [ ] Encode both lookup tables (Skyrim / BG3), indexed by combo `1..7`.
- [ ] Encode per-universe ingredient slot names for the IDENTIFY screen.
- [ ] Long press → toggle universe; persist to NVS via `Preferences`; restore
      on boot.

## Phase 6 — Polish & verify

- [ ] Tune stir feel: progress accumulation, decay rate, gating threshold for
      reveal, pitch curve.
- [ ] Verify non-blocking loop (audio/animation stay smooth; no stalls).
- [ ] On-hardware pass: all 7 combos × both universes, reveal wrapping,
      toggle persists across reboot.
- [ ] Comment pass: pin choices + core-3.x dependency rationale in `main.cpp`.

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
