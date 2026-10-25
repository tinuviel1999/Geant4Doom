#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Weapon sprites as particle-physics instruments, for doomviewer's view (R_DrawPSprite):
    electron gun       a glass electron gun: hot cathode, copper anode rings, HV cable
    pion scattergun    a magnetic focusing horn (as in neutrino beam lines) around a target
    muon chaingun      a copper RF cavity linac whose cells glow in turn
    carbon-ion cannon  a frosted blue LHC-style cryostat with hazard bands and a gauge
    proton rifle       a yellow SRF cryomodule, cut open on a niobium multi-cell cavity
    antiproton BFG     a Penning trap cryostat with a viewport on its antiproton cloud
Each is drawn procedurally (shaded tubes seen from behind, 4x supersampled) over the same
screen area as Doom's sprite, with the flashes at the muzzle. Written to hud/instruments/ with
their own offsets.txt (the patch offsets doomviewer positions them by); `/doom/weaponStyle
instruments` shows them, `/doom/weaponStyle doom` (the default) Doom's own.

    python3 weapon_sprites.py e1m1/hud
"""
import math
import os
import sys

import numpy as np
from PIL import Image

S = 4                        # supersampling
X0, Y0, W, H = 0, 30, 320, 190  # drawn region of Doom's 320x200 screen (the view is rows 0..167)
ASPECT = 1.2                 # Doom's pixels are 1.2 times taller than wide
LIGHT = np.array((-0.45, 0.89))  # from the upper left
# The instruments are drawn in design coordinates, shown KX times wider and KY times as tall
# (foreshortened: they recede from the eye), design (160, 205) at screen (160, 177). Their tops
# stay at about row 118, as Doom's weapons do (they never reach the crosshair); the view ends at
# row 168, so the hands sit at design ~168 (screen ~150).
KX, KY = 1.3, 0.75
HANDS = 168

FONT = {  # 3x5 labels
    "C": "011100100100011", "6": "011100110101010", "+": "000010111010000", "P": "110101110100100",
    "E": "111100110100111", "-": "000000111000000", "H": "101101111101101", "V": "101101101101010",
}


class Canvas:
    """Premultiplied RGBA at S times Doom's resolution, composited front to back by `over`"""

    def __init__(self):
        self.c = np.zeros((H * S, W * S, 3))
        self.a = np.zeros((H * S, W * S))
        self.x = 160 + ((np.arange(W * S) + 0.5) / S + X0 - 160) / KX
        self.y = 205 + ((np.arange(H * S) + 0.5) / S + Y0 - 177) / KY
        self.X, self.Y = np.meshgrid(self.x, self.y)

    def over(self, alpha, rgb):
        alpha = np.clip(alpha, 0, 1)
        rgb = np.broadcast_to(np.asarray(rgb, float), self.c.shape)
        self.c = rgb * alpha[..., None] + self.c * (1 - alpha[..., None])
        self.a = alpha + self.a * (1 - alpha)

    def glow(self, cx, cy, r, rgb, strength=1.0, power=1.6, squash=1.0):
        d = np.hypot(self.X - cx, (self.Y - cy) * ASPECT / squash) / r
        self.over(strength * np.clip(1 - d, 0, 1) ** power, rgb)

    def disc(self, cx, cy, r, rgb, alpha=1.0):
        d = np.hypot(self.X - cx, (self.Y - cy) * ASPECT)
        self.over(alpha * np.clip(r - d + 0.5, 0, 1), rgb)

    def ellipse(self, cx, cy, rx, ry, rgb, alpha=1.0):
        d = np.hypot((self.X - cx) / rx, (self.Y - cy) / ry)
        self.over(alpha * np.clip((1 - d) * min(rx, ry) * S, 0, 1), rgb)

    def line(self, pts, width, rgb, alpha=1.0):
        """A polyline of thickness `width` (screen pixels)"""
        m = np.zeros_like(self.a)
        for (x1, y1), (x2, y2) in zip(pts[:-1], pts[1:]):
            dx, dy = x2 - x1, (y2 - y1) * ASPECT
            L2 = dx * dx + dy * dy or 1e-9
            t = np.clip(((self.X - x1) * dx + (self.Y - y1) * ASPECT * dy) / L2, 0, 1)
            d = np.hypot(self.X - (x1 + t * dx), (self.Y - y1) * ASPECT - t * dy)
            m = np.maximum(m, np.clip(width / 2 - d + 0.5 / S, 0, 1))
        self.over(alpha * m, rgb)

    def text(self, s, x, y, rgb):
        for ch in s:
            for i, bit in enumerate(FONT.get(ch, "")):
                if bit == "1":
                    px, py = x + i % 3, y + i // 3
                    self.over(((self.X >= px) & (self.X < px + 1) & (self.Y >= py) & (self.Y < py + 1)).astype(float), rgb)
            x += 4

    def tube(self, bottom, top, bands, bulge=None, cap=0.06, mask_only=False):
        """A cylinder seen from behind and above, receding from (cx, y, r) at the bottom to
        (cx, y, r) at the top. bands: [(t0, t1, rgb, shine)] along it (t = 0 bottom .. 1 top),
        later bands over earlier ones; bulge(t) scales the radius (cavity cells, horns).
        Returns the coverage and the (t, u) coordinates, u = -1..1 across."""
        (cb, yb, rb), (ct, yt, rt) = bottom, top
        t = (yb - self.Y) / (yb - yt)
        cx = cb + (ct - cb) * t
        r = (rb + (rt - rb) * np.clip(t, 0, 1)) * (bulge(np.clip(t, 0, 1)) if bulge else 1)
        tip = np.where(t > 1, np.sqrt(np.clip(1 - ((t - 1) / cap) ** 2, 0, 1)), 1)  # rounded far end
        r = r * tip
        u = (self.X - cx) / np.maximum(r, 1e-6)
        cover = np.clip((1 - np.abs(u)) * r * S, 0, 1) * (t >= 0) * (t <= 1 + cap)
        if mask_only:
            return cover, t, u
        n = np.sqrt(np.clip(1 - u * u, 0, 1))
        diffuse = np.clip(LIGHT[0] * u + LIGHT[1] * n, 0, 1)
        rgb = np.zeros(self.c.shape)
        tt = np.minimum(t, 1)
        for t0, t1, colour, shine in bands:
            sel = ((tt >= t0) & (tt <= t1))[..., None]
            shade = np.asarray(colour, float) * (0.28 + 0.8 * diffuse[..., None]) + shine * 255 * diffuse[..., None] ** 24
            rgb = np.where(sel, shade, rgb)
        self.over(cover, np.clip(rgb, 0, 255))
        return cover, t, u

    def frost(self, cover, amount, seed):
        rng = np.random.default_rng(seed)
        speck = (rng.random(self.a.shape) < amount).astype(float)
        speck = np.maximum(speck, np.roll(speck, S // 2, 1))
        self.over(cover * speck * 0.85, (225, 238, 250))

    def image(self):
        """Down to Doom's resolution: premultiplied average, then crop to the opaque part"""
        c = self.c.reshape(H, S, W, S, 3).mean((1, 3))
        a = self.a.reshape(H, S, W, S).mean((1, 3))
        rgb = np.where(a[..., None] > 0, c / np.maximum(a[..., None], 1e-6), 0)
        a = np.where(a > 0.12, a, 0)
        rows, cols = np.nonzero(a.any(1))[0], np.nonzero(a.any(0))[0]
        if not len(rows):
            return None, 0, 0
        y0, y1, x0, x1 = rows[0], rows[-1] + 1, cols[0], cols[-1] + 1
        img = np.dstack([rgb, a * 255])[y0:y1, x0:x1]
        return np.clip(img, 0, 255).astype(np.uint8), X0 + x0, Y0 + y0


# --- shared parts ---

def hand_on(cv, bottom, top, side, y=HANDS, kick=0):
    """A hand holding the side of a tube (as given to Canvas.tube) at height y"""
    (cb, yb, rb), (ct, yt, rt) = bottom, top
    t = (yb - y) / (yb - yt)
    r = rb + (rt - rb) * t
    hand(cv, cb + (ct - cb) * t + side * (r + 3), y + kick, side, 2 * r)


def hand(cv, x, y, side, grip_r):
    """A gloved hand under the instrument's side, fingers curled over it, armoured sleeve below"""
    sleeve = [(x + side * 10, 215), (x + side * 2, y + 6)]
    cv.line(sleeve, 16, (52, 78, 40))
    cv.line([(p[0] - side * 2, p[1]) for p in sleeve], 6, (84, 118, 62))
    cv.ellipse(x, y, 9, 8, (46, 44, 40))
    for k in range(4):  # fingers over the tube's edge
        fy = y - 7 + k * 3.2
        cv.line([(x + side * 6, fy), (x - side * (grip_r * 0.35), fy - 1)], 3.0, (58, 55, 50))
        cv.line([(x + side * 5, fy - 0.6), (x - side * (grip_r * 0.3), fy - 1.6)], 0.9, (92, 88, 80))


def cable(cv, pts, rgb, width=4):
    cv.line(pts, width, tuple(0.55 * np.asarray(rgb)))
    cv.line(pts, width * 0.45, rgb)


COPPER, STEEL, DARK = (184, 108, 62), (150, 158, 166), (40, 42, 48)
NIOBIUM, YELLOW, BLUE = (176, 180, 188), (226, 182, 40), (34, 98, 176)


# --- the instruments ---

def electron_gun(frame):
    cv = Canvas()
    kick = 4 if frame == "B" else 0
    cable(cv, [(186, 215), (192, 196), (184, 182 + kick), (176, 176 + kick)], (200, 40, 36))
    bands = [(0, 1, (36, 34, 38), 0.2),                 # bakelite base
             (0.18, 0.75, (120, 190, 200), 0.9),        # glass envelope
             (0.30, 0.34, COPPER, 0.6), (0.48, 0.52, COPPER, 0.6), (0.64, 0.68, COPPER, 0.6),
             (0.75, 1, STEEL, 0.8), (0.93, 1, DARK, 0.3)]
    bottom, top = (166, 205 + kick, 15), (163, 126 + kick, 8.5)
    cv.tube(bottom, top, bands, bulge=lambda t: 1 + 0.12 * np.exp(-((t - 0.45) / 0.2) ** 2))
    # the hot cathode inside the glass, brighter while firing
    yc = 205 + kick - 0.24 * (85)
    cv.glow(165.3, yc, 9 if frame == "B" else 6, (255, 150, 60), 0.9)
    cv.disc(165.3, yc, 1.4, (255, 235, 180))
    hand_on(cv, bottom, top, 1)
    return cv


def electron_flash():
    cv = Canvas()
    cv.glow(163, 118, 16, (90, 160, 255), 0.9)
    cv.glow(163, 118, 7, (230, 245, 255), 1.0)
    return cv


def pion_horn(frame):
    cv = Canvas()
    bands = [(0, 1, STEEL, 0.6), (0.38, 1, (196, 200, 206), 0.9)]
    for k in range(6):  # current sheet windings along the horn
        bands.append((0.42 + 0.09 * k, 0.45 + 0.09 * k, (90, 70, 50), 0.2))
    cv.tube((158, 205, 13), (156, 128, 9), bands, bulge=lambda t: 1 + 1.6 * np.clip(t - 0.35, 0, 1) ** 1.5)
    cv.ellipse(156, 128.5, 15, 3.2, (24, 24, 30))  # the horn's mouth
    cv.glow(156, 129, 5, (255, 120, 60), 0.8)      # the target
    hand_on(cv, (158, 205, 13), (156, 128, 9), -1)
    hand_on(cv, (158, 205, 13), (156, 128, 9), 1, HANDS + 2)
    return cv


def pion_flash():
    cv = Canvas()
    rng = np.random.default_rng(3)
    for k in range(9):  # pion tracks spiralling out of the target, either charge
        a = math.radians(-90 + rng.uniform(-60, 60))
        curl = rng.choice((-1, 1)) * rng.uniform(0.006, 0.02)
        pts, x, y, h = [], 156., 127., a
        for _ in range(26):
            pts.append((x, y))
            x += 1.6 * math.cos(h)
            y += 1.6 * math.sin(h) / ASPECT
            h += curl * 6
        cv.line(pts, 1.1, (255, 90, 70) if curl > 0 else (90, 170, 255), 0.95)
    cv.glow(156, 127, 10, (255, 230, 170), 1.0)
    return cv


def rf_linac(frame):
    cv = Canvas()
    cells = 6
    bulge = lambda t: 1 + 0.32 * np.abs(np.sin(np.pi * cells * np.clip((t - 0.1) / 0.85, 0, 1)))
    bands = [(0, 1, COPPER, 0.8), (0, 0.1, DARK, 0.3), (0.95, 1, STEEL, 0.7)]
    cover, t, u = cv.tube((163, 200, 19), (161, 132, 10), bands, bulge=bulge)
    # RF power on while it fires (frame B): every other cell glows
    if frame == "B":
        cell = np.floor(np.clip((t - 0.1) / 0.85, 0, 0.999) * cells)
        lit = (cell % 2 == 0) & (t > 0.1) & (t < 0.95)
        cv.over(cover * lit * 0.3 * np.sqrt(np.clip(1 - u * u, 0, 1)), (190, 110, 255))
    cable(cv, [(140, 215), (142, 190), (150, 172)], (60, 60, 64), 6)  # waveguide feed
    hand_on(cv, (163, 200, 23), (161, 132, 12), -1)
    hand_on(cv, (163, 200, 23), (161, 132, 12), 1, HANDS + 2)
    return cv


def muon_flash():
    cv = Canvas()
    cv.glow(161, 126, 18, (200, 90, 255), 0.85, squash=1.3)
    cv.glow(161, 126, 7, (255, 230, 255), 1.0)
    return cv


def cryostat(frame):
    cv = Canvas()
    kick = 3 if frame == "B" else 0
    bands = [(0, 1, BLUE, 0.5), (0.22, 0.26, STEEL, 0.9), (0.58, 0.62, STEEL, 0.9), (0.9, 1, STEEL, 0.9)]
    bottom, top = (162, 210 + kick, 33), (160, 126 + kick, 20)
    cover, t, u = cv.tube(bottom, top, bands)
    stripe = ((t > 0.36) & (t < 0.44)) * (np.floor((u + 1) * 7 + t * 40) % 2 == 0)  # radiation hazard band
    cv.over(cover * stripe * ((t > 0.36) & (t < 0.44)), (236, 190, 20))
    cv.over(cover * (~stripe.astype(bool)) * ((t > 0.36) & (t < 0.44)) * 0.9, (26, 24, 20))
    cv.frost(cover * np.clip((t - 0.6) * 3, 0, 1) * np.clip(1 - np.abs(u), 0, 1), 0.05, 5)  # the cold end
    cv.text("C6+", 150, int(147 + kick), (238, 240, 245))
    cv.disc(171, 146 + kick, 4.5, (28, 30, 34))  # a vacuum gauge
    cv.disc(171, 146 + kick, 3.6, (232, 230, 220))
    cv.line([(171, 146 + kick), (173.5, 144 + kick)], 0.8, (200, 30, 30))
    cable(cv, [(204, 215), (196, 192), (190, 182 + kick)], (40, 150, 230), 4)
    hand_on(cv, bottom, top, -1, HANDS + 4)
    hand_on(cv, bottom, top, 1, HANDS + 6)
    return cv


def carbon_flash():
    cv = Canvas()
    cv.glow(160, 120, 26, (255, 120, 40), 0.85)
    cv.glow(160, 120, 11, (255, 240, 200), 1.0)
    return cv


def cryomodule(frame):
    cv = Canvas()
    bottom, top = (172, 205, 30), (170, 128, 17)
    # the niobium cavity inside: nine elliptical cells
    cells = 9
    inner = lambda t: 0.55 * (1 + 0.38 * np.abs(np.sin(np.pi * cells * np.clip((t - 0.08) / 0.84, 0, 1))))
    cv.tube((172, 205, 30), (170, 128, 17), [(0, 1, NIOBIUM, 1.0)], bulge=inner)
    if frame == "B":  # the field on: the cavity glows blue
        cover, t, u = cv.tube(bottom, top, [], bulge=inner, mask_only=True)
        cv.over(cover * 0.3, (90, 120, 255))
    # the yellow vessel, cut open on top: only its sides, rims and end flanges
    cover, t, u = cv.tube(bottom, top, [(0, 1, YELLOW, 0.5), (0, 0.07, STEEL, 0.8), (0.93, 1, STEEL, 0.8)],
                          mask_only=True)
    keep = (np.abs(u) > 0.62) | (t < 0.07) | (t > 0.93)
    n = np.sqrt(np.clip(1 - u * u, 0, 1))
    diffuse = np.clip(LIGHT[0] * u + LIGHT[1] * n, 0, 1)[..., None]
    colour = np.where(((t < 0.07) | (t > 0.93))[..., None], np.array(STEEL, float), np.array(YELLOW, float))
    cv.over(cover * keep, np.clip(colour * (0.28 + 0.8 * diffuse) + 120 * diffuse ** 24, 0, 255))
    cv.over(cover * ((np.abs(np.abs(u) - 0.62) < 0.05)), (90, 70, 20))  # cut edges
    cable(cv, [(206, 215), (205, 190), (198, 176)], (230, 230, 235), 3)  # helium line
    hand_on(cv, bottom, top, -1)
    hand_on(cv, bottom, top, 1, HANDS + 2)
    return cv


def proton_flash(frame):
    cv = Canvas()
    cv.glow(170, 120, 22 if frame == "A" else 26, (110, 120, 255), 0.85)
    cv.glow(170, 120, 9, (235, 240, 255), 1.0)
    return cv


def penning_trap(frame):
    cv = Canvas()
    bands = [(0, 1, (172, 178, 186), 0.9), (0.3, 0.34, (110, 116, 124), 0.4), (0.72, 0.76, (110, 116, 124), 0.4),
             (0.9, 1, (120, 126, 134), 0.6)]
    cover, t, u = cv.tube((161, 212, 52), (160, 132, 36), bands, cap=0.12)
    cv.frost(cover * np.clip((t - 0.55) * 2, 0, 1), 0.04, 9)
    # the viewport on top, and the antiproton cloud in the magnetic bottle
    bright = {"A": 0.45, "B": 0.85, "C": 1.0}.get(frame, 0.45)
    cv.ellipse(160, 151, 19, 11.5, (60, 64, 70))
    cv.ellipse(160, 151, 16, 9.5, (8, 20, 14))
    cv.glow(160, 151, 13, (60, 255, 90), bright, squash=0.7)
    cv.glow(160, 151, 4, (220, 255, 220), bright)
    cv.ellipse(155, 147, 4, 1.6, (255, 255, 255), 0.35)  # reflection on the glass
    for x, colour in ((128, (200, 40, 36)), (192, (40, 120, 230))):  # magnet and RF cables
        cable(cv, [(x - 10 * np.sign(x - 160), 215), (x, 186), (x + 4 * np.sign(160 - x), 172)], colour, 4)
    cv.disc(186, 140, 4.2, (28, 30, 34))
    cv.disc(186, 140, 3.4, (232, 230, 220))
    cv.line([(186, 140), (184, 137.5)], 0.8, (200, 30, 30))
    hand_on(cv, (161, 212, 52), (160, 132, 36), -1, HANDS + 8)
    hand_on(cv, (161, 212, 52), (160, 132, 36), 1, HANDS + 10)
    return cv


def antiproton_flash(frame):
    cv = Canvas()
    cv.glow(160, 138, 64 if frame == "A" else 72, (70, 255, 90), 0.75)
    cv.glow(160, 140, 24, (230, 255, 230), 0.95)
    return cv


SPRITES = {
    "PISGA0": lambda: electron_gun("A"), "PISGB0": lambda: electron_gun("B"), "PISFA0": electron_flash,
    "SHTGA0": lambda: pion_horn("A"), "SHTFA0": pion_flash, "SHTFB0": pion_flash,
    "CHGGA0": lambda: rf_linac("A"), "CHGGB0": lambda: rf_linac("B"), "CHGFA0": muon_flash, "CHGFB0": muon_flash,
    "MISGA0": lambda: cryostat("A"), "MISGB0": lambda: cryostat("B"), "MISFA0": carbon_flash,
    "PLSGA0": lambda: cryomodule("A"), "PLSGB0": lambda: cryomodule("B"),
    "PLSFA0": lambda: proton_flash("A"), "PLSFB0": lambda: proton_flash("B"),
    "BFGGA0": lambda: penning_trap("A"), "BFGGB0": lambda: penning_trap("B"), "BFGGC0": lambda: penning_trap("C"),
    "BFGFA0": lambda: antiproton_flash("A"), "BFGFB0": lambda: antiproton_flash("B"),
}


def main(hud):
    out = os.path.join(hud, "instruments")
    os.makedirs(out, exist_ok=True)
    lines = []
    for name, draw in SPRITES.items():
        img, x0, y0 = draw().image()
        Image.fromarray(img).save(os.path.join(out, name + ".png"))
        lines.append(f"{name} {1 - x0} {16 - y0}\n")  # R_DrawPSprite: x = 1 - left, y = 16 - top
    with open(os.path.join(out, "offsets.txt"), "w") as f:
        f.writelines(lines)
    print(f"{len(lines)} instrument sprites -> {out}/")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "hud")
