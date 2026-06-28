// ============================================================
// Alchemist Study — ESP32-C3 Super Mini firmware
//
// Brain of a 3D-printed alchemist's study: reed switches sense which
// potion bottles are seated, an OLED names the resulting potion, a
// rotary encoder is the "stir" control, and a passive buzzer scores it.
//
// State machine: IDLE -> IDENTIFY -> STIRRING -> REVEAL.
//   IDLE      empty base; "Place ingredients" + realm-switch hint.
//   IDENTIFY  >=1 bottle seated; lists ingredient name(s), "turn to stir".
//   STIRRING  encoder turning; rising trill + swirl animation; decays back.
//   REVEAL    short press (after stirring) names the potion + jingle.
//
// PLATFORM: arduino-esp32 3.x via pioarduino. Core 3.x is required for
//   tone()/noTone() (LEDC-backed) to behave as used here. See platformio.ini.
//
// Bench firmware is intentionally WiFi-free; connectivity (OTA, status
// page, Home Assistant) is a deliberate later stage — see BACKLOG.md.
// ============================================================

#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>

// ---- Pin map (ESP32-C3 Super Mini, native USB) -------------------------
// Reed switches: INPUT_PULLUP; a magnet in the bottle base pulls the line
// LOW when seated. Combo bits: bit0=slot1, bit1=slot2, bit2=slot3.
static constexpr int PIN_REED_SLOT1 = 3;   // bit0
static constexpr int PIN_REED_SLOT2 = 4;   // bit1
static constexpr int PIN_REED_SLOT3 = 10;  // bit2

// OLED (SSD1306 128x64, I2C, white): SDA=5, SCL=6.
static constexpr int PIN_OLED_SDA = 5;
static constexpr int PIN_OLED_SCL = 6;

// Passive buzzer, driven with tone()/noTone() (needs core 3.x).
static constexpr int PIN_BUZZER = 1;

// Rotary encoder with push. GPIO0 is safe here: on the C3 it is NOT a
// strapping pin (unlike the classic ESP32), so a detent resting in
// either state at boot is harmless.
static constexpr int PIN_ENC_A  = 0;
static constexpr int PIN_ENC_B  = 7;
static constexpr int PIN_ENC_SW = 20;  // INPUT_PULLUP

// ---- Encoder decode (GPIO interrupts, no PCNT) -------------------------
// The ESP32-C3 lacks the PCNT pulse-counter peripheral that ESP32Encoder
// needs, so we decode quadrature in software. Both channels fire on CHANGE;
// a transition lookup table turns each (prev,curr) 2-bit state into -1/0/+1.
// This is the equivalent of attachHalfQuad with internal pullups.
volatile int32_t g_encoderCount = 0;
static volatile uint8_t s_encPrev = 0;

// Index = (prev<<2)|curr. Valid single-step transitions map to +/-1;
// invalid / no-change entries are 0 (debounces contact bounce for free).
static const int8_t kQuadTable[16] = {
  0, -1, +1, 0, +1, 0, 0, -1, -1, 0, 0, +1, 0, +1, -1, 0
};

static void IRAM_ATTR encoderISR() {
  uint8_t curr = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  g_encoderCount += kQuadTable[(s_encPrev << 2) | curr];
  s_encPrev = curr;
}

// ---- Potion data -------------------------------------------------------
// Two universes. Ingredient slots map to combo bits: slot1=bit0, slot2=bit1,
// slot3=bit2. Potion tables are indexed by the combo value 1..7 ([0] unused).
enum Universe { UNI_SKYRIM = 0, UNI_BG3 = 1, UNI_COUNT = 2 };

static const char* const kUniverseName[UNI_COUNT] = {
  "Skyrim", "Baldur's Gate 3"
};

static const char* const kIngredients[UNI_COUNT][3] = {
  { "Blue Mountain Flower", "Deathbell", "Nightshade" },
  { "Salts of Mugwort", "Bullywug Crayfish Tail", "Rogue's Morsel" }
};

static const char* const kPotions[UNI_COUNT][8] = {
  // Skyrim
  { "",
    "Potion of Minor Healing",   // 001 Flower
    "Damage Health Poison",      // 010 Deathbell
    "Lingering Damage Poison",   // 011 Flower+Deathbell
    "Fortify Destruction",       // 100 Nightshade
    "Potion of Regeneration",    // 101 Flower+Nightshade
    "Deadly Paralysis Poison",   // 110 Deathbell+Nightshade
    "Philter of the Phantom" },  // 111 all
  // Baldur's Gate 3
  { "",
    "Potion of Healing",             // 001 Mugwort
    "Elixir of Vigilance",           // 010 Crayfish
    "Elixir of the Colossus",        // 011 Mugwort+Crayfish
    "Potion of Speed",               // 100 Morsel
    "Potion of Invisibility",        // 101 Mugwort+Morsel
    "Elixir of Heroism",             // 110 Crayfish+Morsel
    "Elixir of Universal Resistance" } // 111 all
};

// ---- Tunables (revisit on the bench in Phase 6) ------------------------
static constexpr uint32_t REED_DEBOUNCE_MS  = 40;
static constexpr uint32_t LONG_PRESS_MS     = 600;   // hold = toggle universe
static constexpr uint32_t BTN_DEBOUNCE_MS   = 30;
static constexpr uint32_t RENDER_INTERVAL_MS = 33;   // ~30 fps display cap
static constexpr float    STIR_GAIN         = 0.045f; // progress per |count|
static constexpr float    STIR_DECAY_PER_MS = 0.0008f; // progress lost per ms idle
static constexpr float    STIR_ANGLE_STEP   = 0.18f;  // swirl radians per count
static constexpr float    STIR_READY_LEVEL  = 0.5f;   // progress needed to brew
static constexpr uint16_t PITCH_MIN_HZ      = 320;
static constexpr uint16_t PITCH_MAX_HZ      = 1100;

// ---- Hardware objects --------------------------------------------------
// Full-buffer (_F_) SSD1306 over hardware I2C.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, /*reset=*/U8X8_PIN_NONE);
Preferences prefs;
static bool g_haveDisplay = false;  // set at boot; if false we run headless

// ---- Runtime state -----------------------------------------------------
enum State { ST_IDLE, ST_IDENTIFY, ST_STIRRING, ST_REVEAL };
static State    g_state    = ST_IDLE;
static uint8_t  g_combo    = 0;            // committed combo (0..7)
static Universe g_universe = UNI_SKYRIM;

// reed debounce
static uint8_t  s_rawComboLast = 0;
static uint32_t s_comboChangeMs = 0;

// encoder / stir model
static int32_t s_lastEncoder = 0;
static float   s_stirProgress = 0.0f;      // 0..1, decays when idle
static float   s_swirlAngle   = 0.0f;      // radians, follows the encoder
static bool    s_stirReady    = false;     // latched once progress crosses level

// button
static bool     s_btnDown   = false;
static uint32_t s_btnDownMs = 0;
static bool     s_longFired = false;

// timing
static uint32_t s_lastLoopMs   = 0;
static uint32_t s_lastRenderMs = 0;

// ---- Audio: non-blocking melody player + stir trill --------------------
struct Note { uint16_t freq; uint16_t durMs; };  // freq 0 = rest
#define NUM_NOTES(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))

static const Note MEL_SUCCESS[]  = { {523, 90}, {659, 90}, {784, 90}, {1047, 150} };
static const Note MEL_TOGGLE[]   = { {700, 110}, {1100, 150} };   // distinct two-note
static const Note MEL_NOTREADY[] = { {200, 70}, {0, 50}, {200, 70} }; // low "nope"

static const Note* s_melody     = nullptr;
static uint8_t      s_melodyLen = 0;
static uint8_t      s_melodyIdx = 0;
static uint32_t     s_noteStartMs = 0;
static bool         s_melodyActive = false;
static bool         s_buzzerOn = false;

static void playCurrentNote() {
  uint16_t f = s_melody[s_melodyIdx].freq;
  if (f == 0) noTone(PIN_BUZZER);
  else        tone(PIN_BUZZER, f);
}

static void startMelody(const Note* m, uint8_t n, uint32_t now) {
  s_melody = m;
  s_melodyLen = n;
  s_melodyIdx = 0;
  s_melodyActive = true;
  s_noteStartMs = now;
  s_buzzerOn = false;
  playCurrentNote();
}

static void updateAudio(uint32_t now) {
  if (s_melodyActive) {
    if (now - s_noteStartMs >= s_melody[s_melodyIdx].durMs) {
      s_melodyIdx++;
      if (s_melodyIdx >= s_melodyLen) {
        s_melodyActive = false;
        noTone(PIN_BUZZER);
      } else {
        s_noteStartMs = now;
        playCurrentNote();
      }
    }
    return;  // a melody owns the buzzer while it plays
  }

  // Brewing trill: pitch rises with stir progress, decays with it. A small
  // alternating offset gives the "trill" warble.
  bool stirSound = (g_state == ST_STIRRING) && (s_stirProgress > 0.02f);
  if (stirSound) {
    int f = PITCH_MIN_HZ + (int)(s_stirProgress * (PITCH_MAX_HZ - PITCH_MIN_HZ));
    f += ((now / 35) & 1) ? 14 : -14;
    if (f < 50) f = 50;
    tone(PIN_BUZZER, (uint16_t)f);
    s_buzzerOn = true;
  } else if (s_buzzerOn) {
    noTone(PIN_BUZZER);
    s_buzzerOn = false;
  }
}

// ---- Persistence -------------------------------------------------------
static void saveUniverse() {
  prefs.putUChar("universe", (uint8_t)g_universe);
}

// ---- Input helpers -----------------------------------------------------
static uint8_t readRawCombo() {
  uint8_t c = 0;
  if (digitalRead(PIN_REED_SLOT1) == LOW) c |= 0x01;
  if (digitalRead(PIN_REED_SLOT2) == LOW) c |= 0x02;
  if (digitalRead(PIN_REED_SLOT3) == LOW) c |= 0x04;
  return c;
}

// A new combo was committed (debounced). Resets the brew and picks a state.
// This is also the ONLY thing that dismisses a REVEAL.
static void onComboChanged(uint8_t newCombo) {
  g_combo = newCombo;
  s_stirProgress = 0.0f;
  s_stirReady = false;
  g_state = (newCombo == 0) ? ST_IDLE : ST_IDENTIFY;
  // Print MSB-first (slot3 slot2 slot1) to match the spec's "100 Nightshade".
  Serial.printf("[combo] %u%u%u (val %u) -> %s\n",
                (newCombo >> 2) & 1, (newCombo >> 1) & 1, (newCombo >> 0) & 1,
                newCombo, newCombo ? kPotions[g_universe][newCombo] : "idle");
}

static void onShortPress(uint32_t now) {
  if (g_state == ST_REVEAL) return;        // only a combo change leaves reveal
  if (g_combo == 0) return;                // nothing to brew
  if (s_stirReady) {
    g_state = ST_REVEAL;
    startMelody(MEL_SUCCESS, NUM_NOTES(MEL_SUCCESS), now);
    Serial.printf("[reveal] %s\n", kPotions[g_universe][g_combo]);
  } else {
    // Stirring is required first — give a little "not yet" buzz.
    startMelody(MEL_NOTREADY, NUM_NOTES(MEL_NOTREADY), now);
  }
}

static void onLongPress(uint32_t now) {
  g_universe = (Universe)((g_universe + 1) % UNI_COUNT);
  saveUniverse();
  startMelody(MEL_TOGGLE, NUM_NOTES(MEL_TOGGLE), now);
  Serial.printf("[universe] -> %s\n", kUniverseName[g_universe]);
}

static void updateButton(uint32_t now) {
  bool pressed = (digitalRead(PIN_ENC_SW) == LOW);
  if (pressed && !s_btnDown) {
    s_btnDown = true;
    s_btnDownMs = now;
    s_longFired = false;
  } else if (pressed && s_btnDown && !s_longFired &&
             (now - s_btnDownMs) >= LONG_PRESS_MS) {
    s_longFired = true;          // fire long action at the threshold, while held
    onLongPress(now);
  } else if (!pressed && s_btnDown) {
    s_btnDown = false;
    uint32_t held = now - s_btnDownMs;
    if (!s_longFired && held >= BTN_DEBOUNCE_MS) onShortPress(now);
  }
}

static void updateStir(uint32_t now, uint32_t dt) {
  int32_t cnt = g_encoderCount;          // snapshot the ISR counter
  int32_t d = cnt - s_lastEncoder;
  s_lastEncoder = cnt;

  // Stir only matters in identify/stirring; reveal is frozen until combo change.
  if (g_state != ST_IDENTIFY && g_state != ST_STIRRING) return;

  // Continuous decay; turning the knob adds faster than it bleeds off.
  s_stirProgress -= STIR_DECAY_PER_MS * (float)dt;
  if (s_stirProgress < 0.0f) s_stirProgress = 0.0f;

  if (d != 0) {
    int32_t ad = (d < 0) ? -d : d;
    s_stirProgress += STIR_GAIN * (float)ad;
    if (s_stirProgress > 1.0f) s_stirProgress = 1.0f;
    s_swirlAngle += STIR_ANGLE_STEP * (float)d;
    if (g_state == ST_IDENTIFY) g_state = ST_STIRRING;
  }

  if (s_stirProgress >= STIR_READY_LEVEL) s_stirReady = true;

  // Settle back to identify once the brew goes quiet.
  if (g_state == ST_STIRRING && d == 0 && s_stirProgress <= 0.02f) {
    g_state = ST_IDENTIFY;
  }
}

// ---- Rendering ---------------------------------------------------------
static void drawCentered(const char* s, int y) {  // font must be set first
  int w = oled.getStrWidth(s);
  oled.drawStr((128 - w) / 2, y, s);
}

static void renderIdle() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x12_tr);
  drawCentered("Place ingredients", 26);
  oled.setFont(u8g2_font_5x8_tr);
  drawCentered("Hold knob: switch realm", 44);
  drawCentered(kUniverseName[g_universe], 60);
  oled.sendBuffer();
}

static void renderIdentify() {
  oled.clearBuffer();

  // Top: current realm.
  oled.setFont(u8g2_font_5x8_tr);
  oled.drawStr(0, 7, kUniverseName[g_universe]);

  // Ingredient list. Pick a font that keeps the widest present name on-screen.
  oled.setFont(u8g2_font_6x10_tr);
  bool fits = true;
  for (int slot = 0; slot < 3; slot++) {
    if (g_combo & (1 << slot)) {
      if (oled.getStrWidth(kIngredients[g_universe][slot]) > 126) fits = false;
    }
  }
  if (!fits) oled.setFont(u8g2_font_5x8_tr);

  int y = 24;
  for (int slot = 0; slot < 3; slot++) {
    if (g_combo & (1 << slot)) {
      oled.drawStr(2, y, kIngredients[g_universe][slot]);
      y += 13;
    }
  }

  // Prompt reflects whether a brew is armed.
  oled.setFont(u8g2_font_5x8_tr);
  drawCentered(s_stirReady ? "Press to brew!" : "Turn to stir", 62);
  oled.sendBuffer();
}

static void renderStirring() {
  oled.clearBuffer();

  const int cx = 64, cy = 34, R = 20;

  // Ring of dim (single-pixel) dots.
  const int N = 12;
  for (int i = 0; i < N; i++) {
    float a = (float)i * (2.0f * (float)M_PI / N);
    oled.drawPixel(cx + (int)lroundf(R * cosf(a)),
                   cy + (int)lroundf(R * sinf(a)));
  }

  // One bright orbiting dot whose position follows the encoder and whose
  // size grows with stir progress.
  int bx = cx + (int)lroundf(R * cosf(s_swirlAngle));
  int by = cy + (int)lroundf(R * sinf(s_swirlAngle));
  int r = 2 + (int)lroundf(s_stirProgress * 2.0f);
  oled.drawDisc(bx, by, r);

  oled.setFont(u8g2_font_5x8_tr);
  drawCentered("Stirring", 8);
  drawCentered(s_stirReady ? "Press to brew!" : "keep stirring", 62);
  oled.sendBuffer();
}

// Potion name, centered, wrapped to a second line if wider than the panel.
static void drawPotionName(const char* name) {
  oled.setFont(u8g2_font_6x13B_tr);
  if (oled.getStrWidth(name) <= 126) {
    drawCentered(name, 40);
    return;
  }
  char buf[48];
  strncpy(buf, name, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char line1[48] = "";
  char line2[48] = "";
  for (char* tok = strtok(buf, " "); tok != nullptr; tok = strtok(nullptr, " ")) {
    if (line2[0] == '\0') {
      char trial[48];
      strcpy(trial, line1);
      if (line1[0]) strcat(trial, " ");
      strcat(trial, tok);
      if (oled.getStrWidth(trial) <= 126) { strcpy(line1, trial); continue; }
    }
    if (line2[0]) strcat(line2, " ");
    strcat(line2, tok);
  }
  drawCentered(line1, 30);
  drawCentered(line2, 46);
}

static void renderReveal() {
  oled.clearBuffer();
  oled.setFont(u8g2_font_5x8_tr);
  drawCentered(kUniverseName[g_universe], 8);
  drawPotionName(kPotions[g_universe][g_combo]);
  oled.sendBuffer();
}

static void render() {
  if (!g_haveDisplay) return;  // headless: skip drawing if no panel on the bus
  switch (g_state) {
    case ST_IDLE:     renderIdle();     break;
    case ST_IDENTIFY: renderIdentify(); break;
    case ST_STIRRING: renderStirring(); break;
    case ST_REVEAL:   renderReveal();   break;
  }
}

// ---- Arduino entry points ----------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== Alchemist Study (ESP32-C3) booting ===");

  pinMode(PIN_REED_SLOT1, INPUT_PULLUP);
  pinMode(PIN_REED_SLOT2, INPUT_PULLUP);
  pinMode(PIN_REED_SLOT3, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);

  // Encoder channels: internal pullups, decode on every edge of both lines.
  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  s_encPrev = (uint8_t)((digitalRead(PIN_ENC_A) << 1) | digitalRead(PIN_ENC_B));
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_A), encoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_ENC_B), encoderISR, CHANGE);

  pinMode(PIN_BUZZER, OUTPUT);

  // OLED. Let U8g2 own the single Wire.begin(); we only preset the C3's I2C
  // pins. Calling Wire.begin() ourselves AND letting U8g2 begin() again leaves
  // the core-3.x i2c-ng driver stuck in ESP_ERR_INVALID_STATE (every transmit
  // fails). setPins() just records the pins for U8g2's begin() to use.
  // Bus at 400 kHz so a full-buffer send is ~2-3 ms (smooth swirl animation).
  Wire.setPins(PIN_OLED_SDA, PIN_OLED_SCL);
  oled.setBusClock(400000);
  oled.begin();

  // Probe for the panel. If it's absent/miswired, run headless: skip all
  // drawing (otherwise every sendBuffer would flood the log with
  // ESP_ERR_INVALID_STATE and drown the input traces). Wire is already up
  // from oled.begin(), so this probe is just one address transaction.
  Wire.beginTransmission(0x3C);
  g_haveDisplay = (Wire.endTransmission() == 0);
  Serial.printf("[boot] OLED %s\n",
                g_haveDisplay ? "found at 0x3C" : "NOT FOUND — running headless");

  // Restore the chosen universe from NVS (defaults to Skyrim).
  prefs.begin("alchemy", false);
  g_universe = (Universe)prefs.getUChar("universe", UNI_SKYRIM);
  Serial.printf("[boot] universe = %s\n", kUniverseName[g_universe]);

  s_lastEncoder = g_encoderCount;
  s_lastLoopMs = millis();
  render();  // first paint (idle)
}

void loop() {
  uint32_t now = millis();
  uint32_t dt = now - s_lastLoopMs;
  s_lastLoopMs = now;

  // Inputs: debounced reed combo, encoder/stir, button.
  uint8_t raw = readRawCombo();
  if (raw != s_rawComboLast) {
    s_rawComboLast = raw;
    s_comboChangeMs = now;
  }
  if (raw != g_combo && (now - s_comboChangeMs) >= REED_DEBOUNCE_MS) {
    onComboChanged(raw);
  }

  updateButton(now);
  updateStir(now, dt);
  updateAudio(now);

  if (now - s_lastRenderMs >= RENDER_INTERVAL_MS) {
    s_lastRenderMs = now;
    render();
  }
}
