#!/usr/bin/env python3
"""Contact sheet of the firmware's screens, rendered via oledsim.

Transliterations of main.cpp's renderers (keep them in step when screens
change — this is a REVIEW tool, not a source of truth). Text uses the sim's
approximate font, so exact glyphs/widths differ slightly from the device;
judge layout, art and collisions, not typography.

Usage: python3 tools/screens_preview.py [outdir]
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oledsim import Screen, save_png, lroundf

PHI = 1.618034
ICO_V = [(-1, PHI, 0), (1, PHI, 0), (-1, -PHI, 0), (1, -PHI, 0),
         (0, -1, PHI), (0, 1, PHI), (0, -1, -PHI), (0, 1, -PHI),
         (PHI, 0, -1), (PHI, 0, 1), (-PHI, 0, -1), (-PHI, 0, 1)]
ICO_F = [(0, 11, 5), (0, 5, 1), (0, 1, 7), (0, 7, 10), (0, 10, 11),
         (1, 5, 9), (5, 11, 4), (11, 10, 2), (10, 7, 6), (7, 1, 8),
         (3, 9, 4), (3, 4, 2), (3, 2, 6), (3, 6, 8), (3, 8, 9),
         (4, 9, 5), (2, 4, 11), (6, 2, 10), (8, 6, 7), (9, 8, 1)]
D20_NUM = [20, 8, 14, 2, 16, 10, 4, 18, 6, 12, 1, 13, 7, 19, 5, 11, 17, 3, 15, 9]
FACE_AX, FACE_AY = 0.7854, 0.6155
STARS = [(12, 11, 0), (116, 13, 3), (10, 44, 6), (119, 46, 2), (30, 56, 5), (99, 55, 1)]
PLAYER_MAX_HP = 30


def sparkle(s, x, y, r):
    s.draw_pixel(x, y)
    for i in range(1, r + 1):
        s.draw_pixel(x + i, y); s.draw_pixel(x - i, y)
        s.draw_pixel(x, y + i); s.draw_pixel(x, y - i)


def diamond(s, x, y):
    s.draw_hline(x - 1, y, 3)
    s.draw_pixel(x, y - 1); s.draw_pixel(x, y + 1)


def fancy_frame(s):
    s.draw_frame(0, 0, 128, 64)
    s.draw_frame(3, 3, 122, 58)
    for x, y in ((3, 3), (124, 3), (3, 60), (124, 60)):
        diamond(s, x, y)


def twinkles(s, now, dx=0):
    for x, y, ph in STARS:
        p = (now // 110 + ph * 5) % 16
        tri = p if p < 8 else 15 - p
        sparkle(s, x + dx, y, tri // 3)


def moon(s, x, y, rad):
    s.draw_circle(x, y, rad)
    s.set_draw_color(0)
    s.draw_disc(x + (rad + 1) // 2, y - (rad + 1) // 3, rad)
    s.set_draw_color(1)


def d20(s, cx, cy, r, ax, ay, az, roll=14):
    sa, ca = math.sin(ax), math.cos(ax)
    sb, cb = math.sin(ay), math.cos(ay)
    sc, cc = math.sin(az), math.cos(az)
    k = r / 1.902
    px, py = [0.0] * 12, [0.0] * 12
    for i, (x, y, z) in enumerate(ICO_V):
        y1, z1 = y * ca - z * sa, y * sa + z * ca
        x2, z2 = x * cb + z1 * sb, -x * sb + z1 * cb
        x3, y3 = x2 * cc - y1 * sc, x2 * sc + y1 * cc
        f = 4.5 / (4.5 - z2)
        px[i], py[i] = cx + x3 * k * f, cy + y3 * k * f
    for fi, (a, b, c) in enumerate(ICO_F):
        area = (px[b] - px[a]) * (py[c] - py[a]) - (px[c] - px[a]) * (py[b] - py[a])
        if area <= 0:
            continue
        s.draw_line(px[a], py[a], px[b], py[b])
        s.draw_line(px[b], py[b], px[c], py[c])
        s.draw_line(px[c], py[c], px[a], py[a])
        if area < 450:
            continue
        n = ((D20_NUM[fi] - D20_NUM[0] + roll - 1) % 20 + 20) % 20 + 1
        if area >= 1200:
            s.set_font("ncenB14"); cap = 7
        elif area >= 700:
            s.set_font("ncenB10"); cap = 5
        else:
            s.set_font("5x8"); cap = 3
        gx = lroundf((px[a] + px[b] + px[c]) / 3.0)
        gy = lroundf((py[a] + py[b] + py[c]) / 3.0)
        w = s.get_str_width(str(n))
        s.draw_str(gx - w // 2, gy + cap, str(n))


def rat(s, cx, cy, now):
    y = cy + ((now // 300) & 1)
    sw = math.sin(now * 0.006) * 3.0
    s.draw_line(cx + 11, y, cx + 19, y - 2 + int(sw))
    s.draw_line(cx + 19, y - 2 + int(sw), cx + 25, y - 6 + int(sw * 1.6))
    s.draw_filled_ellipse(cx, y, 13, 7)
    s.draw_disc(cx + 7, y - 1, 6)
    s.draw_disc(cx - 12, y - 2, 5)
    s.draw_triangle(cx - 16, y - 4, cx - 22, y, cx - 15, y + 1)
    s.draw_disc(cx - 13, y - 8 - ((now // 700) & 1), 2)
    s.draw_disc(cx - 9, y - 7, 2)
    for i in range(4):
        s.draw_box(cx - 7 + i * 5, y + 5, 2, 5)
    s.set_draw_color(0)
    s.draw_disc(cx - 12, y - 3, 1)
    s.set_draw_color(1)
    s.draw_line(cx - 21, y - 1, cx - 26, y - 2)
    s.draw_line(cx - 21, y + 1, cx - 26, y + 2)


def crown(s, cx, cy, now):
    yb = cy + lroundf(2.0 * math.sin((now % 3142) * 0.002))
    s.draw_box(cx - 16, yb + 3, 33, 6)
    s.draw_triangle(cx - 16, yb + 3, cx - 11, yb - 8, cx - 5, yb + 3)
    s.draw_triangle(cx - 6, yb + 3, cx, yb - 12, cx + 6, yb + 3)
    s.draw_triangle(cx + 5, yb + 3, cx + 11, yb - 8, cx + 16, yb + 3)
    for tx, ty in ((-11, -10), (0, -14), (11, -10)):
        s.draw_disc(cx + tx, yb + ty, 2)
    s.set_draw_color(0)
    for jx in (-9, 0, 9):
        s.draw_disc(cx + jx, yb + 6, 1)
    s.set_draw_color(1)
    tips = ((-11, -10), (0, -14), (11, -10))
    k = (now // 900) % 3
    gr = 4 if ((now // 150) & 1) else 3
    gx, gy = cx + tips[k][0], yb + tips[k][1]
    s.set_draw_color(2)
    for i in range(1, gr + 1):
        s.draw_pixel(gx + i, gy); s.draw_pixel(gx - i, gy)
        s.draw_pixel(gx, gy + i); s.draw_pixel(gx, gy - i)
    s.set_draw_color(1)


def wrapped(s, text, cx, y, line_h, max_lines, w):
    line, n = "", 0
    for tok in text.split():
        trial = (line + " " + tok).strip()
        if not line or s.get_str_width(trial) <= w:
            line = trial
        else:
            s.draw_str(cx - s.get_str_width(line) // 2, y, line)
            y += line_h; n += 1
            line = tok
            if n >= max_lines:
                return
    if line and n < max_lines:
        s.draw_str(cx - s.get_str_width(line) // 2, y, line)


def choice_line(s, opt):
    s.set_font("helvR08")
    w = s.get_str_width(opt)
    if w <= 100:
        s.set_draw_color(0); s.draw_box(0, 52, 128, 12); s.set_draw_color(1)
        x = (128 - w) // 2
        s.draw_str(x, 62, opt)
        s.set_font("5x8")
        s.draw_str(x - 12, 61, "<"); s.draw_str(x + w + 8, 61, ">")
        return
    words = opt.split()
    best, bd = 0, 999
    idx = 0
    for i, ch in enumerate(opt):
        if ch == " " and abs(i - len(opt) // 2) < bd:
            bd, best = abs(i - len(opt) // 2), i
    l1, l2 = opt[:best], opt[best + 1:]
    w1, w2 = s.get_str_width(l1), s.get_str_width(l2)
    wm = max(w1, w2)
    s.set_draw_color(0); s.draw_box(0, 41, 128, 23); s.set_draw_color(1)
    s.draw_str((128 - w1) // 2, 52, l1)
    s.draw_str((128 - w2) // 2, 62, l2)
    s.set_font("5x8")
    s.draw_str((128 - wm) // 2 - 12, 58, "<")
    s.draw_str((128 + wm) // 2 + 8, 58, ">")


def hp_bar(s, x, y, who, hp, maxhp):
    s.set_font("5x8")
    s.draw_str(x, y + 7, who)
    s.draw_frame(x + 22, y, 78, 8)
    w = (76 * lroundf(hp) + maxhp - 1) // maxhp if hp > 0 else 0
    w = min(w, 76)
    if w > 0:
        s.draw_box(x + 23, y + 1, w, 6)
    s.draw_str(x + 104, y + 7, str(lroundf(hp)))


def story_card(s, title, body, now, show_hp=False, php=30):
    fancy_frame(s)
    s.set_font("helvB08")
    s.draw_centered(title, 16)
    s.set_font("5x8")
    if show_hp:
        wrapped(s, body, 64, 27, 8, 2, 114)
        hp_bar(s, 6, 42, "You", php, PLAYER_MAX_HP)
    else:
        wrapped(s, body, 64, 27, 8, 4, 114)
    s.draw_centered("- press -", 58)


def healer(s, cx, cy, now):
    s.draw_filled_ellipse(cx, cy + 6, 13, 8)
    s.set_draw_color(0)
    s.draw_box(cx - 14, cy - 4, 29, 8)
    s.set_draw_color(1)
    s.draw_box(cx - 13, cy + 3, 27, 3)
    s.draw_box(cx - 5, cy + 14, 10, 3)
    a = 0.6 + 0.25 * math.sin(now * 0.004)
    tx = cx + lroundf(16.0 * math.cos(a))
    ty = cy + 4 - lroundf(20.0 * math.sin(a))
    for o in (-1, 0, 1):
        s.draw_line(cx + 2 + o, cy + 4, tx + o, ty)
    s.draw_disc(tx, ty, 3)
    if (now // 400) % 2:
        s.draw_pixel(cx - 6, cy - 2)
        s.draw_pixel(cx - 9, cy - 5)


def steward(s, cx, cy, now):
    s.draw_box(cx - 8, cy - 14, 17, 7)
    s.draw_triangle(cx - 8, cy - 7, cx + 8, cy - 7, cx, cy + 1)
    s.draw_box(cx - 1, cy + 1, 3, 8)
    s.draw_box(cx - 6, cy + 9, 13, 3)
    s.set_draw_color(0)
    for x in range(cx - 7, cx + 8):
        s.draw_pixel(x, cy - 12 + lroundf(1.2 * math.sin(x * 0.9 + now * 0.005)))
    s.set_draw_color(1)
    fall = (now // 60) % 26
    s.draw_pixel(cx + 9, cy - 13 + fall)
    s.draw_pixel(cx + 9, cy - 12 + fall)
    if fall > 13:
        s.draw_pixel(cx + 9, cy - 13)


def speak(s, speaker, words, now, art=None):
    (art or crown)(s, 20, 30, now)
    s.set_font("4x6")
    nw = s.get_str_width(speaker)
    s.draw_str(max(1, 20 - nw // 2), 62, speaker)
    s.draw_rframe(42, 4, 84, 44, 5)
    s.draw_line(42, 26, 36, 30)
    s.draw_line(42, 34, 36, 30)
    s.set_draw_color(0); s.draw_vline(42, 27, 7); s.set_draw_color(1)
    s.set_font("5x8")
    wrapped(s, words, 84, 14, 8, 4, 76)
    s.set_font("4x6")
    s.draw_str(104, 62, "press")


def battle(s, now, php, ehp, mode, msg=""):
    hp_bar(s, 2, 0, "You", php, PLAYER_MAX_HP)
    hp_bar(s, 2, 10, "Rat", ehp, 15)
    rat(s, 72, 34, now)
    if mode == "choose":
        choice_line(s, msg)
    else:
        s.set_font("5x8")
        wrapped(s, msg, 64, 54, 8, 2, 124)


def roll_frame(s, el, roll):
    if el < 1100:
        t = el / 1100.0
        u = 1.0 - t
        s.set_font("5x8")
        s.draw_centered("you attack!", 8)
        s.draw_hline(8, 56, 112)
        cx = 14 + lroundf(t * 50.0)
        bounce = abs(math.sin(t * 3.0 * math.pi))
        cy = 47 - lroundf(24.0 * u * bounce)
        d20(s, cx, cy, 9, FACE_AX + 1.5 + 12 * u * u, FACE_AY + 1.0 + 9 * u * u,
            6.0 * u * u, roll)
        if cx >= 26 and t < 0.7:
            s.draw_hline(cx - 20, cy - 3, 6)
            s.draw_hline(cx - 26, cy + 2, 8)
    elif el < 1700:
        t = (el - 1100) / 600.0
        e = t * t * (3 - 2 * t)
        d20(s, 64, 47 - lroundf(e * 15), 9 + lroundf(e * 19),
            FACE_AX + 1.5 * (1 - e), FACE_AY + 1.0 * (1 - e), 0.0, roll)
    else:
        d20(s, 64, 32, 28, FACE_AX, FACE_AY, 0.0, roll)
        hold = el - 1700
        if hold < 300:
            s.draw_circle(64, 32, 30 + hold // 6)


def brew(s, combo_names, hint=None, title="Brew: healing potion"):
    s.draw_box(0, 0, 128, 13)
    s.set_draw_color(0)
    s.set_font("helvB08")
    s.draw_str(4, 10, title)
    s.set_draw_color(1)
    s.set_font("5x8")
    if not combo_names:
        s.draw_centered("Place ingredients", 28)
        if hint:
            wrapped(s, hint, 64, 41, 8, 2, 120)
        s.draw_centered("press to go back", 62)
    else:
        y = 27
        for nm in combo_names:
            s.draw_centered(nm, y); y += 10
        s.draw_centered("turn to stir", 62)


def tune(s, now):
    for i in range(3):
        rise = (now // 30 + i * 27) % 40
        x = 64 - 30 + i * 30 + lroundf(4.0 * math.sin(now * 0.003 + i * 2.0))
        y = 24 + 14 - rise
        s.draw_disc(x, y, 2)
        s.draw_vline(x + 2, y - 8, 8)
        s.draw_line(x + 2, y - 8, x + 5, y - 5)
    s.set_font("helvB08")
    s.draw_centered("You strum an old tune", 50)
    s.set_font("5x8")
    s.draw_centered("press to stop", 62)


def gear(s, cx, cy, now):
    s.draw_circle(cx, cy, 8)
    s.draw_circle(cx, cy, 3)
    spin = now * 0.0012
    for k in range(8):
        a = spin + k * math.pi / 4
        s.draw_box(cx + lroundf(10 * math.cos(a)) - 1,
                   cy - lroundf(10 * math.sin(a)) - 1, 3, 3)


def home_panel(s, which, dx, now):
    if which == "place":
        twinkles(s, now, dx)
        moon(s, 15 + dx, 15, 6)
        s.set_font("ncenB12")
        s.draw_str((128 - s.get_str_width("Place the")) // 2 + dx, 26, "Place the")
        s.draw_str((128 - s.get_str_width("ingredients")) // 2 + dx, 42, "ingredients")
        s.draw_hline(28 + dx, 48, 72)
        diamond(s, 22 + dx, 48); diamond(s, 106 + dx, 48)
        s.set_font("5x8")
        s.draw_str((128 - s.get_str_width("turn to explore")) // 2 + dx, 57, "turn to explore")
    elif which == "story":
        t = (now % 62832) * 0.001
        d20(s, 64 + dx, 16, 11, 0.9 * t, 0.7 * t, 0.5 * t)
        sr = 2 if ((now // 260) & 1) else 1
        sparkle(s, 38 + dx, 12, sr); sparkle(s, 90 + dx, 12, sr)
        s.set_font("ncenB12")
        s.draw_str((128 - s.get_str_width("Story Mode")) // 2 + dx, 44, "Story Mode")
        s.set_font("5x8")
        s.draw_str((128 - s.get_str_width("press to begin")) // 2 + dx, 57, "press to begin")
    else:
        gear(s, 64 + dx, 16, now)
        s.set_font("ncenB12")
        s.draw_str((128 - s.get_str_width("Settings")) // 2 + dx, 44, "Settings")
        s.set_font("5x8")
        s.draw_str((128 - s.get_str_width("press to enter")) // 2 + dx, 57, "press to enter")


def home(s, now, panel_names, scroll, sel):
    for i, nm in enumerate(panel_names):
        dx = i * 128 - scroll
        W3 = len(panel_names) * 128
        if dx < -W3 // 2: dx += W3
        if dx >= W3 // 2: dx -= W3
        if -128 < dx < 128:
            home_panel(s, nm, dx, now)
    dot_x = (128 - (len(panel_names) - 1) * 12) // 2
    for i in range(len(panel_names)):
        if i == sel:
            s.draw_box(dot_x - 1, 60, 3, 3)
        else:
            s.draw_pixel(dot_x, 61)
        dot_x += 12
    s.draw_line(3, 28, 1, 31); s.draw_line(1, 31, 3, 34)
    s.draw_line(124, 28, 126, 31); s.draw_line(126, 31, 124, 34)


def choice_scene(s, now, prompt, opt, gold, php=30):
    hp_bar(s, 2, 0, "You", php, PLAYER_MAX_HP)
    s.set_font("5x8")
    p = f"{gold} gp"
    s.draw_str(126 - s.get_str_width(p), 19, p)
    s.set_font("helvB08")
    s.draw_centered(prompt, 32)
    choice_line(s, opt)


def flame_layer(s, now, cam, cx0, base_vy, height, max_w, seed, color, sway_seed):
    flick = lroundf(4.0 * math.sin(now * 0.009 + seed) +
                    3.0 * math.sin(now * 0.023 + seed * 2.3))
    tip_vy = base_vy - height - flick
    s.set_draw_color(color)
    for vy in range(tip_vy, base_vy + 1):
        y = vy - cam
        if not (0 <= y < 64):
            continue
        h = (vy - tip_vy) / max(1, base_vy - tip_vy)
        half = max_w * math.sqrt(h)
        sway = (1 - h) * (1 - h) * 5.0 * math.sin(now * 0.0017 + sway_seed)
        lick = (1 - h) * 4.0 + 1.0
        fx = cx0 + sway
        eL = fx - half + lick * math.sin(vy * 0.23 + now * 0.011 + seed)
        eR = fx + half + lick * math.sin(vy * 0.29 + now * 0.013 + seed * 1.7)
        if eR > eL:
            s.draw_hline(lroundf(eL), y, lroundf(eR - eL) + 1)
    s.set_draw_color(1)
    return tip_vy


def log_slab(s, cam, x0, y0, x1, y1, half):
    dx, dy = x1 - x0, y1 - y0
    ln = math.hypot(dx, dy)
    px, py = lroundf(-dy / ln * half), lroundf(dx / ln * half)
    s.draw_triangle(x0 + px, y0 + py - cam, x1 + px, y1 + py - cam, x1 - px, y1 - py - cam)
    s.draw_triangle(x0 + px, y0 + py - cam, x1 - px, y1 - py - cam, x0 - px, y0 - py - cam)
    s.draw_disc(x0, y0 - cam, half)
    s.draw_disc(x1, y1 - cam, half)


def campfire(s, el):
    # v3 organic fire (twin of drawCampfire; see scratch fire3_sketch.py)
    now = el
    t = min(1.0, el / 3500.0)
    e = t * t * (3 - 2 * t)
    cam = lroundf(e * 136.0)
    for i in range(4):                      # smoke wisps
        rise = (now // 18 + i * 47) % 120
        head = 148 - rise
        xc = 52 + i * 10
        for d in range(36):
            y = head + d - cam
            if not (0 <= y < 64):
                continue
            amp = 2.0 + (36 - d) * 0.16
            x = xc + lroundf(amp * math.sin((head + d) * 0.17 + now * 0.002 + i * 1.7))
            s.draw_pixel(x, y); s.draw_pixel(x + 1, y)
    flame_layer(s, now, cam, 46, 193, 24, 14, 4.0, 1, 4.0)
    flame_layer(s, now, cam, 82, 193, 20, 13, 8.5, 1, 8.5)
    tip = flame_layer(s, now, cam, 64, 193, 50, 30, 1.0, 1, 1.0)
    s.set_draw_color(0)                     # tongue separations
    for i in range(2):
        x0 = 56 + i * 15
        top = 166 - lroundf(5 * math.sin(now * 0.006 + i * 2.2))
        for vy in range(top, 192):
            y = vy - cam
            if not (0 <= y < 64):
                continue
            x = x0 + lroundf(3.0 * math.sin(vy * 0.3 + now * 0.008 + i * 2.0))
            s.draw_pixel(x, y); s.draw_pixel(x + 1, y)
    s.set_draw_color(1)
    if math.sin(now * 0.007 + 1.0) > 0.45:  # detached lick
        lx = 64 + lroundf(5.0 * math.sin(now * 0.0017 + 1.0))
        if tip - 5 - cam >= 2:
            s.draw_disc(lx, tip - 5 - cam, 2)
    log_slab(s, cam, 12, 195, 72, 186, 5)
    log_slab(s, cam, 56, 186, 116, 196, 5)
    s.set_draw_color(0)
    s.draw_circle(12, 195 - cam, 2)
    s.draw_circle(116, 196 - cam, 2)
    for d in range(4):
        x = 22 + d * 12
        s.draw_line(x, 196 - d - cam, x + 6, 195 - d - cam)
        s.draw_line(x + 46, 188 + d - cam, x + 52, 189 + d - cam)
    s.set_draw_color(1)
    for i in range(6):                      # embers
        ph = (now // 90 + i * 31) % 60
        if ph < 34:
            ex = 26 + (i * 19) % 76 + lroundf(2 * math.sin(now * 0.004 + i))
            ey = 184 - ph - cam
            s.draw_pixel(ex, ey)
            if ph < 16:
                s.draw_pixel(ex + 1, ey)


def hooded(s, x, y, now, phase):
    bob = lroundf(1.5 * math.sin(now * 0.005 + phase))
    sway = lroundf(1.0 * math.sin(now * 0.002 + phase * 2))
    x += sway
    for r in range(21):
        half = 5 + (r * 5) // 20
        lx, rx = max(0, x - half), min(127, x + half)
        if rx >= 0 and lx <= 127 and rx >= lx:
            s.draw_hline(lx, y - 20 + r, rx - lx + 1)
    lx, rx = max(0, x - 4), min(127, x + 4)
    if rx >= lx:
        s.draw_box(lx, y - 23, rx - lx + 1, 5)
    s.draw_disc(x + 1, y - 26 + bob, 6)
    s.set_draw_color(0)
    s.draw_disc(x + 3, y - 24 + bob, 3)
    s.set_draw_color(1)


def sneak(s, el):
    now = el
    t = min(1.0, el / 4200.0)
    e = t * t * (3 - 2 * t)
    cam = lroundf(e * 110.0)
    s.draw_hline(0, 54, 128)
    for b in range(2):
        x = (156 if b else 66) - cam
        if 0 <= x - 3 and x + 4 < 128:
            s.draw_box(x - 3, 50, 7, 4)
            s.draw_vline(x, 46, 4)
    for i in range(4):
        rise = (now // 20 + i * 43) % 70
        vx = 66 + (i % 2) * 90
        head = 44 - rise
        x0 = vx - cam
        if -8 < x0 < 136 and 0 <= head < 64:
            amp = 1.5 + rise * 0.09
            x = x0 + lroundf(amp * math.sin((head + rise) * 0.2 + now * 0.002 + i))
            s.draw_pixel(x, head)
            s.draw_pixel(x + 1, head)
    for i, vx in enumerate((30, 104, 186, 238)):
        x = vx - cam
        if -20 < x < 148:
            hooded(s, x, 54, now, i * 1.9)
    for k in range(int(el / 260)):
        fx = 6 + k * 9 - cam
        if 0 <= fx < 126:
            s.draw_hline(fx, 60 if (k & 1) else 58, 3)


def power_bar(s, progress):
    bx, by, bw, bh = 14, 41, 100, 10
    s.draw_frame(bx, by, bw, bh)
    fill = lroundf((bw - 4) * progress)
    if fill > 0:
        s.draw_box(bx + 2, by + 2, fill, bh - 4)


def attune(s, now, player, target, ok, progress, msg):
    s.set_font("helvR08")
    s.draw_centered("- attuning -", 8)
    wy = 25
    AMP, KX = 8.0, 0.10
    for x in range(2, 126):
        ty = wy + lroundf(AMP * math.sin(KX * x + target))
        if x % 3 == 0:
            s.draw_pixel(x, ty)
        py = wy + lroundf(AMP * math.sin(KX * x + player))
        s.draw_pixel(x, py)
        if ok:
            s.draw_pixel(x, py + 1)
    if ok:
        sr = 2 if (now // 180) & 1 else 1
        sparkle(s, 6, wy, sr)
        sparkle(s, 122, wy, sr)
    power_bar(s, progress)
    s.set_font("5x8")
    s.draw_centered(msg, 62)


RIT_WORDS = ["turn right", "turn left", "press"]   # SYM_CW, SYM_CCW, SYM_PRESS


def rit_glyph(s, sym, cx, cy):
    R = 13
    if sym == 2:  # press: dotted ring + hub
        s.draw_circle(cx, cy, R)
        s.draw_disc(cx, cy, 5)
        return
    d = 1 if sym == 0 else -1
    t = 0.35
    while t < 5.6:
        a = d * t - math.pi / 2
        s.draw_pixel(cx + lroundf(R * math.cos(a)), cy + lroundf(R * math.sin(a)))
        t += 0.10
    ae = d * 5.6 - math.pi / 2
    tx, ty = -math.sin(ae) * d, math.cos(ae) * d
    nx, ny = math.cos(ae), math.sin(ae)
    hx, hy = cx + lroundf(R * math.cos(ae)), cy + lroundf(R * math.sin(ae))
    s.draw_triangle(hx + lroundf(6.0 * tx), hy + lroundf(6.0 * ty),
                    hx + lroundf(3.5 * nx), hy + lroundf(3.5 * ny),
                    hx - lroundf(3.5 * nx), hy - lroundf(3.5 * ny))


def corner_sparkles(s, now):
    r = 1 if (now // 300) & 1 else 0
    for x, y in ((9, 10), (118, 10), (9, 53), (118, 53)):
        sparkle(s, x, y, r)


def ritual_intro(s):
    fancy_frame(s)
    s.set_font("helvR08")
    s.draw_centered("all three essences...", 16)
    s.set_font("ncenB12")
    s.draw_centered("The Grand", 33)
    s.draw_centered("Brew", 49)
    s.set_font("5x8")
    s.draw_centered("watch the incantation", 60)


def ritual_show(s, verse, rounds, sym):
    s.set_font("5x8")
    s.draw_centered("verse %d of %d - watch" % (verse, rounds), 8)
    rit_glyph(s, sym, 64, 32)
    s.set_font("5x8")
    s.draw_centered(RIT_WORDS[sym], 60)


def ritual_input(s, now, verse, rounds, length, answered):
    s.set_font("5x8")
    s.draw_centered("verse %d of %d - repeat" % (verse, rounds), 8)
    bs, gap = 12, 6
    x0 = (128 - (length * bs + (length - 1) * gap)) // 2
    for i in range(length):
        x = x0 + i * (bs + gap)
        if i < answered:
            s.draw_box(x, 26, bs, bs)
        else:
            s.draw_frame(x, 26, bs, bs)
            if i == answered and (now // 300) & 1:
                s.draw_frame(x + 2, 28, bs - 4, bs - 4)
    s.set_font("5x8")
    s.draw_centered("turn or press to answer", 62)


def ritual_good(s, now):
    corner_sparkles(s, now)
    s.set_font("ncenB12")
    s.draw_centered("Well stirred!", 34)
    s.set_font("5x8")
    s.draw_centered("the brew deepens...", 56)


def ritual_miss(s):
    s.set_font("ncenB12")
    s.draw_centered("It resists!", 34)
    s.set_font("5x8")
    s.draw_centered("listen again...", 56)


def build():
    panels = ["place", "story", "settings"]
    shots = []

    def shot(fn):
        sc = Screen()
        fn(sc)
        shots.append(sc)

    now = 1234
    shot(lambda s: home(s, now, panels, 0, 0))
    shot(lambda s: home(s, now, panels, 64, 1))       # mid-slide
    shot(lambda s: home(s, now, panels, 128, 1))
    shot(lambda s: home(s, now, panels, 256, 2))
    shot(lambda s: story_card(s, "The Alchemist's Quest",
                              "Whiterun sickens. Even the bread tastes wrong. The Jarl summons you.", 100))
    shot(lambda s: choice_scene(s, now, "How do you begin?",
                                "Request audience with Jarl", 7))
    shot(lambda s: speak(s, "Jarl Balgruuf",
                         "Blue mountain flower mends flesh. Trust no one here.", now))
    shot(lambda s: battle(s, now, 30, 15, "msg", "Why is this rat so big? Something unnatural here"))
    shot(lambda s: battle(s, now, 23, 15, "choose", "Attack for 4-7"))
    shot(lambda s: roll_frame(s, 500, 14))
    shot(lambda s: roll_frame(s, 1400, 14))
    shot(lambda s: roll_frame(s, 1900, 14))
    shot(lambda s: battle(s, now, 6, 9, "msg",
                          "CRIT! The bite FESTERS! Your arm goes numb..."))
    shot(lambda s: battle(s, now, 6, 9, "choose", "Brew potion"))
    shot(lambda s: brew(s, [], "'blue mountain flower mends flesh'"))
    shot(lambda s: brew(s, ["Blue Mountain Flower"]))
    shot(lambda s: story_card(s, "The Bannered Mare",
                              "The innkeep eyes your purse. A hot meal or a drink?", 100))
    shot(lambda s: choice_scene(s, now, "Spend your coin on...",
                                "Sweetroll: -5gp, +15HP", 7, php=16))
    shot(lambda s: story_card(s, "Sweetroll",
                              "Sweet as victory - and no thief in sight.", 100,
                              show_hp=True, php=26))
    shot(lambda s: choice_scene(s, now, "The fire burns low...", "Play the lute", 2))
    shot(lambda s: tune(s, now))
    shot(lambda s: campfire(s, 4200))
    shot(lambda s: story_card(s, "Act 2",
                              "You wake healed. Yet the rat bite weeps...", 100,
                              show_hp=True, php=30))
    shot(lambda s: speak(s, "Healer Danica",
                         "Deathbell rot. No rat carries this. Something FED it.",
                         now, art=healer))
    shot(lambda s: story_card(s, "The Granary",
                              "Black petals in the grain. One man holds the key: the steward.", 100))
    shot(lambda s: choice_scene(s, now, "The steward...", "Watch the granary", 2))
    shot(lambda s: brew(s, [], "'deathbell bound to the flower'",
                        title="Brew: lingering poison"))
    shot(lambda s: choice_scene(s, now, "At the feast:", "Lace his goblet", 2))
    shot(lambda s: speak(s, "The Steward",
                         "You fools! Peryite avenges his loyal servants!", now, art=steward))
    shot(lambda s: story_card(s, "Peryite",
                              "Plague god. His shrine smokes in the mountains. Act 3 awaits...", 100))
    shot(lambda s: brew(s, [], "'all three bottles as one'", title="Brew: the Philter"))
    shot(lambda s: sneak(s, 2600))
    shot(lambda s: choice_scene(s, now, "The great cauldron:", "Counter-brew it", 2))
    shot(lambda s: brew(s, [], "'what quenches a plague?'", title="Brew: the counter"))
    shot(lambda s: story_card(s, "Bad Ending",
                              "Skyrim's rivers run grey. Remember the Jarl's words... and try again.", 100))
    shot(lambda s: story_card(s, "Realm Saved",
                              "Skyrim drinks clean. The Jarl names you Alchemist of Whiterun.", 100,
                              show_hp=True, php=30))
    shot(lambda s: speak(s, "Jarl Balgruuf",
                         "You mended more than flesh. Skyrim owes you, alchemist.", now))
    # Brewing acts (minigames): act 2 attuning + act 3 ritual phases.
    shot(lambda s: attune(s, now, 0.3, 2.2, False, 0.15, "seek the resonance"))
    shot(lambda s: attune(s, now, 1.9, 2.0, True, 0.65, "hold the resonance"))
    shot(lambda s: ritual_intro(s))
    shot(lambda s: ritual_show(s, 1, 3, 0))
    shot(lambda s: ritual_show(s, 1, 3, 1))
    shot(lambda s: ritual_show(s, 2, 3, 2))
    shot(lambda s: ritual_input(s, now, 2, 3, 3, 1))
    shot(lambda s: ritual_good(s, now))
    shot(lambda s: ritual_miss(s))
    return shots


if __name__ == "__main__":
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    shots = build()
    half = (len(shots) + 1) // 2
    save_png(os.path.join(outdir, "screens_a.png"), shots[:half], scale=3)
    save_png(os.path.join(outdir, "screens_b.png"), shots[half:], scale=3)
