// ============================================================
// Alchemist Study — ESP32-C3 Super Mini firmware
//
// Brain of a 3D-printed alchemist's study: reed switches sense which
// potion bottles are seated, an OLED names the resulting potion, a
// rotary encoder is the "stir" control, and a passive buzzer scores it.
//
// State machine: IDLE -> IDENTIFY -> STIRRING (-> RITUAL) -> REVEAL, plus
// STORY, SETTINGS + DIAG.
//   IDLE      empty base; a 3-panel home CAROUSEL (Place ingredients /
//             Begin Quest / Settings) — turn slides smoothly between panels,
//             press activates the one in view.
//   IDENTIFY  >=1 bottle seated; features the ingredient name(s) + sparkles.
//   STIRRING  the brewing mechanic scales with the ACT (= bottles seated):
//               act 1 (one bottle):  turn to fill the power bar (classic stir).
//               act 2 (two bottles): "align the essences" — steer your wave
//               into phase with a drifting one; the bar fills only aligned.
//             when full -> "Press to create" (acts 1-2) or the ritual (act 3).
//   RITUAL    act 3 (all three = the master potion): the Grand Brew — repeat
//             a growing incantation of turns/presses (Simon-style) to finish.
//             Story brews route the finished incantation to storyBrewResolve.
//   REVEAL    the potion is named with a random animation; auto-returns ~3s.
//   STORY     the adventure (entered from the carousel's Story Mode panel):
//             narration cards, two-way choices, and a turn-based battle whose
//             beats are authored so the player MUST brew a healing potion
//             (via the real IDENTIFY/STIRRING machinery) to win. Attack
//             damage is an honest 3D d20 roll. One act for now (Skyrim act 1).
//   SETTINGS  entered from the home carousel; realm / mute / brightness / etc.
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

// Act 2 — "align the essences" (two-ingredient brews). The knob shifts your
// wave's phase; the target wave drifts with a PERSISTENT heading (mostly keeps
// its direction, sometimes reverses) so it genuinely travels — a parked knob
// loses. Speed ramps up and the tolerance band tightens as the bar fills.
// See kAlign* (per stir level) below.
static constexpr float    ALIGN_KNOB_STEP   = 0.05f; // phase radians per encoder count
static constexpr uint32_t ALIGN_RETARGET_MS = 700;   // how often the drift picks a new heading
static constexpr float    ALIGN_FLIP_P      = 0.25f; // chance a new heading reverses direction
static constexpr float    ALIGN_RAMP        = 0.5f;  // extra drift speed gained by a full bar (x1.5 at top)
static constexpr float    ALIGN_TOL_SHRINK  = 0.15f; // fraction of tolerance lost by a full bar

// Act 3 — the Grand Brew ritual (three-ingredient master potion).
static constexpr uint32_t RIT_INTRO_MS         = 1800;  // "The Grand Brew" card
static constexpr uint32_t RIT_GLYPH_MS         = 650;   // per glyph while the incantation plays
static constexpr uint32_t RIT_GOOD_MS          = 900;   // "well stirred" between verses
static constexpr uint32_t RIT_MISS_MS          = 1200;  // "it resists" before a replay
static constexpr uint32_t RIT_ECHO_MS          = 280;   // let the last answer's note ring
                                                        // before the interlude jingle takes the buzzer
static constexpr uint32_t RIT_INPUT_TIMEOUT_MS = 12000; // stalled answer -> replay the verse
static constexpr int32_t  RIT_TURN_COUNTS      = 4;     // counts (~1 detent) per turn answer
static constexpr int      RIT_SEQ_LEN          = 6;     // full incantation length
static constexpr int      RIT_ROUNDS           = 4;     // verse lengths 3..6 (prefixes)

// Firmware version — keep in step with the git tag / GitHub release.
#define FW_VERSION "v0.1"

// ---- Hardware objects --------------------------------------------------
// Full-buffer (_F_) SSD1306 over hardware I2C.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, /*reset=*/U8X8_PIN_NONE);
Preferences prefs;
static bool g_haveDisplay = false;  // set at boot; if false we run headless

// ---- Runtime state -----------------------------------------------------
enum State { ST_IDLE, ST_IDENTIFY, ST_STIRRING, ST_RITUAL, ST_REVEAL, ST_STORY, ST_SETTINGS, ST_DIAG };
static uint32_t g_now      = 0;            // single time source: millis() sampled once per loop
static State    g_state    = ST_IDLE;
static uint32_t g_stateMs  = 0;            // g_now when the current state/phase began
static uint8_t  g_combo    = 0;            // committed combo (0..7)
static Universe g_universe = UNI_SKYRIM;

// Persisted settings + UI cursors.
static uint8_t  g_volume      = 3;         // 0 = mute .. 5 = full (LEDC duty)
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

// Home carousel: side-by-side panels on the idle screen, wrapping around at
// both ends. Turning slides smoothly between them: s_homeX is the viewport's
// offset into a circular HP_COUNT * PANEL_W strip, eased toward the selected
// panel along the shortest way round. To add a mode: add an enum entry here
// and a matching row in kHomePanels (defined with the other panel actions).
enum HomePanel { HP_PLACE = 0, HP_QUEST, HP_SETTINGS, HP_COUNT };
static constexpr int PANEL_W = 128;        // each panel is one screen wide
static int   s_homePanel = HP_PLACE;
static float s_homeX     = 0.0f;           // scroll offset into the strip (px)

// The current d20 value (1..20): the 3D die's face numbering is rotated so
// the landing face shows this. Set fresh for every battle attack roll.
static uint8_t s_questRoll = 14;

// Story mode: a data-driven scene graph + one reusable battle engine.
// A story is a TABLE of StoryNodes — cards, choices, battles, an end marker —
// linked by successor indices, so acts and universes are pure data. Choices
// may set bits in a generic flag set; later nodes read those bits back
// (a battle can grant the enemy first strike, the brew screen can surface an
// earned hint) — fold-back branching with no bespoke state per story.
//
// The battle engine: player dice are ALWAYS honest (never fudged); what's
// authored is the enemy's behaviour — its CRIT_ON_BITE'th bite always drops
// the player to CRIT_HP and numbs the sword arm (attacking numb is fatal),
// forcing the brew the fight demands. Brewing consumes a turn: the enemy
// still bites while you're at the cauldron. Enemy HP must exceed two
// attacks' max damage (2 x 7 = 14) so the crit lands before the enemy can
// die, and stay low enough that honest post-heal rolls finish within a turn
// or two.
// N_BREW is a story-level brew (outside any battle): the node's `battle` def
// supplies only the brew fields (title/combo/hint); success advances to
// nextA. A wrong potion is named and re-tried at leisure — UNLESS the node
// sets nextB, which makes the brew ONE-SHOT: the wrong pour routes there
// (the final exam pattern — the player who followed the clues brews right).
enum NodeKind { N_CARD, N_SPEAK, N_CHOICE, N_TUNE, N_SCENE, N_BREW, N_BATTLE, N_END };

struct BattleDef {
  const char* name;       // HP bar label
  const char* intro;      // opening message
  void (*sprite)(int cx, int cy, uint32_t now);  // idle animation
  int         enemyHP;    // see the crit-maths note above
  int         bite;       // enemy damage per turn
  uint8_t     brewCombo;  // the combo the fight demands
  const char* brewTitle;  // brew screen title bar
  const char* hint;       // recipe hint on the brew screen...
  uint8_t     hintFlag;   // ...shown if this flag was earned (0 = always)
  uint8_t     firstStrikeFlag;   // enemy acts first if this flag is set (0 = never)
};

struct StoryNode {
  NodeKind    kind;
  const char* title;      // card title / choice prompt / N_SPEAK speaker / N_TUNE caption
  const char* body;       // card body / speech (fit: ~22 chars/line, max 4 lines)
  const char* optA, *optB;   // choice options (bottom spinner; wraps to 2 lines)
  uint8_t     nextA;      // card/speak/tune press / choice A / battle WIN / scene end
  uint8_t     nextB;      // choice B / battle KO
  uint8_t     setFlag;    // N_CHOICE: flag bits set by choosing option B
  int8_t      heal;       // HP restored on ENTERING this node (99 = to full);
                          // a card that heals also shows the animated HP bar
  int8_t      costA, costB;  // N_CHOICE: gold cost per option (refused if broke)
  uint16_t    ms;         // N_SCENE: play this long, then auto-advance
  void (*art)(int cx, int cy, uint32_t now);  // N_SPEAK portrait / N_TUNE / N_SCENE
  const BattleDef* battle;   // N_BATTLE
};

enum BattlePhase { BP_INTRO, BP_CHOOSE, BP_ROLL, BP_HIT, BP_BITE, BP_MAULED,
                   BP_BREW_OK, BP_BREW_BAD };
static const StoryNode* s_story = nullptr;  // the active script
static int         s_node  = 0;             // current node index
static uint8_t     s_flags = 0;             // choice-outcome bits
static const BattleDef* s_bdef = nullptr;   // active battle definition
static BattlePhase s_bp    = BP_INTRO;
static bool     s_storyBrew = false;  // brewing inside the story (real IDENTIFY/STIRRING)
static bool     s_numb      = false;  // post-crit: attacking is fatal, must brew
static bool     s_healed    = false;  // brewed successfully THIS battle (skips the crit)
static int      s_choiceIdx = 0;      // option showing on the choice spinner
static int      s_php = 0, s_ehp = 0; // battle hit points (player / enemy)
static int      s_gold = 0;           // the purse — inventory system v0 (gold only)
static int      s_biteN = 0;          // enemy bites landed (CRIT_ON_BITE'th = the crit)
static char     s_bmsg[64] = "";      // battle message line
static Universe s_prevUniverse = UNI_SKYRIM;   // restored when the story ends
static constexpr int PLAYER_MAX_HP   = 30;
static constexpr int STORY_START_GOLD = 7;
static constexpr int CRIT_ON_BITE  = 2;   // this bite is the crit (unless already
static constexpr int CRIT_HP       = 6;   // brewed) and leaves the player here
// Cross-app reading minimum: every TIMED message holds a base beat plus
// reading time (~18 chars/sec). Press-gated screens don't need it; anything
// that auto-advances does — never flash text a reader can't finish.
static uint32_t readHoldMs(const char* s) {
  return 900 + (uint32_t)strlen(s) * 55;
}
// The attack roll is a full-screen pinball cutaway: tumble in -> zoom onto
// the face -> hold the landed number, then the damage resolves.
static constexpr uint32_t ROLL_TUMBLE_MS = 1100;
static constexpr uint32_t ROLL_ZOOM_MS   = 600;
static constexpr uint32_t BATTLE_ROLL_MS = 2400;  // total (the rest is the hold)
static constexpr float    HP_DRAIN_RATE  = 20.0f; // shown HP eases at this many HP/sec
static float s_phpShown = 0.0f, s_ehpShown = 0.0f; // animated bar values

// --- Skyrim Act 1 script. More acts append nodes; other universes get their
// own tables (storyBegin picks). Everything story-specific lives HERE.
// Act 1's beats: the road/Jarl choice -> the rat battle (forced healing
// brew) -> the inn (spend coin on mead or a sweetroll, then play the lute
// and/or sleep by the fire) -> act 2 tease.
static void drawRat(int cx, int cy, uint32_t now);      // sprites & scenes,
static void drawJarl(int cx, int cy, uint32_t now);     // defined with the
static void drawHealer(int cx, int cy, uint32_t now);   // rest of the art
static void drawSteward(int cx, int cy, uint32_t now);
static void drawLuteNotes(int cx, int cy, uint32_t now);
static void drawCampfire(int cx, int cy, uint32_t now);
static void drawSneak(int cx, int cy, uint32_t now);

enum { SF_JARL = 0x01 };   // saw the Jarl: hint earned, but ambushed at dusk

static const BattleDef kBtRat = {
  "Rat", "Why is this rat so big? Something unnatural here", drawRat,
  15, 7,
  1,                                   // 001 = Blue Mountain Flower = Minor Healing
  "Brew: healing potion",
  "'blue mountain flower mends flesh'", SF_JARL,
  SF_JARL,
};

// Brew-only defs (no battle fields used): N_BREW nodes borrow just the
// title/combo/hint. Hints always show, per the recipe-seeding rule.
static const BattleDef kBrewGoblet = {
  nullptr, nullptr, nullptr, 0, 0,
  3,                                   // 011 = Flower + Deathbell = Lingering Damage Poison
  "Brew: lingering poison",
  "'deathbell bound to the flower'", 0,
  0,
};
static const BattleDef kBrewPhantom = {
  nullptr, nullptr, nullptr, 0, 0,
  7,                                   // 111 = all three = Philter of the Phantom
  "Brew: the Philter",
  "'all three bottles as one'", 0,
  0,
};
static const BattleDef kBrewCounter = {
  nullptr, nullptr, nullptr, 0, 0,
  1,                                   // 001 = the flower — act 1's first recipe
  "Brew: the counter",
  // The final exam: the hint asks the QUESTION; the answer was the Jarl's
  // act 1 line, re-planted as the Afflicted's taunt on the cauldron card.
  "'what quenches a plague?'", 0,
  0,
};

// Node fields: kind, title, body, optA, optB, nextA, nextB, setFlag,
//              heal, costA, costB, ms, art, battle
static const StoryNode kStorySkyrim[] = {
  /*0 intro  */ { N_CARD, "The Alchemist's Quest",   // plants the grain (act 2)
                  "Whiterun sickens. Even the bread tastes wrong. The Jarl summons you.",
                  nullptr, nullptr, 1, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*1 choice */ { N_CHOICE, "How do you begin?", nullptr,
                  "Take the road", "Request audience with Jarl",
                  2, 3, SF_JARL, 0, 0, 0, 0, nullptr, nullptr },
  /*2 road   */ { N_CARD, "The Road",
                  "You take the road. In the tall grass, something stirs...",
                  nullptr, nullptr, 4, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*3 jarl   */ { N_SPEAK, "Jarl Balgruuf",   // bubble fits ~15 chars x 4 lines;
                  // recipe hint + the intrigue seed (act 2) in nine words
                  "Blue mountain flower mends flesh. Trust no one here.",
                  nullptr, nullptr, 4, 0, 0, 0, 0, 0, 0, drawJarl, nullptr },
  /*4 battle */ { N_BATTLE, nullptr, nullptr, nullptr, nullptr,
                  5, 6, 0, 0, 0, 0, 0, nullptr, &kBtRat },
  /*5 won    */ { N_CARD, "Victory!",   // press-gated clue: the rat ATE poison
                  "The rat lies dead - gums black with deathbell. Whiterun's gates ahead.",
                  nullptr, nullptr, 7, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*6 ko     */ { N_CARD, "Knocked Out",
                  "Darkness takes you... You wake by the roadside. Try again.",
                  nullptr, nullptr, 4, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*7 inn    */ { N_CARD, "The Bannered Mare",
                  "The innkeep eyes your purse. A hot meal or a drink?",
                  nullptr, nullptr, 8, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*8 buy    */ { N_CHOICE, "Spend your coin on...", nullptr,
                  "Nord mead: -3gp, +10HP", "Sweetroll: -5gp, +15HP",
                  9, 10, 0, 0, 3, 5, 0, nullptr, nullptr },
  /*9 mead   */ { N_CARD, "Nord Mead",
                  "It warms you to the toes.",
                  nullptr, nullptr, 11, 0, 0, 10, 0, 0, 0, nullptr, nullptr },
  /*10 sweet */ { N_CARD, "Sweetroll",
                  "Sweet as victory - and no thief in sight.",
                  nullptr, nullptr, 11, 0, 0, 15, 0, 0, 0, nullptr, nullptr },
  /*11 rest  */ { N_CHOICE, "The fire burns low...", nullptr,
                  "Play the lute", "Go to sleep",
                  12, 13, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*12 lute  */ { N_TUNE, "You strum an old tune",   // loops until pressed
                  nullptr, nullptr, nullptr, 11, 0, 0, 0, 0, 0, 0,
                  drawLuteNotes, nullptr },
  /*13 fire  */ { N_SCENE, nullptr, nullptr, nullptr, nullptr,   // pan + hold
                  14, 0, 0, 0, 0, 0, 6500, drawCampfire, nullptr },
  /*14 awake */ { N_CARD, "Act 2",   // the wound walks into act 2 (max 2 lines)
                  "You wake healed. Yet the rat bite weeps...",
                  nullptr, nullptr, 16, 0, 0, 99, 0, 0, 0, nullptr, nullptr },
  /*15 end   */ { N_END, nullptr, nullptr, nullptr, nullptr,
                  0, 0, 0, 0, 0, 0, 0, nullptr, nullptr },

  // ---- Act 2: The Steward's Goblet --------------------------------------
  /*16 diagnose*/ { N_SPEAK, "Healer Danica",   // the bite names the poison
                  "Deathbell rot. No rat carries this. Something FED it.",
                  nullptr, nullptr, 17, 0, 0, 0, 0, 0, 0, drawHealer, nullptr },
  /*17 granary*/ { N_CARD, "The Granary",       // the poison names the man
                  "Black petals in the grain. One man holds the key: the steward.",
                  nullptr, nullptr, 18, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*18 recipe */ { N_SPEAK, "Healer Danica",    // act 2 recipe, spoken first
                  "Deathbell kills quick. Bound to the flower, pain LINGERS.",
                  nullptr, nullptr, 19, 0, 0, 0, 0, 0, 0, drawHealer, nullptr },
  /*19 choice */ { N_CHOICE, "The steward...", nullptr,
                  "Confront him", "Watch the granary",
                  20, 21, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*20 denial */ { N_CARD, "Denial",
                  "'Prove it,' he smiles. Now he knows you know. The feast is your chance.",
                  nullptr, nullptr, 22, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*21 midnight*/{ N_CARD, "Midnight",
                  "Black petals fall from his sleeve into the grain. Now catch him publicly.",
                  nullptr, nullptr, 22, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*22 feast  */ { N_CARD, "The Feast",
                  "Tonight the steward pours for the Jarl's table. Your chance.",
                  nullptr, nullptr, 23, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*23 brew   */ { N_BREW, nullptr, nullptr, nullptr, nullptr,
                  24, 0, 0, 0, 0, 0, 0, nullptr, &kBrewGoblet },
  /*24 use    */ { N_CHOICE, "At the feast:", nullptr,
                  "Lace his goblet", "Draw your blade",
                  26, 25, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*25 seized */ { N_CARD, "Seized!",          // the blade is the trap here
                  "Guards see only a drawn blade. A cold night in the cells.",
                  nullptr, nullptr, 24, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*26 goblet */ { N_CARD, "The Goblet",
                  "He drinks - and blackens. The hall gasps. Dying, he laughs...",
                  nullptr, nullptr, 27, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*27 dying  */ { N_SPEAK, "The Steward",     // dying words name his master
                  "You fools! Peryite avenges his loyal servants!",
                  nullptr, nullptr, 28, 0, 0, 0, 0, 0, 0, drawSteward, nullptr },
  /*28 peryite*/ { N_CARD, "Peryite",          // stakes go realm-wide
                  "Plague god. His shrine smokes in the mountains. Act 3 awaits...",
                  nullptr, nullptr, 29, 0, 0, 0, 0, 0, 0, nullptr, nullptr },

  // ---- Act 3: The Cauldron -----------------------------------------------
  /*29 shrine */ { N_CARD, "The Shrine",
                  "Green smoke crowns the shrine. The Afflicted guard every path.",
                  nullptr, nullptr, 30, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*30 recipe */ { N_SPEAK, "Healer Danica",   // act 3 recipe, spoken first
                  "The Philter hides you. ALL THREE bottles, alchemist.",
                  nullptr, nullptr, 31, 0, 0, 0, 0, 0, 0, drawHealer, nullptr },
  /*31 philter*/ { N_BREW, nullptr, nullptr, nullptr, nullptr,
                  32, 0, 0, 0, 0, 0, 0, nullptr, &kBrewPhantom },
  /*32 sneak  */ { N_SCENE, nullptr, nullptr, nullptr, nullptr,   // invisible
                  33, 0, 0, 0, 0, 0, 5600, drawSneak, nullptr },
  /*33 cauldron*/{ N_CARD, "The Cauldron",     // the taunt IS the clue
                  "It seethes. The Afflicted chant: 'No flower mends THIS.' One pour - one chance.",
                  nullptr, nullptr, 34, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*34 use    */ { N_CHOICE, "The great cauldron:", nullptr,
                  "Tip it over", "Counter-brew it",
                  35, 36, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*35 spilled*/ { N_CARD, "Spilled!",         // force is the trap, one last time
                  "It floods the floor - the mist rises hungry. You flee to try again.",
                  nullptr, nullptr, 34, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*36 counter*/ { N_BREW, nullptr, nullptr, nullptr, nullptr,   // ONE SHOT:
                  37, 41, 0, 0, 0, 0, 0, nullptr, &kBrewCounter },  // wrong -> 41
  /*37 poured */ { N_CARD, "The Counter-Brew", // act 1's flower saves the realm
                  "The flower falls in. The cauldron clears... Peryite SCREAMS.",
                  nullptr, nullptr, 38, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*38 saved  */ { N_CARD, "Realm Saved",
                  "Skyrim drinks clean. The Jarl names you Alchemist of Whiterun.",
                  nullptr, nullptr, 39, 0, 0, 99, 0, 0, 0, nullptr, nullptr },
  /*39 jarl   */ { N_SPEAK, "Jarl Balgruuf",   // the act 1 line, paid off
                  "You mended more than flesh. Skyrim owes you, alchemist.",
                  nullptr, nullptr, 40, 0, 0, 0, 0, 0, 0, drawJarl, nullptr },
  /*40 the end*/ { N_CARD, "The End",          // and a tease for other realms
                  "Your study glows warm. Seven realms await new tales...",
                  nullptr, nullptr, 15, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*41 wrong  */ { N_CARD, "The Wrong Brew",   // the exam, failed
                  "The cauldron drinks it - and ROARS. The mist pours into the night.",
                  nullptr, nullptr, 42, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
  /*42 bad end*/ { N_CARD, "Bad Ending",       // fail forward: whisper the clue
                  "Skyrim's rivers run grey. Remember the Jarl's words... and try again.",
                  nullptr, nullptr, 15, 0, 0, 0, 0, 0, 0, nullptr, nullptr },
};

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

// Act 2 alignment model: your wave's phase chases a drifting target phase.
// Both are kept wrapped to [-pi, pi]; only their (wrapped) difference matters.
static float    s_alignPlayer = 0.0f;      // your phase (the knob moves this)
static float    s_alignTarget = 0.0f;      // the drifting phase to match
static float    s_alignVel    = 0.0f;      // current drift velocity (rad/s)
static uint32_t s_alignHeadMs = 0;         // when the drift last picked a heading
static float    s_alignErr    = 1.0f;      // |wrapped diff| / pi (0 = in phase)
static bool     s_alignOk     = false;     // inside the level's tolerance band

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

// Act 2 flavour text: one pool while hunting for the phase, one while holding
// it. A fresh random line is picked whenever the aligned/seeking state flips.
static const char* const kAlignSeek[] = {
  "align the essences", "seek the resonance", "feel for the current", "chase the harmony",
};
static const char* const kAlignHold[] = {
  "hold the resonance", "steady... steady", "they sing as one", "don't lose it now",
};
static int         s_alignMsgOk = -1;                  // -1 unset / 0 seeking / 1 holding
static const char* s_alignMsg   = kAlignSeek[0];

// reveal sub-phases (timed from g_stateMs; advancing a phase re-stamps it)
enum RevealPhase { RP_ANIM, RP_NAME };
static RevealPhase s_revealPhase = RP_ANIM;

// Act 3 ritual (Simon-style incantation). Sub-phases re-stamp g_stateMs.
enum RitSymbol { SYM_CW = 0, SYM_CCW = 1, SYM_PRESS = 2 };
enum RitPhase  { RI_INTRO, RI_SHOW, RI_INPUT, RI_GOOD, RI_MISS };
static RitPhase s_ritPhase    = RI_INTRO;
static uint8_t  s_ritSeq[RIT_SEQ_LEN];     // the full incantation (verses are prefixes)
static int      s_ritRound    = 0;         // current verse (0..RIT_ROUNDS-1)
static int      s_ritShowIdx  = -1;        // glyph last sounded while showing
static int      s_ritInputIdx = 0;         // how many answers given this verse
static int32_t  s_ritAccum    = 0;         // encoder counts toward a turn answer
static bool     s_ritDone     = false;     // final verse answered; GOOD leads to the reveal
static bool     s_ritChimed   = false;     // interlude jingle started (delayed past the echo)

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
static const Note MEL_TICK[]     = { {1175, 12} };                // carousel detent tick
// Ritual voices. Each incantation symbol has its own note, so the sequence can
// be memorised by ear as well as by eye (and answers echo the same note back).
static const Note MEL_SYM_CW[]    = { {880, 240} };               // turn right = high
static const Note MEL_SYM_CCW[]   = { {392, 240} };               // turn left  = low
static const Note MEL_SYM_PRESS[] = { {587, 240} };               // press      = mid
static const Note MEL_RITBEGIN[]  = { {330, 140}, {392, 140}, {494, 220} }; // mystic rise
// NOTE: no verse-complete jingle. The symbol notes ARE the puzzle (the final
// verse is answered by ear alone), so nothing melodic may share the channel —
// the RI_GOOD screen confirms visually and the last answer's echo rings free.
// Dice rattle: descending "thuds" roughly aligned with the tumble's bounces
// (impacts near t = 0, 1/3, 2/3, 1 of QUEST_TUMBLE_MS).
static const Note MEL_DICE_RATTLE[] = {
  {1900, 20}, {0, 447}, {1700, 25}, {0, 442}, {1500, 30}, {0, 437}, {1300, 35}
};
static const Note MEL_DICE_FANFARE[] = { {659, 80}, {784, 80}, {988, 80}, {1319, 220} };
// Battle beats: a low bite thud, a nastier crit, and a falling KO dirge.
static const Note MEL_BITE[] = { {170, 90}, {120, 140} };
static const Note MEL_CRIT[] = { {200, 90}, {150, 90}, {100, 220} };
static const Note MEL_KO[]   = { {392, 160}, {330, 160}, {262, 160}, {196, 320} };
// The lute: a slow, melancholy folk air in the Dorian mode (looped while the
// N_TUNE node is up) — firelight music, not a jig.
static const Note MEL_LUTE[] = {
  {330, 400}, {392, 400}, {440, 650}, {392, 400}, {440, 400}, {523, 650},
  {494, 400}, {440, 850}, {0, 250},
  {523, 400}, {587, 400}, {659, 650}, {587, 400}, {523, 400}, {440, 650},
  {392, 400}, {330, 400}, {440, 1100}, {0, 500},
};

static const Note* s_melody     = nullptr;
static uint8_t      s_melodyLen = 0;
static uint8_t      s_melodyIdx = 0;
static uint32_t     s_noteStartMs = 0;
static bool         s_melodyActive = false;
static bool         s_buzzerOn = false;
static uint16_t     s_trillHz  = 0;   // last trill pitch sent to tone() (0 = none)

// ---- Small shared helpers ----------------------------------------------
static int comboCount(uint8_t c) {         // bottles seated = the brewing "act"
  return ((c >> 0) & 1) + ((c >> 1) & 1) + ((c >> 2) & 1);
}

static float frand() {                     // uniform 0..1 from the HW RNG
  return (float)(esp_random() & 0xFFFF) / 65535.0f;
}

static float wrapPi(float a) {             // wrap an angle to [-pi, pi]
  while (a >  (float)M_PI) a -= 2.0f * (float)M_PI;
  while (a < -(float)M_PI) a += 2.0f * (float)M_PI;
  return a;
}

// All buzzer output funnels through here so Volume works. The passive buzzer
// is driven via LEDC directly (tone() hides the duty cycle) and loudness is
// the pulse width: kVolDuty maps Volume 0..5 onto a 10-bit duty, 512 being
// the 50% square that tone() used to produce. PIN_BUZZER is ledcAttach'd
// once in setup().
static const uint16_t kVolDuty[6] = { 0, 6, 20, 60, 180, 512 };

static void buzzTone(uint16_t f) {
  if (f == 0 || g_volume == 0) { ledcWriteTone(PIN_BUZZER, 0); return; }
  ledcWriteTone(PIN_BUZZER, f);
  ledcWrite(PIN_BUZZER, kVolDuty[g_volume]);
}
static void buzzOff() { ledcWriteTone(PIN_BUZZER, 0); }

static void playCurrentNote() {
  buzzTone(s_melody[s_melodyIdx].freq);
}

static void startMelody(const Note* m, uint8_t n, uint32_t now) {
  if (g_volume == 0) return;
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
  if (g_volume == 0) {
    if (s_buzzerOn || s_melodyActive) { buzzOff(); s_buzzerOn = false; s_melodyActive = false; }
    s_trillHz = 0;
    return;
  }
  if (s_melodyActive) {
    if (now - s_noteStartMs >= s_melody[s_melodyIdx].durMs) {
      s_melodyIdx++;
      if (s_melodyIdx >= s_melodyLen) {
        s_melodyActive = false;
        buzzOff();
      } else {
        s_noteStartMs = now;
        playCurrentNote();
      }
    }
    return;  // a melody owns the buzzer while it plays
  }

  // Brewing trill. Act 1/3: pitch rises with stir progress, small warble.
  // Act 2: the trill IS the hot/cold aid — closer to phase = higher and
  // steadier; far off = lower with a wide warble. It sounds from the first
  // turn (before any progress) because it's guidance, not garnish.
  const int range = PITCH_MAX_HZ - PITCH_MIN_HZ;
  bool act2 = (g_state == ST_STIRRING) && (comboCount(g_combo) == 2) && !s_stirReady;
  bool stirSound = (g_state == ST_STIRRING) && (act2 || s_stirProgress > 0.02f);
  if (stirSound) {
    int f, wob;
    if (act2) {
      f = PITCH_MIN_HZ + (int)((1.0f - s_alignErr) * range * 0.6f
                               + s_stirProgress * range * 0.4f);
      wob = s_alignOk ? 6 : 12 + (int)(s_alignErr * 40.0f);
    } else {
      f = PITCH_MIN_HZ + (int)(s_stirProgress * range);
      wob = 14;
    }
    f += ((now / 35) & 1) ? wob : -wob;
    if (f < 50) f = 50;
    if ((uint16_t)f != s_trillHz) {   // re-arm LEDC only when the pitch changes
      s_trillHz = (uint16_t)f;
      buzzTone(s_trillHz);
    }
    s_buzzerOn = true;
  } else if (s_buzzerOn) {
    buzzOff();
    s_buzzerOn = false;
    s_trillHz = 0;
  }
}

// ---- Input helpers -----------------------------------------------------
#ifdef BENCH_SIM_COMBO
// Dev-build bench shortcut: simulated bottle bits, OR'd into the raw reed
// read so latching/debounce/resync behave exactly as with real magnets.
// Cycled by pressing the Place panel; dropped by a long-press mid-brew.
static uint8_t s_simCombo = 0;
#endif

static uint8_t readRawCombo() {
  uint8_t c = 0;
  if (digitalRead(PIN_REED_SLOT1) == LOW) c |= 0x01;
  if (digitalRead(PIN_REED_SLOT2) == LOW) c |= 0x02;
  if (digitalRead(PIN_REED_SLOT3) == LOW) c |= 0x04;
#ifdef BENCH_SIM_COMBO
  c |= s_simCombo;
#endif
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
  if (c == 0) { s_homePanel = HP_PLACE; s_homeX = 0.0f; }  // fresh idle: panel 1
  // During a story brew an empty base just waits for ingredients (the story
  // owns the exits) — everywhere else an empty base means idle.
  enterState(c == 0 ? (s_storyBrew ? ST_IDENTIFY : ST_IDLE) : ST_IDENTIFY);
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
//   - STIRRING/RITUAL/REVEAL: combo is fully latched; only a genuinely new
//     arrangement (a bottle ADDED, or base cleared then refilled) starts fresh.
static void onSensedComboChange() {
  uint8_t raw = s_sensedCombo;

  // Menus (and the quest dice roll) own the screen and ignore the base
  // entirely; whatever is seated is resynced on exit (settingsExit, questExit
  // and the reveal end call enterCombo with the sensed combo). REVEAL falls
  // through to the latch logic below, so a flaky reed still can't disrupt it
  // but a genuinely added bottle restarts.
  if (g_state == ST_SETTINGS || g_state == ST_DIAG || g_state == ST_STORY) return;

  if (raw == 0) {                    // base emptied: latch it; never bail instantly
    s_baseEmptied = true;
    s_baseEmptyMs = g_now;
    return;
  }

  if (g_state == ST_IDLE || g_state == ST_IDENTIFY) {
    enterCombo(raw);                 // live: compose the combo before stirring
    return;
  }

  // STIRRING / RITUAL / REVEAL: latched — only a new arrangement restarts.
  bool addedNew = (raw & ~g_combo) != 0;
  if (s_baseEmptied || addedNew) enterCombo(raw);
}

// ---- Story mode (ST_STORY) -----------------------------------------------
static int navSteps(int32_t d);   // defined with the settings model below

// Phase/node transitions re-stamp g_stateMs, per the architecture rules.
static void battlePhase(BattlePhase p) {
  s_bp = p;
  g_stateMs = g_now;
  if (p == BP_CHOOSE) s_choiceIdx = s_numb ? 1 : 0;  // nudge toward the potion
}

static void battleReset() {
  s_php = PLAYER_MAX_HP;
  s_ehp = s_bdef->enemyHP;
  s_phpShown = (float)s_php;   // bars start full, no drain-in
  s_ehpShown = (float)s_ehp;
  s_biteN = 0;
  s_numb = false;
  s_healed = false;
  snprintf(s_bmsg, sizeof(s_bmsg), "%s", s_bdef->intro);
  s_bp = BP_INTRO;
}

// Story over (won or abandoned): restore the realm, resync to the base; if
// that lands on idle, stay on the carousel's Story panel.
static void storyEnd() {
  g_universe = s_prevUniverse;
  s_storyBrew = false;
  enterCombo(s_sensedCombo);
  if (g_state == ST_IDLE) { s_homePanel = HP_QUEST; s_homeX = (float)(PANEL_W * HP_QUEST); }
}

// THE story-graph step: enter a node by index. Battles arm themselves from
// their BattleDef; brews hand straight off to the cauldron; an entry heal
// applies (and its card shows the bar rising); the end marker routes home.
static void storyBrewStart();
static void storyGoto(int idx) {
  s_node = idx;
  g_stateMs = g_now;
  const StoryNode& n = s_story[idx];
  if (n.heal) {
    s_php += n.heal;                       // 99 = "restore to full" in practice
    if (s_php > PLAYER_MAX_HP) s_php = PLAYER_MAX_HP;
  }
  if      (n.kind == N_BATTLE) { s_bdef = n.battle; battleReset(); }
  else if (n.kind == N_BREW)   { s_bdef = n.battle; storyBrewStart(); }
  else if (n.kind == N_CHOICE) s_choiceIdx = 0;
  else if (n.kind == N_END)    storyEnd();
}

// The story plays in Skyrim for now regardless of the chosen realm (only a
// Skyrim script exists); the realm is restored on exit and NVS never touched.
// When more universes get scripts, pick the table by g_universe here.
static void storyBegin() {
  s_navAccum = 0;
  s_prevUniverse = g_universe;
  g_universe = UNI_SKYRIM;
  s_flags = 0;
  s_gold = STORY_START_GOLD;
  s_php = PLAYER_MAX_HP;
  s_phpShown = (float)s_php;
  s_storyBrew = false;
  s_story = kStorySkyrim;
  enterState(ST_STORY);
  storyGoto(0);
}

// The enemy's turn. Authored beat: the CRIT_ON_BITE'th bite is the crit that
// drops the player to CRIT_HP and numbs the sword arm — UNLESS they already
// brewed this battle. An early brewer has engaged the potion mechanic, so
// they fight it out on attrition instead (worst-case all-minimum rolls still
// win at 2 HP); without this the crit would land right after a spent heal
// and the "forced brew" beat became a death sentence.
static void doBite() {
  s_biteN++;
  if (s_biteN == CRIT_ON_BITE && !s_healed) {
    s_php = CRIT_HP;
    s_numb = true;
    snprintf(s_bmsg, sizeof(s_bmsg), "CRIT! The bite FESTERS! Your arm goes numb...");
    startMelody(MEL_CRIT, ARRAY_COUNT(MEL_CRIT), g_now);
  } else {
    s_php -= s_bdef->bite;
    if (s_php < 0) s_php = 0;
    snprintf(s_bmsg, sizeof(s_bmsg), "%s bites for %d!", s_bdef->name, s_bdef->bite);
    startMelody(MEL_BITE, ARRAY_COUNT(MEL_BITE), g_now);
  }
  battlePhase(BP_BITE);
}

static void chooseAttack() {
  if (s_numb) {                    // the lesson: you cannot fight this off
    s_php = 0;
    snprintf(s_bmsg, sizeof(s_bmsg), "Your numb arm fails! You are savaged!");
    startMelody(MEL_CRIT, ARRAY_COUNT(MEL_CRIT), g_now);
    battlePhase(BP_MAULED);
    return;
  }
  s_questRoll = (uint8_t)(1 + esp_random() % 20);   // always an honest roll
  startMelody(MEL_DICE_RATTLE, ARRAY_COUNT(MEL_DICE_RATTLE), g_now);
  battlePhase(BP_ROLL);
}

// Hand the screen to the real brew machinery (IDENTIFY/STIRRING) with the
// story flag up; onShortPress routes the finished stir back here.
static void storyBrewStart() {
  s_storyBrew = true;
  s_bmsg[0] = '\0';                        // clear any wrong-potion note
  enterCombo(s_sensedCombo);
}

static void storyBrewResolve() {
  const StoryNode& n = s_story[s_node];
  bool ok = (g_combo == s_bdef->brewCombo);
  Serial.printf("[story] brew %s (combo %u)\n", ok ? "OK" : "wrong", g_combo);

  if (n.kind == N_BREW) {
    if (ok) {
      s_storyBrew = false;
      startMelody(MEL_SUCCESS, ARRAY_COUNT(MEL_SUCCESS), g_now);
      enterState(ST_STORY);
      storyGoto(n.nextA);
    } else if (n.nextB) {                  // one-shot brew: the wrong pour lands
      s_storyBrew = false;
      startMelody(MEL_KO, ARRAY_COUNT(MEL_KO), g_now);
      enterState(ST_STORY);
      storyGoto(n.nextB);
    } else {                               // ordinary story brew: retry at leisure
      snprintf(s_bmsg, sizeof(s_bmsg), "That was %s!",
               kPotions[g_universe][g_combo]);
      startMelody(MEL_NOTREADY, ARRAY_COUNT(MEL_NOTREADY), g_now);
      enterCombo(s_sensedCombo);           // reset the stir, stay at the cauldron
    }
    return;
  }

  // Battle brew: consumes the turn either way; the enemy still bites.
  s_storyBrew = false;
  enterState(ST_STORY);
  if (ok) {
    s_php = PLAYER_MAX_HP;
    s_numb = false;
    s_healed = true;                       // mechanic engaged: no scripted crit
    snprintf(s_bmsg, sizeof(s_bmsg), "The draught mends you! Strength returns.");
    startMelody(MEL_SUCCESS, ARRAY_COUNT(MEL_SUCCESS), g_now);
    battlePhase(BP_BREW_OK);
  } else {
    snprintf(s_bmsg, sizeof(s_bmsg), "You made %s! No use here.",
             kPotions[g_universe][g_combo]);
    startMelody(MEL_NOTREADY, ARRAY_COUNT(MEL_NOTREADY), g_now);
    battlePhase(BP_BREW_BAD);
  }
}

// Backing out of the cauldron with nothing seated: in a battle it costs no
// turn; a story brew (N_BREW) is the road forward, so there's no backing out.
static void storyBrewCancel() {
  if (s_story[s_node].kind == N_BREW) return;
  s_storyBrew = false;
  enterState(ST_STORY);
  battlePhase(BP_CHOOSE);
}

// ---- Reveal kick-off (shared by the press path and the ritual) ----------
static void startReveal(uint32_t now) {
  s_revealPhase = RP_ANIM;
  s_revealStyle = (int)(esp_random() % 3);   // pick a random reveal animation
  enterState(ST_REVEAL);                     // stamps the phase clock
  startMelody(MEL_SUCCESS, ARRAY_COUNT(MEL_SUCCESS), now);
  Serial.printf("[reveal] %s\n", kPotions[g_universe][g_combo]);
}

// ---- Act 3: the Grand Brew ritual ---------------------------------------
// All three essences demand more than stirring: after the bar fills, the brew
// speaks a growing incantation of glyphs (turn right / turn left / press) and
// you must repeat it back. RIT_ROUNDS verses, each a longer prefix of the same
// RIT_SEQ_LEN-symbol sequence (classic Simon). A wrong answer just replays the
// verse — the master potion should feel earned, not punishing. Inside a story
// brew the finished incantation hands to storyBrewResolve, not the reveal.

static int ritRoundLen() { return s_ritRound + 3; }   // verse lengths 3, 4, 5, 6

static void playSymTone(uint8_t sym, uint32_t now) {
  switch (sym) {
    case SYM_CW:  startMelody(MEL_SYM_CW,  ARRAY_COUNT(MEL_SYM_CW),  now); break;
    case SYM_CCW: startMelody(MEL_SYM_CCW, ARRAY_COUNT(MEL_SYM_CCW), now); break;
    default:      startMelody(MEL_SYM_PRESS, ARRAY_COUNT(MEL_SYM_PRESS), now); break;
  }
}

// Begin (or replay) the current verse's SHOW phase. Re-stamps the phase clock;
// glyph i sounds/draws during elapsed [i*RIT_GLYPH_MS, (i+1)*RIT_GLYPH_MS).
static void ritShowBegin(uint32_t now) {
  s_ritPhase = RI_SHOW;
  g_stateMs = now;
  s_ritShowIdx = -1;
  s_ritInputIdx = 0;
  s_ritAccum = 0;
}

static void ritualEnter(uint32_t now) {
  for (int i = 0; i < RIT_SEQ_LEN; i++) s_ritSeq[i] = (uint8_t)(esp_random() % 3);
  s_ritRound = 0;
  s_ritDone = false;
  s_ritPhase = RI_INTRO;
  enterState(ST_RITUAL);
  startMelody(MEL_RITBEGIN, ARRAY_COUNT(MEL_RITBEGIN), now);
  Serial.println("[ritual] the Grand Brew begins");
}

// One answer (turn or press) during RI_INPUT. Echoes the symbol's note, then
// advances the verse, finishes the ritual, or flags a miss. The interlude
// jingle/buzz is NOT started here — startMelody would steal the buzzer from
// the echo just played; updateRitual starts it after RIT_ECHO_MS.
static void ritInput(uint8_t sym, uint32_t now) {
  if (s_ritPhase != RI_INPUT) return;
  playSymTone(sym, now);
  if (sym != s_ritSeq[s_ritInputIdx]) {
    s_ritPhase = RI_MISS;
    g_stateMs = now;
    s_ritChimed = false;
    return;
  }
  s_ritInputIdx++;
  g_stateMs = now;                      // each answer refreshes the stall timeout
  if (s_ritInputIdx < ritRoundLen()) return;
  s_ritPhase = RI_GOOD;                 // verse (maybe the whole incantation) done
  g_stateMs = now;
  s_ritChimed = false;
  s_ritDone = (s_ritRound >= RIT_ROUNDS - 1);
}

static void updateRitual(uint32_t now, int32_t d) {
  uint32_t el = now - g_stateMs;
  switch (s_ritPhase) {
    case RI_INTRO:
      if (el >= RIT_INTRO_MS) ritShowBegin(now);
      break;
    case RI_SHOW: {
      int idx = (int)(el / RIT_GLYPH_MS);
      int len = ritRoundLen();
      if (idx < len && idx != s_ritShowIdx) {
        s_ritShowIdx = idx;
        playSymTone(s_ritSeq[idx], now);
      }
      if (idx >= len) {                 // verse spoken -> your turn
        s_ritPhase = RI_INPUT;
        g_stateMs = now;
        s_ritInputIdx = 0;
        s_ritAccum = 0;
      }
      break;
    }
    case RI_INPUT:
      if (d != 0) {
        // A direction flip clears the accumulator so jiggle can't add up.
        if ((d > 0 && s_ritAccum < 0) || (d < 0 && s_ritAccum > 0)) s_ritAccum = 0;
        s_ritAccum += d;
        if      (s_ritAccum >=  RIT_TURN_COUNTS) { s_ritAccum = 0; ritInput(SYM_CW,  now); }
        else if (s_ritAccum <= -RIT_TURN_COUNTS) { s_ritAccum = 0; ritInput(SYM_CCW, now); }
      }
      if (now - g_stateMs >= RIT_INPUT_TIMEOUT_MS) ritShowBegin(now);  // lost? hear it again
      break;
    case RI_GOOD:
      // Deliberately silent (beyond the last answer's echo): melody would
      // muddy the symbol notes the player is memorising by ear.
      if (el >= RIT_GOOD_MS) {
        if (s_ritDone) {                         // incantation complete
          if (s_storyBrew) storyBrewResolve();   // the story judges the brew
          else startReveal(now);
        } else {
          s_ritRound++;
          ritShowBegin(now);
        }
      }
      break;
    case RI_MISS:
      if (!s_ritChimed && el >= RIT_ECHO_MS) {   // hear your wrong note, THEN the buzz
        s_ritChimed = true;
        startMelody(MEL_NOTREADY, ARRAY_COUNT(MEL_NOTREADY), now);
      }
      if (el >= RIT_MISS_MS) ritShowBegin(now);
      break;
  }
}

// Turning only matters when a choice spinner is on screen: each detent
// cycles the option shown on the bottom line (either direction works).
static void storyNav(int32_t d) {
  const StoryNode& n = s_story[s_node];
  bool choosing = (n.kind == N_CHOICE) ||
                  (n.kind == N_BATTLE && s_bp == BP_CHOOSE);
  if (!choosing) return;
  int steps = navSteps(d);
  if (steps) s_choiceIdx = ((s_choiceIdx + steps) % 2 + 2) % 2;
}

static void storyPress() {
  const StoryNode& n = s_story[s_node];
  switch (n.kind) {
    case N_CARD:
    case N_SPEAK:
      storyGoto(n.nextA);
      break;
    case N_TUNE:
      s_melodyActive = false;                // stop strumming mid-bar
      buzzOff();
      storyGoto(n.nextA);
      break;
    case N_SCENE:
      storyGoto(n.nextA);                    // impatient? skip the cinematic
      break;
    case N_BREW:
      break;   // unreachable: the brew states own the button while brewing
    case N_CHOICE: {
      int cost = (s_choiceIdx == 1) ? n.costB : n.costA;
      if (cost > s_gold) {                   // can't afford it: the "nope" buzz
        startMelody(MEL_NOTREADY, ARRAY_COUNT(MEL_NOTREADY), g_now);
        break;
      }
      s_gold -= cost;
      if (s_choiceIdx == 1) { s_flags |= n.setFlag; storyGoto(n.nextB); }
      else storyGoto(n.nextA);
      break;
    }
    case N_BATTLE:
      if (s_bp != BP_CHOOSE) break;          // beats play out on their own
      if (s_choiceIdx == 0) chooseAttack();
      else                  storyBrewStart();
      break;
    case N_END:
      break;                                 // transient — storyGoto routed home
  }
}

// Ease a shown HP value toward the real one at a fixed rate, so damage and
// healing visibly drain/refill the bar (and tick its number) instead of
// snapping — the change would otherwise be easy to miss.
static void easeHP(float* shown, int actual, uint32_t dt) {
  float target = (float)actual;
  float step = HP_DRAIN_RATE * (float)dt / 1000.0f;
  if (*shown > target) { *shown -= step; if (*shown < target) *shown = target; }
  else if (*shown < target) { *shown += step; if (*shown > target) *shown = target; }
}

// Timed story/battle beats. Shown HP always eases (heal cards animate their
// bar too). Tunes loop until pressed; scenes run out their clock; battle
// message phases hold, then hand the turn over (win -> nextA, KO -> nextB).
static void updateStory(uint32_t now, uint32_t dt) {
  easeHP(&s_phpShown, s_php, dt);
  easeHP(&s_ehpShown, s_ehp, dt);
  const StoryNode& n = s_story[s_node];
  if (n.kind == N_TUNE) {
    if (!s_melodyActive) startMelody(MEL_LUTE, ARRAY_COUNT(MEL_LUTE), now);
    return;
  }
  if (n.kind == N_SCENE) {
    if (now - g_stateMs >= n.ms) storyGoto(n.nextA);
    return;
  }
  if (n.kind != N_BATTLE) return;      // cards/choices advance on press
  uint32_t el = now - g_stateMs;
  switch (s_bp) {
    case BP_INTRO:
      if (el >= readHoldMs(s_bmsg)) {
        bool ambush = s_bdef->firstStrikeFlag && (s_flags & s_bdef->firstStrikeFlag);
        if (ambush && s_biteN == 0) doBite();
        else battlePhase(BP_CHOOSE);
      }
      break;
    case BP_ROLL:
      if (el >= BATTLE_ROLL_MS) {
        int dmg = 4 + (s_questRoll - 1) / 5;           // roll 1..20 -> 4..7
        s_ehp -= dmg;
        if (s_ehp < 0) s_ehp = 0;
        if (s_ehp == 0) {
          snprintf(s_bmsg, sizeof(s_bmsg), "Rolled %d: hit for %d - it falls!", s_questRoll, dmg);
          startMelody(MEL_DICE_FANFARE, ARRAY_COUNT(MEL_DICE_FANFARE), now);
        } else {
          snprintf(s_bmsg, sizeof(s_bmsg), "Rolled %d: you hit for %d!", s_questRoll, dmg);
          startMelody(MEL_TOGGLE, ARRAY_COUNT(MEL_TOGGLE), now);
        }
        battlePhase(BP_HIT);
      }
      break;
    case BP_HIT:
      if (el >= readHoldMs(s_bmsg)) {
        if (s_ehp <= 0) { startMelody(MEL_SUCCESS, ARRAY_COUNT(MEL_SUCCESS), now);
                          storyGoto(n.nextA); }        // won
        else doBite();
      }
      break;
    case BP_BITE:
      if (el >= readHoldMs(s_bmsg)) {
        if (s_php <= 0) { startMelody(MEL_KO, ARRAY_COUNT(MEL_KO), now);
                          storyGoto(n.nextB); }        // knocked out
        else battlePhase(BP_CHOOSE);
      }
      break;
    case BP_MAULED:
      if (el >= readHoldMs(s_bmsg)) { startMelody(MEL_KO, ARRAY_COUNT(MEL_KO), now);
                                      storyGoto(n.nextB); }
      break;
    case BP_BREW_OK:
    case BP_BREW_BAD:
      if (el >= readHoldMs(s_bmsg)) doBite();  // brewing consumed the turn
      break;
    case BP_CHOOSE:
      break;                                 // waiting on the player
  }
}

// ---- Settings model (reusable mini-menu) -------------------------------

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

// Act 2 "align the essences", per stir level. The bar fills only while your
// phase is inside the tolerance band around the drifting target, and drains
// while it isn't — so aligned-time to fill is ~1/fill sec plus hunting time.
// Drift speed is multiplied by up to (1 + ALIGN_RAMP) and the tolerance
// shrinks by ALIGN_TOL_SHRINK as the bar fills: the endgame is the fight.
// Tolerance sizing: the band must be wide enough to REACT inside — the time
// to drift across it (tol / ramped drift speed) should stay a few tenths of
// a second, or "aligned" only exists at bang-on and the game feels rigged.
static const float        kAlignTol[]   = { 0.80f, 0.60f, 0.45f };  // radians either side, before the shrink
static const float        kAlignDrift[] = { 0.90f, 1.40f, 2.00f };  // max drift speed (rad/s), before the ramp
static const float        kAlignFill[]  = { 0.30f, 0.22f, 0.16f };  // bar added per aligned sec
static const float        kAlignDrain[] = { 0.07f, 0.10f, 0.14f };  // bar drained per misaligned sec

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
static int  getVolume()      { return g_volume; }
static void setVolume(int v) {         // applied live: blip at the new loudness
  g_volume = (uint8_t)v;
  if (g_volume == 0) buzzOff();
  else startMelody(MEL_TOGGLE, ARRAY_COUNT(MEL_TOGGLE), g_now);
}
static void persistVolume()  { prefs.putUChar("vol", g_volume); }
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
static void settingsExit()  { s_navAccum = 0; s_menuEditing = false; enterCombo(s_sensedCombo);
                              // Back on the carousel, stay on the Settings panel.
                              if (g_state == ST_IDLE) { s_homePanel = HP_SETTINGS; s_homeX = (float)(PANEL_W * HP_SETTINGS); } }
static void diagEnter()     { s_navAccum = 0; s_menuEditing = false; enterState(ST_DIAG); }

// ---- Home carousel model -------------------------------------------------
// One row per panel: its renderer and what a press does. Order must match the
// HomePanel enum (static_assert below keeps them honest).
static void renderHomePlace(int dx, uint32_t now);
static void renderHomeQuest(int dx, uint32_t now);
static void renderHomeSettings(int dx, uint32_t now);
static void homePressNothing() {   // nothing to confirm until bottles arrive
  startMelody(MEL_NOTREADY, ARRAY_COUNT(MEL_NOTREADY), g_now);
}

#ifdef BENCH_SIM_COMBO
// Bench shortcut on the Place panel: SHORT press fakes two bottles (act 2's
// align game), LONG press fakes all three (stir -> the Grand Brew). Long-press
// mid-brew drops the fakes again (see onLongPress). Turn to start brewing.
static void homePressSimCombo() {
  s_simCombo = 0x03;
  startMelody(MEL_TOGGLE, ARRAY_COUNT(MEL_TOGGLE), g_now);
  Serial.println("[sim] two bottles faked -> act 2");
}
#endif

struct HomePanelDef {
  void (*render)(int dx, uint32_t now);   // draw the panel shifted by dx
  void (*press)();                        // short press while it's in view
};
static const HomePanelDef kHomePanels[] = {
#ifdef BENCH_SIM_COMBO
  { renderHomePlace,    homePressSimCombo },  // HP_PLACE (dev: fake bottles)
#else
  { renderHomePlace,    homePressNothing },  // HP_PLACE
#endif
  { renderHomeQuest,    storyBegin       },  // HP_QUEST
  { renderHomeSettings, settingsEnter    },  // HP_SETTINGS
};
static_assert(ARRAY_COUNT(kHomePanels) == HP_COUNT,
              "kHomePanels must have one row per HomePanel entry");

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
  { "Volume",       K_RANGE,  getVolume,    setVolume,    persistVolume,    nullptr,       0,         0, 5, nullptr,    nullptr      },
  { "Bright",       K_RANGE,  getBright,    setBright,    persistBright,    nullptr,       0,         1, 5, nullptr,    nullptr      },
  { "Difficulty",   K_CHOICE, getStirLevel, setStirLevel, persistStirLevel, kStirLabels,   kStirN,    0, 0, nullptr,    nullptr      },
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

// ---- Home carousel navigation -------------------------------------------
// Turn on the idle screen: move between panels, wrapping at both ends (left
// from the first panel arrives at the last, and vice versa).
static void updateHomeNav(int32_t d) {
  int steps = navSteps(d);
  if (!steps) return;
  int p = ((s_homePanel + steps) % HP_COUNT + HP_COUNT) % HP_COUNT;
  if (p != s_homePanel) {
    s_homePanel = p;
    startMelody(MEL_TICK, ARRAY_COUNT(MEL_TICK), g_now);
  }
}

// Ease the scroll offset toward the selected panel along the SHORTEST way
// around the circular strip (framerate-independent exponential ease, ~90 ms
// time constant — quick but visibly a slide).
static void updateHomeScroll(uint32_t dt) {
  const float W = (float)(HP_COUNT * PANEL_W);
  float target = (float)(s_homePanel * PANEL_W);
  float diff = target - s_homeX;
  if (diff >  W * 0.5f) diff -= W;      // the other way round is shorter
  if (diff < -W * 0.5f) diff += W;
  if (fabsf(diff) < 0.5f) { s_homeX = target; return; }
  float k = 1.0f - expf(-(float)dt / 90.0f);
  s_homeX += diff * k;
  if (s_homeX < 0.0f) s_homeX += W;     // keep the offset inside the strip
  if (s_homeX >= W)   s_homeX -= W;
}

// ---- Button actions (state-aware) --------------------------------------
static void onShortPress(uint32_t now) {
  switch (g_state) {
    case ST_IDLE:
      kHomePanels[s_homePanel].press();   // activate the panel in view
      break;
    case ST_IDENTIFY:
    case ST_STIRRING:
      if (g_combo == 0) {
        if (s_storyBrew) storyBrewCancel();  // empty cauldron: back to the fight
        break;
      }
      if (s_stirReady) {
        if (s_storyBrew) { storyBrewResolve(); break; }  // story judges the brew
        startReveal(now);
      } else {
        startMelody(MEL_NOTREADY, ARRAY_COUNT(MEL_NOTREADY), now);  // stir first
      }
      break;
    case ST_RITUAL:
      if (s_ritPhase == RI_INTRO) ritShowBegin(now);   // impatient? skip the card
      else ritInput(SYM_PRESS, now);                   // ignored outside RI_INPUT
      break;
    case ST_SETTINGS:
      menuPress();
      break;
    case ST_STORY:
      storyPress();
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
    case ST_STORY:    storyEnd();      break;     // abandon the quest
#ifdef BENCH_SIM_COMBO
    case ST_IDLE:                                 // Place panel: fake all three
      if (s_homePanel == HP_PLACE) {
        s_simCombo = 0x07;
        startMelody(MEL_TOGGLE, ARRAY_COUNT(MEL_TOGGLE), g_now);
        Serial.println("[sim] three bottles faked -> act 3");
      }
      break;
#endif
    case ST_IDENTIFY:
    case ST_STIRRING:
      if (s_storyBrew) storyEnd();                // abandon mid-brew too
#ifdef BENCH_SIM_COMBO
      else s_simCombo = 0;                        // drop simulated bottles -> idle
#endif
      break;
    case ST_RITUAL:
      if (s_storyBrew) storyEnd();                // abandon mid-brew too
      else {
#ifdef BENCH_SIM_COMBO
        s_simCombo = 0;                           // drop simulated bottles too
#endif
        enterCombo(s_sensedCombo);                // abandon the ritual, resync
      }
      break;
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

  // Story brew with nothing seated: just wait — a stir can't start until an
  // ingredient arrives (reeds drive enterCombo), and the story owns the exits.
  if (s_storyBrew && g_combo == 0) return;

  // Identify resilience: if the base is physically empty we keep showing the
  // combo (so turning still starts the stir) until a grace period lapses; only
  // then fall back to idle (or, mid-story-brew, to the empty brew prompt).
  if (g_state == ST_IDENTIFY && g_combo != 0 && s_sensedCombo == 0 &&
      (uint32_t)(now - s_baseEmptyMs) >= REED_GRACE_MS) {
    enterCombo(0);
    return;
  }

  // Knob motion spins the swirl and (from identify) starts the stir.
  if (d != 0) {
    s_swirlAngle += STIR_ANGLE_STEP * (float)d;
    if (g_state == ST_IDENTIFY) {
      enterState(ST_STIRRING);
      s_stirZeroMs = now; s_stirMsgBand = -1;
      s_stirCps = 0; s_rateAcc = 0; s_rateWinMs = now;   // fresh rate window
      // Act 2 alignment: start out of phase (1.2..3.2 rad off) with a fresh
      // drift heading, so there's always a hunt before the bar can fill.
      s_alignPlayer = 0.0f;
      s_alignTarget = 1.2f + frand() * 2.0f;
      s_alignVel    = 0.0f;
      s_alignHeadMs = 0;                 // forces a heading pick on the first tick
      s_alignErr    = 1.0f;
      s_alignOk     = false;
      s_alignMsgOk  = -1;
    }
    s_stirZeroMs = now;                  // turning counts as activity: no mid-hunt fizzle
  }

  if (s_stirReady) return;          // armed: hold the full bar until press/combo change
  if (g_state != ST_STIRRING) return;

  const int lvl = g_stirLevelIdx;
  const int act = comboCount(g_combo);   // bottles seated pick the mechanic

  if (act == 2) {
    // ---- Act 2: align the essences. The knob shifts your phase; the target
    // phase drifts on a heading re-rolled every ALIGN_RETARGET_MS — but the
    // heading is PERSISTENT (usually keeps its direction, ALIGN_FLIP_P chance
    // of reversing), so the target genuinely travels instead of random-walking
    // in place: leave the knob parked and it escapes. In tolerance the bar
    // fills; out of it the bar drains — no spin speed helps, only tracking.
    s_alignPlayer = wrapPi(s_alignPlayer + ALIGN_KNOB_STEP * (float)d);
    if (now - s_alignHeadMs >= ALIGN_RETARGET_MS) {
      s_alignHeadMs = now;
      float mag = (0.5f + 0.5f * frand()) * kAlignDrift[lvl];
      float dir = (s_alignVel == 0.0f) ? ((esp_random() & 1) ? 1.0f : -1.0f)
                : (frand() < ALIGN_FLIP_P ? -1.0f : 1.0f) * copysignf(1.0f, s_alignVel);
      s_alignVel = dir * mag;
    }
    // The endgame is the fight: drift speeds up and the band tightens with
    // the bar, so the last stretch demands real tracking, not luck.
    float ramp = 1.0f + ALIGN_RAMP * s_stirProgress;
    s_alignTarget = wrapPi(s_alignTarget + s_alignVel * ramp * (float)dt / 1000.0f);

    float tol  = kAlignTol[lvl] * (1.0f - ALIGN_TOL_SHRINK * s_stirProgress);
    float diff = wrapPi(s_alignPlayer - s_alignTarget);
    s_alignErr = fabsf(diff) / (float)M_PI;
    s_alignOk  = fabsf(diff) <= tol;
    s_stirProgress += (s_alignOk ?  kAlignFill[lvl]
                                 : -kAlignDrain[lvl]) * (float)dt / 1000.0f;

    // Caption flips between "seek" and "hold" pools with the aligned state.
    int ok = s_alignOk ? 1 : 0;
    if (ok != s_alignMsgOk) {
      s_alignMsgOk = ok;
      s_alignMsg = ok ? kAlignHold[esp_random() % ARRAY_COUNT(kAlignHold)]
                      : kAlignSeek[esp_random() % ARRAY_COUNT(kAlignSeek)];
    }
  } else {
    // ---- Acts 1 & 3: the classic stir. The bar ALWAYS drains; stirring adds
    // against it, with diminishing returns toward the top — so you fight the
    // decay, and must stir ever faster near full. The decay itself is the
    // "grace": stop and it bleeds down, returning to identify at zero.

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
  }

  if (s_stirProgress >= 1.0f) {
    s_stirProgress = 1.0f;
    if (act == 3) ritualEnter(now);      // the master potion demands the Grand Brew
    else          s_stirReady = true;    // acts 1-2: arm "Press to create"
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

// Centered with a horizontal offset — used by the home carousel to slide a
// whole panel's content sideways (U8g2 clips at the buffer edge for free).
static void drawCenteredFX(const char* s, int y, int dx) {
  oled.drawStr((128 - oled.getStrWidth(s)) / 2 + dx, y, s);
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
static void drawTwinkles(uint32_t now, int dx = 0) {
  for (auto& s : kStars) {
    int ph = (int)((now / 110 + s.phase * 5) % 16);
    int tri = ph < 8 ? ph : 15 - ph;       // 0..7..0
    drawSparkle(s.x + dx, s.y, tri / 3);   // radius 0..2
  }
}

// U8g2's drawLine takes UNSIGNED coordinates: anything even partly off-screen
// wraps to ~65k and Bresenham smears a stray line across the buffer (this was
// visible as horizontal streaks when the die entered from off-screen). So all
// line drawing that can leave the screen goes through this Liang-Barsky clip.
static void drawLineClipped(float x0, float y0, float x1, float y1) {
  float dx = x1 - x0, dy = y1 - y0;
  float t0 = 0.0f, t1 = 1.0f;
  const float p[4] = { -dx, dx, -dy, dy };
  const float q[4] = { x0, 127.0f - x0, y0, 63.0f - y0 };
  for (int i = 0; i < 4; i++) {
    if (p[i] == 0.0f) { if (q[i] < 0.0f) return; continue; }
    float r = q[i] / p[i];
    if (p[i] < 0.0f) { if (r > t1) return; if (r > t0) t0 = r; }
    else             { if (r < t0) return; if (r < t1) t1 = r; }
  }
  oled.drawLine((u8g2_uint_t)lroundf(x0 + t0 * dx), (u8g2_uint_t)lroundf(y0 + t0 * dy),
                (u8g2_uint_t)lroundf(x0 + t1 * dx), (u8g2_uint_t)lroundf(y0 + t1 * dy));
}

// ---- The 3D d20: an icosahedron wireframe --------------------------------
// Golden-ratio vertex table (coordinate magnitude sqrt(1 + phi^2) ~= 1.902)
// and the standard 20-face index list, wound consistently so the SIGN of each
// face's projected area says which way it faces — back faces aren't drawn,
// which is what makes the wireframe read as a solid object.
static constexpr float PHI = 1.618034f;
static const float kIcoV[12][3] = {
  { -1,  PHI, 0 }, { 1,  PHI, 0 }, { -1, -PHI, 0 }, { 1, -PHI, 0 },
  { 0, -1,  PHI }, { 0, 1,  PHI }, { 0, -1, -PHI }, { 0, 1, -PHI },
  {  PHI, 0, -1 }, {  PHI, 0, 1 }, { -PHI, 0, -1 }, { -PHI, 0, 1 },
};
static const uint8_t kIcoF[20][3] = {
  { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 },
  { 1, 5, 9 }, { 5, 11, 4 }, { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 },
  { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 }, { 3, 8, 9 },
  { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 },
};

// Orientation (Rx then Ry) that points the {0,11,5} face normal (-1,1,1)/√3
// straight at the camera — the "landed, face up" pose the roll settles into.
static constexpr float D20_FACE_AX = 0.7854f;   // pi/4
static constexpr float D20_FACE_AY = 0.6155f;   // atan(1/sqrt(2))

// Fixed scrambled numbering for the 20 faces (like a real die's layout). At
// draw time it's rotated so face 0 — the face the roll lands on — carries
// this quest's rolled number; every other face keeps a stable number relative
// to it, so numbers travel WITH their faces as the die tumbles.
static const uint8_t kD20FaceNum[20] = {
  20, 8, 14, 2, 16, 10, 4, 18, 6, 12, 1, 13, 7, 19, 5, 11, 17, 3, 15, 9
};

// Rotate the icosahedron about all three axes (Rx, then Ry, then Rz), apply
// weak perspective, and draw the front-facing faces' edges. Faces that are
// big enough on screen also get their number at the projected centroid, in a
// font scaled to the face — so the numbers sit ON the die. (Leaves the U8g2
// font changed; set your font after calling.) `r` is the on-screen
// circumradius in pixels.
static void drawD20Tumbling(int cx, int cy, int r, float ax, float ay, float az) {
  float sa = sinf(ax), ca = cosf(ax);
  float sb = sinf(ay), cb = cosf(ay);
  float sc = sinf(az), cc = cosf(az);
  const float k = (float)r / 1.902f;         // model radius -> pixels
  float px[12], py[12];
  for (int i = 0; i < 12; i++) {
    float x = kIcoV[i][0], y = kIcoV[i][1], z = kIcoV[i][2];
    float y1 = y * ca - z * sa,  z1 = y * sa + z * ca;    // Rx
    float x2 = x * cb + z1 * sb, z2 = -x * sb + z1 * cb;  // Ry
    float x3 = x2 * cc - y1 * sc, y3 = x2 * sc + y1 * cc; // Rz
    float f = 4.5f / (4.5f - z2);            // weak perspective (z in ±1.9)
    px[i] = (float)cx + x3 * k * f;
    py[i] = (float)cy + y3 * k * f;
  }
  for (int fc = 0; fc < 20; fc++) {
    int a = kIcoF[fc][0], b = kIcoF[fc][1], c = kIcoF[fc][2];
    float area = (px[b] - px[a]) * (py[c] - py[a]) -
                 (px[c] - px[a]) * (py[b] - py[a]);
    if (area <= 0.0f) continue;              // back face
    drawLineClipped(px[a], py[a], px[b], py[b]);
    drawLineClipped(px[b], py[b], px[c], py[c]);
    drawLineClipped(px[c], py[c], px[a], py[a]);

    // Number on the face, once it's big enough to read. `area` is 2x the
    // projected triangle area; the landed front face at r=28 is ~1700, the
    // small home-screen icon tops out ~220 (below threshold, so no numbers).
    if (area < 450.0f) continue;
    int n = ((kD20FaceNum[fc] - kD20FaceNum[0] + (int)s_questRoll - 1) % 20 + 20) % 20 + 1;
    char buf[3];
    snprintf(buf, sizeof(buf), "%d", n);
    const uint8_t* fnt;
    int capHalf;                             // half the digit height, to centre
    if      (area >= 1200.0f) { fnt = u8g2_font_ncenB14_tr; capHalf = 7; }
    else if (area >=  700.0f) { fnt = u8g2_font_ncenB10_tr; capHalf = 5; }
    else                      { fnt = u8g2_font_5x8_tr;     capHalf = 3; }
    oled.setFont(fnt);
    int gx = (int)lroundf((px[a] + px[b] + px[c]) / 3.0f);
    int gy = (int)lroundf((py[a] + py[b] + py[c]) / 3.0f);
    int w = oled.getStrWidth(buf);
    int x = gx - w / 2, ybl = gy + capHalf;
    // drawStr coords are unsigned too — skip a number that would leave the screen
    if (x >= 0 && x + w < 128 && ybl - 2 * capHalf >= 0 && ybl <= 63)
      oled.drawStr(x, ybl, buf);
  }
}

// A small gear: ring + slowly-orbiting teeth + hub hole (Settings panel icon).
static void drawGear(int cx, int cy, uint32_t now) {
  oled.drawCircle(cx, cy, 8);
  oled.drawCircle(cx, cy, 3);
  float spin = (float)now * 0.0012f;
  for (int k = 0; k < 8; k++) {
    float a = spin + (float)k * ((float)M_PI / 4.0f);
    int tx = cx + (int)lroundf(10.0f * cosf(a));
    int ty = cy - (int)lroundf(10.0f * sinf(a));
    oled.drawBox(tx - 1, ty - 1, 3, 3);
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

// --- Home carousel panels: each draws its 128px page shifted by dx ---------
static void renderHomePlace(int dx, uint32_t now) {
  drawTwinkles(now, dx);
  drawMoon(15 + dx, 15, 6);
  oled.setFont(u8g2_font_ncenB12_tr);
  drawCenteredFX("Place the", 26, dx);
  drawCenteredFX("ingredients", 42, dx);
  oled.drawHLine(28 + dx, 48, 72);
  drawDiamond(22 + dx, 48); drawDiamond(106 + dx, 48);
  oled.setFont(u8g2_font_5x8_tr);
  drawCenteredFX("turn to explore", 57, dx);
}

static void renderHomeQuest(int dx, uint32_t now) {
  // The d20 tumbles lazily in 3D, teasing a roll. Time wraps every 62832 ms
  // (= 2*pi * 10 s), where every 0.1-multiple spin rate completes whole
  // turns — so the angles stay small for sinf/cosf and the wrap is seamless.
  float t = (float)(now % 62832U) * 0.001f;
  drawD20Tumbling(64 + dx, 16, 11, 0.9f * t, 0.7f * t, 0.5f * t);
  int sr = ((now / 260) & 1) ? 2 : 1;
  drawSparkle(38 + dx, 12, sr); drawSparkle(90 + dx, 12, sr);
  oled.setFont(u8g2_font_ncenB12_tr);
  drawCenteredFX("Story Mode", 44, dx);
  oled.setFont(u8g2_font_5x8_tr);
  drawCenteredFX("press to begin", 57, dx);
}

static void renderHomeSettings(int dx, uint32_t now) {
  drawGear(64 + dx, 16, now);
  oled.setFont(u8g2_font_ncenB12_tr);
  drawCenteredFX("Settings", 44, dx);
  oled.setFont(u8g2_font_5x8_tr);
  drawCenteredFX("press to enter", 57, dx);
}

// The idle screen is the home carousel. Panels sit side by side in a circular
// HP_COUNT * PANEL_W strip; s_homeX is the viewport's offset into it, so
// mid-slide the outgoing and incoming panels are both visible, moving
// together — including across the wrap seam (last panel <-> first). Page dots
// + edge chevrons are a fixed overlay (they don't slide).
static void renderHome(uint32_t now) {
  oled.clearBuffer();
  const int W = HP_COUNT * PANEL_W;
  int scroll = (int)lroundf(s_homeX) % W;
  for (int p = 0; p < HP_COUNT; p++) {
    int dx = p * PANEL_W - scroll;
    if (dx < -W / 2) dx += W;                // nearest copy on the loop
    if (dx >= W / 2) dx -= W;
    if (dx <= -PANEL_W || dx >= PANEL_W) continue;   // fully off-screen
    kHomePanels[p].render(dx, now);
  }
  const int dotStep = 12;                    // page dots, centered as a group
  int dotX = (128 - (HP_COUNT - 1) * dotStep) / 2;
  for (int i = 0; i < HP_COUNT; i++, dotX += dotStep) {
    if (i == s_homePanel) oled.drawBox(dotX - 1, 60, 3, 3);
    else                  oled.drawPixel(dotX, 61);
  }
  // Chevrons both sides — the carousel wraps, so there's always a next panel.
  oled.drawLine(3, 28, 1, 31);     oled.drawLine(1, 31, 3, 34);
  oled.drawLine(124, 28, 126, 31); oled.drawLine(126, 31, 124, 34);
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

// Power bar shared by the stirring screens — fills with s_stirProgress.
static void drawPowerBar() {
  const int bx = 14, by = 41, bw = 100, bh = 10;
  oled.drawFrame(bx, by, bw, bh);
  int fill = (int)lroundf((float)(bw - 4) * s_stirProgress);
  if (fill > 0) oled.drawBox(bx + 2, by + 2, fill, bh - 4);
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

  // Act 2: two waves to bring into resonance. Your wave is solid; the essence
  // you're chasing is a ghost (every 3rd pixel). Turn the knob to slide your
  // wave until they merge — aligned, the wave doubles up ("glows") and
  // sparkles; the bar fills only while you hold it there.
  if (comboCount(g_combo) == 2) {
    oled.setFont(u8g2_font_helvR08_tr);
    drawCenteredF("- attuning -", 8);

    const int wy = 25;            // wave centreline
    const float AMP = 8.0f, KX = 0.10f;
    for (int x = 2; x < 126; x++) {
      int ty = wy + (int)lroundf(AMP * sinf(KX * (float)x + s_alignTarget));
      if ((x % 3) == 0) oled.drawPixel(x, ty);              // the ghost essence
      int py = wy + (int)lroundf(AMP * sinf(KX * (float)x + s_alignPlayer));
      oled.drawPixel(x, py);                                // your essence
      if (s_alignOk) oled.drawPixel(x, py + 1);             // in resonance: glow
    }
    if (s_alignOk) {
      int sr = ((now / 180) & 1) ? 2 : 1;
      drawSparkle(6, wy, sr);
      drawSparkle(122, wy, sr);
    }

    drawPowerBar();
    oled.setFont(u8g2_font_5x8_tr);
    drawCenteredF(s_alignMsg, 62);
    oled.sendBuffer();
    return;
  }

  // Acts 1 & 3: the brewing vortex + power bar.
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
  drawPowerBar();

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

// --- Act 3 ritual screens -------------------------------------------------
// The incantation glyphs, drawn with primitives (no icon font, no flash cost):
// fat straight left/right arrows for the turns (circular arrows proved too
// alike at a glance), a ring with a hub for press.
static void drawRitGlyph(uint8_t sym, int cx, int cy) {
  if (sym == SYM_PRESS) {
    oled.drawCircle(cx, cy, 13);
    oled.drawDisc(cx, cy, 5);
    return;
  }
  int dir = (sym == SYM_CW) ? 1 : -1;   // right / left
  oled.drawBox(cx - (dir > 0 ? 20 : 6), cy - 2, 26, 5);            // shaft
  oled.drawTriangle(cx + dir * 6, cy - 9,                          // big head
                    cx + dir * 6, cy + 9,
                    cx + dir * 20, cy);
}

static const char* ritSymWord(uint8_t sym) {
  return sym == SYM_CW ? "turn right" : sym == SYM_CCW ? "turn left" : "press";
}

static void renderRitual(uint32_t now) {
  oled.clearBuffer();
  uint32_t el = now - g_stateMs;
  char hdr[24];

  switch (s_ritPhase) {
    case RI_INTRO:
      drawFancyFrame();
      oled.setFont(u8g2_font_helvR08_tr);
      drawCenteredF("all three essences...", 16);
      oled.setFont(u8g2_font_ncenB12_tr);
      drawCenteredF("The Grand", 33);
      drawCenteredF("Brew", 49);
      oled.setFont(u8g2_font_5x8_tr);
      drawCenteredF("watch the incantation", 60);
      break;

    case RI_SHOW: {
      // The final verse is spoken BLIND — notes only, no glyphs — so the
      // per-symbol voices become load-bearing. Muted players get glyphs
      // back, or the verse would be unpassable.
      bool blind = (s_ritRound == RIT_ROUNDS - 1) && g_volume > 0;
      snprintf(hdr, sizeof(hdr), "verse %d of %d - %s",
               s_ritRound + 1, RIT_ROUNDS, blind ? "listen" : "watch");
      oled.setFont(u8g2_font_5x8_tr);
      drawCenteredF(hdr, 8);
      int idx = (int)(el / RIT_GLYPH_MS);
      uint32_t glyphEl = el - (uint32_t)idx * RIT_GLYPH_MS;
      // A short blank tail on each slot separates repeated symbols.
      if (blind) {
        oled.setFont(u8g2_font_helvR08_tr);
        drawCenteredF("the brew whispers...", 34);
        // A pulse marks each spoken slot so the rhythm still reads on screen.
        if (idx < ritRoundLen() && glyphEl < RIT_GLYPH_MS - 160)
          drawSparkle(64, 48, 1 + (int)((glyphEl / 120) % 2));
      } else if (idx < ritRoundLen() && glyphEl < RIT_GLYPH_MS - 160) {
        drawRitGlyph(s_ritSeq[idx], 64, 32);
        oled.setFont(u8g2_font_5x8_tr);
        drawCenteredF(ritSymWord(s_ritSeq[idx]), 60);
      }
      break;
    }

    case RI_INPUT: {
      snprintf(hdr, sizeof(hdr), "verse %d of %d - repeat", s_ritRound + 1, RIT_ROUNDS);
      oled.setFont(u8g2_font_5x8_tr);
      drawCenteredF(hdr, 8);
      // One slot per symbol this verse: filled = answered, the next blinks.
      const int len = ritRoundLen(), bs = 12, gap = 6;
      int x0 = (128 - (len * bs + (len - 1) * gap)) / 2;
      for (int i = 0; i < len; i++) {
        int x = x0 + i * (bs + gap);
        if (i < s_ritInputIdx) oled.drawBox(x, 26, bs, bs);
        else {
          oled.drawFrame(x, 26, bs, bs);
          if (i == s_ritInputIdx && ((now / 300) & 1))
            oled.drawFrame(x + 2, 28, bs - 4, bs - 4);
        }
      }
      oled.setFont(u8g2_font_5x8_tr);
      drawCenteredF("turn or press to answer", 62);
      break;
    }

    case RI_GOOD:
      drawCornerSparkles(now);
      oled.setFont(u8g2_font_ncenB12_tr);
      drawCenteredF(s_ritDone ? "It is done!" : "Well stirred!", 34);
      oled.setFont(u8g2_font_5x8_tr);
      drawCenteredF(s_ritDone ? "the incantation holds..." : "the brew deepens...", 56);
      break;

    case RI_MISS:
      oled.setFont(u8g2_font_ncenB12_tr);
      drawCenteredF("It resists!", 34);
      oled.setFont(u8g2_font_5x8_tr);
      drawCenteredF("listen again...", 56);
      break;
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

// ---- Story mode rendering ------------------------------------------------
// Word-wrap `s` in the CURRENT font into lines of width `w` centered on
// `cx`, first baseline `y`, spacing `lineH`, at most `maxLines` (overflow is
// dropped). cx matters when the text block isn't screen-centered (bubbles).
static void drawWrapped(const char* s, int cx, int y, int lineH, int maxLines, int w) {
  char buf[160];
  strncpy(buf, s, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  char line[40] = "";
  int n = 0;
  for (char* tok = strtok(buf, " "); tok && n < maxLines; tok = strtok(nullptr, " ")) {
    char trial[40];
    snprintf(trial, sizeof(trial), "%s%s%s", line, line[0] ? " " : "", tok);
    if (!line[0] || oled.getStrWidth(trial) <= w) {
      strncpy(line, trial, sizeof(line) - 1);
      line[sizeof(line) - 1] = '\0';
    } else {
      oled.drawStr(cx - oled.getStrWidth(line) / 2, y, line);
      y += lineH;
      n++;
      strncpy(line, tok, sizeof(line) - 1);
      line[sizeof(line) - 1] = '\0';
    }
  }
  if (line[0] && n < maxLines)
    oled.drawStr(cx - oled.getStrWidth(line) / 2, y, line);
}

// The giant rat — procedural like the rest of the firmware's art: a solid
// silhouette facing left (body, haunch, head, ears, snout, legs, eye,
// whiskers) with an idle bob, twitching ear and swaying tail. Referenced by
// kBtRat as its sprite; other universes' battles bring their own.
static void drawRat(int cx, int cy, uint32_t now) {
  int y = cy + (int)((now / 300) & 1);           // idle bob
  float sw = sinf((float)now * 0.006f) * 3.0f;   // tail sway
  oled.drawLine(cx + 11, y, cx + 19, y - 2 + (int)sw);
  oled.drawLine(cx + 19, y - 2 + (int)sw, cx + 25, y - 6 + (int)(sw * 1.6f));
  oled.drawFilledEllipse(cx, y, 13, 7);          // body
  oled.drawDisc(cx + 7, y - 1, 6);               // haunch
  oled.drawDisc(cx - 12, y - 2, 5);              // head
  oled.drawTriangle(cx - 16, y - 4, cx - 22, y, cx - 15, y + 1);  // snout
  oled.drawDisc(cx - 13, y - 8 - (int)((now / 700) & 1), 2);      // ear (twitch)
  oled.drawDisc(cx - 9, y - 7, 2);               // far ear
  for (int i = 0; i < 4; i++)                    // legs
    oled.drawBox(cx - 7 + i * 5, y + 5, 2, 5);
  oled.setDrawColor(0);
  oled.drawDisc(cx - 12, y - 3, 1);              // eye
  oled.setDrawColor(1);
  oled.drawLine(cx - 21, y - 1, cx - 26, y - 2); // whiskers
  oled.drawLine(cx - 21, y + 1, cx - 26, y + 2);
}

// The Jarl's crown — an emblem stands in for a face (a literal bust reads
// badly at this size). Upright and pixel-crisp (rotation aliases to mush in
// 1-bit at 30 px); the life comes from a slow bob and an XOR glint hopping
// from tip to tip. Art like this is previewed off-device with
// tools/oledsim.py — keep that sketch and this code line-for-line twins.
static void drawJarl(int cx, int cy, uint32_t now) {
  int yb = cy + (int)lroundf(2.0f * sinf((float)(now % 3142U) * 0.002f));
  oled.drawBox(cx - 16, yb + 3, 33, 6);                        // band
  oled.drawTriangle(cx - 16, yb + 3, cx - 11, yb - 8,  cx - 5, yb + 3);
  oled.drawTriangle(cx - 6,  yb + 3, cx,      yb - 12, cx + 6, yb + 3);
  oled.drawTriangle(cx + 5,  yb + 3, cx + 11, yb - 8,  cx + 16, yb + 3);
  oled.drawDisc(cx - 11, yb - 10, 2);                          // ball tips
  oled.drawDisc(cx,      yb - 14, 2);
  oled.drawDisc(cx + 11, yb - 10, 2);
  oled.setDrawColor(0);                                        // band jewels
  oled.drawDisc(cx - 9, yb + 6, 1);
  oled.drawDisc(cx,     yb + 6, 1);
  oled.drawDisc(cx + 9, yb + 6, 1);
  oled.setDrawColor(1);
  static const int8_t gtx[3] = { -11, 0, 11 };                 // glint hops
  static const int8_t gty[3] = { -10, -14, -10 };              // tip to tip
  int k = (int)((now / 900) % 3);
  int gr = ((now / 150) & 1) ? 4 : 3;
  int gx = cx + gtx[k], gy = yb + gty[k];
  oled.setDrawColor(2);                                        // XOR: reads on
  for (int i = 1; i <= gr; i++) {                              // white or black
    oled.drawPixel(gx + i, gy);
    oled.drawPixel(gx - i, gy);
    oled.drawPixel(gx, gy + i);
    oled.drawPixel(gx, gy - i);
  }
  oled.setDrawColor(1);
}

// Healer Danica — mortar & pestle, the pestle grinding in slow circles and
// the odd fleck of herb rising. Twin: emblem sketch via tools/oledsim.py.
static void drawHealer(int cx, int cy, uint32_t now) {
  oled.drawFilledEllipse(cx, cy + 6, 13, 8);       // bowl...
  oled.setDrawColor(0);
  oled.drawBox(cx - 14, cy - 4, 29, 8);            // ...with the top carved flat
  oled.setDrawColor(1);
  oled.drawBox(cx - 13, cy + 3, 27, 3);            // rim
  oled.drawBox(cx - 5, cy + 14, 10, 3);            // foot
  float a = 0.6f + 0.25f * sinf((float)now * 0.004f);
  int tx = cx + (int)lroundf(16.0f * cosf(a));     // pestle rocks as it grinds
  int ty = cy + 4 - (int)lroundf(20.0f * sinf(a));
  for (int o = -1; o <= 1; o++)
    drawLineClipped(cx + 2 + o, cy + 4, tx + o, ty);
  oled.drawDisc(tx, ty, 3);                        // knob
  if ((now / 400) & 1) {                           // a pinch of the grind
    oled.drawPixel(cx - 6, cy - 2);
    oled.drawPixel(cx - 9, cy - 5);
  }
}

// The steward — a goblet with a wobbling wine line and a drip beading off
// the rim. His emblem is the murder weapon.
static void drawSteward(int cx, int cy, uint32_t now) {
  oled.drawBox(cx - 8, cy - 14, 17, 7);            // bowl
  oled.drawTriangle(cx - 8, cy - 7, cx + 8, cy - 7, cx, cy + 1);  // taper
  oled.drawBox(cx - 1, cy + 1, 3, 8);              // stem
  oled.drawBox(cx - 6, cy + 9, 13, 3);             // foot
  oled.setDrawColor(0);                            // wine line, wobbling
  for (int x = cx - 7; x < cx + 8; x++)
    oled.drawPixel(x, cy - 12 + (int)lroundf(1.2f * sinf((float)x * 0.9f +
                                                         (float)now * 0.005f)));
  oled.setDrawColor(1);
  int fall = (int)((now / 60) % 26);               // a drop falls...
  oled.drawPixel(cx + 9, cy - 13 + fall);
  oled.drawPixel(cx + 9, cy - 12 + fall);
  if (fall > 13) oled.drawPixel(cx + 9, cy - 13);  // ...as the next beads up
}

// Quavers drifting upward on a sway — the lute is heard, not seen. Stems and
// flags go through drawLineClipped: as a note rises off-screen its stem top
// goes negative, and raw u8g2 lines would wrap and smear (the vertical
// cousin of the dice-roll streak bug).
static void drawLuteNotes(int cx, int cy, uint32_t now) {
  for (int i = 0; i < 3; i++) {
    int rise = (int)((now / 45 + i * 27) % 40);    // unhurried, like the tune
    int x = cx - 30 + i * 30 + (int)lroundf(4.0f * sinf((float)now * 0.002f + i * 2.0f));
    int y = cy + 14 - rise;
    oled.drawDisc(x, y, 2);                        // note head (per-pixel safe)
    drawLineClipped(x + 2, y - 8, x + 2, y);       // stem
    drawLineClipped(x + 2, y - 8, x + 5, y - 5);   // flag
  }
}

// One flame tongue, scanned per ROW: left/right edges wander with layered
// sine noise (licking hardest near the tip), the whole tongue sways, and its
// height flickers — an organic outline, not geometry. Carve/inner layers
// pass the parent's swaySeed so they stay nested. Returns the tip's virtual
// y (pre-pan coordinates; caller subtracts cam).
static int drawFlameLayer(int cam, uint32_t now, int cx0, int baseVy,
                          int height, int maxW, float seed, uint8_t color,
                          float swaySeed) {
  int flick = (int)lroundf(4.0f * sinf((float)now * 0.009f + seed) +
                           3.0f * sinf((float)now * 0.023f + seed * 2.3f));
  int tipVy = baseVy - height - flick;
  oled.setDrawColor(color);
  for (int vy = tipVy; vy <= baseVy; vy++) {
    int y = vy - cam;
    if (y < 0 || y >= 64) continue;
    float h = (float)(vy - tipVy) / (float)(baseVy - tipVy);
    float half = (float)maxW * sqrtf(h);           // pointed tip, broad body
    float sway = (1.0f - h) * (1.0f - h) * 5.0f * sinf((float)now * 0.0017f + swaySeed);
    float lick = (1.0f - h) * 4.0f + 1.0f;
    float fx = (float)cx0 + sway;
    float eL = fx - half + lick * sinf((float)vy * 0.23f + (float)now * 0.011f + seed);
    float eR = fx + half + lick * sinf((float)vy * 0.29f + (float)now * 0.013f + seed * 1.7f);
    if (eR > eL)
      oled.drawHLine((u8g2_uint_t)lroundf(eL), (u8g2_uint_t)y,
                     (u8g2_uint_t)(lroundf(eR - eL) + 1));
  }
  oled.setDrawColor(1);
  return tipVy;
}

// A thick log: filled rotated slab with round ends.
static void drawLog(int cam, int x0, int y0, int x1, int y1, int half) {
  float dx = (float)(x1 - x0), dy = (float)(y1 - y0);
  float ln = sqrtf(dx * dx + dy * dy);
  int px = (int)lroundf(-dy / ln * (float)half);
  int py = (int)lroundf(dx / ln * (float)half);
  oled.drawTriangle(x0 + px, y0 + py - cam, x1 + px, y1 + py - cam,
                    x1 - px, y1 - py - cam);
  oled.drawTriangle(x0 + px, y0 + py - cam, x1 - px, y1 - py - cam,
                    x0 - px, y0 - py - cam);
  oled.drawDisc(x0, y0 - cam, half);
  oled.drawDisc(x1, y1 - cam, half);
}

// Campfire scene (N_SCENE), framed CLOSE like a pinball cutaway — the fire
// fills the frame rather than sitting neatly inside it. The camera pans down
// a 200px virtual scene: sinuous smoke wisps first, then flames rising into
// frame, landing with the blaze near full-height and the crossed logs
// cropped by the bottom edge. (Even at an inn, it's always a campfire.)
// Twin: fire3_sketch.py via tools/oledsim.py. Node entry stamped g_stateMs.
static void drawCampfire(int cx, int cy, uint32_t now) {
  (void)cx; (void)cy;                              // full-screen scene
  uint32_t el = now - g_stateMs;
  float t = (el >= 3500) ? 1.0f : (float)el / 3500.0f;
  float e = t * t * (3.0f - 2.0f * t);             // smoothstep pan
  int cam = (int)lroundf(e * 136.0f);              // viewport top in the scene

  // smoke: S-shaped wisps (pixel trails, no closed shapes), widening as they
  // rise; during the hold their tails still lick the top of the frame
  for (int i = 0; i < 4; i++) {
    int rise = (int)((now / 18 + i * 47) % 120);
    int head = 148 - rise;
    int xc = 52 + i * 10;
    for (int d = 0; d < 36; d++) {
      int y = head + d - cam;
      if (y < 0 || y >= 64) continue;
      float amp = 2.0f + (float)(36 - d) * 0.16f;  // older smoke wanders wider
      int x = xc + (int)lroundf(amp * sinf((float)(head + d) * 0.17f +
                                           (float)now * 0.002f + i * 1.7f));
      oled.drawPixel(x, y);
      oled.drawPixel(x + 1, y);
    }
  }

  // flames: an organic silhouette, scanned per row — main blaze plus two
  // side tongues merge into one many-peaked outline whose edges lick
  drawFlameLayer(cam, now, 46, 193, 24, 14, 4.0f, 1, 4.0f);   // left tongue
  drawFlameLayer(cam, now, 82, 193, 20, 13, 8.5f, 1, 8.5f);   // right tongue
  int tip = drawFlameLayer(cam, now, 64, 193, 50, 30, 1.0f, 1, 1.0f);

  // tongue separations: thin black wavy trails INSIDE the silhouette (a
  // carved core reads as a donut once the logs close its bottom edge)
  oled.setDrawColor(0);
  for (int i = 0; i < 2; i++) {
    int x0 = 56 + i * 15;
    int top = 166 - (int)lroundf(5.0f * sinf((float)now * 0.006f + i * 2.2f));
    for (int vy = top; vy < 192; vy++) {
      int y = vy - cam;
      if (y < 0 || y >= 64) continue;
      int x = x0 + (int)lroundf(3.0f * sinf((float)vy * 0.3f + (float)now * 0.008f + i * 2.0f));
      oled.drawPixel(x, y);
      oled.drawPixel(x + 1, y);
    }
  }
  oled.setDrawColor(1);

  // a detached lick above the tip, sometimes
  if (sinf((float)now * 0.007f + 1.0f) > 0.45f) {
    int lx = 64 + (int)lroundf(5.0f * sinf((float)now * 0.0017f + 1.0f));
    int ly = tip - 5 - cam;
    if (ly >= 2) oled.drawDisc(lx, ly, 2);
  }

  // logs: two thick crossed slabs with round ends, end-grain rings and
  // dashed bark, cropped by the bottom edge of the frame
  drawLog(cam, 12, 195, 72, 186, 5);
  drawLog(cam, 56, 186, 116, 196, 5);
  oled.setDrawColor(0);
  oled.drawCircle(12, 195 - cam, 2);
  oled.drawCircle(116, 196 - cam, 2);
  for (int d = 0; d < 4; d++) {
    int x = 22 + d * 12;
    oled.drawLine(x, 196 - d - cam, x + 6, 195 - d - cam);
    oled.drawLine(x + 46, 188 + d - cam, x + 52, 189 + d - cam);
  }
  oled.setDrawColor(1);

  // embers drifting up around the blaze
  for (int i = 0; i < 6; i++) {
    int ph = (int)((now / 90 + i * 31) % 60);
    if (ph < 34) {
      int ex = 26 + (i * 19) % 76 + (int)lroundf(2.0f * sinf((float)now * 0.004f + i));
      int ey = 186 - ph - cam;
      oled.drawPixel(ex, ey);
      if (ph < 16) oled.drawPixel(ex + 1, ey);
    }
  }
}

// One Afflicted cultist: robed trapezoid (drawn as hand-clamped row scans so
// a figure can slide half off the frame edge without u8g2's unsigned hlines
// dropping whole rows), bowed hood, coughing bob, slow sway.
static void drawHooded(int x, int y, uint32_t now, float phase) {
  int bob  = (int)lroundf(1.5f * sinf((float)now * 0.005f + phase));
  int sway = (int)lroundf(1.0f * sinf((float)now * 0.002f + phase * 2.0f));
  x += sway;
  for (int r = 0; r <= 20; r++) {                  // robe: 5 -> 10 half-width
    int half = 5 + (r * 5) / 20;
    int lx = x - half, rx = x + half;
    if (rx < 0 || lx > 127) continue;
    if (lx < 0) lx = 0;
    if (rx > 127) rx = 127;
    oled.drawHLine((u8g2_uint_t)lx, (u8g2_uint_t)(y - 20 + r),
                   (u8g2_uint_t)(rx - lx + 1));
  }
  int lx = x - 4, rx = x + 4;                      // shoulders/neck
  if (rx >= 0 && lx <= 127) {
    if (lx < 0) lx = 0;
    if (rx > 127) rx = 127;
    oled.drawBox((u8g2_uint_t)lx, (u8g2_uint_t)(y - 23),
                 (u8g2_uint_t)(rx - lx + 1), 5);
  }
  oled.drawDisc(x + 1, y - 26 + bob, 6);           // hood (per-pixel safe)
  oled.setDrawColor(0);
  oled.drawDisc(x + 3, y - 24 + bob, 3);           // the dark inside the cowl
  oled.setDrawColor(1);
}

// Sneak scene (N_SCENE, act 3): the camera tracks right through the shrine —
// swaying Afflicted, brazier smoke — and the only trace of YOU is a trail of
// footprints appearing across the floor. Invisible, walking. Twin:
// afflicted_sketch.py via tools/oledsim.py.
static void drawSneak(int cx, int cy, uint32_t now) {
  (void)cx; (void)cy;
  uint32_t el = now - g_stateMs;
  float t = (el >= 4200) ? 1.0f : (float)el / 4200.0f;
  float e = t * t * (3.0f - 2.0f * t);
  int cam = (int)lroundf(e * 110.0f);              // pans right, then holds

  oled.drawHLine(0, 54, 128);                      // shrine floor
  for (int b = 0; b < 2; b++) {                    // braziers
    int x = (b ? 156 : 66) - cam;
    if (x - 3 >= 0 && x + 4 < 128) {
      oled.drawBox(x - 3, 50, 7, 4);
      oled.drawVLine(x, 46, 4);
    }
  }
  for (int i = 0; i < 4; i++) {                    // brazier smoke wisps
    int rise = (int)((now / 20 + i * 43) % 70);
    int vx = 66 + (i % 2) * 90;
    int head = 44 - rise;
    int x0 = vx - cam;
    if (x0 > -8 && x0 < 136 && head >= 0 && head < 64) {
      float amp = 1.5f + (float)rise * 0.09f;
      int x = x0 + (int)lroundf(amp * sinf((float)(head + rise) * 0.2f +
                                           (float)now * 0.002f + i));
      oled.drawPixel(x, head);
      oled.drawPixel(x + 1, head);
    }
  }
  static const int16_t kVx[4] = { 30, 104, 186, 238 };   // the Afflicted
  for (int i = 0; i < 4; i++) {
    int x = kVx[i] - cam;
    if (x > -20 && x < 148) drawHooded(x, 54, now, i * 1.9f);
  }
  int steps = (int)(el / 260);                     // your footprints, appearing
  for (int k = 0; k < steps; k++) {
    int fx = 6 + k * 9 - cam;
    if (fx >= 0 && fx < 126) oled.drawHLine((u8g2_uint_t)fx, (k & 1) ? 60 : 58, 3);
  }
}

// Labelled HP bar with its number ("You  [######  ] 23"). Takes the ANIMATED
// value (easeHP's output), so damage visibly drains and healing refills.
// `x` lets it sit inside a card frame as well as flush in the battle layout.
static void drawHPBar(int x, int y, const char* who, float hp, int maxhp) {
  int shown = (int)lroundf(hp);
  oled.setFont(u8g2_font_5x8_tr);
  oled.drawStr(x, y + 7, who);
  oled.drawFrame(x + 22, y, 78, 8);
  int w = (shown > 0) ? (76 * shown + maxhp - 1) / maxhp : 0;  // ceil: 1 HP stays visible
  if (w > 76) w = 76;
  if (w > 0) oled.drawBox(x + 23, y + 1, w, 6);
  char b[8];
  snprintf(b, sizeof(b), "%d", shown);
  oled.drawStr(x + 104, y + 7, b);
}

// THE choice interface, used everywhere a decision is made: one option at a
// time on the bottom of the screen, flanked by < > chevrons — turn to cycle
// options, press to select. Options too wide for one line wrap onto two
// (split at the space nearest the middle). Masks what's behind it.
static void drawChoiceLine(const char* opt) {
  oled.setFont(u8g2_font_helvR08_tr);
  int w = oled.getStrWidth(opt);
  if (w <= 100) {                                  // fits on one line
    oled.setDrawColor(0);
    oled.drawBox(0, 52, 128, 12);
    oled.setDrawColor(1);
    int x = (128 - w) / 2;
    oled.drawStr(x, 62, opt);
    oled.setFont(u8g2_font_5x8_tr);
    oled.drawStr(x - 12, 61, "<");
    oled.drawStr(x + w + 8, 61, ">");
    return;
  }
  char l1[24], l2[24];                             // two lines: break mid-way
  int len = (int)strlen(opt), best = 0, bestDist = 999;
  for (int i = 0; opt[i]; i++) {
    if (opt[i] != ' ') continue;
    int di = i - len / 2;
    if (di < 0) di = -di;
    if (di < bestDist) { bestDist = di; best = i; }
  }
  if (best == 0) {                                 // no space to break at:
    oled.drawStr((128 - w) / 2, 62, opt);          // draw as-is (will clip)
    return;
  }
  snprintf(l1, sizeof(l1), "%.*s", best, opt);
  snprintf(l2, sizeof(l2), "%s", opt + best + 1);
  int w1 = oled.getStrWidth(l1), w2 = oled.getStrWidth(l2);
  int wm = (w1 > w2) ? w1 : w2;
  oled.setDrawColor(0);
  oled.drawBox(0, 41, 128, 23);
  oled.setDrawColor(1);
  oled.drawStr((128 - w1) / 2, 52, l1);
  oled.drawStr((128 - w2) / 2, 62, l2);
  oled.setFont(u8g2_font_5x8_tr);
  oled.drawStr((128 - wm) / 2 - 12, 58, "<");
  oled.drawStr((128 + wm) / 2 + 8, 58, ">");
}

// Framed narration card: title, wrapped body, blinking press cue. A card
// whose node heals also shows the player's HP bar refilling (a).
static void renderStoryCard(const char* title, const char* body, uint32_t now,
                            bool showHP) {
  drawFancyFrame();
  oled.setFont(u8g2_font_helvB08_tr);
  drawCenteredF(title, 16);
  oled.setFont(u8g2_font_5x8_tr);
  if (showHP) {
    drawWrapped(body, 64, 27, 8, 2, 114);    // shorter body: the bar needs room
    drawHPBar(6, 42, "You", s_phpShown, PLAYER_MAX_HP);
  } else {
    drawWrapped(body, 64, 27, 8, 4, 114);
  }
  if ((now / 600) & 1) drawCenteredF("- press -", 58);
}

// The attack roll: a full-screen pinball cutaway. The die tumbles in along a
// table line spinning on all three axes, the camera zooms onto the face as
// the spin bleeds into the landing pose, and the rolled number holds front
// and centre (face numbers ride the die, so the result arrives ON its face).
// Rotation is continuous across all three stages.
static void renderBattleRoll(uint32_t el) {
  if (el < ROLL_TUMBLE_MS) {
    float t = (float)el / (float)ROLL_TUMBLE_MS;
    float u = 1.0f - t;
    oled.setFont(u8g2_font_5x8_tr);
    drawCenteredF("you attack!", 8);
    oled.drawHLine(8, 56, 112);                       // the table it rolls along
    int cx = 14 + (int)lroundf(t * 50.0f);            // rolls in from the left
    float bounce = fabsf(sinf(t * 3.0f * (float)M_PI));
    int cy = 47 - (int)lroundf(24.0f * u * bounce);   // decaying bounces
    drawD20Tumbling(cx, cy, 9,
                    D20_FACE_AX + 1.5f + 12.0f * u * u,
                    D20_FACE_AY + 1.0f + 9.0f * u * u,
                    6.0f * u * u);
    if (cx >= 26 && t < 0.7f) {                       // speed streaks behind it
      oled.drawHLine(cx - 20, cy - 3, 6);
      oled.drawHLine(cx - 26, cy + 2, 8);
    }
  } else if (el < ROLL_TUMBLE_MS + ROLL_ZOOM_MS) {
    float t = (float)(el - ROLL_TUMBLE_MS) / (float)ROLL_ZOOM_MS;
    float e = t * t * (3.0f - 2.0f * t);              // smoothstep push-in
    int cy = 47 - (int)lroundf(e * 15.0f);            // 47 -> 32
    int r  = 9 + (int)lroundf(e * 19.0f);             // 9  -> 28
    drawD20Tumbling(64, cy, r,
                    D20_FACE_AX + 1.5f * (1.0f - e),
                    D20_FACE_AY + 1.0f * (1.0f - e),
                    0.0f);
  } else {                                            // landed: hold the number
    uint32_t hold = el - ROLL_TUMBLE_MS - ROLL_ZOOM_MS;
    drawD20Tumbling(64, 32, 28, D20_FACE_AX, D20_FACE_AY, 0.0f);
    if (hold < 300) oled.drawCircle(64, 32, 30 + (int)(hold / 6));  // impact ring
  }
}

// Battle layout: both HP bars stacked at the top (the key information), the
// enemy beneath, and the bottom text line carrying messages or the choice
// spinner. The roll is a full-screen cutaway; damage flashes the WHOLE screen.
static void renderStoryBattle(uint32_t now) {
  uint32_t el = now - g_stateMs;
  if (s_bp == BP_ROLL) { renderBattleRoll(el); return; }
  drawHPBar(2, 0, "You", s_phpShown, PLAYER_MAX_HP);
  drawHPBar(2, 10, s_bdef->name, s_ehpShown, s_bdef->enemyHP);
  int lunge = (s_bp == BP_BITE && el < 300) ? -8 : 0;   // the bite's pounce
  s_bdef->sprite(72 + lunge, 34, now);
  if (s_bp == BP_CHOOSE) {
    drawChoiceLine(s_choiceIdx == 0
                       ? (s_numb ? "Attack (arm numb!)" : "Attack for 4-7")
                       : "Brew potion");
  } else {
    oled.setFont(u8g2_font_5x8_tr);
    drawWrapped(s_bmsg, 64, 54, 8, 2, 124);
  }
  // A landed hit — either side's — blink-inverts the whole screen.
  if ((s_bp == BP_HIT || s_bp == BP_BITE || s_bp == BP_MAULED) &&
      el < 360 && ((el / 90) & 1) == 0) {
    oled.setDrawColor(2);
    oled.drawBox(0, 0, 128, 64);
    oled.setDrawColor(1);
  }
}

// Character speech: portrait bust on the left, speech bubble (with a tail
// toward the speaker) on the right, speaker name beneath — a face beats a
// wall of text. Node.title is the speaker, node.body the words.
static void renderStorySpeak(const StoryNode& n, uint32_t now) {
  n.art(20, 30, now);
  oled.setFont(u8g2_font_4x6_tr);
  int nw = oled.getStrWidth(n.title);
  int nx = 20 - nw / 2;
  if (nx < 1) nx = 1;
  oled.drawStr(nx, 62, n.title);
  oled.drawRFrame(42, 4, 84, 44, 5);               // the bubble
  oled.drawLine(42, 26, 36, 30);                   // its tail, mouth-ward
  oled.drawLine(42, 34, 36, 30);
  oled.setDrawColor(0);
  oled.drawVLine(42, 27, 7);                       // open the bubble into the tail
  oled.setDrawColor(1);
  oled.setFont(u8g2_font_5x8_tr);
  drawWrapped(n.body, 84, 14, 8, 4, 76);
  if ((now / 600) & 1) {
    oled.setFont(u8g2_font_4x6_tr);
    oled.drawStr(104, 62, "press");
  }
}

static void renderStory(uint32_t now) {
  oled.clearBuffer();
  const StoryNode& n = s_story[s_node];
  switch (n.kind) {
    case N_CARD:
      renderStoryCard(n.title, n.body, now, n.heal != 0);
      break;
    case N_SPEAK:
      renderStorySpeak(n, now);
      break;
    case N_CHOICE: {
      // Decision HUD: your HP bar (so "+15 HP" means something) and the
      // purse, then the prompt and the spinner.
      drawHPBar(2, 0, "You", s_phpShown, PLAYER_MAX_HP);
      char purse[10];                        // the inventory, such as it is
      snprintf(purse, sizeof(purse), "%d gp", s_gold);
      oled.setFont(u8g2_font_5x8_tr);
      oled.drawStr(126 - oled.getStrWidth(purse), 19, purse);
      oled.setFont(u8g2_font_helvB08_tr);
      drawCenteredF(n.title, 32);
      drawChoiceLine(s_choiceIdx == 0 ? n.optA : n.optB);
      break;
    }
    case N_TUNE:
      if (n.art) n.art(64, 24, now);
      oled.setFont(u8g2_font_helvB08_tr);
      drawCenteredF(n.title, 50);
      oled.setFont(u8g2_font_5x8_tr);
      drawCenteredF("press to stop", 62);
      break;
    case N_SCENE:
      if (n.art) n.art(64, 32, now);         // full-screen cinematic
      break;
    case N_BREW:
      break;   // transient — the brew states own the screen
    case N_BATTLE:
      renderStoryBattle(now);
      break;
    case N_END:
      break;   // transient — storyGoto already routed home
  }
  oled.sendBuffer();
}

// Story brew screen (replaces IDENTIFY while a quest brew is on): names the
// target, lists what's seated, and surfaces the recipe hint if it was earned.
// With nothing seated a press backs out to the fight (no turn consumed).
// STIRRING keeps its normal vortex/power-bar screen.
static void renderStoryBrew(uint32_t now) {
  oled.clearBuffer();
  drawTitleBar(s_bdef->brewTitle);
  oled.setFont(u8g2_font_5x8_tr);
  bool canBack = (s_story[s_node].kind != N_BREW);   // story brews must be brewed
  const char* hint = (s_bdef->hint &&
                      (s_bdef->hintFlag == 0 || (s_flags & s_bdef->hintFlag)))
                         ? s_bdef->hint : nullptr;
  if (g_combo == 0) {
    drawCenteredF("Place ingredients", 28);
    if (hint) drawWrapped(hint, 64, 41, 8, 2, 120);
    if (canBack) drawCenteredF("press to go back", 62);
  } else {
    int y = 27;
    for (int s = 0; s < 3; s++)
      if (g_combo & (1 << s)) { drawCenteredF(kIngredients[g_universe][s], y); y += 10; }
    // Bottom note: after a wrong brew, alternate WHAT went wrong with the
    // recipe hint (what to do instead) — feedback plus the fix, not a shrug.
    if (s_bmsg[0] && hint)
      drawWrapped(((now / 2500) & 1) ? hint : s_bmsg, 64, 53, 8, 2, 124);
    else if (s_bmsg[0])
      drawWrapped(s_bmsg, 64, 53, 8, 2, 124);
    else if (hint)
      drawWrapped(((now / 2500) & 1) ? "turn to stir" : hint, 64, 53, 8, 2, 124);
    else
      drawCenteredF("turn to stir", 62);
  }
  oled.sendBuffer();
}

static void render() {
  if (!g_haveDisplay) return;  // headless: skip drawing if no panel on the bus
  uint32_t now = g_now;        // same clock everything else uses this tick
  switch (g_state) {
    case ST_IDLE:     renderHome(now);     break;
    case ST_IDENTIFY: if (s_storyBrew) renderStoryBrew(now);
                      else renderIdentify(now);
                      break;
    case ST_STIRRING: renderStirring(now); break;  // shared by story brews
    case ST_RITUAL:   renderRitual(now);   break;
    case ST_REVEAL:   renderReveal(now);   break;
    case ST_STORY:    renderStory(now);    break;
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
    if (f == 0) buzzTone(392);   // G4
    if (f == 6) buzzTone(523);   // C5
    delay(58);
  }

  // Phase 2: the frame draws on; "Welcome to the" slides down into place (~0.5s).
  for (int f = 0; f <= 10; f++) {
    oled.clearBuffer();
    drawFancyFrame();
    title(u8g2_font_ncenR08_tr, -4 + (17 * f) / 10);  // welcome slides -4 -> 13
    stars(f + 12);
    oled.sendBuffer();
    if (f == 0) buzzTone(587);   // D5
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
    buzzTone(notes[s]);
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
    if (f == 1) buzzTone(1319);  // E6 sparkle
    if (f == 6) buzzTone(1047);  // settle on C6
    delay(56);
  }
  buzzOff();
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

  ledcAttach(PIN_BUZZER, 2000, 10);   // buzzer PWM; buzzTone drives freq + volume duty

  // ---- Load persisted settings (before the chime, so Mute is honored) ----
  prefs.begin("alchemy", false);
  g_universe   = (Universe)prefs.getUChar("universe", UNI_SKYRIM);
  if (g_universe >= UNI_COUNT) g_universe = UNI_SKYRIM;
  // Volume 0-5 (0 = mute); migrates the old boolean "mute" key on first boot.
  g_volume     = prefs.getUChar("vol", prefs.getUChar("mute", 0) ? 0 : 3);
  if (g_volume > 5) g_volume = 5;
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
  Serial.printf("  buzzer:  GPIO%d  (volume=%u)\n", PIN_BUZZER, g_volume);
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
    for (int i = 0; i < 3; i++) { buzzTone(180); delay(120); buzzOff(); delay(90); }
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
    case ST_IDLE:     if (d) updateHomeNav(d);
                      updateHomeScroll(dt);       break;  // carousel slide
    case ST_IDENTIFY:
    case ST_STIRRING: updateStir(now, dt, d);     break;
    case ST_RITUAL:   updateRitual(now, d);       break;  // show -> repeat verses
    case ST_SETTINGS: if (d) updateMenuNav(d);    break;
    case ST_REVEAL:   updateReveal(now);          break;  // ANIM -> NAME -> idle
    case ST_STORY:    if (d) storyNav(d);
                      updateStory(now, dt);       break;  // scenes + battle beats
    default:          break;  // DIAG: encoder not navigated
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
