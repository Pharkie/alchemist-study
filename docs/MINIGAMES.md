# Mini-games — the brewing acts, and ideas on the shelf

The encoder + OLED are a natural mini-game platform, and the brewing plot gives
each game a reason to exist. Rather than one mechanic for every brew, the
mechanic **scales with the act** — the number of bottles seated on the base.
Each act is a step up in ceremony, so the three-bottle master potion feels
earned.

## The acts (shipped)

| Act | Bottles | Mechanic | In code |
|-----|---------|----------|---------|
| 1 | one | **Classic stir** — spin to fill the power bar against constant decay (capped add rate, see [TUNING.md](TUNING.md)). | `updateStir()` act 1/3 path |
| 2 | two | **Align the essences** — your wave (solid) must be brought into phase with a drifting ghost wave; the bar fills only while aligned, drains while not. The drift has a persistent heading (occasionally reversing), speeds up and the tolerance tightens as the bar fills — a parked knob loses. The trill is the hot/cold aid: closer = higher and steadier, far off = low with a wide warble. | `updateStir()` act 2 path, `kAlign*`/`ALIGN_*` tunables |
| 3 | three | **The Grand Brew** — the classic stir fills the bar, then the brew speaks a growing incantation of glyphs (turn right / turn left / press, each with its own note) and you repeat it back over three verses (Simon-style, verse lengths 2/3/4). A miss just replays the verse; finishing goes straight to the reveal. | `ST_RITUAL`, `updateRitual()`, `RIT_*` tunables |

Design notes:

- **Forgiving by default.** This is a shelf model, not an arcade cabinet: act 2
  drains rather than fails, act 3 replays rather than punishes, and a stalled
  ritual answer (12 s) replays the verse instead of aborting.
- **Audio is mechanical, not decorative.** Act 2's pitch/warble carries real
  information; act 3's per-symbol notes make the sequence memorisable by ear.
  Everything still gates behind Mute.
- **The state machine stays canonical.** The ritual is a state with re-stamped
  sub-phases (`RI_INTRO → RI_SHOW → RI_INPUT → RI_GOOD/RI_MISS`), per
  [ARCHITECTURE.md](ARCHITECTURE.md). The combo stays latched through
  STIRRING/RITUAL/REVEAL, so a flaky reed can't kill a ritual mid-verse.
- **Story brews get the same acts.** The story escalates its brews 1 → 2 → 3
  bottles, deliberately walking the player up the three mechanics; a finished
  ritual inside a story brew routes to `storyBrewResolve` (the story judges
  the potion) instead of REVEAL, and long-press abandons to the carousel as
  with any story brew.

## Ideas on the shelf (future reference)

Considered and liked, not yet built. Any of these could slot in as a new act
variant, a settings-menu action, or an easter egg.

- **Attunement dial (hidden sweet spot).** Skyrim-lockpicking re-skinned: after
  the bar fills, a hidden target angle is picked; turn the knob hunting for it
  with pitch-steadiness as the hot/cold cue, press to commit. Inside tolerance
  = "Perfect brew" (extra sparkles, brighter jingle); outside = a plainer
  reveal, never a failure. ~100–150 lines as a sub-phase between bar-full and
  REVEAL. Could become an optional "Master" finishing step for any act.
- **Essence catcher.** Sparks drift down from a cracked retort; the encoder
  slides a vial along the bottom to catch bright sparks and dodge soot. Best as
  an easter egg (hidden menu item, or a press during the boot splash); catch
  score could show as "potency" on the next reveal.
- **Alembic balance.** A drop of essence on a tilting beam; the beam drifts,
  the knob counter-tilts, keep the drop centred while a flask fills. Same
  skill-loop as act 2 but with momentum physics — tenser, more spectacle,
  more code.
- **Wheel of fate.** On brew completion, flick the knob hard to spin a rune
  wheel that coasts down with friction (flick velocity is already measured as
  `s_stirCps`) and lands on a cosmetic modifier for the reveal ("Potent",
  "Unstable", "Blessed…"). Near-zero risk, big charm per line of code.
- **Per-realm ritual flavour.** The Grand Brew's card, captions and glyph names
  could take on the selected realm's voice (e.g. Ultima runes, Minecraft
  enchantment-table glyphs) without touching the mechanic.
