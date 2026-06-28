# Firmware architecture & state-machine conventions

The firmware is a single **non-blocking loop** driving an explicit state machine.
Earlier versions had timing bugs because separate timers were anchored to
*different* events and only worked if they happened to line up, and because
control flow sampled *instantaneous* input (a reed level) mid-flow. These
conventions exist to make that class of bug impossible. Follow them.

## States

`IDLE · IDENTIFY · STIRRING · REVEAL · SETTINGS · DIAG`
(`REVEAL` has sub-phases: `RP_ANIM` → `RP_NAME`.)

## The four rules

1. **One transition point.** `enterState(s)` is the *only* place `g_state`
   changes. It also stamps `g_stateMs = millis()`. Never write `g_state = …`
   anywhere else. Data-carrying transitions (`enterCombo()`) and the reveal
   phase advance go through it.

2. **Time-in-state, never cross-timers.** Measure every duration as
   `now - g_stateMs`. Do **not** anchor a duration to one event and a later
   action to another and hope they align. A sequence of timed steps becomes
   explicit **sub-phases**, each of which re-stamps `g_stateMs` when it begins —
   so the steps *compose* (step B starts when step A finishes), they don't race.

   *Example — REVEAL:* `RP_ANIM` runs for `REVEAL_ANIM_MS`; when it ends it
   re-stamps the clock and switches to `RP_NAME`, which runs for
   `REVEAL_NAME_MS`, then `enterCombo(0)` returns to idle. The name is shown for
   exactly `REVEAL_NAME_MS` *after the animation finishes*, regardless of the
   animation's length.

3. **`update*()` mutates, `render*()` is pure.** Only `update*()` functions
   change state or data. `render*()` functions are a pure function of
   `(state, now - g_stateMs)` → pixels: no transitions, no side effects. You can
   delete every render call and the logic still runs correctly (just blind).

4. **Latch inputs; recover via grace timers, never instantaneous reads.**
   Debounced reed/encoder/button events update *intent*; control flow reads the
   latched intent. A time-critical flow latches the input it cares about so a
   transient change (a jostled magnet, a flaky reed, an inter-detent gap) can't
   derail it. Fallbacks are explicit timeouts, not "is the reed closed *right
   now*".

   *Examples:*
   - **Combo:** removing bottles never bails instantly. An empty base is latched
     with a timestamp; only after `REED_GRACE_MS` does IDENTIFY fall back to
     idle — so turning still starts the (latched) stir during the cooldown after
     a magnet leaves. Once STIRRING/REVEAL, the combo is fully latched.
   - **Stir:** `STIR_ACTIVE_MS` is the fill/drain boundary (moved within it =
     filling), `STIR_DECAY_PER_S` is the drain rate, `STIR_IDLE_BACK_MS` is the
     give-up timeout. Three distinct jobs, no overlap.

## Adding behaviour

- New screen/mode → add a `State`, transition in via `enterState()`, give it an
  `update*()` (transitions) and a pure `render*()`.
- Multi-step sequence → model each step as a sub-phase that re-stamps `g_stateMs`.
- New tunable timing → add a named constant to the **Tunables** block in
  `main.cpp`; never inline a magic number.
