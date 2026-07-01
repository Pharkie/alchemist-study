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
#include "pins.h"        // shared pin map (also used by hwcheck)
#include "quadrature.h"  // shared encoder decode: g_encoderCount + encoderBegin()

#define ARRAY_COUNT(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))

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
static constexpr uint32_t REVEAL_ANIM_MS     = 1300; // phase 1: build-up animation (no name yet)
static constexpr uint32_t REVEAL_NAME_MS     = 3000; // phase 2: hold the potion name, then idle
static constexpr uint32_t REED_GRACE_MS      = 2500; // identify: keep the combo this long after the base empties
static constexpr uint32_t STIR_ZERO_GRACE_MS = 3000; // stirring: hold this long at an empty bar before reverting
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
static uint32_t g_now      = 0;            // single time source: millis() sampled once per loop
static State    g_state    = ST_IDLE;
static uint32_t g_stateMs  = 0;            // g_now when the current state/phase began
static uint8_t  g_combo    = 0;            // committed combo (0..7)
static Universe g_universe = UNI_SKYRIM;

// Persisted settings + UI cursors.
static bool     g_mute        = false;
static uint8_t  g_brightness  = 3;         // 1..5
static uint8_t  g_stirLevelIdx = 1;        // index into kStirLabels (Easy/Medium/Hard)
static uint8_t  g_blankIdx     = 0;        // screen-blank timeout (index into kBlankMs/kBlankLabels)
static uint32_t g_lastActivityMs = 0;      // last input (encoder/reed/button), for blanking
static bool     g_screenOff    = false;    // true while the panel is blanked (asleep)
static int      s_menuIdx     = 0;         // highlighted settings item
static bool     s_menuEditing = false;     // true while adjusting the highlighted item
static int      s_editVal0    = 0;         // value when the edit began (long-press cancel reverts)
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
static uint32_t s_stirZeroMs  = 0;         // last time the bar was non-empty (zero-grace timer)
// Stir-rate estimate (counts/sec over a short window). File-scope, not
// function statics, so it can be reset whenever a stir/brew starts — a stale
// rate would otherwise bleed a phantom head start into the next brew.
static uint32_t s_rateWinMs   = 0;         // window start
static int32_t  s_rateAcc     = 0;         // |encoder counts| accumulated this window
static int      s_stirCps     = 0;         // current estimate (counts/sec)

// Flavour text shown while stirring, escalating as the bar fills. The band is
// chosen by progress (<50 / <75 / <90 / >=90 %); a random line from that band
// is picked whenever the band changes (update*() picks, render*() just draws).
static const char* const kStirEarly[] = {
  "start stirring", "round and round", "coax the brew", "wake the cauldron",
  "rouse the mixture", "get it swirling", "stir it up", "summon the swirl",
};
static const char* const kStirMid[] = {
  "keep going", "halfway there", "it's bubbling", "building power",
  "the magic stirs", "don't stop now", "feel it churn", "the brew quickens",
};
static const char* const kStirLate[] = {
  "not long now", "nearly there", "almost brewed", "gathering force",
  "so close", "hold the rhythm", "keep it up", "the potion wakes",
};
static const char* const kStirFinal[] = {
  "another few goes", "one last push", "almost potion!", "final swirls",
  "to the brim!", "nearly potion!", "give it everything", "don't quit now!",
};
static const char* const* const kStirPools[4] = { kStirEarly, kStirMid, kStirLate, kStirFinal };
static const uint8_t kStirPoolN[4] = {
  ARRAY_COUNT(kStirEarly), ARRAY_COUNT(kStirMid),
  ARRAY_COUNT(kStirLate),  ARRAY_COUNT(kStirFinal)
};
static int         s_stirMsgBand = -1;                 // current band (-1 = unset)
static const char* s_stirMsg     = kStirEarly[0];      // line currently shown

// reveal sub-phases (timed from g_stateMs; advancing a phase re-stamps it)
enum RevealPhase { RP_ANIM, RP_NAME };
static RevealPhase s_revealPhase = RP_ANIM;

// button
static bool     s_btnDown   = false;
static uint32_t s_btnDownMs = 0;
static bool     s_longFired = false;
static bool     s_btnSwallow = false;      // ignore the press that woke the screen, until release

// timing
static uint32_t s_lastLoopMs   = 0;
static uint32_t s_lastRenderMs = 0;

// ---- Audio: non-blocking melody player + stir trill --------------------
struct Note { uint16_t freq; uint16_t durMs; };  // freq 0 = rest

static const Note MEL_SUCCESS[]  = { {523, 90}, {659, 90}, {784, 90}, {1047, 150} };
static const Note MEL_TOGGLE[]   = { {700, 110}, {1100, 150} };   // distinct two-note
static const Note MEL_NOTREADY[] = { {200, 70}, {0, 50}, {200, 70} }; // low "nope"

static const Note* s_melody     = nullptr;
static uint8_t      s_melodyLen = 0;
static uint8_t      s_melodyIdx = 0;
static uint32_t     s_noteStartMs = 0;
static bool         s_melodyActive = false;
static bool         s_buzzerOn = false;
static uint16_t     s_trillHz  = 0;   // last trill pitch sent to tone() (0 = none)

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
  s_trillHz = 0;         // melody owns the buzzer; force a re-arm when the trill resumes
  playCurrentNote();
}

static void updateAudio(uint32_t now) {
  if (g_mute) {
    if (s_buzzerOn || s_melodyActive) { noTone(PIN_BUZZER); s_buzzerOn = false; s_melodyActive = false; }
    s_trillHz = 0;
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
    if ((uint16_t)f != s_trillHz) {   // re-arm LEDC only when the pitch changes
      s_trillHz = (uint16_t)f;
      tone(PIN_BUZZER, s_trillHz);
    }
    s_buzzerOn = true;
  } else if (s_buzzerOn) {
    noTone(PIN_BUZZER);
    s_buzzerOn = false;
    s_trillHz = 0;
  }
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
  g_stateMs = g_now;
}

// Commit a combo as the effective one: reset the brew and pick a fresh state.
static void enterCombo(uint8_t c) {
  g_combo = c;
  s_stirProgress = 0.0f;
  s_stirReady = false;
  s_baseEmptied = false;
  s_stirCps = 0; s_rateAcc = 0; s_rateWinMs = g_now;   // no stale rate carry-over
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

  // Menus own the screen and ignore the base entirely; whatever is seated is
  // resynced on exit (settingsExit and the reveal end call enterCombo with
  // the sensed combo). REVEAL falls through to the latch logic below, so a
  // flaky reed still can't disrupt it but a genuinely added bottle restarts.
  if (g_state == ST_SETTINGS || g_state == ST_DIAG) return;

  if (raw == 0) {                    // base emptied: latch it; never bail instantly
    s_baseEmptied = true;
    s_baseEmptyMs = g_now;
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
// The add rate is CAPPED, so spinning faster than (cap/gain) counts/sec gives
// NO benefit. Each level therefore has a guaranteed MINIMUM fill time of about
// 1/(cap - decay) seconds, independent of how fast anyone can spin:
//   Easy ~3s, Medium ~5s, Hard ~10s. You must also sustain >~(decay/gain) c/s
//   or the constant decay wins and the bar stalls/drains.
static const float        kStirGain[]   = { 0.0320f, 0.0167f, 0.0120f }; // add/sec per (count/sec), until capped
static const float        kStirCap[]    = { 0.48f,   0.50f,   0.60f };   // max add/sec (the ceiling — can't cheese past it)
static const float        kStirResist[] = { 0.30f,   0.20f,   0.10f };   // mild end-loading (small so the top stays reachable)
static const float        kStirDecay[]  = { 0.15f,   0.30f,   0.50f };   // bar drained per second, ALWAYS
static constexpr int      kStirN        = 3;

static void applyBrightness() {
  if (g_haveDisplay) oled.setContrast((uint8_t)map(g_brightness, 1, 5, 16, 255));
}

// Each editable item has a get/set pair (set APPLIES the value live while
// scrolling) and a persist function (writes NVS). Persist runs only when the
// edit is CONFIRMED — so scrolling through choices doesn't hammer flash, and
// a long-press cancel can revert without having saved anything.
static int  getRealm()       { return (int)g_universe; }
static void setRealm(int v)  { g_universe = (Universe)v;
                               startMelody(MEL_TOGGLE, ARRAY_COUNT(MEL_TOGGLE), g_now); }
static void persistRealm()   { prefs.putUChar("universe", (uint8_t)g_universe); }
static int  getMute()        { return g_mute ? 1 : 0; }
static void setMute(int v)   { g_mute = (v != 0); if (g_mute) noTone(PIN_BUZZER); }
static void persistMute()    { prefs.putUChar("mute", g_mute ? 1 : 0); }
static int  getBright()      { return g_brightness; }
static void setBright(int v) { g_brightness = (uint8_t)v; applyBrightness(); }
static void persistBright()  { prefs.putUChar("bright", g_brightness); }
static int  getStirLevel()    { return g_stirLevelIdx; }
static void setStirLevel(int v){ g_stirLevelIdx = (uint8_t)v; }
static void persistStirLevel(){ prefs.putUChar("stirlvl", g_stirLevelIdx); }

// Screen-blank (screensaver) timeout. Index 0 = Never; the panel powers down
// after this much input idle and wakes on any encoder/reed/button activity.
static const char* const kBlankLabels[] = { "Never", "10s", "1m", "5m", "30m" };
static const uint32_t    kBlankMs[]     = { 0, 10000UL, 60000UL, 300000UL, 1800000UL };
static constexpr int     kBlankN        = 5;
static int  getBlank()       { return g_blankIdx; }
static void setBlank(int v)  { g_blankIdx = (uint8_t)v; }
static void persistBlank()   { prefs.putUChar("blank", g_blankIdx); }

static void settingsEnter() { s_menuIdx = 0; s_navAccum = 0; s_menuEditing = false; enterState(ST_SETTINGS); }
// Leaving the menu resyncs to whatever is physically on the base (bottles may
// have been seated/removed while the menu owned the screen).
static void settingsExit()  { s_navAccum = 0; s_menuEditing = false; enterCombo(s_sensedCombo); }
static void diagEnter()     { s_navAccum = 0; s_menuEditing = false; enterState(ST_DIAG); }

enum ItemKind { K_CHOICE, K_RANGE, K_INFO, K_ACTION };
struct MenuItem {
  const char*        label;
  ItemKind           kind;
  int  (*get)();
  void (*set)(int);              // apply live while editing
  void (*persist)();             // write NVS — called on confirm only
  const char* const* choices;    // K_CHOICE
  int                nChoices;
  int                rmin, rmax;  // K_RANGE
  const char*        info;        // K_INFO
  void (*action)();              // K_ACTION
};

static const MenuItem kMenu[] = {
  { "Realm",        K_CHOICE, getRealm,     setRealm,     persistRealm,     kUniverseName, UNI_COUNT, 0, 0, nullptr,    nullptr      },
  { "Mute",         K_CHOICE, getMute,      setMute,      persistMute,      kOnOff,        2,         0, 0, nullptr,    nullptr      },
  { "Bright",       K_RANGE,  getBright,    setBright,    persistBright,    nullptr,       0,         1, 5, nullptr,    nullptr      },
  { "Stir Level",   K_CHOICE, getStirLevel, setStirLevel, persistStirLevel, kStirLabels,   kStirN,    0, 0, nullptr,    nullptr      },
  { "Sleep",        K_CHOICE, getBlank,     setBlank,     persistBlank,     kBlankLabels,  kBlankN,   0, 0, nullptr,    nullptr      },
  { "Hardware Test",K_ACTION, nullptr,      nullptr,      nullptr,          nullptr,       0,         0, 0, nullptr,    diagEnter    },
  { "Firmware",     K_INFO,   nullptr,      nullptr,      nullptr,          nullptr,       0,         0, 0, FW_VERSION, nullptr      },
  { "Exit",         K_ACTION, nullptr,      nullptr,      nullptr,          nullptr,       0,         0, 0, nullptr,    settingsExit },
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

// Press the knob in Settings: confirm an edit (persisting it), run an action,
// or enter edit (remembering the starting value so cancel can revert).
static void menuPress() {
  if (s_menuEditing) {
    s_menuEditing = false;
    const MenuItem& m = kMenu[s_menuIdx];
    if (m.persist) m.persist();
    return;
  }
  const MenuItem& m = kMenu[s_menuIdx];
  if (m.kind == K_ACTION) { if (m.action) m.action(); }
  else if ((m.kind == K_CHOICE || m.kind == K_RANGE) && m.set) {
    s_editVal0 = m.get ? m.get() : 0;
    s_menuEditing = true;
  }
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
        startMelody(MEL_SUCCESS, ARRAY_COUNT(MEL_SUCCESS), now);
        Serial.printf("[reveal] %s\n", kPotions[g_universe][g_combo]);
      } else {
        startMelody(MEL_NOTREADY, ARRAY_COUNT(MEL_NOTREADY), now);  // stir first
      }
      break;
    case ST_SETTINGS:
      menuPress();
      break;
    case ST_REVEAL:
    case ST_DIAG:
      break;  // reveal: runs out its own timer (an added bottle restarts);
              // diag: long-press to exit
  }
}

static void onLongPress(uint32_t now) {
  (void)now;
  switch (g_state) {
    case ST_SETTINGS:
      if (s_menuEditing) {                       // cancel an edit: revert to the
        const MenuItem& m = kMenu[s_menuIdx];    // value it had when editing began
        if (m.set) m.set(s_editVal0);            // (nothing was persisted yet)
        s_menuEditing = false;
      } else settingsExit();                     // ...otherwise leave the menu
      break;
    case ST_DIAG:     settingsEnter(); break;     // diagnostic back to the menu
    default:          break;
  }
}

static void updateButton(uint32_t now) {
  bool pressed = (digitalRead(PIN_ENC_SW) == LOW);
  if (s_btnSwallow) {                 // this press only woke the screen — ignore it
    if (!pressed) { s_btnSwallow = false; s_btnDown = false; s_longFired = false; }
    return;
  }
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

  // Knob motion spins the swirl and (from identify) starts the stir.
  if (d != 0) {
    s_swirlAngle += STIR_ANGLE_STEP * (float)d;
    if (g_state == ST_IDENTIFY) {
      enterState(ST_STIRRING);
      s_stirZeroMs = now; s_stirMsgBand = -1;
      s_stirCps = 0; s_rateAcc = 0; s_rateWinMs = now;   // fresh rate window
    }
  }

  if (s_stirReady) return;          // armed: hold the full bar until press/combo change
  if (g_state != ST_STIRRING) return;

  // The bar ALWAYS drains; stirring adds against it, with diminishing returns
  // toward the top — so you fight the decay, and must stir ever faster near
  // full. The decay itself is the "grace": stop and it bleeds down, returning
  // to identify only once it hits zero.
  const int lvl = g_stirLevelIdx;

  // Estimate current stir rate (counts/sec) over a short window, so the cap
  // applies smoothly. (The window is reset whenever a stir/brew starts.)
  s_rateAcc += (d < 0) ? -d : d;
  if (now - s_rateWinMs >= 120) {
    s_stirCps = (int)(s_rateAcc * 1000 / (now - s_rateWinMs));
    s_rateAcc = 0; s_rateWinMs = now;
  }

  // Capped add vs. constant decay. Past cap/gain c/s, faster spinning adds
  // nothing, so the minimum fill time is fixed regardless of spin speed.
  float addRate = kStirGain[lvl] * (float)s_stirCps;
  if (addRate > kStirCap[lvl]) addRate = kStirCap[lvl];
  s_stirProgress += addRate * (1.0f - kStirResist[lvl] * s_stirProgress) * (float)dt / 1000.0f;
  s_stirProgress -= kStirDecay[lvl] * (float)dt / 1000.0f;

  // Escalating stir text: pick a fresh random line each time the band changes.
  int band = s_stirProgress >= 0.90f ? 3 : s_stirProgress >= 0.75f ? 2
           : s_stirProgress >= 0.50f ? 1 : 0;
  if (band != s_stirMsgBand) {
    s_stirMsgBand = band;
    s_stirMsg = kStirPools[band][esp_random() % kStirPoolN[band]];
  }

  if (s_stirProgress >= 1.0f) {
    s_stirProgress = 1.0f;
    s_stirReady = true;
  } else if (s_stirProgress > 0.0f) {
    s_stirZeroMs = now;                  // still brewing — keep the grace clock fresh
  } else {
    s_stirProgress = 0.0f;               // empty: hold a grace before giving up
    if (now - s_stirZeroMs >= STIR_ZERO_GRACE_MS) {
      if (s_sensedCombo != g_combo) enterCombo(s_sensedCombo);  // base changed -> resync
      else enterState(ST_IDENTIFY);                             // give up -> back to identify
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

// The recurring four-corner sparkle motif: one sparkle at each corner of the
// (x1,y1)-(x2,y2) rectangle. Callers pick their own blink period and radii.
static void drawCornerSparkles4(int x1, int y1, int x2, int y2, int r) {
  drawSparkle(x1, y1, r);  drawSparkle(x2, y1, r);
  drawSparkle(x1, y2, r);  drawSparkle(x2, y2, r);
}

static void drawCornerSparkles(uint32_t now) {
  drawCornerSparkles4(9, 10, 118, 53, ((now / 300) & 1) ? 1 : 0);
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
        // Safety invariant: l1 and tok are disjoint substrings of buf (<=47
        // chars, separated by at least one space), so l1 + ' ' + tok fits 48.
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

// Serif faces, largest first — the fit-fallback ladder for featured names
// (single ingredients and potion reveals).
static const uint8_t* const kSerifFonts[] = {
  u8g2_font_ncenB14_tr, u8g2_font_ncenB12_tr, u8g2_font_ncenB10_tr
};

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

static const char* ordinalWord(int i) {   // 0,1,2 -> position among seated bottles
  static const char* const w[3] = { "First ingredient", "Second ingredient", "Third ingredient" };
  return (i >= 0 && i < 3) ? w[i] : "";
}

static void renderIdentify(uint32_t now) {
  oled.clearBuffer();
  drawCornerSparkles(now);

  int count = 0, first = -1;
  for (int s = 0; s < 3; s++) if (g_combo & (1 << s)) { count++; if (first < 0) first = s; }

  if (count == 1) {
    // One ingredient: small "First ingredient" caption above a large name.
    oled.setFont(u8g2_font_5x8_tr);
    drawCenteredF("First ingredient", 13);
    drawFitted(kIngredients[g_universe][first], kSerifFonts, 3, 110, 42, 36, 52);
    int sr = ((now / 220) & 1) ? 2 : 1;
    drawSparkle(7, 40, sr);
    drawSparkle(121, 40, sr);
  } else {
    // Two/three ingredients: a small ordinal caption above each name, so it's
    // clear the ingredients STACK (you're building a combo, not replacing).
    const uint8_t* labelFont = (count == 3) ? u8g2_font_4x6_tr     : u8g2_font_5x8_tr;
    const uint8_t* nameFont  = (count == 3) ? u8g2_font_helvB08_tr : u8g2_font_ncenB10_tr;
    oled.setFont(nameFont);
    for (int s = 0; s < 3; s++)
      if ((g_combo & (1 << s)) && oled.getStrWidth(kIngredients[g_universe][s]) > 116)
        nameFont = u8g2_font_helvB08_tr;

    int y      = (count == 2) ? 16 : 9;    // first caption baseline
    int nameDy = (count == 2) ? 11 : 8;    // name baseline below its caption
    int blockH = (count == 2) ? 24 : 17;   // per-ingredient row height
    int ord = 0;
    for (int s = 0; s < 3; s++) {
      if (!(g_combo & (1 << s))) continue;
      oled.setFont(labelFont);
      drawCenteredF(ordinalWord(ord), y);
      oled.setFont(nameFont);
      const char* nm = kIngredients[g_universe][s];
      int w = oled.getStrWidth(nm);
      int x = (128 - w) / 2;
      int ny = y + nameDy;
      if (count == 2) { drawDiamond(x - 7, ny - 3); drawDiamond(x + w + 5, ny - 3); }
      oled.drawStr(x, ny, nm);
      y += blockH;
      ord++;
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
    drawCornerSparkles4(15, 14, 113, 50, sr);
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
  drawCenteredF(s_stirMsg, 62);
  oled.sendBuffer();
}

// Potion name in an elegant serif, scaled to the largest size that fits the
// cartouche on one or two lines (falls back through smaller faces).
static void drawPotionName(const char* name) {
  drawFitted(name, kSerifFonts, 3, 112, 39, 32, 47);
}

// --- Reveal animations: three bold, continuously-moving styles ------------
// `el` is time elapsed in the ANIM phase (spans REVEAL_ANIM_MS). Each keeps
// strong on-screen motion for the WHOLE phase so it can't read as frozen.
//
// Style 0: radar sweep — a fan of lines spins around the centre.
static void animStarburst(uint32_t el) {
  float ang = (float)el * 0.011f;
  for (int t = 0; t < 6; t++) {
    float a = ang - (float)t * 0.20f;
    oled.drawLine(64, 33,
                  64 + (int)lroundf(58.0f * cosf(a)),
                  33 + (int)lroundf(28.0f * sinf(a)));
  }
  oled.drawDisc(64, 33, 2);
}

// Style 1: brewing surge — liquid rises from the bottom and fills the screen.
static void animBubbles(uint32_t el) {
  float p = (float)el / (float)REVEAL_ANIM_MS * 1.2f;   // reach full a touch early, then hold
  if (p > 1.0f) p = 1.0f;
  int base = 63 - (int)lroundf(p * 63.0f);             // surface climbs all the way to the top
  for (int x = 2; x < 126; x++) {
    int surf = base + (int)lroundf(2.5f * sinf((float)x * 0.22f + (float)el * 0.010f));
    if (surf < 0) surf = 0;
    for (int y = surf; y < 64; y++)
      if (((x + y) & 1) == 0) oled.drawPixel(x, y);    // dithered fill
  }
}

// Style 2: magic pulse — bold rings continuously emanate from the centre.
static void animRings(uint32_t el) {
  for (int k = 0; k < 4; k++) {
    int rad = (((int)el / 11) + k * 16) % 64;
    if (rad > 3) { oled.drawCircle(64, 33, rad); oled.drawCircle(64, 33, rad - 1); }
  }
}

// Advance the reveal sequence: ANIM phase -> NAME phase -> idle. Each phase is
// timed from when IT began (g_stateMs), so durations compose, not race.
static void updateReveal(uint32_t now) {
  uint32_t el = now - g_stateMs;
  if (s_revealPhase == RP_ANIM) {
    if (el >= REVEAL_ANIM_MS) { s_revealPhase = RP_NAME; g_stateMs = now; }
  } else {  // RP_NAME
    // Hold elapsed: resync to the base — idle if it was cleared, straight
    // back to identify if bottles are still seated (re-brew without a jiggle).
    if (el >= REVEAL_NAME_MS) enterCombo(s_sensedCombo);
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
    drawCornerSparkles4(11, 11, 117, 53, ((now / 250) & 1) ? 1 : 0);
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

// Inverted title bar shared by the menu screens.
static void drawTitleBar(const char* title) {
  oled.drawBox(0, 0, 128, 13);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_helvB08_tr);
  oled.drawStr(4, 10, title);
  oled.setDrawColor(1);
}

static void renderSettings() {
  oled.clearBuffer();
  drawTitleBar("Settings");

  // Rows keep clear of the rightmost 4px, which belong to the scrollbar.
  const int VIS = 5, ROW_W = 124;
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
    if (sel && !editing) { oled.drawBox(0, y - 8, ROW_W, 10); oled.setDrawColor(0); }
    oled.drawStr(4, y, kMenu[i].label);
    char tmp[16];
    const char* v = menuValueStr(kMenu[i], tmp, sizeof(tmp));
    if (v && v[0]) {
      int vw = oled.getStrWidth(v);
      int vx = ROW_W - vw - 4;
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

  // Scrollbar: a hairline track with a thumb whose position tracks `first`,
  // so it's obvious there are more items than the five on screen.
  if (kMenuN > VIS) {
    const int ty = 15, th = 48;                             // track extent
    int thumbH = th * VIS / kMenuN;
    int thumbY = ty + (th - thumbH) * first / (kMenuN - VIS);
    oled.drawVLine(127, ty, th);
    oled.drawBox(125, thumbY, 3, thumbH);
  }
  oled.sendBuffer();
}

// Built-in live diagnostic (reachable from Settings -> Hardware Test).
static void renderDiag() {
  oled.clearBuffer();
  drawTitleBar("Hardware Test");

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
  uint32_t now = g_now;        // same clock everything else uses this tick
  switch (g_state) {
    case ST_IDLE:     renderIdle(now);     break;
    case ST_IDENTIFY: renderIdentify(now); break;
    case ST_STIRRING: renderStirring(now); break;
    case ST_REVEAL:   renderReveal(now);   break;
    case ST_SETTINGS: renderSettings();    break;
    case ST_DIAG:     renderDiag();        break;
  }
}

// Animated boot splash (~3s): the scene materialises from emanating rings, a
// starfield twinkles, "Welcome to the" slides in, "Alchemist's Study" grows in
// big, then motes orbit the title — all over a rising sparkle-chime. Boot-only,
// so the blocking delays here are fine (we're not in the main loop yet).
static void bootSplash() {
  if (!g_haveDisplay) return;

  static const uint8_t sx[12] = { 9, 119, 11, 117, 30, 98, 64, 20, 108, 64, 44, 84 };
  static const uint8_t sy[12] = { 11, 11, 53, 53, 7, 7, 5, 58, 58, 60, 6, 6 };
  auto stars = [&](int f) {                       // twinkle the scattered stars
    for (int i = 0; i < 12; i++) {
      int ph = (f + i * 5) & 7;
      int r = ph < 2 ? 2 : ph < 4 ? 1 : 0;
      if (r) drawSparkle(sx[i], sy[i], r);
    }
  };
  auto title = [&](const uint8_t* font, int wy) {  // welcome (sliding) + grown title
    oled.setFont(u8g2_font_helvR08_tr);
    drawCenteredF("Welcome to the", wy);
    oled.setFont(font);
    drawCenteredF("Alchemist's", 38);
    drawCenteredF("Study", 57);
  };

  // Phase 1: rings emanate from the centre as the scene materialises (~0.7s).
  for (int f = 0; f < 12; f++) {
    oled.clearBuffer();
    for (int k = 0; k < 3; k++) {
      int rad = (f * 5 + k * 16) % 48;
      if (rad > 2 && rad < 47) oled.drawCircle(64, 32, rad);
    }
    stars(f);
    oled.sendBuffer();
    if (!g_mute && f == 0) tone(PIN_BUZZER, 392);   // G4
    if (!g_mute && f == 6) tone(PIN_BUZZER, 523);   // C5
    delay(58);
  }

  // Phase 2: the frame draws on; "Welcome to the" slides down into place (~0.5s).
  for (int f = 0; f <= 10; f++) {
    oled.clearBuffer();
    drawFancyFrame();
    title(u8g2_font_ncenR08_tr, -4 + (17 * f) / 10);  // welcome slides -4 -> 13
    stars(f + 12);
    oled.sendBuffer();
    if (!g_mute && f == 0) tone(PIN_BUZZER, 587);   // D5
    delay(46);
  }

  // Phase 3: the title grows through serif sizes with a rising chime (~0.85s).
  const uint8_t* grow[4] = { u8g2_font_ncenR08_tr, u8g2_font_ncenB10_tr,
                             u8g2_font_ncenB12_tr, u8g2_font_ncenB14_tr };
  const int notes[4] = { 659, 784, 880, 1047 };     // E5 G5 A5 C6
  for (int s = 0; s < 4; s++) {
    for (int rep = 0; rep < 4; rep++) {
      oled.clearBuffer();
      drawFancyFrame();
      title(grow[s], 13);
      stars(s * 4 + rep);
      oled.sendBuffer();
      delay(52);
    }
    if (!g_mute) tone(PIN_BUZZER, notes[s]);
  }

  // Phase 4: hold with motes orbiting the title, then a final flourish (~0.9s).
  for (int f = 0; f < 16; f++) {
    oled.clearBuffer();
    drawFancyFrame();
    title(u8g2_font_ncenB14_tr, 13);
    stars(f);
    float a = (float)f * 0.45f;
    oled.drawDisc(64 + (int)lroundf(52.0f * cosf(a)), 32 + (int)lroundf(24.0f * sinf(a)), 1);
    oled.drawDisc(64 - (int)lroundf(52.0f * cosf(a)), 32 - (int)lroundf(24.0f * sinf(a)), 1);
    oled.sendBuffer();
    if (!g_mute && f == 1) tone(PIN_BUZZER, 1319);  // E6 sparkle
    if (!g_mute && f == 6) tone(PIN_BUZZER, 1047);  // settle on C6
    delay(56);
  }
  noTone(PIN_BUZZER);
}

// ---- Arduino entry points ----------------------------------------------
void setup() {
  Serial.begin(115200);
  // CRITICAL: USB-CDC Serial.write() blocks until a host drains the port. With
  // no monitor attached that stall (up to seconds) freezes the whole loop — it
  // was what made the reveal animation appear frozen and lagged input. A 0ms TX
  // timeout makes logging non-blocking (drops bytes if nobody's listening).
  Serial.setTxTimeoutMs(0);
  delay(200);
  Serial.println("\n=== Alchemist Study (ESP32-C3) booting ===");

  pinMode(PIN_REED_SLOT1, INPUT_PULLUP);
  pinMode(PIN_REED_SLOT2, INPUT_PULLUP);
  pinMode(PIN_REED_SLOT3, INPUT_PULLUP);
  pinMode(PIN_ENC_SW, INPUT_PULLUP);

  encoderBegin();   // internal pullups, ISR decode on every edge (quadrature.h)

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
  g_blankIdx   = prefs.getUChar("blank", 0);
  if (g_blankIdx >= kBlankN) g_blankIdx = 0;

  // OLED. Let U8g2 own the single Wire.begin(); we only preset the C3's I2C
  // pins (calling Wire.begin() ourselves AND letting U8g2 begin() again wedges
  // the core-3.x i2c-ng driver in ESP_ERR_INVALID_STATE). The bus runs at
  // 100 kHz — reliable on breadboard; a full-buffer send is ~30 ms, which is
  // why RENDER_INTERVAL_MS caps at ~30 fps. Set the clock AFTER begin():
  // U8g2's setBusClock() before begin() does not stick.
  Wire.setPins(PIN_OLED_SDA, PIN_OLED_SCL);
  oled.begin();            // inits the panel at U8g2's default address 0x3C
  Wire.setClock(100000);

  // Probe both common SSD1306 addresses. 0x3C was already initialised by
  // begin(); a panel strapped to 0x3D needs the address set and the init
  // sequence re-sent. If neither answers, run headless (skip drawing so we
  // don't flood the log with ESP_ERR_INVALID_STATE every frame).
  uint8_t oledAddr = 0;
  for (uint8_t a : {0x3C, 0x3D}) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { oledAddr = a; break; }
  }
  if (oledAddr == 0x3D) {
    oled.setI2CAddress(0x3D << 1);
    oled.initDisplay();     // what begin() did, re-aimed at 0x3D
    oled.clearDisplay();
    oled.setPowerSave(0);
  }
  g_haveDisplay = (oledAddr != 0);
  applyBrightness();

  // ---- Boot self-check: state what we found, loudly, and don't pretend. ----
  Serial.println("---- self-check ----");
  Serial.printf("  reeds:   GPIO%d/%d/%d (slots 1/2/3)\n",
                PIN_REED_SLOT1, PIN_REED_SLOT2, PIN_REED_SLOT3);
  Serial.printf("  encoder: A=GPIO%d B=GPIO%d SW=GPIO%d\n",
                PIN_ENC_A, PIN_ENC_B, PIN_ENC_SW);
  Serial.printf("  buzzer:  GPIO%d  (mute=%s)\n", PIN_BUZZER, g_mute ? "on" : "off");
  if (g_haveDisplay)
    Serial.printf("  OLED:    SDA=GPIO%d SCL=GPIO%d -> FOUND at 0x%02X [OK]\n",
                  PIN_OLED_SDA, PIN_OLED_SCL, oledAddr);
  else
    Serial.printf("  OLED:    SDA=GPIO%d SCL=GPIO%d -> NOT FOUND [FAIL]\n",
                  PIN_OLED_SDA, PIN_OLED_SCL);
  Serial.printf("  realm=%s  bright=%d  fw=%s\n",
                kUniverseName[g_universe], g_brightness, FW_VERSION);
  if (!g_haveDisplay) {
    Serial.println("  !! No OLED at 0x3C/0x3D. Running HEADLESS (no screen).");
    Serial.println("  !! Check VCC=3V3, GND, SDA->GPIO5, SCL->GPIO6; or run c3-hwcheck.");
    if (!g_mute)
      for (int i = 0; i < 3; i++) { tone(PIN_BUZZER, 180); delay(120); noTone(PIN_BUZZER); delay(90); }
  }
  Serial.println("--------------------");

  bootSplash();   // animated welcome + rising chime, then on to idle

  s_lastEncoder = g_encoderCount;
  g_now = millis();
  s_lastLoopMs = g_now;
  g_lastActivityMs = g_now;
  render();  // first paint (idle)
}

void loop() {
  g_now = millis();            // one time source for the whole tick
  uint32_t now = g_now;
  uint32_t dt = now - s_lastLoopMs;
  s_lastLoopMs = now;

  // Inputs: debounced reed combo, encoder/stir, button.
  uint8_t raw = readRawCombo();
  bool reedAct = (raw != s_rawComboLast);
  if (reedAct) {
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

  // Screen blanking: any encoder/reed/button input resets the idle timer and
  // wakes the panel. The gesture that wakes is swallowed (turn -> no stir,
  // press -> no menu) so waking doesn't also trigger an action.
  bool btnAct = (digitalRead(PIN_ENC_SW) == LOW);
  if (reedAct || d != 0 || btnAct) {
    g_lastActivityMs = now;
    if (g_screenOff) {
      if (g_haveDisplay) oled.setPowerSave(0);   // wake the panel
      g_screenOff = false;
      d = 0;                                      // swallow wake-turn
      if (btnAct) { s_btnSwallow = true; s_btnDown = true; }  // swallow wake-press
    }
  }

  switch (g_state) {
    case ST_IDENTIFY:
    case ST_STIRRING: updateStir(now, dt, d);     break;
    case ST_SETTINGS: if (d) updateMenuNav(d);    break;
    case ST_REVEAL:   updateReveal(now);          break;  // ANIM -> NAME -> idle
    default:          break;  // IDLE / DIAG: encoder not navigated
  }

  updateButton(now);
  updateAudio(now);

  // Blank the panel after the configured idle period (Settings -> Sleep).
  uint32_t blankMs = kBlankMs[g_blankIdx];
  if (blankMs && !g_screenOff && (uint32_t)(now - g_lastActivityMs) >= blankMs) {
    if (g_haveDisplay) oled.setPowerSave(1);   // panel off; buffer retained
    g_screenOff = true;
  }

  if (!g_screenOff && now - s_lastRenderMs >= RENDER_INTERVAL_MS) {
    s_lastRenderMs = now;
    render();
  }

  // Never let a degraded (screenless) run look healthy: nag periodically.
  static uint32_t s_lastWarnMs = 0;
  if (!g_haveDisplay && now - s_lastWarnMs >= 5000) {
    s_lastWarnMs = now;
    Serial.println("[warn] HEADLESS — no OLED at 0x3C/0x3D on SDA=5/SCL=6 (check wiring)");
  }
}
