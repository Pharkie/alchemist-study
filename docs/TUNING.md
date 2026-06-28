# Tuning the feel

All the knobs that shape how the brewing *feels* are `constexpr` values grouped
under `// ---- Tunables` near the top of `src/main.cpp`. Edit, then reflash:

```sh
pio run -e c3-dev -t upload
```

## Stir model (decay vs. add)

The power bar **always drains**; turning the knob **adds** against it, with
diminishing returns toward the top:

    progress -= kStirDecay[level] * dt            // every tick
    progress += kStirGain[level] * |counts| * (1 - kStirResist[level] * progress)   // on motion

So you *fight the decay*, and must stir ever faster near full (where the add
shrinks). Stop and the bar bleeds down; when it reaches zero it returns to
identify — the decay **is** the grace, so there's no separate idle timer. A full
bar arms ("Press to create") and holds until a press or a real combo change. The
**Stir Level** setting picks the numbers.

| Constant | Easy / Med / Hard | Effect |
|---|---|---|
| `kStirGain[]` | `0.060 / 0.050 / 0.026` | Bar added per encoder count (when empty). Lower = harder. |
| `kStirResist[]` | `0.35 / 0.60 / 0.52` | How much the add shrinks toward full. Higher = harder top (but `1/(1-R)` makes the very top unreachable if too high). |
| `kStirDecay[]` | `0.15 / 0.26 / 0.72` | Bar drained per second, **always**. Higher = punishes hesitation. |

Felt effort to advance at position `p` is `decay / (gain·(1−R·p))` — a hyperbola
(gentle then steep near the top). To make a level uniformly N× harder, scale
gain down and/or decay up by ~N; raising `resist` only steepens the *end*.
| `STIR_ANGLE_STEP` | `0.18` | Swirl radians per encoder count (visual only). |

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
confirm): **Realm**, **Mute**, **Brightness** (1–5), **Stir Level**
(Easy/Medium/Hard), **Hardware Test**, **Firmware**, **Exit**. Mute, brightness, and realm persist to NVS. The menu is a
data table (`kMenu` in `main.cpp`) — add an item by appending one row
(`{label, kind, get, set, …}`); kinds are choice / range / info / action.

## Encoder direction

If the swirl orbits / pitch ramps *opposite* to how you turn, either swap the
**CLK/DT** wires, or negate the direction in firmware: flip the sign of `d` in
`updateStir()` (or swap `PIN_ENC_A`/`PIN_ENC_B`).

## Combo latching (reliability)

Once stirring starts, the combo is latched through the reveal — removing magnets
is ignored; a new brew needs a bottle *added*, or the base *cleared then
refilled*. This lives in `onSensedComboChange()` in `src/main.cpp`.
