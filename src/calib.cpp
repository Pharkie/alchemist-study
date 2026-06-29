// ============================================================
// Stir calibration — standalone. Build/flash:
//   pio run -e c3-calib -t upload
//   pio device monitor -b 115200
//
// Shows live encoder motion (|counts|/sec) and the peak, big on the OLED.
// Spin the knob exactly how you'd stir (left<->right is fine — it counts the
// motion, not net rotation). Read the PEAK; that's what difficulty is tuned to.
// Press the knob to reset the peak.
// ============================================================

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>

static constexpr int PIN_OLED_SDA = 5, PIN_OLED_SCL = 6;
static constexpr int PIN_ENC_A = 0, PIN_ENC_B = 7, PIN_ENC_SW = 20;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

volatile int32_t g_enc = 0;
static volatile uint8_t s_prev = 0;
static const int8_t QT[16] = { 0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0 };
static void IRAM_ATTR isr() {
  uint8_t c = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  g_enc += QT[(s_prev << 2) | c];
  s_prev = c;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== STIR CALIBRATION (spin both ways; read peak counts/s) ===");
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);
  s_prev = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), isr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), isr, CHANGE);
  Wire.setPins(PIN_OLED_SDA, PIN_OLED_SCL);
  oled.setBusClock(400000);
  oled.begin();
}

void loop() {
  static int32_t last = 0;
  static uint32_t win = 0;
  static int32_t acc = 0;
  static int cps = 0, peak = 0;
  uint32_t now = millis();

  int32_t c = g_enc;
  int32_t d = c - last;
  last = c;
  acc += (d < 0) ? -d : d;

  if (now - win >= 200) {
    cps = (int)(acc * 1000 / (now - win));
    if (cps > peak) peak = cps;
    acc = 0;
    win = now;
    Serial.printf("cps=%d  peak=%d\n", cps, peak);
  }

  static bool pb = false;
  bool pr = (digitalRead(PIN_ENC_SW) == LOW);
  if (pr && !pb) peak = 0;       // press resets the peak
  pb = pr;

  static uint32_t draw = 0;
  if (now - draw >= 70) {
    draw = now;
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x10_tr);
    oled.drawStr(2, 9, "STIR CALIB - spin hard");
    oled.setFont(u8g2_font_logisoso24_tn);
    char b[12];
    snprintf(b, sizeof(b), "%d", cps);
    oled.drawStr((128 - oled.getStrWidth(b)) / 2, 44, b);
    oled.setFont(u8g2_font_5x8_tr);
    char m[24];
    snprintf(m, sizeof(m), "now c/s   peak %d", peak);
    oled.drawStr(2, 54, m);
    oled.drawStr(2, 63, "press = reset peak");
    oled.sendBuffer();
  }
}
