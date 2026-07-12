# Tuning the feel

All the knobs that shape how the brewing *feels* are `constexpr` values grouped
under `// ---- Tunables` near the top of `src/main.cpp`. Edit, then reflash:

```sh
pio run -e c3-dev -t upload
```

## Stir model (capped add vs. decay)

The power bar **always drains**; stirring **adds** against it — but the add rate
is **capped**, so spinning faster than `cap/gain` counts/sec does nothing:

    rate = min(kStirGain[level] * counts_per_sec, kStirCap[level])
    progress += rate * (1 - kStirResist[level]*progress) * dt   // capped add
    progress -= kStirDecay[level] * dt                          // always drains

This is the crux: because the add is **capped**, each level has a **guaranteed
minimum fill time ≈ 1/(cap − decay)** that no spin speed can beat — so difficulty
does **not** depend on measuring anyone's max stir rate. You must also sustain
**> decay/gain c/s** or the decay wins and the bar stalls/drains. Stop and it
bleeds to zero (grace, then back to identify). A full bar arms ("Press to create").

| Constant | Easy / Med / Hard | Effect |
|---|---|---|
| `kStirGain[]` | `0.032 / 0.0167 / 0.012` | add/sec per (count/sec) until capped; sets the sustain threshold. |
| `kStirCap[]` | `0.48 / 0.50 / 0.60` | **max** add/sec — the ceiling; can't be cheesed. |
| `kStirResist[]` | `0.30 / 0.20 / 0.10` | mild end-loading (small so the capped top stays reachable). |
| `kStirDecay[]` | `0.15 / 0.30 / 0.50` | bar drained per second, **always**. |
| `STIR_ANGLE_STEP` | `0.18` | Swirl radians per encoder count (visual only). |

Min fill ≈ `1/(cap − decay)`: Easy ~3 s, Medium ~5 s, **Hard ~10 s+**. To make a
level harder, move `cap` toward `decay` (raises the floor) or raise `decay`. Keep
`cap > decay` or it's unfillable.

This model runs for **acts 1 and 3** (one or three bottles seated). Act 2 uses
the alignment model below; act 3 follows the full bar with the ritual.

## Act 2 — "align the essences" (two bottles)

The knob shifts your wave's phase; the target wave drifts on a random heading.
Inside the tolerance band the bar fills; outside it drains. Per stir level:

| Constant | Easy / Med / Hard | Effect |
|---|---|---|
| `kAlignTol[]` | `0.40 / 0.30 / 0.22` | tolerance band, radians either side, before the shrink. |
| `kAlignDrift[]` | `0.90 / 1.40 / 2.00` | max drift speed (rad/s), before the ramp; a new heading every `ALIGN_RETARGET_MS`. |
| `kAlignFill[]` | `0.30 / 0.22 / 0.16` | bar added per **aligned** second (~3.3/4.5/6.3 s of aligned time). |
| `kAlignDrain[]` | `0.20 / 0.28 / 0.40` | bar drained per misaligned second — parking the knob loses. |
| `ALIGN_KNOB_STEP` | `0.05` | phase radians per encoder count (higher = twitchier knob). |
| `ALIGN_RETARGET_MS` | `700` | how often the drift re-rolls its heading. |
| `ALIGN_FLIP_P` | `0.25` | chance a re-roll reverses direction — the rest keep it, so the target genuinely travels. |
| `ALIGN_RAMP` | `1.0` | extra drift speed gained by a full bar (2x at the top). |
| `ALIGN_TOL_SHRINK` | `0.4` | fraction of the tolerance band lost by a full bar. |

Bench-tuned 2026-07-12: the original random-walk drift (0.35-0.8 rad/s, sign
re-rolled every 900 ms) cancelled itself out — a parked knob passed. The fix
is persistence + the progress ramp, not raw speed alone.

The trill doubles as the hot/cold aid here: closer to phase = higher and
steadier; far off = lower with a wide warble.

## Act 3 — the Grand Brew ritual (all three bottles)

After the stir bar fills, a Simon-style incantation of turns/presses plays and
must be repeated back. All timing/shape knobs are `RIT_*` constants:

| Constant | Default | Effect |
|---|---|---|
| `RIT_SEQ_LEN` / `RIT_ROUNDS` | `4` / `3` | incantation length; verses are prefixes (lengths 2, 3, 4). |
| `RIT_GLYPH_MS` | `650` | per-glyph time while the incantation plays. |
| `RIT_TURN_COUNTS` | `4` | encoder counts (~1 detent) that register a turn answer (bench-tuned: 2 detents felt unresponsive). |
| `RIT_INPUT_TIMEOUT_MS` | `12000` | stalled answer → the verse replays (no fail). |
| `RIT_INTRO_MS` / `RIT_GOOD_MS` / `RIT_MISS_MS` | `1800/900/1200` | card + interlude durations. |

## Audio (needs a passive buzzer)

| Constant | Default | Effect |
|---|---|---|
| `PITCH_MIN_HZ` | `320` | Trill pitch at zero stir progress. |
| `PITCH_MAX_HZ` | `1100` | Trill pitch at full stir progress. |

Melodies (success jingle, realm-toggle beep, "not ready" buzz) are the
`MEL_*` note tables just below the tunables.

## Timing / input

| Constant | Default | Effect |
|---|---|---|
| `REED_DEBOUNCE_MS` | `40` | Settle time before a reed change is committed. |
| `LONG_PRESS_MS` | `600` | Hold to leave a menu / cancel an edit. |
| `BTN_DEBOUNCE_MS` | `30` | Minimum press to count as a short press. |
| `RENDER_INTERVAL_MS` | `33` | Display refresh cap (~30 fps). ~30 ms is the I²C floor; lower won't help. |
| `STIR_ZERO_GRACE_MS` | `3000` | Hold the brewing screen this long at an empty bar before reverting. |
| `REED_GRACE_MS` | `2500` | Identify: keep the combo this long after the base empties (turn still works). |
| `ENC_STEP` | `4` | Encoder counts per menu/selector step. Lower = menu moves faster per detent. |

## Settings menu

Runtime settings live in the on-device **Settings** menu (press the knob on the
idle screen; turn to move, press to edit a value, turn to change, press to
confirm — a long-press cancels the edit and reverts the value): **Realm**,
**Mute**, **Brightness** (1–5), **Stir Level** (Easy/Medium/Hard), **Sleep**
(screen-blank timeout), **Hardware Test**, **Firmware**, **Exit**. Realm, Mute,
Brightness, Stir Level and Sleep persist to NVS (written on confirm, not per
step). The menu is a data table (`kMenu` in `main.cpp`) — add an item by
appending one row (`{label, kind, get, set, persist, …}`); kinds are choice /
range / info / action.

## Encoder direction

If the swirl orbits / pitch ramps *opposite* to how you turn, either swap the
**CLK/DT** wires, or negate the direction in firmware: flip the sign of `d` in
`updateStir()` (or swap `PIN_ENC_A`/`PIN_ENC_B`).

## Combo latching (reliability)

Once stirring starts, the combo is latched through the reveal — removing magnets
is ignored; a new brew needs a bottle *added*, or the base *cleared then
refilled*. This lives in `onSensedComboChange()` in `src/main.cpp`.
