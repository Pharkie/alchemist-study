# Tuning the feel

All the knobs that shape how the brewing *feels* are `constexpr` values grouped
under `// ---- Tunables` near the top of `src/main.cpp`. Edit, then reflash:

```sh
pio run -e c3-dev -t upload
```

## Stir model

| Constant | Default | Effect |
|---|---|---|
| `STIR_GAIN` | `0.045` | Progress added per encoder count. **Higher = fewer turns to arm** "Press to brew". |
| `STIR_DECAY_PER_MS` | `0.0008` | Progress bled off per ms when idle. **Higher = fades faster** when you stop stirring. |
| `STIR_READY_LEVEL` | `0.5` | Progress (0–1) needed before a press will reveal. **Higher = must stir more.** |
| `STIR_ANGLE_STEP` | `0.18` | Radians the swirl mote moves per encoder count (visual speed only). |

If arming feels too hard/easy, change `STIR_GAIN` first. If it "cools" too
quickly while you reach for the button, lower `STIR_DECAY_PER_MS`.

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

## Encoder direction

If the swirl orbits / pitch ramps *opposite* to how you turn, either swap the
**CLK/DT** wires, or negate the direction in firmware: flip the sign of `d` in
`updateStir()` (or swap `PIN_ENC_A`/`PIN_ENC_B`).

## Combo latching (reliability)

Once stirring starts, the combo is latched through the reveal — removing magnets
is ignored; a new brew needs a bottle *added*, or the base *cleared then
refilled*. This lives in `onSensedComboChange()` in `src/main.cpp`.
