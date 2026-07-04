#!/usr/bin/env python3
"""Build the shared screen-review contact sheet (an HTML page of every
firmware screen rendered by oledsim, grouped by flow, captions per frame).

Usage: python3 tools/make_screen_sheet.py [out.html]

Workflow when screens change: update the twin renderers in
screens_preview.py, run this, then redeploy the SAME artifact URL (recorded
in CLAUDE.md) so the shared page stays current.
"""
import base64
import html
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import screens_preview as sp
from oledsim import save_png

# Frame index -> caption, grouped by flow. Keep in step with sp.build().
SECTIONS = [
    ("Home carousel", [
        (0, "Panel 1 — Place ingredients"),
        (1, "Mid-slide (Place → Story)"),
        (2, "Panel 2 — Story Mode"),
        (3, "Panel 3 — Settings"),
    ]),
    ("Story — opening", [
        (4, "Intro card"),
        (5, "Choice + purse (2-line option)"),
        (6, "Jarl speaks (crown + bubble)"),
    ]),
    ("Battle", [
        (7, "Battle opens"),
        (8, "Your turn — spinner"),
        (9, "Attack roll — tumble"),
        (10, "Attack roll — zoom"),
        (11, "Attack roll — landed (14)"),
        (12, "The scripted crit"),
        (13, "Numb — brew is the way out"),
        (14, "Brew screen, empty + hint"),
        (15, "Brew screen, flower seated"),
    ]),
    ("The Bannered Mare & rest", [
        (16, "Inn card"),
        (17, "Spend your coin (7 gp)"),
        (18, "Sweetroll — HP bar refills"),
        (19, "Rest choice (2 gp left)"),
        (20, "Playing the lute"),
        (21, "Campfire scene (sleep)"),
        (22, "Act 2 — awake, full HP"),
    ]),
]


def build_page():
    shots = sp.build()
    imgs = []
    with tempfile.TemporaryDirectory() as td:
        for i, s in enumerate(shots):
            p = os.path.join(td, f"f{i}.png")
            save_png(p, [s], scale=2, gap=0)
            with open(p, "rb") as fh:
                imgs.append(base64.b64encode(fh.read()).decode())

    cards = []
    for name, frames in SECTIONS:
        tiles = "".join(
            f'<figure><img src="data:image/png;base64,{imgs[i]}" alt="{html.escape(cap)}">'
            f"<figcaption>{html.escape(cap)}</figcaption></figure>"
            for i, cap in frames)
        cards.append(f'<section><h2>{html.escape(name)}</h2>'
                     f'<div class="grid">{tiles}</div></section>')

    return f"""<title>Alchemist Study — screen review</title>
<style>
  :root {{
    --bg: #0b0e14; --panel: #131826; --line: #232b3d;
    --ink: #e8ecf4; --dim: #8c96ab; --amber: #e3b34c;
  }}
  body {{ background: var(--bg); color: var(--ink); margin: 0;
         font: 14px/1.5 ui-monospace, SFMono-Regular, Menlo, monospace; }}
  main {{ max-width: 1180px; margin: 0 auto; padding: 40px 28px 80px; }}
  header h1 {{ font-family: Georgia, 'Times New Roman', serif; font-weight: 400;
               font-size: 30px; margin: 0 0 6px; letter-spacing: .3px; text-wrap: balance; }}
  header p {{ color: var(--dim); margin: 0; max-width: 68ch; }}
  header {{ border-bottom: 1px solid var(--line); padding-bottom: 22px; margin-bottom: 10px; }}
  h2 {{ color: var(--amber); font-size: 12px; text-transform: uppercase;
        letter-spacing: .18em; font-weight: 600; margin: 40px 0 14px; }}
  .grid {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(272px, 1fr));
           gap: 18px; }}
  figure {{ margin: 0; background: var(--panel); border: 1px solid var(--line);
            border-radius: 6px; padding: 10px; }}
  figure img {{ width: 100%; image-rendering: pixelated; display: block;
                border-radius: 2px; }}
  figcaption {{ color: var(--dim); font-size: 12px; padding: 8px 2px 0; }}
</style>
<main>
  <header>
    <h1>Alchemist Study &mdash; every screen, off the bench</h1>
    <p>Rendered by tools/oledsim.py, the U8g2-primitive simulator. One caveat:
       the sim&rsquo;s stand-in font runs ~15% wider than the device fonts, so text
       that looks tight here has more room on the real panel &mdash; judge layout,
       art and collisions, not letterforms.</p>
  </header>
  {''.join(cards)}
</main>
"""


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "screen_review.html"
    with open(out, "w") as fh:
        fh.write(build_page())
    print(f"wrote {out}")
