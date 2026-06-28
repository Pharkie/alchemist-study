// ============================================================
// Alchemist Study — ESP32-C3 Super Mini firmware
//
// Brain of a 3D-printed alchemist's study: reed switches sense which
// potion bottles are seated, an OLED names the resulting potion, a
// rotary encoder is the "stir" control, and a passive buzzer scores it.
//
// State machine: IDLE -> IDENTIFY -> STIRRING -> REVEAL, plus SETTINGS + DIAG.
//   IDLE      empty base; "Place ingredients" + "Press for settings".
//   IDENTIFY  >=1 bottle seated; features the ingredient name(s) + sparkles.
//   STIRRING  turn to fill the power bar; when full -> "Press to create".
//   REVEAL    press names the potion with a random animation; auto-returns ~3s.
//   SETTINGS  press on idle; realm / mute / brightness / stir level / etc.
//   DIAG      built-in hardware test (Settings -> Hardware Test).
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
#include "esp_random.h"

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
enum Universe {
  UNI_SKYRIM = 0,
  UNI_BG3,
  UNI_WITCHER,
  UNI_WOW,
  UNI_ZELDA,
  UNI_MINECRAFT,
  UNI_ULTIMA,
  UNI_COUNT
};

static const char* const kUniverseName[UNI_COUNT] = {
  "Skyrim",
  "Baldur's Gate 3",
  "The Witcher 3",
  "World of Warcraft",
  "Zelda",
  "Minecraft",
  "Ultima VII"
};

static const char* const kIngredients[UNI_COUNT][3] = {
  { "Blue Mountain Flower", "Deathbell", "Nightshade" },           // Skyrim
  { "Salts of Mugwort", "Bullywug Crayfish Tail", "Rogue's Morsel" }, // Baldur's Gate 3
  { "Celandine", "Drowner Brain", "Dwarven Spirit" },             // The Witcher 3
  { "Peacebloom", "Silverleaf", "Earthroot" },                    // World of Warcraft
  { "Mushroom", "Bottled Fairy", "Lon Lon Milk" },                // Zelda (classic)
  { "Nether Wart", "Glistering Melon Slice", "Blaze Powder" },     // Minecraft
  { "Garlic", "Ginseng", "Spider Silk" }                          // Ultima VII
};

// Indexed by combo value 1..7 ([0] unused). Combo bits: slot1=1, slot2=2,
// slot3=4 -> [1]=s1, [2]=s2, [3]=s1+s2, [4]=s3, [5]=s1+s3, [6]=s2+s3, [7]=all.
static const char* const kPotions[UNI_COUNT][8] = {
  // Skyrim — Blue Mountain Flower / Deathbell / Nightshade
  { "",
    "Potion of Minor Healing", "Damage Health Poison", "Lingering Damage Poison",
    "Fortify Destruction", "Potion of Regeneration", "Deadly Paralysis Poison",
    "Philter of the Phantom" },
  // Baldur's Gate 3 — Mugwort / Crayfish / Morsel
  { "",
    "Potion of Healing", "Elixir of Vigilance", "Elixir of the Colossus",
    "Potion of Speed", "Potion of Invisibility", "Elixir of Heroism",
    "Elixir of Universal Resistance" },
  // The Witcher 3 — Celandine / Drowner Brain / Dwarven Spirit
  { "",
    "White Honey", "Black Blood", "Full Moon",
    "Tawny Owl", "White Raffard's Decoction", "Ekimmara Decoction",
    "Swallow" },
  // World of Warcraft — Peacebloom / Silverleaf / Earthroot
  { "",
    "Minor Rejuvenation Potion", "Elixir of Minor Defense", "Minor Healing Potion",
    "Elixir of Minor Fortitude", "Weak Troll's Blood Potion", "Elixir of Lion's Strength",
    "Flask of the Titans" },
  // Zelda (classic) — Mushroom / Bottled Fairy / Lon Lon Milk
  { "",
    "Green Potion", "Red Potion", "Blue Potion",
    "Lon Lon Milk", "Elixir Soup", "Life Potion",
    "Chateau Romani" },
  // Minecraft — Nether Wart / Glistering Melon Slice / Blaze Powder
  { "",
    "Awkward Potion", "Potion of Healing", "Potion of Regeneration",
    "Potion of Strength", "Potion of Swiftness", "Potion of Fire Resistance",
    "Potion of Harming" },
  // Ultima VII — Garlic / Ginseng / Spider Silk
  { "",
    "Cure Poison", "Mana Potion", "Awaken",
    "Sleep", "Protection", "Invisibility",
    "Heal" }
};

// ---- Tunables (revisit on the bench in Phase 6) ------------------------
static constexpr uint32_t REED_DEBOUNCE_MS  = 40;
static constexpr uint32_t LONG_PRESS_MS     = 600;   // hold = leave a menu / cancel an edit
static constexpr uint32_t BTN_DEBOUNCE_MS   = 30;
static constexpr uint32_t RENDER_INTERVAL_MS = 33;   // ~30 fps display cap
static constexpr uint32_t REVEAL_ANIM_MS     = 1000; // phase 1: build-up animation (no name yet)
static constexpr uint32_t REVEAL_NAME_MS     = 3000; // phase 2: hold the potion name, then idle
static constexpr uint32_t STIR_ACTIVE_MS     = 150;  // fill/drain boundary: moved within this = fill, else drain
static constexpr uint32_t STIR_IDLE_BACK_MS  = 2500; // empty + idle this long -> return to identify
static constexpr uint32_t REED_GRACE_MS      = 2500; // identify: keep the combo this long after the base empties
static constexpr float    STIR_ANGLE_STEP   = 0.18f;  // swirl radians per encoder count
static constexpr uint16_t PITCH_MIN_HZ      = 320;
static constexpr uint16_t PITCH_MAX_HZ      = 1100;
static constexpr int32_t  ENC_STEP          = 4;     // encoder counts per menu/select step

// Firmware version — keep in step with the git tag / GitHub release.
#define FW_VERSION "v0.1"

// ---- Hardware objects --------------------------------------------------
// Full-buffer (_F_) SSD1306 over hardware I2C.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, /*reset=*/U8X8_PIN_NONE);
Preferences prefs;
static bool g_haveDisplay = false;  // set at boot; if false we run headless

// ---- Runtime state -----------------------------------------------------
enum State { ST_IDLE, ST_IDENTIFY, ST_STIRRING, ST_REVEAL, ST_SETTINGS, ST_DIAG };
static State    g_state    = ST_IDLE;
static uint32_t g_stateMs  = 0;            // millis() when the current state/phase began
static uint8_t  g_combo    = 0;            // committed combo (0..7)
static Universe g_universe = UNI_SKYRIM;

// Persisted settings + UI cursors.
static bool     g_mute        = false;
static uint8_t  g_brightness  = 3;         // 1..5
static uint8_t  g_stirLevelIdx = 1;        // index into kStirLabels (Easy/Medium/Hard)
static int      s_menuIdx     = 0;         // highlighted settings item
static bool     s_menuEditing = false;     // true while adjusting the highlighted item
static int32_t  s_navAccum    = 0;         // encoder accumulator for discrete steps
static int      s_revealStyle = 0;         // which reveal animation is playing (0..2)

// reed debounce / latch
static uint8_t  s_rawComboLast = 0;
static uint32_t s_comboChangeMs = 0;
static uint8_t  s_sensedCombo  = 0;     // actual debounced combo (reality)
static bool     s_baseEmptied  = false; // base cleared since the brew latched
static uint32_t s_baseEmptyMs  = 0;     // when the base last went physically empty

// encoder / stir model
static int32_t s_lastEncoder = 0;
static float   s_stirProgress = 0.0f;      // 0..1 power bar (time-based fill)
static float   s_swirlAngle   = 0.0f;      // radians, follows the encoder
static bool    s_stirReady    = false;     // latched once the bar fills
static uint32_t s_lastStirMs  = 0;         // last time the knob actually moved

// reveal sub-phases (timed from g_stateMs; advancing a phase re-stamps it)
enum RevealPhase { RP_ANIM, RP_NAME };
static RevealPhase s_revealPhase = RP_ANIM;

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
  if (g_mute) return;
  s_melody = m;
  s_melodyLen = n;
  s_melodyIdx = 0;
  s_melodyActive = true;
  s_noteStartMs = now;
  s_buzzerOn = false;
  playCurrentNote();
}

static void updateAudio(uint32_t now) {
  if (g_mute) {
    if (s_buzzerOn || s_melodyActive) { noTone(PIN_BUZZER); s_buzzerOn = false; s_melodyActive = false; }
    return;
  }
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

// THE single state-transition point. Records when the (sub)state began so all
// in-state timing is measured as (now - g_stateMs). See docs/ARCHITECTURE.md.
static void enterState(State s) {
  g_state = s;
  g_stateMs = millis();
}

// Commit a combo as the effective one: reset the brew and pick a fresh state.
static void enterCombo(uint8_t c) {
  g_combo = c;
  s_stirProgress = 0.0f;
  s_stirReady = false;
  s_baseEmptied = false;
  enterState(c == 0 ? ST_IDLE : ST_IDENTIFY);
  // Print MSB-first (slot3 slot2 slot1) to match the spec's "100 Nightshade".
  Serial.printf("[combo] %u%u%u (val %u) -> %s\n",
                (c >> 2) & 1, (c >> 1) & 1, (c >> 0) & 1,
                c, c ? kPotions[g_universe][c] : "idle");
}

// React to a newly-sensed (debounced) combo.
//
// Resilience rule: removing bottles NEVER bails instantly. An empty base is
// just latched (with a timestamp); a grace timer decides when to fall back to
// idle, so turning still starts the stir on the cooldown after a magnet leaves.
//   - IDLE/IDENTIFY: a present bottle updates the combo live (compose before stir).
//   - STIRRING/REVEAL: combo is fully latched; only a genuinely new arrangement
//     (a bottle ADDED, or base cleared then refilled) starts a fresh brew.
static void onSensedComboChange() {
  uint8_t raw = s_sensedCombo;

  if (g_state == ST_SETTINGS || g_state == ST_DIAG) return;  // menus own the screen

  if (raw == 0) {                    // base emptied: latch it; never bail instantly
    s_baseEmptied = true;
    s_baseEmptyMs = millis();
    return;
  }

  if (g_state == ST_IDLE || g_state == ST_IDENTIFY) {
    enterCombo(raw);                 // live: compose the combo before stirring
    return;
  }

  // STIRRING / REVEAL: latched — only a new arrangement restarts.
  bool addedNew = (raw & ~g_combo) != 0;
  if (s_baseEmptied || addedNew) enterCombo(raw);
}

// ---- Settings model (reusable mini-menu) -------------------------------
static const char* const kOnOff[] = { "Off", "On" };

// Stir level: difficulty curve for filling the power bar. Each increment gets
// harder the fuller (further right) the bar already is; higher levels start a
// touch slower and slow down much more steeply toward the top.
static const char* const  kStirLabels[] = { "Easy", "Medium", "Hard" };
static const float        kStirSpeed[]  = { 0.55f, 0.48f, 0.40f }; // fill rate (progress/sec) at empty
static const float        kStirResist[] = { 0.40f, 0.74f, 0.92f }; // how steeply it slows toward full
static const float        kStirDecay[]  = { 0.20f, 0.24f, 0.28f }; // drain/sec when paused (+20%/level)
static constexpr int      kStirN        = 3;

static void applyBrightness() {
  if (g_haveDisplay) oled.setContrast((uint8_t)map(g_brightness, 1, 5, 16, 255));
}

static int  getRealm()       { return (int)g_universe; }
static void setRealm(int v)  { g_universe = (Universe)v; saveUniverse();
                               startMelody(MEL_TOGGLE, NUM_NOTES(MEL_TOGGLE), millis()); }
static int  getMute()        { return g_mute ? 1 : 0; }
static void setMute(int v)   { g_mute = (v != 0); prefs.putUChar("mute", g_mute ? 1 : 0);
                               if (g_mute) noTone(PIN_BUZZER); }
static int  getBright()      { return g_brightness; }
static void setBright(int v) { g_brightness = (uint8_t)v; prefs.putUChar("bright", g_brightness);
                               applyBrightness(); }
static int  getStirLevel()    { return g_stirLevelIdx; }
static void setStirLevel(int v){ g_stirLevelIdx = (uint8_t)v; prefs.putUChar("stirlvl", g_stirLevelIdx); }

static void settingsEnter() { s_menuIdx = 0; s_navAccum = 0; s_menuEditing = false; enterState(ST_SETTINGS); }
static void settingsExit()  { s_navAccum = 0; s_menuEditing = false; enterState(ST_IDLE); }
static void diagEnter()     { s_navAccum = 0; s_menuEditing = false; enterState(ST_DIAG); }

enum ItemKind { K_CHOICE, K_RANGE, K_INFO, K_ACTION };
struct MenuItem {
  const char*        label;
  ItemKind           kind;
  int  (*get)();
  void (*set)(int);
  const char* const* choices;    // K_CHOICE
  int                nChoices;
  int                rmin, rmax;  // K_RANGE
  const char*        info;        // K_INFO
  void (*action)();              // K_ACTION
};

static const MenuItem kMenu[] = {
  { "Realm",        K_CHOICE, getRealm,  setRealm,  kUniverseName, UNI_COUNT, 0, 0, nullptr,    nullptr      },
  { "Mute",         K_CHOICE, getMute,   setMute,   kOnOff,        2,         0, 0, nullptr,    nullptr      },
  { "Bright",       K_RANGE,  getBright,   setBright,   nullptr,     0,      1, 5, nullptr,    nullptr      },
  { "Stir Level",   K_CHOICE, getStirLevel, setStirLevel, kStirLabels, kStirN, 0, 0, nullptr,  nullptr      },
  { "Hardware Test",K_ACTION, nullptr,   nullptr,   nullptr,       0,         0, 0, nullptr,    diagEnter    },
  { "Firmware",     K_INFO,   nullptr,   nullptr,   nullptr,       0,         0, 0, FW_VERSION, nullptr      },
  { "Exit",         K_ACTION, nullptr,   nullptr,   nullptr,       0,         0, 0, nullptr,    settingsExit },
};
static const int kMenuN = (int)(sizeof(kMenu) / sizeof(kMenu[0]));

// Encoder raw delta -> discrete detent steps (so menus move one item per click).
static int navSteps(int32_t d) {
  s_navAccum += d;
  int steps = (int)(s_navAccum / ENC_STEP);
  s_navAccum -= (int32_t)steps * ENC_STEP;
  return steps;
}

// Turn the knob in Settings: navigate items, or (in edit mode) scroll the
// highlighted item's value live.
static void updateMenuNav(int32_t d) {
  int steps = navSteps(d);
  if (!steps) return;
  if (s_menuEditing) {
    const MenuItem& m = kMenu[s_menuIdx];
    if (m.kind == K_CHOICE && m.set) {
      int n = m.nChoices;
      m.set(((m.get() + steps) % n + n) % n);   // wrap through choices
    } else if (m.kind == K_RANGE && m.set) {
      int v = m.get() + steps;
      if (v < m.rmin) v = m.rmin;
      if (v > m.rmax) v = m.rmax;                // clamp the range
      m.set(v);
    }
  } else {
    s_menuIdx = ((s_menuIdx + steps) % kMenuN + kMenuN) % kMenuN;
  }
}

// Press the knob in Settings: confirm an edit, run an action, or enter edit.
static void menuPress() {
  if (s_menuEditing) { s_menuEditing = false; return; }
  const MenuItem& m = kMenu[s_menuIdx];
  if (m.kind == K_ACTION) { if (m.action) m.action(); }
  else if (m.kind == K_CHOICE || m.kind == K_RANGE) { s_menuEditing = true; }
}

// ---- Button actions (state-aware) --------------------------------------
static void onShortPress(uint32_t now) {
  switch (g_state) {
    case ST_IDLE:
      settingsEnter();                       // the knob opens Settings
      break;
    case ST_IDENTIFY:
    case ST_STIRRING:
      if (g_combo == 0) break;
      if (s_stirReady) {
        s_revealPhase = RP_ANIM;
        s_revealStyle = (int)(esp_random() % 3);   // pick a random reveal animation
        enterState(ST_REVEAL);                     // stamps the phase clock
        startMelody(MEL_SUCCESS, NUM_NOTES(MEL_SUCCESS), now);
        Serial.printf("[reveal] %s\n", kPotions[g_universe][g_combo]);
      } else {
        startMelody(MEL_NOTREADY, NUM_NOTES(MEL_NOTREADY), now);  // stir first
      }
      break;
    case ST_SETTINGS:
      menuPress();
      break;
    case ST_REVEAL:
    case ST_DIAG:
      break;  // reveal: only a combo change leaves; diag: long-press to exit
  }
}

static void onLongPress(uint32_t now) {
  (void)now;
  switch (g_state) {
    case ST_SETTINGS:
      if (s_menuEditing) s_menuEditing = false;  // cancel an edit...
      else settingsExit();                       // ...otherwise leave the menu
      break;
    case ST_DIAG:     settingsEnter(); break;     // diagnostic back to the menu
    default:          break;
  }
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

static void updateStir(uint32_t now, uint32_t dt, int32_t d) {
  // Stir only matters in identify/stirring (loop dispatches the delta by state).
  if (g_state != ST_IDENTIFY && g_state != ST_STIRRING) return;

  // Identify resilience: if the base is physically empty we keep showing the
  // combo (so turning still starts the stir) until a grace period lapses; only
  // then fall back to idle. This is the cooldown after a magnet is removed.
  if (g_state == ST_IDENTIFY && s_sensedCombo == 0 &&
      (uint32_t)(now - s_baseEmptyMs) >= REED_GRACE_MS) {
    enterCombo(0);   // -> idle
    return;
  }

  // Any knob motion = actively stirring; it also spins the swirl.
  if (d != 0) {
    s_lastStirMs = now;
    s_swirlAngle += STIR_ANGLE_STEP * (float)d;
    if (g_state == ST_IDENTIFY) enterState(ST_STIRRING);
  }

  if (s_stirReady) return;          // armed: hold the full bar until press/combo change
  if (g_state != ST_STIRRING) return;

  uint32_t idle = now - s_lastStirMs;
  if (idle < STIR_ACTIVE_MS) {
    // Actively stirring: fill, but with diminishing returns toward the right.
    // rate = speed * (1 - resist*progress) — higher levels slow more steeply.
    float p = s_stirProgress;
    float rate = kStirSpeed[g_stirLevelIdx] * (1.0f - kStirResist[g_stirLevelIdx] * p);
    if (rate < 0.03f) rate = 0.03f;  // floor so the bar can always top out
    s_stirProgress += rate * (float)dt / 1000.0f;
    if (s_stirProgress >= 1.0f) { s_stirProgress = 1.0f; s_stirReady = true; }
  } else {
    // Paused: drain gradually (20%/sec) rather than snapping to zero, so brief
    // hesitations barely cost progress and you resume from where it dropped.
    s_stirProgress -= kStirDecay[g_stirLevelIdx] * (float)dt / 1000.0f;
    if (s_stirProgress < 0.0f) s_stirProgress = 0.0f;
    // Once fully drained and idle a while, drift back to the ingredient screen.
    if (s_stirProgress <= 0.0f && idle >= STIR_IDLE_BACK_MS) {
      if (s_sensedCombo != g_combo) enterCombo(s_sensedCombo);
      else enterState(ST_IDENTIFY);
    }
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

  // The way into Settings (no realm shown here — it lives in the menu).
  oled.setFont(u8g2_font_5x8_tr);
  drawCenteredF("Press for settings", 62);
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

  // Bar full: a dedicated full-screen "ready to brew" call-to-action.
  if (s_stirReady) {
    drawFancyFrame();
    int sr = ((now / 200) & 1) ? 2 : 1;
    drawSparkle(15, 14, sr);  drawSparkle(113, 14, sr);
    drawSparkle(15, 50, sr);  drawSparkle(113, 50, sr);
    oled.setFont(u8g2_font_helvR08_tr);
    drawCenteredF("potion ready", 14);
    oled.setFont(u8g2_font_ncenB12_tr);
    drawCenteredF("Press to", 36);
    drawCenteredF("create", 52);
    drawSparkle(28, 45, sr);  drawSparkle(100, 45, sr);
    oled.sendBuffer();
    return;
  }

  // Otherwise: the brewing vortex + power bar.
  const int cx = 64, cy = 23;
  const int Rout = 16;

  oled.setFont(u8g2_font_helvR08_tr);
  drawCenteredF("- brewing -", 8);

  // Faint outer ring of stationary dots.
  const int N = 16;
  for (int i = 0; i < N; i++) {
    float a = (float)i * (2.0f * (float)M_PI / N);
    oled.drawPixel(cx + (int)lroundf(Rout * cosf(a)), cy + (int)lroundf(Rout * sinf(a)));
  }

  // Twin spiral arms swirling with the knob; reach grows with progress.
  float reach = 9.0f + s_stirProgress * 6.0f;
  for (int arm = 0; arm < 2; arm++) {
    float base = s_swirlAngle + arm * (float)M_PI;
    for (float t = 0.0f; t < 3.0f; t += 0.25f) {
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

  // Power bar — fills as you stir, drains gently if you pause.
  const int bx = 14, by = 41, bw = 100, bh = 10;
  oled.drawFrame(bx, by, bw, bh);
  int fill = (int)lroundf((float)(bw - 4) * s_stirProgress);
  if (fill > 0) oled.drawBox(bx + 2, by + 2, fill, bh - 4);

  oled.setFont(u8g2_font_5x8_tr);
  drawCenteredF("keep stirring", 62);
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

// --- Reveal animations: three styles, picked at random per brew ----------
// `el` is time elapsed in the ANIM phase; each animation spans REVEAL_ANIM_MS.
//
// Style 0: a dense sparkle starburst exploding outward (elliptical).
static void animStarburst(uint32_t el) {
  float p = (float)el / (float)REVEAL_ANIM_MS;
  if (p > 1.0f) p = 1.0f;
  for (int i = 0; i < 18; i++) {
    float a = i * 0.349f + p * 1.4f;
    float rad = 4.0f + p * 60.0f;
    int x = 64 + (int)lroundf(rad * cosf(a));
    int y = 33 + (int)lroundf(rad * sinf(a) * 0.46f);
    int r = 3 - (int)(p * 3.0f);           // start big, shrink as they fly out
    if (x > 5 && x < 123 && y > 5 && y < 59) drawSparkle(x, y, r < 0 ? 0 : r);
  }
}

// Style 1: a brew surge — bubbles rise from the bottom and grow as they climb.
static void animBubbles(uint32_t el) {
  float gp = (float)el / (float)REVEAL_ANIM_MS;   // 0..1 across the phase
  for (int i = 0; i < 12; i++) {
    float f = gp * 1.3f - i * 0.06f;              // staggered rise
    if (f < 0.0f || f > 1.0f) continue;
    int x = 9 + i * 10 + (int)lroundf(3.0f * sinf(f * 12.0f));
    int y = 57 - (int)lroundf(f * 52.0f);
    int r = 1 + (int)lroundf(f * 3.0f);
    if (y > 4 && x > 5 && x < 123) oled.drawCircle(x, y, r);
  }
}

// Style 2: magic pulse — bold concentric rings expand outward from the centre.
static void animRings(uint32_t el) {
  float k2r = 66.0f / (float)REVEAL_ANIM_MS;
  for (int k = 0; k < 4; k++) {
    int t = (int)el - k * 180;
    if (t < 0) continue;
    int rad = (int)((float)t * k2r);
    if (rad > 2 && rad < 64) {
      oled.drawCircle(64, 33, rad);
      if (rad < 63) oled.drawCircle(64, 33, rad + 1);   // doubled for visibility
    }
  }
}

// Advance the reveal sequence: ANIM phase -> NAME phase -> idle. Each phase is
// timed from when IT began (g_stateMs), so durations compose, not race.
static void updateReveal(uint32_t now) {
  uint32_t el = now - g_stateMs;
  if (s_revealPhase == RP_ANIM) {
    if (el >= REVEAL_ANIM_MS) { s_revealPhase = RP_NAME; g_stateMs = now; }
  } else {  // RP_NAME
    if (el >= REVEAL_NAME_MS) enterCombo(0);   // hold elapsed -> back to idle
  }
}

static void renderReveal(uint32_t now) {
  oled.clearBuffer();
  drawFancyFrame();
  uint32_t el = now - g_stateMs;

  if (s_revealPhase == RP_ANIM) {
    // Phase 1: pure build-up animation, no name yet.
    switch (s_revealStyle) {
      case 1:  animBubbles(el);   break;
      case 2:  animRings(el);     break;
      default: animStarburst(el); break;
    }
  } else {
    // Phase 2: the named potion, with gentle corner twinkles.
    int r = ((now / 250) & 1) ? 1 : 0;
    drawSparkle(11, 11, r); drawSparkle(117, 11, r);
    drawSparkle(11, 53, r); drawSparkle(117, 53, r);
    oled.drawHLine(44, 15, 40);
    drawDiamond(40, 15);
    drawDiamond(88, 15);
    drawPotionName(kPotions[g_universe][g_combo]);
  }
  oled.sendBuffer();
}

static const char* menuValueStr(const MenuItem& m, char* buf, int n) {
  switch (m.kind) {
    case K_CHOICE: return m.get ? m.choices[m.get()] : "";
    case K_RANGE:  snprintf(buf, n, "%d", m.get ? m.get() : 0); return buf;
    case K_INFO:   return m.info ? m.info : "";
    case K_ACTION: return "";
  }
  return "";
}

static void renderSettings() {
  oled.clearBuffer();
  oled.drawBox(0, 0, 128, 13);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_helvB08_tr);
  oled.drawStr(4, 10, "Settings");
  oled.setDrawColor(1);

  const int VIS = 5;
  int first = 0;
  if (kMenuN > VIS) {
    first = s_menuIdx - VIS / 2;
    if (first < 0) first = 0;
    if (first > kMenuN - VIS) first = kMenuN - VIS;
  }
  oled.setFont(u8g2_font_helvR08_tr);
  for (int row = 0; row < VIS && first + row < kMenuN; row++) {
    int i = first + row;
    int y = 22 + row * 10;
    bool sel = (i == s_menuIdx);
    bool editing = sel && s_menuEditing;
    if (sel && !editing) { oled.drawBox(0, y - 8, 128, 10); oled.setDrawColor(0); }
    oled.drawStr(4, y, kMenu[i].label);
    char tmp[16];
    const char* v = menuValueStr(kMenu[i], tmp, sizeof(tmp));
    if (v && v[0]) {
      int vw = oled.getStrWidth(v);
      int vx = 128 - vw - 4;
      if (editing) {                      // editing: invert just the value cell
        oled.drawBox(vx - 3, y - 8, vw + 6, 10);
        oled.setDrawColor(0);
        oled.drawStr(vx, y, v);
        oled.setDrawColor(1);
      } else {
        oled.drawStr(vx, y, v);
      }
    }
    if (sel && !editing) oled.setDrawColor(1);
  }
  oled.sendBuffer();
}

// Built-in live diagnostic (reachable from Settings -> Hardware Test).
static void renderDiag() {
  oled.clearBuffer();
  oled.drawBox(0, 0, 128, 13);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_helvB08_tr);
  oled.drawStr(4, 10, "Hardware Test");
  oled.setDrawColor(1);

  oled.setFont(u8g2_font_helvR08_tr);
  const int rp[3] = { PIN_REED_SLOT1, PIN_REED_SLOT2, PIN_REED_SLOT3 };
  int y = 24;
  for (int i = 0; i < 3; i++) {
    bool on = (digitalRead(rp[i]) == LOW);
    char lbl[18];
    snprintf(lbl, sizeof(lbl), "Reed %d  G%d", i + 1, rp[i]);
    oled.drawStr(4, y, lbl);
    if (on) oled.drawBox(96, y - 8, 9, 9);
    else    oled.drawFrame(96, y - 8, 9, 9);
    y += 10;
  }
  bool btn = (digitalRead(PIN_ENC_SW) == LOW);
  char e[26];
  snprintf(e, sizeof(e), "Btn %s   enc %ld", btn ? "DOWN" : "up", (long)g_encoderCount);
  oled.drawStr(4, y, e);

  oled.setFont(u8g2_font_5x8_tr);
  drawCenteredF("hold knob to exit", 63);
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
    case ST_SETTINGS: renderSettings();    break;
    case ST_DIAG:     renderDiag();        break;
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

  // ---- Load persisted settings (before the chime, so Mute is honored) ----
  prefs.begin("alchemy", false);
  g_universe   = (Universe)prefs.getUChar("universe", UNI_SKYRIM);
  if (g_universe >= UNI_COUNT) g_universe = UNI_SKYRIM;
  g_mute       = prefs.getUChar("mute", 0) != 0;
  g_brightness = prefs.getUChar("bright", 3);
  if (g_brightness < 1) g_brightness = 1;
  if (g_brightness > 5) g_brightness = 5;
  g_stirLevelIdx = prefs.getUChar("stirlvl", 1);
  if (g_stirLevelIdx >= kStirN) g_stirLevelIdx = 1;

  // Boot chime: a rising two-note "awake" signal (also a buzzer test).
  if (!g_mute) {
    tone(PIN_BUZZER, 660); delay(120);
    tone(PIN_BUZZER, 990); delay(150);
    noTone(PIN_BUZZER);
  }

  // OLED. Let U8g2 own the single Wire.begin(); we only preset the C3's I2C
  // pins (calling Wire.begin() ourselves AND letting U8g2 begin() again wedges
  // the core-3.x i2c-ng driver in ESP_ERR_INVALID_STATE). 400 kHz keeps a
  // full-buffer send to ~2-3 ms for a smooth swirl.
  Wire.setPins(PIN_OLED_SDA, PIN_OLED_SCL);
  oled.setBusClock(400000);
  oled.begin();

  // Probe for the panel; if absent/miswired, run headless (skip drawing so we
  // don't flood the log with ESP_ERR_INVALID_STATE every frame).
  Wire.beginTransmission(0x3C);
  g_haveDisplay = (Wire.endTransmission() == 0);
  applyBrightness();

  // ---- Boot self-check: state what we found, loudly, and don't pretend. ----
  Serial.println("---- self-check ----");
  Serial.printf("  reeds:   GPIO%d/%d/%d (slots 1/2/3)\n",
                PIN_REED_SLOT1, PIN_REED_SLOT2, PIN_REED_SLOT3);
  Serial.printf("  encoder: A=GPIO%d B=GPIO%d SW=GPIO%d\n",
                PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW);
  Serial.printf("  buzzer:  GPIO%d  (mute=%s)\n", PIN_BUZZER, g_mute ? "on" : "off");
  Serial.printf("  OLED:    SDA=GPIO%d SCL=GPIO%d -> %s\n",
                PIN_OLED_SDA, PIN_OLED_SCL,
                g_haveDisplay ? "FOUND at 0x3C [OK]" : "NOT FOUND [FAIL]");
  Serial.printf("  realm=%s  bright=%d  fw=%s\n",
                kUniverseName[g_universe], g_brightness, FW_VERSION);
  if (!g_haveDisplay) {
    Serial.println("  !! OLED not on the bus. Running HEADLESS (no screen).");
    Serial.println("  !! Check VCC=3V3, GND, SDA->GPIO5, SCL->GPIO6; or run c3-hwcheck.");
    if (!g_mute)
      for (int i = 0; i < 3; i++) { tone(PIN_BUZZER, 180); delay(120); noTone(PIN_BUZZER); delay(90); }
  }
  Serial.println("--------------------");

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

  // Encoder delta, dispatched by state.
  int32_t cnt = g_encoderCount;
  int32_t d = cnt - s_lastEncoder;
  s_lastEncoder = cnt;
  switch (g_state) {
    case ST_IDENTIFY:
    case ST_STIRRING: updateStir(now, dt, d);     break;
    case ST_SETTINGS: if (d) updateMenuNav(d);    break;
    case ST_REVEAL:   updateReveal(now);          break;  // ANIM -> NAME -> idle
    default:          break;  // IDLE / DIAG: encoder not navigated
  }

  updateButton(now);
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
