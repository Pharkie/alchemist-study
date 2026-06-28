// ============================================================
// Alchemist Study — HARDWARE CHECK (standalone diagnostic)
//
// A separate program from the real firmware. Build & flash with:
//   pio run -e c3-hwcheck -t upload
//   pio device monitor -b 115200
//
// On boot it beeps the buzzer and scans for the OLED. Then it shows a LIVE
// status screen on the OLED — each reed/button as an ON/off box plus the
// encoder count — and logs every edge to serial so you can verify magnet
// make/break:
//   Reed 1 (GPIO3) ON
//   Reed 1 (GPIO3) off
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
static bool s_oledOK = false;

// ---- encoder decode (same scheme as the firmware) ----------------------
volatile int32_t g_encCount = 0;
static volatile uint8_t s_encPrev = 0;
static const int8_t kQuad[16] = { 0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0 };
static void IRAM_ATTR encISR() {
  uint8_t c = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  g_encCount += kQuad[(s_encPrev << 2) | c];
  s_encPrev = c;
}

// ---- monitored inputs (ON == pin pulled LOW) ---------------------------
struct In { int pin; const char* name; const char* shortName; int last; };
static In inputs[] = {
  {PIN_REED_SLOT1, "Reed 1", "R1", HIGH},
  {PIN_REED_SLOT2, "Reed 2", "R2", HIGH},
  {PIN_REED_SLOT3, "Reed 3", "R3", HIGH},
  {PIN_ENC_SW,     "Button", "SW", HIGH},
};
static const int NIN = sizeof(inputs) / sizeof(inputs[0]);

static void beep(uint16_t f, uint16_t ms) { tone(PIN_BUZZER, f); delay(ms); noTone(PIN_BUZZER); }

static void checkBuzzer() {
  Serial.println("\n[1/3] BUZZER (GPIO1): 3 rising beeps...");
  beep(523, 160); delay(70); beep(784, 160); delay(70); beep(1047, 220);
  Serial.println("      (silent? needs a PASSIVE buzzer + wiring on GPIO1/GND)");
}

static void checkOLED() {
  Serial.println("\n[2/3] OLED (I2C, SDA=GPIO5 SCL=GPIO6):");
  Wire.setPins(PIN_OLED_SDA, PIN_OLED_SCL);
  Wire.begin();
  uint8_t addr = 0;
  for (uint8_t a : {0x3C, 0x3D}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { addr = a; break; }
  }
  if (!addr) {
    Serial.println("      FAIL: no display on GPIO5/6. Live status will be serial-only.");
    Serial.println("      Check VCC=3.3V at the panel, GND common, SDA=GPIO5, SCL=GPIO6.");
    return;
  }
  Serial.printf("      OK: found 0x%02X.\n", addr);
  oled.setI2CAddress(addr << 1);
  oled.setBusClock(400000);
  oled.begin();
  s_oledOK = true;
}

// Live status screen: each input as an ON/off box, plus the encoder count.
static void renderStatus() {
  if (!s_oledOK) return;
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tr);

  oled.drawStr(2, 9, "HW CHECK");
  char e[16];
  snprintf(e, sizeof(e), "enc:%ld", (long)g_encCount);
  oled.drawStr(128 - oled.getStrWidth(e) - 2, 9, e);
  oled.drawHLine(0, 12, 128);

  int y = 24;
  for (int i = 0; i < NIN; i++) {
    bool on = (digitalRead(inputs[i].pin) == LOW);
    char lbl[20];
    snprintf(lbl, sizeof(lbl), "%s GPIO%d", inputs[i].shortName, inputs[i].pin);
    oled.drawStr(2, y, lbl);
    if (on) oled.drawBox(82, y - 8, 9, 9);
    else    oled.drawFrame(82, y - 8, 9, 9);
    oled.drawStr(96, y, on ? "ON" : "off");
    y += 11;
  }
  oled.sendBuffer();
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

  for (int i = 0; i < NIN; i++) {
    pinMode(inputs[i].pin, INPUT_PULLUP);
    inputs[i].last = digitalRead(inputs[i].pin);
  }
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  s_encPrev = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encISR, CHANGE);

  Serial.println("\n[3/3] LIVE — move a magnet to each reed, press/turn the knob.");
  Serial.println("      ON = magnet present (reed closed). Watch the OLED too.\n");
}

void loop() {
  // Log both edges of every monitored input.
  for (int i = 0; i < NIN; i++) {
    int v = digitalRead(inputs[i].pin);
    if (v != inputs[i].last) {
      inputs[i].last = v;
      bool on = (v == LOW);
      Serial.printf("%s (GPIO%d) %s\n", inputs[i].name, inputs[i].pin, on ? "ON" : "off");
      if (on) beep(900, 30);
    }
  }

  // Encoder activity + direction.
  static int32_t lastEnc = 0;
  static uint32_t lastRep = 0;
  int32_t c = g_encCount;
  if (c != lastEnc && millis() - lastRep > 120) {
    Serial.printf("Encoder (GPIO0/7) count=%ld  (%s)\n", (long)c, (c > lastEnc) ? "CW +" : "CCW -");
    lastEnc = c;
    lastRep = millis();
  }

  // Refresh the OLED status ~20 fps.
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 50) {
    lastDraw = millis();
    renderStatus();
  }

  delay(2);
}
