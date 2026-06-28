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

// reed debounce / latch
static uint8_t  s_rawComboLast = 0;
static uint32_t s_comboChangeMs = 0;
static uint8_t  s_sensedCombo  = 0;     // actual debounced combo (reality)
static bool     s_baseEmptied  = false; // base cleared since the brew latched

// encoder / stir model
static int32_t s_lastEncoder = 0;
static float   s_stirProgress = 0.0f;      // 0..1, decays when idle
static float   s_swirlAngle   = 0.0f;      // radians, follows the encoder
static bool    s_stirReady    = false;     // latched once progress crosses level
static uint32_t s_revealMs    = 0;         // when REVEAL began (drives the burst)

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

// Commit a combo as the effective one: reset the brew and pick a fresh state.
static void enterCombo(uint8_t c) {
  g_combo = c;
  s_stirProgress = 0.0f;
  s_stirReady = false;
  s_baseEmptied = false;
  g_state = (c == 0) ? ST_IDLE : ST_IDENTIFY;
  // Print MSB-first (slot3 slot2 slot1) to match the spec's "100 Nightshade".
  Serial.printf("[combo] %u%u%u (val %u) -> %s\n",
                (c >> 2) & 1, (c >> 1) & 1, (c >> 0) & 1,
                c, c ? kPotions[g_universe][c] : "idle");
}

// Decide how to react to a newly-sensed (debounced) combo.
//
// IDLE/IDENTIFY are live — they always follow the bottles. But once a brew is
// underway (STIRRING or REVEAL) the combo is LATCHED for reliability: pulling
// a magnet away — fully or partially, deliberately or by a flaky reed — is
// ignored, so the swirl and the revealed potion survive. A brew only restarts
// on a genuinely NEW arrangement: a bottle ADDED to the set, or the base
// cleared to empty and then refilled.
static void onSensedComboChange() {
  uint8_t raw = s_sensedCombo;
  if (g_state == ST_STIRRING || g_state == ST_REVEAL) {
    if (raw == 0) { s_baseEmptied = true; return; }   // full removal: hold the brew
    bool addedNew = (raw & ~g_combo) != 0;            // a bottle not in the latched set
    if (s_baseEmptied || addedNew) enterCombo(raw);   // deliberate new arrangement
    // else: partial removal of the latched set — ignore it
    return;
  }
  enterCombo(raw);
}

static void onShortPress(uint32_t now) {
  if (g_state == ST_REVEAL) return;        // only a combo change leaves reveal
  if (g_combo == 0) return;                // nothing to brew
  if (s_stirReady) {
    g_state = ST_REVEAL;
    s_revealMs = now;
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

  if (s_stirReady) {
    // Armed: hold the brewing screen lively until a press or a real combo
    // change. Don't let releasing the knob flick back to the ingredient list,
    // and keep the swirl from fully dying so it reads as "ready".
    if (s_stirProgress < STIR_READY_LEVEL) s_stirProgress = STIR_READY_LEVEL;
    if (g_state == ST_IDENTIFY) g_state = ST_STIRRING;
  } else if (g_state == ST_STIRRING && d == 0 && s_stirProgress <= 0.02f) {
    // Not armed and the swirl fizzled: settle back, resyncing to reality
    // (an emptied base drops to idle; bottles still present stay in identify).
    if (s_sensedCombo != g_combo) enterCombo(s_sensedCombo);
    else g_state = ST_IDENTIFY;
  }
}

// ---- Rendering ---------------------------------------------------------
// Aesthetic: alchemical / celestial. Elegant serif type (New Century
// Schoolbook), ornamental frames with corner flourishes, twinkling stars,
// a swirling vortex, and a sparkle-burst reveal. All on 1 bit, 128x64.

static void drawCenteredF(const char* s, int y) {  // font must be set first
  oled.drawStr((128 - oled.getStrWidth(s)) / 2, y, s);
}

// A 4-point sparkle / star of radius r (0..3).
static void drawSparkle(int x, int y, int r) {
  oled.drawPixel(x, y);
  for (int i = 1; i <= r; i++) {
    oled.drawPixel(x + i, y); oled.drawPixel(x - i, y);
    oled.drawPixel(x, y + i); oled.drawPixel(x, y - i);
  }
}

// A small filled diamond (ornament).
static void drawDiamond(int x, int y) {
  oled.drawHLine(x - 1, y, 3);
  oled.drawPixel(x, y - 1);
  oled.drawPixel(x, y + 1);
}

// Double border with corner diamonds — an illuminated-manuscript frame.
static void drawFancyFrame() {
  oled.drawFrame(0, 0, 128, 64);
  oled.drawFrame(3, 3, 122, 58);
  drawDiamond(3, 3); drawDiamond(124, 3);
  drawDiamond(3, 60); drawDiamond(124, 60);
}

// Crescent moon: a ring with a bite taken out of it.
static void drawMoon(int x, int y, int rad) {
  oled.drawCircle(x, y, rad);
  oled.setDrawColor(0);
  oled.drawDisc(x + (rad + 1) / 2, y - (rad + 1) / 3, rad);
  oled.setDrawColor(1);
}

// Fixed constellation that twinkles via per-star phase (kept off-centre).
struct Star { int8_t x, y, phase; };
static const Star kStars[] = {
  {12, 11, 0}, {116, 13, 3}, {10, 44, 6}, {119, 46, 2}, {30, 56, 5}, {99, 55, 1}
};
static void drawTwinkles(uint32_t now) {
  for (auto& s : kStars) {
    int ph = (int)((now / 110 + s.phase * 5) % 16);
    int tri = ph < 8 ? ph : 15 - ph;       // 0..7..0
    drawSparkle(s.x, s.y, tri / 3);        // radius 0..2
  }
}

// A "Press to brew" call-to-action as a glowing rounded pill with sparkles.
static void drawBrewPill(int y) {
  const char* t = "Press to brew";
  oled.setFont(u8g2_font_helvR08_tr);
  int w = oled.getStrWidth(t);
  int bx = (128 - w) / 2 - 6;
  oled.drawRBox(bx, y, w + 12, 11, 3);
  oled.setDrawColor(0);
  oled.drawStr((128 - w) / 2, y + 8, t);
  oled.setDrawColor(1);
  drawSparkle(bx - 4, y + 5, 1);
  drawSparkle(bx + w + 15, y + 5, 1);
}

static void drawCornerSparkles(uint32_t now) {
  int r = ((now / 300) & 1) ? 1 : 0;
  drawSparkle(9, 10, r);  drawSparkle(118, 10, r);
  drawSparkle(9, 53, r);  drawSparkle(118, 53, r);
}

// Draw `s` centered, in the largest of `fonts` that fits `budget`, on one line
// (baseline y1) or two greedy lines (y2a/y2b). Falls back to the smallest font.
static void drawFitted(const char* s, const uint8_t* const fonts[], int nf,
                       int budget, int y1, int y2a, int y2b) {
  for (int f = 0; f < nf; f++) {
    oled.setFont(fonts[f]);
    if (oled.getStrWidth(s) <= budget) { drawCenteredF(s, y1); return; }

    char buf[48];
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char l1[48] = "", l2[48] = "";
    for (char* tok = strtok(buf, " "); tok; tok = strtok(nullptr, " ")) {
      if (l2[0] == '\0') {
        char trial[48];
        strcpy(trial, l1);
        if (l1[0]) strcat(trial, " ");
        strcat(trial, tok);
        if (oled.getStrWidth(trial) <= budget) { strcpy(l1, trial); continue; }
      }
      if (l2[0]) strcat(l2, " ");
      strcat(l2, tok);
    }
    bool ok = oled.getStrWidth(l1) <= budget && oled.getStrWidth(l2) <= budget;
    if (ok || f == nf - 1) { drawCenteredF(l1, y2a); drawCenteredF(l2, y2b); return; }
  }
}

static void renderIdle(uint32_t now) {
  oled.clearBuffer();
  drawTwinkles(now);
  drawMoon(15, 15, 6);

  oled.setFont(u8g2_font_ncenB12_tr);
  drawCenteredF("Place the", 28);
  drawCenteredF("ingredients", 44);

  // Divider with diamond end-caps.
  oled.drawHLine(28, 51, 72);
  drawDiamond(22, 51); drawDiamond(106, 51);

  // Bottom caption gently alternates the realm and the hidden gesture.
  oled.setFont(u8g2_font_helvR08_tr);
  bool hint = ((now / 2600) & 1);
  drawCenteredF(hint ? "hold knob - change realm" : kUniverseName[g_universe], 61);
  oled.sendBuffer();
}

static void renderIdentify(uint32_t now) {
  oled.clearBuffer();
  drawCornerSparkles(now);

  int count = 0, first = -1;
  for (int s = 0; s < 3; s++) if (g_combo & (1 << s)) { count++; if (first < 0) first = s; }

  if (count == 1) {
    // Feature the single ingredient large, with flanking sparkles.
    static const uint8_t* const fonts[] = {
      u8g2_font_ncenB14_tr, u8g2_font_ncenB12_tr, u8g2_font_ncenB10_tr
    };
    drawFitted(kIngredients[g_universe][first], fonts, 3, 110, 35, 29, 45);
    int sr = ((now / 220) & 1) ? 2 : 1;
    drawSparkle(7, 34, sr);
    drawSparkle(121, 34, sr);
  } else {
    // Two or three ingredients: an elegant serif list, each flanked by a
    // diamond, vertically centred for the count.
    oled.setFont(u8g2_font_ncenB10_tr);
    bool fits = true;
    for (int s = 0; s < 3; s++)
      if ((g_combo & (1 << s)) && oled.getStrWidth(kIngredients[g_universe][s]) > 104) fits = false;
    if (!fits) oled.setFont(u8g2_font_helvB08_tr);

    int y = (count == 2) ? 30 : 24;
    for (int s = 0; s < 3; s++) {
      if (g_combo & (1 << s)) {
        const char* nm = kIngredients[g_universe][s];
        int w = oled.getStrWidth(nm);
        int x = (128 - w) / 2;
        drawDiamond(x - 7, y - 3);
        oled.drawStr(x, y, nm);
        drawDiamond(x + w + 5, y - 3);
        y += (count == 2) ? 16 : 13;
      }
    }
  }

  // A subtle stir cue (realm + gestures live on the idle screen).
  oled.setFont(u8g2_font_5x8_tr);
  drawCenteredF("turn to stir", 62);
  oled.sendBuffer();
}

static void renderStirring(uint32_t now) {
  oled.clearBuffer();
  const int cx = 64, cy = 31;
  const int Rout = 22;

  oled.setFont(u8g2_font_helvR08_tr);
  drawCenteredF("- brewing -", 8);

  // Faint outer ring of stationary dots.
  const int N = 16;
  for (int i = 0; i < N; i++) {
    float a = (float)i * (2.0f * (float)M_PI / N);
    oled.drawPixel(cx + (int)lroundf(Rout * cosf(a)), cy + (int)lroundf(Rout * sinf(a)));
  }

  // Twin spiral arms swirling with the knob; reach grows with progress.
  float reach = 14.0f + s_stirProgress * 6.0f;
  for (int arm = 0; arm < 2; arm++) {
    float base = s_swirlAngle + arm * (float)M_PI;
    for (float t = 0.0f; t < 3.0f; t += 0.22f) {
      float r = 3.0f + (t / 3.0f) * reach;
      float a = base + t * 1.7f;
      oled.drawPixel(cx + (int)lroundf(r * cosf(a)), cy + (int)lroundf(r * sinf(a)));
    }
  }

  // Lead mote with a fading comet tail, orbiting the outer ring.
  for (int k = 0; k < 4; k++) {
    float a = s_swirlAngle - k * 0.34f;
    int x = cx + (int)lroundf(Rout * cosf(a));
    int yy = cy + (int)lroundf(Rout * sinf(a));
    if (k == 0)      oled.drawDisc(x, yy, 2 + (int)lroundf(s_stirProgress * 2.0f));
    else if (k < 2)  oled.drawDisc(x, yy, 1);
    else             oled.drawPixel(x, yy);
  }

  // Progress arc sweeping clockwise from the top.
  const int Rarc = 27;
  int seg = (int)(s_stirProgress * 46.0f);
  for (int i = 0; i < seg; i++) {
    float a = -(float)M_PI / 2.0f + (float)i * (2.0f * (float)M_PI / 46.0f);
    oled.drawPixel(cx + (int)lroundf(Rarc * cosf(a)), cy + (int)lroundf(Rarc * sinf(a)));
  }

  // When ready: sparkles flare around the rim + the brew pill.
  if (s_stirReady) {
    for (int i = 0; i < 3; i++) {
      float a = s_swirlAngle * 0.5f + i * 2.094f;
      int x = cx + (int)lroundf((Rout + 4) * cosf(a));
      int yy = cy + (int)lroundf((Rout + 4) * sinf(a));
      drawSparkle(x, yy, ((now / 150 + i) & 1) ? 1 : 2);
    }
    drawBrewPill(53);
  } else {
    oled.setFont(u8g2_font_helvR08_tr);
    drawCenteredF("keep stirring", 61);
  }
  oled.sendBuffer();
}

// Potion name in an elegant serif, scaled to the largest size that fits the
// cartouche on one or two lines (falls back through smaller faces).
static void drawPotionName(const char* name) {
  static const uint8_t* const fonts[] = {
    u8g2_font_ncenB14_tr, u8g2_font_ncenB12_tr, u8g2_font_ncenB10_tr
  };
  drawFitted(name, fonts, 3, 112, 39, 32, 47);
}

static void renderReveal(uint32_t now) {
  oled.clearBuffer();
  drawFancyFrame();

  uint32_t since = now - s_revealMs;

  // Opening flourish: a ring of sparkles bursting outward, fading over ~1.1s.
  if (since < 1100) {
    float p = (float)since / 1100.0f;
    for (int i = 0; i < 10; i++) {
      float a = i * 0.6283f + p * 1.5f;
      float rad = 6.0f + p * 52.0f;
      int x = 64 + (int)lroundf(rad * cosf(a));
      int y = 33 + (int)lroundf(rad * sinf(a) * 0.5f);
      int r = 2 - (int)(p * 2.0f);
      if (x > 5 && x < 123 && y > 5 && y < 59) drawSparkle(x, y, r < 0 ? 0 : r);
    }
  } else {
    // Settled: gentle twinkles in the corners.
    int r = ((now / 250) & 1) ? 1 : 0;
    drawSparkle(11, 11, r); drawSparkle(117, 11, r);
    drawSparkle(11, 53, r); drawSparkle(117, 53, r);
  }

  // The potion name alone — the realm is implicit. A small ornamental
  // flourish up top where the caption used to sit, then the name.
  oled.drawHLine(44, 15, 40);
  drawDiamond(40, 15);
  drawDiamond(88, 15);
  drawPotionName(kPotions[g_universe][g_combo]);
  oled.sendBuffer();
}

static void render() {
  if (!g_haveDisplay) return;  // headless: skip drawing if no panel on the bus
  uint32_t now = millis();
  switch (g_state) {
    case ST_IDLE:     renderIdle(now);     break;
    case ST_IDENTIFY: renderIdentify(now); break;
    case ST_STIRRING: renderStirring(now); break;
    case ST_REVEAL:   renderReveal(now);   break;
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

  // Boot chime: a rising two-note "awake" signal. Doubles as a buzzer test —
  // if you hear it, GPIO1 and the buzzer are wired and working.
  tone(PIN_BUZZER, 660); delay(120);
  tone(PIN_BUZZER, 990); delay(150);
  noTone(PIN_BUZZER);

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

  // ---- Boot self-check: state what we found, loudly, and don't pretend. ----
  Serial.println("---- self-check ----");
  Serial.printf("  reeds:   GPIO%d/%d/%d (slots 1/2/3)\n",
                PIN_REED_SLOT1, PIN_REED_SLOT2, PIN_REED_SLOT3);
  Serial.printf("  encoder: A=GPIO%d B=GPIO%d SW=GPIO%d\n",
                PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW);
  Serial.printf("  buzzer:  GPIO%d\n", PIN_BUZZER);
  Serial.printf("  OLED:    SDA=GPIO%d SCL=GPIO%d -> %s\n",
                PIN_OLED_SDA, PIN_OLED_SCL,
                g_haveDisplay ? "FOUND at 0x3C [OK]" : "NOT FOUND [FAIL]");
  if (!g_haveDisplay) {
    Serial.println("  !! OLED not on the bus. Running HEADLESS (no screen).");
    Serial.println("  !! Check: VCC=3V3 (measure at the panel), GND, SDA->GPIO5,");
    Serial.println("  !! SCL->GPIO6. Run the hwcheck build to map your wiring:");
    Serial.println("  !!   pio run -e c3-hwcheck -t upload");
    // Fail loudly on the one output we DO have: a low error triple-beep.
    for (int i = 0; i < 3; i++) { tone(PIN_BUZZER, 180); delay(120); noTone(PIN_BUZZER); delay(90); }
  }
  Serial.println("--------------------");

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
  if (raw != s_sensedCombo && (now - s_comboChangeMs) >= REED_DEBOUNCE_MS) {
    s_sensedCombo = raw;          // reality, debounced
    onSensedComboChange();        // honor or latch, depending on state
  }

  updateButton(now);
  updateStir(now, dt);
  updateAudio(now);

  if (now - s_lastRenderMs >= RENDER_INTERVAL_MS) {
    s_lastRenderMs = now;
    render();
  }

  // Never let a degraded (screenless) run look healthy: nag periodically.
  static uint32_t s_lastWarnMs = 0;
  if (!g_haveDisplay && now - s_lastWarnMs >= 5000) {
    s_lastWarnMs = now;
    Serial.println("[warn] HEADLESS — no OLED at 0x3C on SDA=5/SCL=6 (check wiring)");
  }
}
