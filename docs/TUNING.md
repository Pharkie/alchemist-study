# Tuning the feel

All the knobs that shape how the brewing *feels* are `constexpr` values grouped
under `// ---- Tunables` near the top of `src/main.cpp`. Edit, then reflash:

```sh
pio run -e c3-dev -t upload
```

## Stir model (difficulty curve)

The power bar fills as you stir, with **diminishing returns toward the right**:
`rate = kStirSpeed[level] * (1 - kStirResist[level] * progress)`. The **Stir
Level** setting (Easy/Medium/Hard) picks the curve. Pause and the bar **drains
gradually** (not a reset); resume and it continues. Empty + idle a while → back
to identify. A full bar arms ("Press to brew") and holds until a press or combo
change.

| Constant | Default | Effect |
|---|---|---|
| `kStirSpeed[]` | `0.55 / 0.48 / 0.40` | Fill rate (progress/sec) at empty, per Easy/Medium/Hard. |
| `kStirResist[]` | `0.40 / 0.74 / 0.92` | How steeply the fill slows toward full. Higher = harder right side. |
| `kStirDecay[]` | `0.20 / 0.24 / 0.28` | Drain/sec while paused, per level (+20%/level). |
| `STIR_ACTIVE_MS` | `150` | Motion within this window counts as "still stirring". |
| `STIR_IDLE_BACK_MS` | `2500` | Empty + idle this long → return to identify. |
| `STIR_ANGLE_STEP` | `0.18` | Radians the swirl mote moves per encoder count (visual only). |

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
| `LONG_PRESS_MS` | `600` | Hold duration that toggles the realm. |
| `BTN_DEBOUNCE_MS` | `30` | Minimum press to count as a short press. |
| `RENDER_INTERVAL_MS` | `33` | Display refresh cap (~30 fps). ~30 ms is the I²C floor; lower won't help. |
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
