# Assembly Guide

From a bag of parts to a brewing study. No soldering skill beyond basic
through-hole joints is needed; every module is a common hobbyist board.

**You'll need:** the parts from the [bill of materials](../README.md#what-you-need),
a soldering iron (or patience with Dupont jumpers for a first prototype),
super glue or hot glue, and side cutters.

## 1. Print the model

*Print files: coming soon — see the backlog.* The study prints in several
pieces: the room shell, the base with three bottle homes, and the three
bottles with hollow bases that each take a magnet.

Suggested settings: 0.2 mm layers, 2 walls, 15% infill, no supports needed
for the main shell. *(To be confirmed with the final files.)*

## 2. Magnets into bottles

Glue one neodymium disc magnet (~6×3 mm) into the recess under each bottle.
Orientation doesn't matter — reed switches respond to either pole — but keep
the magnet as close to the bottle's base surface as possible: the reed
switches sit a few millimetres below and the make/break distance is what
gives seating its satisfying snap.

## 3. Reed switches into the base

Push one reed switch into the slot under each bottle home. The glass body is
fragile — bend the legs with pliers, never at the glass, and don't force it.
Route the legs toward the electronics bay.

Reed switches have no polarity: one leg goes to its GPIO, the other to GND.

## 4. Wire the circuit

<!-- Fritzing diagram: docs/assets/wiring-fritzing.png
<p align="center"><img src="assets/wiring-fritzing.png" alt="Wiring diagram" width="80%"></p>
-->
*(Fritzing diagram coming soon — the table below is the source of truth and
is verified against the firmware's pin map by the build.)*

All connections go to the ESP32-C3 Super Mini. Logic is 3.3 V throughout —
power the OLED and encoder from the board's **3V3** pin, never 5 V.

| Connection | ESP32-C3 pin |
|---|---|
| Reed 1 / slot 1 (other leg → GND) | 1 |
| Reed 2 / slot 2 (other leg → GND) | 3 |
| Reed 3 / slot 3 (other leg → GND) | 4 |
| OLED SDA | 5 |
| OLED SCL | 6 |
| OLED VCC / GND | 3V3 / GND |
| Buzzer + (− → GND) | 7 |
| Encoder CLK | 10 |
| Encoder DT | 20 |
| Encoder SW | 21 |
| Encoder + / GND | 3V3 / GND |

Notes:

- The buzzer must be **passive** (two bare legs, no drive circuit). An active
  buzzer drones one note and can't play the trills and jingles.
- Deeper power/grounding details (and why GPIO 21 briefly chirps at boot):
  [CIRCUIT.md](CIRCUIT.md).

## 5. Mount the electronics

Screw or hot-glue the OLED behind its window, the encoder through its panel
hole (nut on the outside, knob last), and the C3 board where its USB-C port
meets the case opening. Keep the buzzer's sound holes unobstructed.
*(Exact screw sizes: to be confirmed with the final model files.)*

## 6. Flash the firmware

Install [PlatformIO](https://platformio.org/install) (Core ≥ 6.1.19), clone
this repo, connect the board over USB-C, then:

```sh
pio run -e c3-prod -t upload
```

## 7. First boot

You should get a rising chime and an animated welcome, then the home
carousel. Test the full chain:

1. Seat each bottle — its ingredient should appear (and stack with others)
2. Turn the knob — the stir starts; a trill rises with the bar
3. Press when full — your first potion is revealed

If a bottle isn't sensed or the screen stays dark, flash the wiring doctor —
`pio run -e c3-hwcheck -t upload` — and follow
[HARDWARE_TEST.md](HARDWARE_TEST.md): it beeps the buzzer, scans for the
OLED, and shows every input live so you can find the loose wire in seconds.
