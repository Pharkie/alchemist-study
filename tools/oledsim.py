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

    def set_draw_color(self, c):
        self.color = c

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
