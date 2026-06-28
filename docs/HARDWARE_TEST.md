# Hardware test (`hwcheck`)

A standalone diagnostic that verifies every component independently of the game
firmware. Keep it handy — reach for it whenever a peripheral misbehaves, you
rewire something, or you assemble a new unit.

It is a separate program (`src/hwcheck.cpp`) built by its own PlatformIO
environment (`c3-hwcheck`), so flashing it never disturbs the real firmware.

## Run it

```sh
pio run -e c3-hwcheck -t upload     # flash the checker
pio device monitor -b 115200        # watch the serial log
```

Return to the game afterwards with:

```sh
pio run -e c3-dev -t upload         # real firmware (verbose)
```

## What it does

On boot it runs three quick checks, then a live monitor:

1. **Buzzer** — plays three rising beeps on GPIO1.
2. **OLED** — scans I²C (SDA=GPIO5/SCL=GPIO6) at 0x3C/0x3D; if found, takes over
   the panel for the live status screen.
3. **Inputs (live)** — continuously shows and logs the reeds, button and encoder.

### On the OLED

```
HW CHECK              enc:NN     <- live encoder count (top-right)
----------------------------------
R1 GPIO3      [ ] off            <- filled box = magnet present (reed closed)
R2 GPIO4      [#] ON
R3 GPIO10     [ ] off
SW GPIO20     PRESS! #3          <- a button tap inverts this row for 2s
```

- A box **fills** the instant a magnet closes its reed — move the magnet toward
  and away to find each reed's make/break distance.
- The **encoder count** tracks the knob live (redraws ~20 ms while active).
- A **button tap latches** an inverted `PRESS! #N` banner for 2 seconds (with a
  running count) so a quick press can't be missed.

### On serial

Both edges of every input are logged, so you can verify make *and* break:

```
Reed 1 (GPIO3) ON
Reed 1 (GPIO3) off
Button (GPIO20) ON
Encoder (GPIO0/7) count=12  (CW +)
[time] sendBuffer = 30 ms          <- I2C render time (≈30 ms is healthy)
```

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `OLED ... FAIL / no display` | Power first: measure **3.3 V at the panel's VCC/GND**. Then SDA→GPIO5, SCL→GPIO6. On a breakout, confirm those holes really are those GPIOs. |
| OLED works then drops out | Marginal wire (breadboard flex). Reseat/secure SDA/SCL/VCC/GND; solder for permanence. A flaky bus also causes multi-second lag (each dropped I²C transaction blocks the loop). |
| Buzzer silent during the 3 beeps | You likely have an **active** buzzer (drones on DC, ignores `tone()`). This project needs a **passive** buzzer for pitched audio. See it sound on plain 3.3 V = active. |
| A reed never shows ON | That line isn't connected, or the magnet is too weak/far. Check the reed leg → its GPIO and the other leg → GND. |
| The *wrong* GPIO reacts | Silkscreen/breakout mismatch — the wire is on a different GPIO than labelled. |
| Encoder count doesn't move | Check CLK→GPIO0, DT→GPIO7, + → 3V3, GND→GND. |
| `[time] sendBuffer` spikes to hundreds of ms | Flaky I²C (see OLED drop-out above). Healthy is ~30 ms. |

## Confirmed-good pin map

| Component | GPIO |
|---|---|
| Reed 1 / slot 1 | 3 |
| Reed 2 / slot 2 | 4 |
| Reed 3 / slot 3 | 10 |
| OLED SDA / SCL | 5 / 6 |
| Buzzer | 1 |
| Encoder A / B / SW | 0 / 7 / 20 |
