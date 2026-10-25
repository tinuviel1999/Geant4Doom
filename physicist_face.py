#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Give Doomguy a totally-not-a-famous-physicist makeover: swept-back, wavy black hair with a
receding hairline and a bit more volume on top, and bushy dark eyebrows.

Reads the face sprites that doom2gdml.py extracted (hud/STF*.png, hud/offsets.txt) and writes
the restyled ones to hud/physicist/ with their own offsets.txt. doomviewer uses them when
`/doom/face physicist` is set (the default if the folder exists); `/doom/face doomguy` goes back.

    python3 physicist_face.py e1m1/hud
"""
import colorsys
import os
import sys

import numpy as np
from PIL import Image

# black hair (the younger look), with dark-brown wave highlights; the brows stay darkest
HAIR = np.array([(10, 8, 8), (24, 20, 18), (40, 34, 30), (62, 54, 48), (88, 78, 70)], float)
BROW = np.array((14, 10, 8), float)
DARK = 0.42  # hair is darker than this (skin is lighter)


def luminance(rgb):
    return colorsys.rgb_to_hls(*(np.asarray(rgb[:3]) / 255))[1]


def is_blood(rgb):
    h, l, s = colorsys.rgb_to_hls(*(np.asarray(rgb[:3]) / 255))
    return s > 0.45 and (h < 0.03 or h > 0.95) and l < 0.5


def hair_mask(img):
    """Dark, non-bloody pixels connected to the top of the head, above the eyes."""
    h, w = img.shape[:2]
    alpha = img[..., 3] > 0
    dark = np.zeros((h, w), bool)
    for y in range(h):
        for x in range(w):
            if alpha[y, x] and not is_blood(img[y, x]) and luminance(img[y, x]) < DARK:
                dark[y, x] = True
    limit = int(0.42 * h)  # below this it is cheek shading and stubble, not hair
    xs = np.nonzero(alpha.any(axis=0))[0]
    cx = (xs[0] + xs[-1]) / 2
    # over the forehead the hair stops higher than at the sides (where the sideburns are), so
    # forehead wrinkles (the "ouch" face) stay skin
    reach = [int(0.3 * h) if abs(x - cx) < 5 else limit for x in range(w)]
    mask = np.zeros((h, w), bool)
    stack = []
    for x in range(w):  # seed: the topmost opaque pixel of every column
        ys = np.nonzero(alpha[:, x])[0]
        if len(ys) and ys[0] < limit and dark[ys[0], x]:
            stack.append((ys[0], x))
    while stack:
        y, x = stack.pop()
        if mask[y, x]:
            continue
        mask[y, x] = True
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ny, nx = y + dy, x + dx
            if 0 <= nx < w and 0 <= ny < reach[nx] and dark[ny, nx] and not mask[ny, nx]:
                stack.append((ny, nx))
    # lighter strands inside the hair (Doomguy's brown highlights) are hair too
    for _ in range(2):
        for y in range(1, limit):
            for x in range(1, w - 1):
                if alpha[y, x] and not mask[y, x] and not is_blood(img[y, x]) and y < reach[x] \
                        and mask[y - 1, x] + mask[y + 1, x] + mask[y, x - 1] + mask[y, x + 1] >= 3:
                    mask[y, x] = True
    return mask


def restyle(img):
    img = img.copy()
    h, w = img.shape[:2]
    alpha = img[..., 3] > 0
    xs = np.nonzero(alpha.any(axis=0))[0]
    cx = (xs[0] + xs[-1]) / 2
    mask = hair_mask(img)

    # Doomguy's fringe hangs onto the forehead: it recedes too. Very dark pixels joined to the
    # hair below it (over the forehead) become skin, from the nearest skin below in the column.
    fringe = np.zeros_like(mask)
    stack = [(y, x) for y, x in zip(*np.nonzero(mask))]
    while stack:
        y, x = stack.pop()
        for dy, dx in ((1, 0), (0, 1), (0, -1)):
            ny, nx = y + dy, x + dx
            if 0 <= ny < int(0.45 * h) and 0 <= nx < w and abs(nx - cx) < 6 and alpha[ny, nx] \
                    and not mask[ny, nx] and not fringe[ny, nx] and not is_blood(img[ny, nx]) \
                    and luminance(img[ny, nx]) < 0.22:
                fringe[ny, nx] = True
                stack.append((ny, nx))
    for y, x in sorted(zip(*np.nonzero(fringe)), reverse=True):  # bottom up: skin spreads upwards
        for yy in range(y + 1, h):
            if alpha[yy, x] and not fringe[yy, x] and not mask[yy, x] and luminance(img[yy, x]) >= 0.3:
                img[y, x] = img[yy, x]
                break

    # receding hairline: the hair just above the forehead becomes forehead, most at the temples
    for x in range(w):
        ys = np.nonzero(mask[:, x])[0]
        if not len(ys):
            continue
        d = abs(x - cx)
        recede = 3 if 3.5 <= d <= 6.5 else 2 if 2.5 <= d < 3.5 else 1 if d < 2.5 else 0  # an M
        bottom = ys[-1]
        below = bottom + 1
        if recede == 0 or below >= h or not alpha[below, x] or mask[below, x]:
            continue
        skin = img[below, x].copy()
        for k in range(recede):
            y = bottom - k
            if y > ys[0] + 1 and mask[y, x]:  # always leave some hair on top
                img[y, x] = skin
                mask[y, x] = False

    # swept back in waves: the original shading, in black, plus wave crests that
    # arc back from the parting over the top of the head
    for y, x in zip(*np.nonzero(mask)):
        t = min(1.0, luminance(img[y, x]) / DARK)
        level = 0.2 + 2.2 * t
        band = (2 * y + int(abs(x - cx)) // 2) % 5
        level += 1.3 if band == 0 else 0.6 if band == 1 else -0.3 if band == 3 else 0
        level = min(level, len(HAIR) - 1)
        lo = int(level)
        hi = min(lo + 1, len(HAIR) - 1)
        img[y, x, :3] = HAIR[lo] + (HAIR[hi] - HAIR[lo]) * (level - lo)

    # volume: one more row of hair over the top of the head, rounded at the ends
    top = np.zeros((1, w, 4), np.uint8)
    first = {x: np.nonzero(alpha[:, x])[0][0] for x in xs}
    crown = [x for x in xs if first[x] == min(first.values()) or (first[x] <= min(first.values()) + 1 and mask[first[x], x])]
    crown = [x for x in crown if mask[first[x], x]]
    if len(crown) > 4:
        for x in crown[1:-1]:
            if first[x] == 0:
                top[0, x, :3] = HAIR[1 if x % 3 else 3]
                top[0, x, 3] = 255
            else:
                img[first[x] - 1, x, :3] = HAIR[1 if x % 3 else 3]
                img[first[x] - 1, x, 3] = 255
    grown = top[0, :, 3].any()
    if grown:
        img = np.concatenate([top, img])

    # bushy dark brows: the row above each eye, darkened and a pixel wider
    off = 1 if grown else 0
    eyes = eye_rows(img[off:], mask)
    for y, x0, x1 in eyes:
        for x in range(max(0, x0 - 1), min(w, x1 + 2)):
            yy = y - 1 + off
            if yy >= 0 and img[yy, x, 3] and not is_blood(img[yy, x]):
                img[yy, x, :3] = BROW + (img[yy, x, :3] - BROW) * 0.15
    return img, grown


def eye_rows(img, hair):
    """Where the eyes are: the topmost run of whitish/blueish pixels on each side of the face."""
    h, w = img.shape[:2]
    found = []
    xs = np.nonzero((img[..., 3] > 0).any(axis=0))[0]
    cx = (xs[0] + xs[-1]) / 2
    for side in (-1, 1):
        for y in range(int(0.25 * h), int(0.6 * h)):
            run = [x for x in range(w) if side * (x - cx) > 0.5 and img[y, x, 3] and not hair[min(y, hair.shape[0] - 1), x]
                   and int(img[y, x, 2]) >= int(img[y, x, 0]) - 10 and luminance(img[y, x]) > 0.45]
            if run:
                found.append((y, min(run), max(run)))
                break
    return found


def main(hud):
    out = os.path.join(hud, "physicist")
    os.makedirs(out, exist_ok=True)
    offsets = {}
    for line in open(os.path.join(hud, "offsets.txt")):
        name, left, top = line.split()
        offsets[name] = (int(left), int(top))
    written = []
    for name, (left, top) in offsets.items():
        if not name.startswith("STF") or name.startswith("STFB"):  # faces, not their backgrounds
            continue
        img = np.asarray(Image.open(os.path.join(hud, name + ".png")).convert("RGBA")).copy()
        img, grown = restyle(img)
        Image.fromarray(img.astype(np.uint8)).save(os.path.join(out, name + ".png"))
        written.append(f"{name} {left} {top + 1 if grown else top}\n")  # taller by one row: same place
    with open(os.path.join(out, "offsets.txt"), "w") as f:
        f.writelines(written)
    print(f"{len(written)} faces -> {out}/")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "hud")
