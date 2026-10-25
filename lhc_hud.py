#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""An accelerator style for doomviewer's status bar: the bar becomes a blue LHC-style dipole
cryostat with steel flanges, bolts and radiation-area hazard stripes; the readouts are dark
control-room screens with green LED digits; Doomguy looks out of a detector cross-section
(tracker, calorimeters, muon chambers around him); the messages turn amber.

Reads the pictures that doom2gdml.py extracted (hud/*.png, hud/offsets.txt) and writes the
restyled ones to hud/lhc/ with their own offsets.txt. doomviewer uses them with
`/doom/hudStyle lhc` (the default if the folder exists); `/doom/hudStyle doom` goes back.

    python3 lhc_hud.py e1m1/hud
"""
import math
import os
import sys

import numpy as np
from PIL import Image

W, H = 320, 32  # the status bar, as in Doom (it sits on rows 168..199 of the 320x200 screen)

CRYOSTAT = np.array((28, 96, 168), float)  # LHC dipoles are painted blue
STEEL = np.array((150, 156, 162), float)
SCREEN = np.array((6, 12, 18), float)
LABEL = np.array((228, 234, 240), float)
HAZARD = np.array((236, 190, 20), float)
# detector layers around the face, outside in: muon chambers, hadron and EM calorimeters, tracker
LAYERS = [(16.5, (120, 124, 130)), (15.5, (190, 40, 40)), (14.5, (200, 120, 40)), (13.5, (60, 170, 80)),
          (12.5, (220, 200, 60)), (0, (10, 14, 30))]

# 3x5 pixel font for the labels
FONT = {
    "A": "010101111101101", "B": "110101110101110", "C": "011100100100011", "D": "110101101101110",
    "E": "111100110100111", "F": "111100110100100", "G": "011100101101011", "H": "101101111101101",
    "I": "111010010010111", "L": "100100100100111", "M": "101111111101101", "N": "110101101101101",
    "O": "010101101101010", "P": "110101110100100", "R": "110101110101101", "S": "011100010001110",
    "T": "111010010010010", "U": "101101101101111", "V": "101101101101010", "/": "001001010100100",
}


def blank(w, h):
    return np.zeros((h, w, 4), float)


def put(img, x, y, rgb, a=255):
    if 0 <= y < img.shape[0] and 0 <= x < img.shape[1]:
        img[y, x, :3] = rgb
        img[y, x, 3] = a


def text(img, s, x, y, rgb=LABEL):
    """3x5 text with a dark drop shadow; returns the width"""
    x0 = x
    for ch in s:
        glyph = FONT.get(ch)
        if glyph:
            for i, bit in enumerate(glyph):
                if bit == "1":
                    put(img, x + i % 3 + 1, y + i // 3 + 1, (10, 20, 35))
            for i, bit in enumerate(glyph):
                if bit == "1":
                    put(img, x + i % 3, y + i // 3, rgb)
        x += 4
    return x - x0 - 1


def centred(img, s, cx, y, rgb=LABEL):
    text(img, s, int(cx - (4 * len(s) - 1) / 2), y, rgb)


def screen(img, x0, y0, x1, y1):
    """A recessed control-room screen: dark, with a bevel (shadow above, light below)"""
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            edge = y == y0 or x == x0
            light = y == y1 or x == x1
            rgb = (8, 20, 36) if edge else (120, 170, 215) if light else SCREEN + (y - y0) * 0.6
            put(img, x, y, rgb)
    for x in range(x0 + 2, x1 - 1, 3):  # faint scan grid, as on a control-room display
        for y in range(y0 + 2, y1 - 1, 3):
            put(img, x, y, (14, 26, 32))


def bolt(img, x, y):
    put(img, x, y, (225, 228, 232))
    put(img, x + 1, y + 1, (70, 75, 80))


def flange(img, x):
    """A stainless steel flange across the cryostat, with bolts"""
    for y in range(2, H):
        for dx, shade in ((0, 1.15), (1, 0.85)):
            put(img, x + dx, y, np.clip(STEEL * shade * (0.8 + 0.25 * math.cos((y - 9) / 9)), 0, 255))
    for y in (6, 16, 26):
        bolt(img, x, y)


def status_bar():
    img = blank(W, H)
    # the cryostat: a horizontal cylinder, lit from above
    for y in range(H):
        c = (y - 3) / (H - 3)
        shade = 0.55 + 0.6 * math.sin(math.pi * min(max(c, 0), 1)) + (0.35 if 6 <= y <= 7 else 0)
        for x in range(W):
            put(img, x, y, np.clip(CRYOSTAT * shade + (x % 37 == 0) * -12, 0, 255))
    # radiation-area hazard stripes along the top
    for y in (0, 1):
        for x in range(W):
            put(img, x, y, HAZARD if (x + y) // 3 % 2 == 0 else (24, 22, 18))

    # readouts: ammo, health (digits end at x 90, then %), armor (221, then %)
    screen(img, 3, 3, 47, 20)
    centred(img, "BUNCHES", 25, 24)
    screen(img, 50, 3, 104, 20)
    centred(img, "HEALTH", 77, 24)
    screen(img, 180, 3, 235, 20)
    centred(img, "SHIELD", 207, 24)
    # keys: three empty slots
    for y in (3, 13, 23):
        screen(img, 239, y, 247, y + 6)
    # ammo table: particle species, count / maximum (digits end at x 288 and 314)
    screen(img, 251, 3, 318, 30)
    for label, y in (("LEP", 5), ("PI", 11), ("ION", 17), ("P", 23)):
        text(img, label, 254, y, (120, 220, 255))
        text(img, "/", 293, y, (110, 130, 150))
    for x in (48, 142, 178, 237, 249):
        flange(img, x)

    # Doomguy's slot: a detector cross-section, layer by layer, with a steel rim
    cx, cy = 160.5, 16.5
    for y in range(2, H):
        for x in range(143, 178):
            r = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
            if r > 17.5:
                continue
            for radius, rgb in LAYERS:
                if r >= radius:
                    put(img, x, y, rgb)
                    break
    for k in range(8):  # muon chamber spokes, as in an end view of the barrel
        a = k * math.pi / 4 + math.pi / 8
        put(img, round(cx - 0.5 + 16 * math.cos(a)), round(cy - 0.5 + 16 * math.sin(a)), (60, 62, 66))
    return img


def arms_panel():
    """STARMS (38x32, drawn at x 104): weapon slots 2-7 as six small screens"""
    img = blank(38, 32)
    for y in range(32):
        c = (y - 3) / 29
        shade = 0.55 + 0.6 * math.sin(math.pi * min(max(c, 0), 1)) + (0.35 if 6 <= y <= 7 else 0)
        for x in range(38):
            put(img, x, y, np.clip(CRYOSTAT * shade, 0, 255))
    for y in (0, 1):
        for x in range(38):
            put(img, x, y, HAZARD if (x + 104 + y) // 3 % 2 == 0 else (24, 22, 18))
    for row in range(2):
        for col in range(3):
            x, y = 5 + 12 * col, 3 + 10 * row  # digits at 7 + 12 col, 4 + 10 row
            screen(img, x, y, x + 7, y + 7)
    centred(img, "BEAMS", 19, 24)
    return img


def recolour(img, rgb):
    """Recolour Doom's red digits/font by their red channel: same shading, new colour"""
    out = img.astype(float).copy()
    v = out[..., 0] / 255
    for c in range(3):
        out[..., c] = np.clip(v * rgb[c] * 1.15, 0, 255)
    return out


def main(hud):
    out = os.path.join(hud, "lhc")
    os.makedirs(out, exist_ok=True)
    offsets = {}
    for line in open(os.path.join(hud, "offsets.txt")):
        name, left, top = line.split()
        offsets[name] = (left, top)
    pictures = {"STBAR": status_bar(), "STARMS": arms_panel()}
    for name in offsets:
        img = np.asarray(Image.open(os.path.join(hud, name + ".png")).convert("RGBA"))
        if name.startswith(("STTNUM", "STTPRCNT", "STTMINUS")):
            pictures[name] = recolour(img, (70, 255, 110))  # green LED digits
        elif name.startswith("STCFN"):
            pictures[name] = recolour(img, (255, 185, 60))  # amber messages
    lines = []
    for name, img in pictures.items():
        Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(os.path.join(out, name + ".png"))
        lines.append(f"{name} {offsets[name][0]} {offsets[name][1]}\n")
    with open(os.path.join(out, "offsets.txt"), "w") as f:
        f.writelines(lines)
    print(f"{len(lines)} accelerator pictures -> {out}/")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "hud")
