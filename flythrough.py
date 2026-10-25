#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
flythrough.py - write a Geant4 macro that renders a short "demo" from the player start:
a 360 degree look-around, then a walk straight ahead (eye height follows the floor).

    python3 flythrough.py freedoom1.wad E1M1 e1m1.json --walk 640 --out demo.mac
    GEANT4_DATA_DIR=... python3 render_g4.py e1m1.gdml demo.mac     # writes frames/f_XXX.png
    python3 flythrough.py --gif frames demo.gif                      # assemble the GIF
"""
import argparse
import glob
import json
import math
import os

import doom2gdml as D


def macro(wadpath, mapname, meta, walk, spin_frames, walk_frames, size, outdir, driver):
    m = D.Map(D.Wad(wadpath), mapname)
    ox, oy = meta["offset_units"]
    zc = meta["z_offset_units"]
    S = meta["scale_mm_per_unit"]
    hx, hy, hz = meta["world_half_mm"]
    ex, _, ez = meta["player_start"]["eye_mm"]
    ang0 = meta["player_start"]["angle_deg"]
    sx, sy = ex / S + ox, -ez / S + oy  # back to Doom map coordinates

    def eye(x, y):
        floor = m.sectors[m.point_sector(x, y)]["floor"]
        return ((x - ox) * S, (floor + D.EYE_HEIGHT - zc) * S, -(y - oy) * S)

    fov = 37.0 if driver.startswith("TSG") else 45.0
    lines = [f"/vis/open {driver} {size}-0+0", "/vis/drawVolume", "/vis/viewer/set/style surface",
             "/vis/viewer/set/background 0.45 0.6 0.85", "/vis/viewer/set/lightsVector 0.4 1 0.6",
             "/vis/viewer/set/autoRefresh false"]
    frames = []
    for i in range(spin_frames):
        frames.append((sx, sy, ang0 + 360.0 * i / spin_frames))
    a = math.radians(ang0)
    for i in range(1, walk_frames + 1):
        d = walk * i / walk_frames
        frames.append((sx + d * math.cos(a), sy + d * math.sin(a), ang0))
    for k, (x, y, ang) in enumerate(frames):
        if driver.startswith("TSG"):
            lines.append(f"/vis/tsg/offscreen/set/file {outdir}/f_{k:03d}.png")
        lines.append(D.camera_macro(eye(x, y), ang, (hx, hy, hz), fov))
        if driver.startswith("TSG"):
            lines.append("/vis/viewer/rebuild")
        lines.append("/vis/viewer/flush")  # the RayTracer traces once per flush
    return "\n".join(lines) + "\n", len(frames)


def gif(framedir, out, ms):
    from PIL import Image
    files = sorted(glob.glob(os.path.join(framedir, "*.png")) + glob.glob(os.path.join(framedir, "*.jpeg")))
    ims = [Image.open(f).convert("RGB") for f in files]
    ims[0].save(out, save_all=True, append_images=ims[1:], duration=ms, loop=0, optimize=True)
    print(f"{len(ims)} frames -> {out}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wad", nargs="?")
    ap.add_argument("map", nargs="?")
    ap.add_argument("json", nargs="?")
    ap.add_argument("--walk", type=float, default=640, help="map units to walk forward")
    ap.add_argument("--spin", type=int, default=36, help="frames for the 360 look-around")
    ap.add_argument("--steps", type=int, default=32, help="frames for the walk")
    ap.add_argument("--size", default="480x360")
    ap.add_argument("--driver", default="TSG_OFFSCREEN")
    ap.add_argument("--frames", default="frames")
    ap.add_argument("--out", default="demo.mac")
    ap.add_argument("--gif", nargs=2, metavar=("FRAMEDIR", "OUT.gif"))
    ap.add_argument("--ms", type=int, default=80)
    a = ap.parse_args()
    if a.gif:
        gif(a.gif[0], a.gif[1], a.ms)
    else:
        os.makedirs(a.frames, exist_ok=True)
        text, n = macro(a.wad, a.map, json.load(open(a.json)), a.walk, a.spin, a.steps, a.size, a.frames, a.driver)
        open(a.out, "w").write(text)
        print(f"{n} frames -> {a.out}")
