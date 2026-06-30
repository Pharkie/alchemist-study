# Circuit & power

Power, grounding and wiring for the bench build. The GPIO **pin map** lives in
[CLAUDE.md](../CLAUDE.md#hardware--pin-map-esp32-c3-super-mini-native-usb); this
doc covers how it's powered and the high-power (LED coil) domain.

```mermaid
flowchart TB
  %% ===== Power input - off-board =====
  subgraph PWRIN["Power input (off-board)"]
    SRC5["5V supply"]
    SRC24["Coil/LED board supply<br/>(own USB-C, 24V to coil)"]
    SW["Rocker - DPDT MASTER KILL<br/>breaks BOTH feeds at once"]
  end

  %% ===== ElectroCookie rails - logic domain =====
  R5V(["5V rail"])
  R3V3(["3V3 rail"])
  RGND(["GND rail (logic)"])
  C3["ESP32-C3"]

  %% ===== LED coil - separate power domain =====
  COIL["LED coil board<br/>wireless power to LEDs<br/>REAR, vertical"]
  NOTE["Coil ground stays SEPARATE from logic GND<br/>(no control/sense line crosses to the C3)"]

  %% ----- Master switch cuts both supplies -----
  SRC5 -->|"+5V"| SW
  SRC24 -->|"coil supply"| SW
  SW -->|"switched +5V"| R5V
  SW -->|"switched coil supply"| COIL
  COIL -.- NOTE

  %% ----- Logic power tree -----
  R5V -->|"5V pin"| C3
  C3 -->|"3V3 out pin"| R3V3
  C3 ---|"GND pin"| RGND
  R3V3 -->|"VCC"| OLED["OLED SSD1306 128x64"]
  R3V3 -->|"+"| ENC["Rotary encoder KY-040"]

  %% ----- Signals from the C3 -----
  C3 -->|"GPIO5 SDA"| OLED
  C3 -->|"GPIO6 SCL"| OLED
  C3 -->|"GPIO1 tone"| BUZ["Buzzer (passive)"]
  C3 -->|"GPIO0 CLK"| ENC
  C3 -->|"GPIO7 DT"| ENC
  C3 -->|"GPIO20 SW"| ENC
  C3 -->|"GPIO3"| RD1["Reed 1 - front, horizontal"]
  C3 -->|"GPIO4"| RD2["Reed 2 - front, horizontal"]
  C3 -->|"GPIO10"| RD3["Reed 3 - front, horizontal"]

  %% ----- Logic ground returns -----
  OLED ---|"GND"| RGND
  ENC ---|"GND"| RGND
  BUZ ---|"-"| RGND
  RD1 ---|"leg B"| RGND
  RD2 ---|"leg B"| RGND
  RD3 ---|"leg B"| RGND

  classDef rail fill:#1f3a5f,stroke:#5b8fd6,color:#ffffff,stroke-width:2px;
  classDef power fill:#5a2a2a,stroke:#c77,color:#ffffff;
  classDef coil fill:#3a2f1f,stroke:#c9a05b,color:#ffffff;
  classDef note fill:#222,stroke:#888,color:#ddd,stroke-dasharray:3 3;
  class R5V,R3V3,RGND rail;
  class SRC5,SRC24,SW power;
  class COIL coil;
  class NOTE note;
```

## Power & the master switch

- **One DPDT rocker = master kill.** Pole 1 breaks the +5V logic feed; pole 2
  breaks the coil/LED board's supply — so OFF cuts *everything* and never leaves
  the coil warm. Switch the coil board on its **DC input** (or the supply's
  mains), not inside a USB-C cable. If the coil current is more than a rocker
  should carry, let the rocker drive a **relay/SSR** on the 24V side and keep the
  switch itself low-current.
- **Don't double-feed 5V.** For dev, power the C3 from **USB-C only**; in
  deployment, from the switched **5V pin** — not both at once unless the Super
  Mini has OR-ing diodes (most don't), or you'll back-feed two 5V sources.

## Grounding

- Logic peripherals (OLED, encoder, buzzer, reeds) return to **one logic GND
  rail**; the C3's GND pin ties that rail to its regulator ground.
- The **coil/LED board is its own power domain** with its own supply, and **no
  signal crosses** to the C3 — so its ground is kept **separate** (not bonded to
  logic GND). That keeps coil/LED switching noise out of the I²C and the reeds.
  *If* you ever add a C3→coil control or sense line, bond the two grounds at
  exactly one point (a star) at that time.

## LED coil vs. reed switches

- Reed switches are **magnetic** sensors, so an energized coil is the one real
  risk to the core sensing. Mitigated here by geometry: coil is **vertical at the
  rear**, reeds are **horizontal at the front** — separated, with the field axis
  roughly perpendicular to the reed axes.
- **Bench-check:** energize the coil and bring it toward the reeds while watching
  **Settings → Hardware Test** — confirm no false trips.
- Confirm the coil board has a **flyback diode / snubber** across the coil. Run
  the coil leads as a **twisted pair**, routed away from the I²C and reed wiring.

## Decoupling

- With the coil nearby, add bulk + HF caps on both rails (≈**100 µF + 100 nF** on
  5V and on 3V3, near the C3). A small cap across a reed line helps if one proves
  noisy.

## Logic peripherals (3V3 domain)

- OLED + encoder run from the C3's **3V3-out** pin (small load — fine for the
  onboard LDO). Everything logic-side is 3V3, so there's **no level shifting**.
  The KY-040's pull-ups go to 3V3 (not 5V), keeping the GPIOs safe.
- Pin choices avoid the C3 **strapping pins** (GPIO2/8/9) and the **USB** pins
  (GPIO18/19). Note GPIO20 is UART0-RX by default but free here because the
  console runs over **USB-CDC**.
