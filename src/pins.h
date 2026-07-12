// ============================================================
// Alchemist Study — shared pin map (ESP32-C3 Super Mini, native USB)
//
// Single source of truth for every program in this repo: the firmware
// (main.cpp) and the standalone diagnostic (hwcheck.cpp) both include
// this header, so the wiring can never drift between them.
//
// Choices avoid the C3 strapping pins (GPIO2/8/9) and the USB pins
// (GPIO18/19). GPIO20/21 are UART0 RX/TX by default but free here because
// the console runs over USB-CDC. Power/grounding: see docs/CIRCUIT.md.
// ============================================================
#pragma once

// Reed switches: INPUT_PULLUP; a magnet in the bottle base pulls the line
// LOW when seated. Combo bits: bit0=slot1, bit1=slot2, bit2=slot3.
static constexpr int PIN_REED_SLOT1 = 1;   // bit0
static constexpr int PIN_REED_SLOT2 = 3;   // bit1
static constexpr int PIN_REED_SLOT3 = 4;   // bit2

// OLED (SSD1306 128x64, I2C, white).
static constexpr int PIN_OLED_SDA = 5;
static constexpr int PIN_OLED_SCL = 6;

// Passive buzzer, driven with tone()/noTone() (needs arduino-esp32 core 3.x).
static constexpr int PIN_BUZZER = 7;

// Rotary encoder with push. SW on GPIO21 (UART0-TX by default): fine as
// INPUT_PULLUP since the console is USB-CDC, but the ROM briefly drives it
// as TX during boot — avoid holding the button through a reset.
static constexpr int PIN_ENC_A  = 10;  // CLK
static constexpr int PIN_ENC_B  = 20;  // DT
static constexpr int PIN_ENC_SW = 21;  // INPUT_PULLUP
