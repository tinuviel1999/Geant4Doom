#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The map for doomviewer: what moves (sectors, lines, specials), and Doom's textures.

Geant4's visualisation draws every solid in one flat colour, so doomviewer draws the textured
level itself, in OpenGL, into Geant4's own view; and it runs Doom's doors, lifts and switches,
which move the sectors' floor and ceiling slabs (the GDML volumes) and the drawn surfaces
together. This module writes what it needs, next to the GDML:

    <map>_level.txt   origin, textures, animations, switch pairs, sky, sectors, sidedefs,
                      linedefs, the floor/ceiling triangles of every sector, and the things the
                      viewer handles itself (keys, weapons, ammo, health, armour, teleport
                      destinations); in Doom map units
    textures/         the wall textures (composed from their patches), flats and sky as PNG

The walls are not triangulated here: doomviewer builds them from the current sector heights
with Doom's rules (r_segs.c), so they follow doors and lifts:
  - a one-sided line shows its middle texture from floor to ceiling, its top at the ceiling,
    or its bottom on the floor if the line is "lower unpegged";
  - a two-sided line shows an upper texture where the sector behind has a lower ceiling
    (bottom at that ceiling, or top at ours if "upper unpegged"; none between two skies), a
    lower texture where it has a higher floor (top at that floor, or aligned with our ceiling
    if "lower unpegged"), and its middle texture (grates, see-through) once, not tiled;
  - texture x runs along the line from its first vertex (the second, on the back side), plus
    the sidedef offsets; flats are 64x64 on the map grid;
  - lines along x are one light step darker, lines along y one lighter ("fake contrast");
  - some flats and textures cycle through the lumps between two names, 8 tics a frame;
  - switches swap SW1xxx and SW2xxx.
"""
import math
import os
import struct

import numpy as np
from shapely import constrained_delaunay_triangles
from shapely.geometry import Polygon
from shapely.geometry.polygon import orient

# p_spec.c animdefs: (last, first) of each sequence; flats, then wall textures
ANIM_FLATS = [("NUKAGE3", "NUKAGE1"), ("FWATER4", "FWATER1"), ("SWATER4", "SWATER1"), ("LAVA4", "LAVA1"),
              ("BLOOD3", "BLOOD1"), ("RROCK08", "RROCK05"), ("SLIME04", "SLIME01"), ("SLIME08", "SLIME05"),
              ("SLIME12", "SLIME09")]
ANIM_TEXTURES = [("BLODGR4", "BLODGR1"), ("SLADRIP3", "SLADRIP1"), ("BLODRIP4", "BLODRIP1"),
                 ("FIREWALL", "FIREWALA"), ("GSTFONT3", "GSTFONT1"), ("FIRELAVA", "FIRELAV3"),
                 ("FIREMAG3", "FIREMAG1"), ("FIREBLU2", "FIREBLU1"), ("ROCKRED3", "ROCKRED1"),
                 ("BFALL4", "BFALL1"), ("SFALL4", "SFALL1"), ("WFALL4", "WFALL1"), ("DBRAIN4", "DBRAIN1")]

ML_DONTPEGTOP, ML_DONTPEGBOTTOM = 8, 16


def name8(b):
    return b.rstrip(b"\0").decode("ascii", "replace").upper()


class Textures:
    """Wall textures (TEXTURE1/2 + PNAMES + patches) and flats, as RGBA arrays"""

    def __init__(self, wad, picture_rgba, rgb):
        self.wad, self.picture_rgba, self.rgb = wad, picture_rgba, rgb
        self.flats = wad.between({"F_START", "FF_START"}, {"F_END", "FF_END"})
        self.flat_order = sorted(self.flats, key=self.flats.get)
        patches = wad.between({"P_START", "PP_START"}, {"P_END", "PP_END"})
        pn = wad.get("PNAMES")
        n = struct.unpack_from("<i", pn, 0)[0]
        self.patch_lumps = []
        for i in range(n):
            pname = name8(pn[4 + 8 * i:12 + 8 * i])
            j = patches.get(pname)
            self.patch_lumps.append(j if j is not None else wad.index(pname))
        self.defs, self.texture_order = {}, []
        for tl in ("TEXTURE1", "TEXTURE2"):
            d = wad.get(tl)
            if d is None:
                continue
            for k in range(struct.unpack_from("<i", d, 0)[0]):
                o = struct.unpack_from("<i", d, 4 + 4 * k)[0]
                tname = name8(d[o:o + 8])
                w, h = struct.unpack_from("<hh", d, o + 12)
                npatch = struct.unpack_from("<h", d, o + 20)[0]
                parts = [struct.unpack_from("<hhh", d, o + 22 + 10 * j) for j in range(npatch)]
                self.defs[tname] = (w, h, parts)
                self.texture_order.append(tname)

    def texture(self, name):
        w, h, parts = self.defs[name]
        img = np.zeros((h, w, 4), np.uint8)
        for ox, oy, p in parts:
            if p >= len(self.patch_lumps) or self.patch_lumps[p] < 0:
                continue
            pic, _, _ = self.picture_rgba(self.wad.lump(self.patch_lumps[p]), self.rgb)
            ph, pw = pic.shape[:2]
            x0, y0, x1, y1 = max(ox, 0), max(oy, 0), min(ox + pw, w), min(oy + ph, h)
            if x0 >= x1 or y0 >= y1:
                continue
            src = pic[y0 - oy:y1 - oy, x0 - ox:x1 - ox]
            dst = img[y0:y1, x0:x1]
            opaque = src[..., 3] > 0
            dst[opaque] = src[opaque]
        return img

    def flat(self, name):
        data = self.wad.lump(self.flats[name])
        idx = np.frombuffer(data[:4096], np.uint8).reshape(64, 64)
        img = np.zeros((64, 64, 4), np.uint8)
        img[..., :3] = self.rgb[idx]
        img[..., 3] = 255
        return img

    def sequence(self, order, last, first):
        if first not in order or last not in order:
            return None
        i, j = order.index(first), order.index(last)
        return order[i:j + 1] if j > i else None


def sky_name(mapname):
    m = mapname.upper()
    if m.startswith("E") and len(m) >= 2 and m[1].isdigit():
        return f"SKY{m[1]}"
    n = int(m[3:]) if m.startswith("MAP") and m[3:].isdigit() else 1
    return "SKY1" if n < 12 else "SKY2" if n < 21 else "SKY3"


# the things doomviewer handles itself: key cards, skull keys, teleport destinations, weapons
# (shotgun, super shotgun, chaingun, rocket launcher, plasma gun, BFG) and ammo (clip, box of
# bullets, shells, box of shells, rocket, box of rockets, cell, cell pack, backpack)
ITEMS = (5, 6, 13, 38, 39, 40, 14, 2001, 82, 2002, 2003, 2004, 2006,
         2007, 2048, 2008, 2049, 2010, 2046, 2047, 17, 8,
         2014, 2011, 2012, 2015, 2018, 2019, 2013, 83)  # health/armour bonus, stimpack, medikit,
                                                        # armours, soulsphere, megasphere


def export(wad, m, mapname, sector_shapes, offset, zc, out_txt, picture_rgba, write_png, rgb, skill_bit):
    """offset: the map units at the Geant4 origin (x, y); zc: the height there"""
    tex = Textures(wad, picture_rgba, rgb)
    outdir = os.path.join(os.path.dirname(out_txt) or ".", "textures")
    os.makedirs(outdir, exist_ok=True)
    used = {}  # key ("flat"|"wall", name) -> index
    sizes = []

    def texture_id(kind, name):
        key = (kind, name)
        if key not in used:
            if kind == "flat" and name not in tex.flats:
                return -1
            if kind == "wall" and name not in tex.defs:
                return -1
            img = tex.flat(name) if kind == "flat" else tex.texture(name)
            write_png(os.path.join(outdir, f"{kind}_{name}.png"), img)
            used[key] = len(sizes)
            sizes.append((kind, name, img.shape[1], img.shape[0]))
        return used[key]

    sectors = []
    for s in m.sectors:
        ceil = -2 if s["ceilpic"] == "F_SKY1" else texture_id("flat", s["ceilpic"])
        sectors.append((s["floor"], s["ceil"], texture_id("flat", s["floorpic"]), ceil, s["light"], s["special"], s["tag"]))
    sides = [(sd["sector"], sd["xoff"], sd["yoff"], texture_id("wall", sd["upper"]), texture_id("wall", sd["lower"]),
              texture_id("wall", sd["mid"])) for sd in m.sides]

    flats = []
    for sec, polys in sector_shapes.items():
        for poly in polys:
            for t in constrained_delaunay_triangles(orient(poly, sign=1.0)).geoms:
                pts = list(t.exterior.coords)[:3]
                area = (pts[1][0] - pts[0][0]) * (pts[2][1] - pts[0][1]) - (pts[1][1] - pts[0][1]) * (pts[2][0] - pts[0][0])
                if area < 0:
                    pts = [pts[0], pts[2], pts[1]]
                flats.append((sec, pts))

    # switch partners, and the frames of animated flats and textures that the level uses
    switches = []
    for kind, name in list(used):
        if kind == "wall" and name[:3] in ("SW1", "SW2"):
            other = ("SW2" if name[:3] == "SW1" else "SW1") + name[3:]
            if other in tex.defs:
                pair = tuple(sorted((used[(kind, name)], texture_id("wall", other))))
                if pair not in switches:
                    switches.append(pair)
    anims = []
    for kind, order, defs in (("flat", tex.flat_order, ANIM_FLATS), ("wall", tex.texture_order, ANIM_TEXTURES)):
        for last, first in defs:
            seq = tex.sequence(order, last, first)
            if seq and any((kind, n) in used for n in seq):
                anims.append([texture_id(kind, n) for n in seq])

    sky = sky_name(mapname)
    sky_file = None
    if sky in tex.defs:
        sky_file = f"sky_{sky}.png"  # per episode: several maps share textures/
        write_png(os.path.join(outdir, sky_file), tex.texture(sky))

    with open(out_txt, "w") as fo:
        fo.write("# doom_surfaces.py: the map for doomviewer (Doom map units; Geant4 x = (x - ox) * mm_per_unit,\n"
                 "# y = (height - zc) * mm_per_unit, z = -(y - oy) * mm_per_unit)\n"
                 "# origin <ox> <oy> <zc> <mm per unit>\n"
                 "# texture <id> <png in textures/> <width> <height> <name>\n"
                 "# anim <id> ...        flats/textures cycled 8 tics a frame\n"
                 "# switch <id> <id>     SW1/SW2 pair\n"
                 "# sky <png>\n"
                 "# sector <i> <floor> <ceiling> <floor tex> <ceiling tex, -2 sky> <light> <special> <tag>\n"
                 "# side <i> <sector> <x offset> <y offset> <upper> <lower> <middle>   (texture ids, -1 none)\n"
                 "# line <i> <x1> <y1> <x2> <y2> <flags> <special> <tag> <front side> <back side, -1 none>\n"
                 "# flat <sector> <x y> x 3  (anticlockwise seen from above)\n"
                 "# thing <type> <x> <y> <angle> <sector>\n")
        fo.write(f"origin {offset[0]} {offset[1]} {zc} 32\n")
        for i, (kind, name, w, h) in enumerate(sizes):
            fo.write(f"texture {i} {kind}_{name}.png {w} {h} {name}\n")
        for seq in anims:
            fo.write("anim " + " ".join(map(str, seq)) + "\n")
        for a, b in switches:
            fo.write(f"switch {a} {b}\n")
        if sky_file:
            fo.write(f"sky {sky_file}\n")
        for i, sec in enumerate(sectors):
            fo.write(f"sector {i} " + " ".join(map(str, sec)) + "\n")
        for i, sd in enumerate(sides):
            fo.write(f"side {i} " + " ".join(map(str, sd)) + "\n")
        for i, (v1, v2, flags, special, tag, rs, ls) in enumerate(m.lines):
            (x1, y1), (x2, y2) = m.verts[v1], m.verts[v2]
            fo.write(f"line {i} {x1} {y1} {x2} {y2} {flags} {special} {tag} {rs if rs != 0xFFFF else -1} "
                     f"{ls if ls != 0xFFFF else -1}\n")
        for sec, pts in flats:
            fo.write(f"flat {sec}" + "".join(f" {x:.3f} {y:.3f}" for x, y in pts) + "\n")
        for (x, y, ang, typ, flags) in m.things:
            if typ in ITEMS and (flags & skill_bit) and not (flags & 16):
                fo.write(f"thing {typ} {x} {y} {ang} {m.point_sector(x, y)}\n")
    return len(flats), len(sizes)
