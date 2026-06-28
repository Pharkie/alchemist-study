// ============================================================
// Alchemist Study — HARDWARE CHECK (standalone diagnostic)
//
// A separate program from the real firmware. Build & flash with:
//   pio run -e c3-hwcheck -t upload
//   pio device monitor -b 115200
//
// It walks every component and reports clearly, making NO assumptions:
//   1. Buzzer    — plays 3 beeps (you confirm you heard them).
//   2. OLED      — scans I2C on SDA=5/SCL=6; if found, draws a test pattern.
//   3. Inputs    — watches ALL header pins; when you trigger a reed / press
//                  the button, it names the GPIO that reacted. If the GPIO
//                  that lights up is NOT the expected one, your silkscreen
//                  labels don't match the silicon (or the breakout shifted
//                  the mapping) — which it will say out loud.
//   4. Encoder   — reports counts + direction as you turn the knob.
//
// Pin map below MUST mirror main.cpp.
// ============================================================

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

static constexpr int PIN_REED_SLOT1 = 3;
static constexpr int PIN_REED_SLOT2 = 4;
static constexpr int PIN_REED_SLOT3 = 10;
static constexpr int PIN_OLED_SDA   = 5;
static constexpr int PIN_OLED_SCL   = 6;
static constexpr int PIN_BUZZER     = 1;
static constexpr int PIN_ENC_A      = 0;
static constexpr int PIN_ENC_B      = 7;
static constexpr int PIN_ENC_SW     = 20;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, /*reset=*/U8X8_PIN_NONE);

// ---- encoder decode (same scheme as the firmware) ----------------------
volatile int32_t g_encCount = 0;
static volatile uint8_t s_encPrev = 0;
static const int8_t kQuad[16] = { 0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0 };
static void IRAM_ATTR encISR() {
  uint8_t c = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  g_encCount += kQuad[(s_encPrev << 2) | c];
  s_encPrev = c;
}

// All header GPIOs worth monitoring as inputs (exclude buzzer 1 and I2C 5/6).
static const int MON[] = {0, 2, 3, 4, 7, 8, 9, 10, 20, 21};
static const int NMON = sizeof(MON) / sizeof(MON[0]);
static int s_last[NMON];

static const char* expectedName(int pin) {
  switch (pin) {
    case PIN_REED_SLOT1: return "Reed 1 / slot1";
    case PIN_REED_SLOT2: return "Reed 2 / slot2";
    case PIN_REED_SLOT3: return "Reed 3 / slot3";
    case PIN_ENC_SW:     return "Encoder button";
    case PIN_ENC_A:      return "Encoder A (turn)";
    case PIN_ENC_B:      return "Encoder B (turn)";
    default:             return nullptr;  // unexpected pin
  }
}

static void beep(uint16_t f, uint16_t ms) { tone(PIN_BUZZER, f); delay(ms); noTone(PIN_BUZZER); }

static void checkBuzzer() {
  Serial.println("\n[1/4] BUZZER (GPIO1): playing 3 rising beeps...");
  beep(523, 160); delay(70); beep(784, 160); delay(70); beep(1047, 220);
  Serial.println("      -> Heard 3 beeps? If silent: check +=GPIO1, -=GND, and that");
  Serial.println("         it's a PASSIVE buzzer (an active one only drones).");
}

static void checkOLED() {
  Serial.println("\n[2/4] OLED (I2C, SDA=GPIO5 SCL=GPIO6):");
  Wire.setPins(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.begin();
  uint8_t addr = 0;
  for (uint8_t a : {0x3C, 0x3D}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { addr = a; break; }
  }
  if (!addr) {
    Serial.println("      FAIL: no display ACK at 0x3C/0x3D on GPIO5/6.");
    Serial.println("      Check, in order:");
    Serial.println("        - VCC at the panel measures 3.3V (this is the #1 cause)");
    Serial.println("        - GND actually common with the C3");
    Serial.println("        - SDA lands on GPIO5, SCL on GPIO6");
    Serial.println("        - if on a breakout, confirm those holes ARE GPIO5/6/3V3/GND");
    return;
  }
  Serial.printf("      Found 0x%02X. Drawing test pattern...\n", addr);
  oled.setI2CAddress(addr << 1);
  oled.setBusClock(400000);
  oled.begin();
  oled.clearBuffer();
  oled.drawFrame(0, 0, 128, 64);
  oled.setFont(u8g2_font_7x13B_tr);
  oled.drawStr(20, 26, "HW CHECK");
  oled.drawStr(28, 46, "OLED OK");
  oled.sendBuffer();
  Serial.println("      PASS: screen should show a bordered 'HW CHECK / OLED OK'.");
}

void setup() {
  Serial.begin(115200);
  delay(600);
  Serial.println("\n============================================");
  Serial.println(" ALCHEMIST STUDY — HARDWARE CHECK");
  Serial.println("============================================");

  pinMode(PIN_BUZZER, OUTPUT);
  checkBuzzer();
  checkOLED();

  // Inputs + encoder.
  for (int i = 0; i < NMON; i++) pinMode(MON[i], INPUT_PULLUP);
  s_encPrev = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);
  for (int i = 0; i < NMON; i++) s_last[i] = digitalRead(MON[i]);

  Serial.println("\n[3/4] REEDS + BUTTON — trigger each ONE AT A TIME:");
  Serial.println("      hold a magnet to reed 1, then 2, then 3; press the knob.");
  Serial.println("      I'll name the GPIO that reacts. Expected: reeds=3/4/10, button=20.");
  Serial.println("[4/4] ENCODER — turn the knob both ways (expect counts on GPIO0/7).");
  Serial.println("\nwatching...\n");
}

void loop() {
  // Report any monitored pin that goes LOW (switch closed / pressed).
  for (int i = 0; i < NMON; i++) {
    int v = digitalRead(MON[i]);
    if (v != s_last[i]) {
      s_last[i] = v;
      if (v == LOW) {
        const char* name = expectedName(MON[i]);
        if (name) {
          Serial.printf("  [OK]  GPIO%d LOW  ->  %s\n", MON[i], name);
        } else {
          Serial.printf("  [??]  GPIO%d LOW  ->  UNEXPECTED pin! Something is wired here\n", MON[i]);
          Serial.println("        that the firmware doesn't use — silkscreen/breakout mismatch?");
        }
        beep(900, 35);
      }
    }
  }

  // Encoder activity + direction.
  static int32_t lastEnc = 0;
  static uint32_t lastRep = 0;
  int32_t c = g_encCount;
  if (c != lastEnc && millis() - lastRep > 120) {
    Serial.printf("  [ENC] count=%ld  (%s)\n", (long)c, (c > lastEnc) ? "CW +" : "CCW -");
    lastEnc = c;
    lastRep = millis();
  }

  delay(5);
}
