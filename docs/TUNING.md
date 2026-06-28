# Tuning the feel

All the knobs that shape how the brewing *feels* are `constexpr` values grouped
under `// ---- Tunables` near the top of `src/main.cpp`. Edit, then reflash:

```sh
pio run -e c3-dev -t upload
```

## Stir model (time-based)

The power bar fills over **Stir Time** seconds of *active* stirring (Stir Time is
a runtime setting: 1/3/5/8 s — `kStirSecs` in `main.cpp`). Pause longer than
`STIR_IDLE_RESET_MS` and the bar resets to zero (you stay on the brewing screen);
after `STIR_IDLE_BACK_MS` it drifts back to identify. When the bar fills it arms
("Press to brew") and holds until a press or a real combo change.

| Constant | Default | Effect |
|---|---|---|
| `STIR_IDLE_RESET_MS` | `500` | Pause longer than this → the bar empties to zero. |
| `STIR_IDLE_BACK_MS` | `2500` | Idle longer than this (bar empty) → return to identify. |
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
idle screen): **Realm**, **Mute**, **Brightness** (1–5), **Stir Time** (1/3/5/8 s),
**Hardware Test**, **Firmware**, **Exit**. Mute, brightness, and realm persist to NVS. The menu is a
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
