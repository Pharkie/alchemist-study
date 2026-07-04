#!/usr/bin/env python3
"""OLED simulator: preview the firmware's procedural art off-device.

Replicates the subset of U8g2 drawing primitives main.cpp uses (same integer
rounding, same y-down coordinates, same draw-color semantics) on a 128x64
buffer, and writes an upscaled PNG so a human (or Claude) can look at frames
before flashing. Stdlib only — the PNG writer is hand-rolled over zlib.

Usage: import Screen from a sketch script, transliterate the C++ drawing
function, render a few `now` timestamps, save_png(), look, iterate, then port
the final maths back to C++ (keep the two literally line-for-line).
"""

import math
import struct
import zlib

W, H = 128, 64


def lroundf(x):
    """C lroundf: round half away from zero (Python's round() is banker's)."""
    return int(math.floor(x + 0.5)) if x >= 0 else -int(math.floor(-x + 0.5))


class Screen:
    def __init__(self):
        self.px = [[0] * W for _ in range(H)]
        self.color = 1          # 0 black, 1 white, 2 XOR (u8g2 setDrawColor)
        self.font = ("5x8")

    def set_draw_color(self, c):
        self.color = c

    def set_font(self, name):
        self.font = name

    def get_str_width(self, text):
        _, adv = _fontspec(self.font)
        return len(text) * adv

    def draw_str(self, x, y, text):
        """u8g2 semantics: y is the BASELINE."""
        scale, adv = _fontspec(self.font)
        for ch in text:
            cols = _glyph(ch)
            for c in range(5):
                bits = cols[c]
                for b in range(7):
                    if bits & (1 << b):
                        for sy in range(scale):
                            for sx in range(scale):
                                self.draw_pixel(x + c * scale + sx,
                                                y - (7 - b) * scale + sy)
            x += adv

    def draw_centered(self, text, y):
        self.draw_str((W - self.get_str_width(text)) // 2, y, text)

    def draw_pixel(self, x, y):
        x, y = int(x), int(y)
        if 0 <= x < W and 0 <= y < H:
            if self.color == 2:
                self.px[y][x] ^= 1
            else:
                self.px[y][x] = self.color

    def draw_hline(self, x, y, w):
        for i in range(int(w)):
            self.draw_pixel(x + i, y)

    def draw_vline(self, x, y, h):
        for i in range(int(h)):
            self.draw_pixel(x, y + i)

    def draw_box(self, x, y, w, h):
        for j in range(int(h)):
            self.draw_hline(x, y + j, w)

    def draw_frame(self, x, y, w, h):
        self.draw_hline(x, y, w)
        self.draw_hline(x, y + h - 1, w)
        self.draw_vline(x, y, h)
        self.draw_vline(x + w - 1, y, h)

    def draw_rframe(self, x, y, w, h, r):
        self.draw_hline(x + r, y, w - 2 * r)
        self.draw_hline(x + r, y + h - 1, w - 2 * r)
        self.draw_vline(x, y + r, h - 2 * r)
        self.draw_vline(x + w - 1, y + r, h - 2 * r)
        for cx, cy, sx, sy in ((x + r, y + r, -1, -1), (x + w - 1 - r, y + r, 1, -1),
                               (x + r, y + h - 1 - r, -1, 1), (x + w - 1 - r, y + h - 1 - r, 1, 1)):
            for i in range(r + 1):
                j = lroundf(math.sqrt(max(0.0, r * r - i * i)))
                self.draw_pixel(cx + sx * i, cy + sy * j)
                self.draw_pixel(cx + sx * j, cy + sy * i)

    def draw_line(self, x0, y0, x1, y1):
        x0, y0, x1, y1 = int(x0), int(y0), int(x1), int(y1)
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            self.draw_pixel(x0, y0)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def draw_disc(self, cx, cy, r):
        for y in range(-r, r + 1):
            for x in range(-r, r + 1):
                if x * x + y * y <= r * r + r:   # u8g2 discs run a hair fat
                    self.draw_pixel(cx + x, cy + y)

    def draw_circle(self, cx, cy, r):
        for i in range(0, int(r * 8) + 8):
            a = i / (r * 8 + 8) * 2 * math.pi
            self.draw_pixel(cx + lroundf(r * math.cos(a)), cy + lroundf(r * math.sin(a)))

    def draw_filled_ellipse(self, cx, cy, rx, ry):
        for y in range(-ry, ry + 1):
            for x in range(-rx, rx + 1):
                if (x * x) / (rx * rx) + (y * y) / (ry * ry) <= 1.0:
                    self.draw_pixel(cx + x, cy + y)

    def draw_triangle(self, x0, y0, x1, y1, x2, y2):
        """Filled, like u8g2's drawTriangle."""
        ymin, ymax = min(y0, y1, y2), max(y0, y1, y2)
        for y in range(int(ymin), int(ymax) + 1):
            xs = []
            for (ax, ay), (bx, by) in (((x0, y0), (x1, y1)), ((x1, y1), (x2, y2)),
                                       ((x2, y2), (x0, y0))):
                if ay == by:
                    if y == ay:
                        xs += [ax, bx]
                    continue
                lo, hi = (ay, by) if ay < by else (by, ay)
                if lo <= y <= hi:
                    xs.append(ax + (bx - ax) * (y - ay) / (by - ay))
            if xs:
                self.draw_hline(lroundf(min(xs)), y, lroundf(max(xs)) - lroundf(min(xs)) + 1)


# Classic 5x7 column font (bit 0 = top row), ASCII 32..126. Used to
# APPROXIMATE the firmware's fonts for layout review: scale 1 stands in for
# 4x6/5x8/helv08 (runs ~15% wide), scale 2 for the ncenB12/14 serifs.
_F = [
    "0000000000", "00005F0000", "0007000700", "147F147F14", "242A7F2A12",
    "2313086462", "3649552250", "0005030000", "001C224100", "0041221C00",
    "082A1C2A08", "08083E0808", "0050300000", "0808080808", "0060600000",
    "2010080402", "3E5149453E", "00427F4000", "4261514946", "2141454B31",
    "181412 7F10".replace(" ", ""), "2745454539", "3C4A494930", "0171090503",
    "3649494936", "064949291E", "0036360000", "0056360000", "0008142241",
    "1414141414", "4122140800", "0201510906", "324979413E", "7E1111117E",
    "7F49494936", "3E41414122", "7F4141221C", "7F49494941", "7F09090101",
    "3E41415132", "7F0808087F", "00417F4100", "2040413F01", "7F08142241",
    "7F40404040", "7F0204027F", "7F0408107F", "3E4141413E", "7F09090906",
    "3E4151215E", "7F09192946", "4649494931", "01017F0101", "3F4040403F",
    "1F2040201F", "7F2018207F", "6314081463", "0304780403", "6151494543",
    "00007F4141", "0204081020", "41417F0000", "0402010204", "4040404040",
    "0001020400", "2054545478", "7F48444438", "3844444420", "384444487F",
    "3854545418", "087E090102", "0814545 43C".replace(" ", ""), "7F08040478",
    "00447D4000", "2040443D00", "007F102844", "00417F4000", "7C04180478",
    "7C08040478", "3844444438", "7C14141408", "081414187C", "7C08040408",
    "4854545420", "043F444020", "3C4040207C", "1C2040201C", "3C4030403C",
    "4428102844", "0C5050503C", "4464544C44", "0008364100", "00007F0000",
    "0041360800", "08082A1C08",
]
FONT = [[int(g[i:i + 2], 16) for i in (0, 2, 4, 6, 8)] for g in _F]


def _glyph(ch):
    o = ord(ch)
    return FONT[o - 32] if 32 <= o < 127 else FONT[31]   # '?' for exotics


class Font:
    """Named font -> (pixel scale, x advance). Approximate, for layout only."""
    MAP = {
        "4x6": (1, 5), "5x8": (1, 6), "helvR08": (1, 6), "helvB08": (1, 6),
        "ncenR08": (1, 6), "ncenB10": (1, 6),
        "ncenB12": (2, 12), "ncenB14": (2, 12), "ncenB18": (2, 12),
    }


def _fontspec(name):
    return Font.MAP.get(name, (1, 6))


def save_png(path, screens, scale=4, gap=4):

    """Write the given Screens side by side as one grayscale PNG."""
    n = len(screens)
    iw = n * W * scale + (n - 1) * gap
    ih = H * scale
    rows = []
    for y in range(ih):
        row = bytearray([0])                      # PNG filter byte: none
        for f in range(n):
            s = screens[f]
            for x in range(W):
                v = 255 if s.px[y // scale][x] else 8
                row += bytes([v]) * scale
            if f < n - 1:
                row += bytes([70]) * gap
        rows.append(bytes(row))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", iw, ih, 8, 0, 0, 0, 0)   # 8-bit grayscale
    idat = zlib.compress(b"".join(rows), 6)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
                 chunk(b"IDAT", idat) + chunk(b"IEND", b""))
    print(f"wrote {path} ({iw}x{ih}, {n} frame(s))")
