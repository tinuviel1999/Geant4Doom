#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
doom2gdml.py - convert a Doom map (from a WAD file) into Geant4 GDML geometry.

    python3 doom2gdml.py freedoom1.wad E1M1 -o e1m1.gdml

How it works
------------
Doom maps are 2.5D: every sector is a 2D region with a floor height, a
ceiling height and a light level.  The WAD already contains a BSP tree
(NODES/SSECTORS/SEGS).  Walking that tree and clipping a big rectangle by
every partition line, then by the segs of each leaf, gives the exact convex
polygon of every subsector - the same trick GL source ports use.

Per sector the convex pieces are merged (shapely) into one polygon (with
holes), triangulated (GEOS constrained Delaunay) and extruded into two closed
G4TessellatedSolids:
    floor slab    from BASE        up to the floor height
    ceiling slab  from the ceiling height up to TOP   (omitted for sky)
One-sided linedefs (outer walls) become thin tessellated prisms just outside
the sector, clipped so they never overlap each other or the sectors.
Monsters and barrels from the THINGS lump are added as simple solids.

Colours (average colour of each flat / wall texture, scaled by the sector
light level) are written as GDML <auxiliary auxtype="Color"> tags; the
companion app (doomviewer.cc / render_g4.py) turns them into G4VisAttributes.

Units: 1 Doom map unit = 32 mm (a 56-unit-tall marine is ~1.8 m).
The map is re-centred so the world volume sits at the origin.

Axes: Geant4 y is UP (Doom x -> G4 x, Doom height -> G4 y, Doom y -> G4 -z).
That matches the default vis up vector, and it is required for the RayTracer
in multithreaded Geant4 builds: G4RTPrimaryGeneratorAction::SetUp() (11.4)
never copies the viewer's up vector / head angle, so MT ray tracing always
assumes up = +y.
"""
import argparse
import json
import math
import struct
import sys

import numpy as np
import shapely
from shapely import constrained_delaunay_triangles
from shapely.geometry import Polygon, MultiPolygon, box
from shapely.geometry.polygon import orient
from shapely.ops import unary_union


def robust_union(geoms):
    """unary_union, snapped to a 1e-6 grid if GEOS hits a topology error (near-coincident edges)."""
    try:
        return unary_union(geoms)
    except shapely.errors.GEOSException:
        pass
    geoms = [shapely.make_valid(g) for g in geoms]
    for grid in (1e-6, 1e-3, 1e-2):
        try:
            return shapely.union_all(geoms, grid_size=grid)
        except shapely.errors.GEOSException:
            pass
    # last resort: grow every piece a little, union, shrink back
    return unary_union([g.buffer(1e-3) for g in geoms]).buffer(-1e-3)

SCALE = 32.0  # mm per Doom map unit
WALL_THICK = 8.0  # map units
EYE_HEIGHT = 41.0  # Doom player view height, map units

LIQUIDS = ("NUKAGE", "FWATER", "LAVA", "BLOOD", "SLIME")

# type: (name, radius, height, shape, colour, z_offset)
THING_TYPES = {
    3004: ("zombieman", 20, 56, "tube", (0.55, 0.45, 0.35), 0),
    9:    ("shotgunguy", 20, 56, "tube", (0.45, 0.45, 0.35), 0),
    65:   ("chaingunner", 20, 56, "tube", (0.6, 0.3, 0.3), 0),
    3001: ("imp", 20, 56, "tube", (0.6, 0.35, 0.2), 0),
    3002: ("demon", 30, 56, "tube", (0.9, 0.45, 0.55), 0),
    58:   ("spectre", 30, 56, "tube", (0.5, 0.5, 0.6), 0),
    3006: ("lostsoul", 16, 32, "orb", (1.0, 0.8, 0.3), 16),
    3005: ("cacodemon", 31, 56, "orb", (0.85, 0.1, 0.1), 32),
    3003: ("baron", 24, 64, "tube", (0.75, 0.6, 0.5), 0),
    2035: ("barrel", 10, 42, "tube", (0.2, 0.6, 0.2), 0),
}


# --------------------------------------------------------------------- WAD
class Wad:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        ident, n, off = struct.unpack_from("<4sii", self.data, 0)
        if ident not in (b"IWAD", b"PWAD"):
            sys.exit(f"{path}: not a WAD file")
        self.lumps = []
        for i in range(n):
            pos, size, name = struct.unpack_from("<ii8s", self.data, off + 16 * i)
            self.lumps.append((name.rstrip(b"\0").decode("ascii", "replace").upper(), pos, size))

    def index(self, name, start=0):
        for i in range(start, len(self.lumps)):
            if self.lumps[i][0] == name:
                return i
        return -1

    def lump(self, i):
        _, pos, size = self.lumps[i]
        return self.data[pos:pos + size]

    def get(self, name):
        i = self.index(name)
        return self.lump(i) if i >= 0 else None

    def between(self, start, end):
        """All lumps between markers like F_START/F_END (also FF_START...)."""
        out, inside = {}, False
        for i, (name, pos, size) in enumerate(self.lumps):
            if name in start:
                inside = True
            elif name in end:
                inside = False
            elif inside and size > 0:
                out[name] = i
        return out


def name8(b):
    return b.rstrip(b"\0").decode("ascii", "replace").upper()


class Map:
    def __init__(self, wad, mapname):
        i = wad.index(mapname.upper())
        if i < 0:
            sys.exit(f"map {mapname} not found")
        L = {wad.lumps[j][0]: wad.lump(j) for j in range(i + 1, i + 11)}
        if L["NODES"][:4] in (b"XNOD", b"ZNOD", b"XGLN"):
            sys.exit("extended/compressed nodes are not supported; rebuild with vanilla nodes")
        self.verts = [struct.unpack_from("<hh", L["VERTEXES"], o) for o in range(0, len(L["VERTEXES"]), 4)]
        self.lines = [struct.unpack_from("<HHHHHHH", L["LINEDEFS"], o) for o in range(0, len(L["LINEDEFS"]), 14)]
        self.sides = []
        for o in range(0, len(L["SIDEDEFS"]), 30):
            xo, yo, up, lo, mid, sec = struct.unpack_from("<hh8s8s8sH", L["SIDEDEFS"], o)
            self.sides.append(dict(upper=name8(up), lower=name8(lo), mid=name8(mid), sector=sec, xoff=xo, yoff=yo))
        self.sectors = []
        for o in range(0, len(L["SECTORS"]), 26):
            fh, ch, fp, cp, light, special, tag = struct.unpack_from("<hh8s8shhh", L["SECTORS"], o)
            self.sectors.append(dict(floor=fh, ceil=ch, floorpic=name8(fp), ceilpic=name8(cp), light=light,
                                     special=special, tag=tag))
        self.segs = [struct.unpack_from("<HHhHhh", L["SEGS"], o) for o in range(0, len(L["SEGS"]), 12)]
        self.ssectors = [struct.unpack_from("<HH", L["SSECTORS"], o) for o in range(0, len(L["SSECTORS"]), 4)]
        self.nodes = [struct.unpack_from("<hhhh8h HH", L["NODES"], o) for o in range(0, len(L["NODES"]), 28)]
        self.things = [struct.unpack_from("<hhhhh", L["THINGS"], o) for o in range(0, len(L["THINGS"]), 10)]

    def seg_sector(self, seg):
        v1, v2, ang, line, direction, off = seg
        sd = self.lines[line][5 + direction]
        return self.sides[sd]["sector"]

    def point_sector(self, x, y):
        n = len(self.nodes) - 1
        while True:
            nd = self.nodes[n]
            nx, ny, dx, dy = nd[:4]
            child = nd[12] if dy * (x - nx) - dx * (y - ny) >= 0 else nd[13]
            if child & 0x8000:
                cnt, first = self.ssectors[child & 0x7FFF]
                return self.seg_sector(self.segs[first])
            n = child


# ----------------------------------------------------------------- textures
class Palette:
    def __init__(self, wad):
        pal = wad.get("PLAYPAL")
        self.rgb = np.frombuffer(pal[:768], dtype=np.uint8).reshape(256, 3).astype(float) / 255.0

    def mean(self, idx):
        idx = np.asarray(idx, dtype=np.int64)
        if idx.size == 0:
            return (0.5, 0.5, 0.5)
        return tuple(self.rgb[idx].mean(axis=0))


def picture_pixels(data):
    """Palette indices of all opaque pixels of a Doom 'picture' format patch."""
    w, h, lo, to = struct.unpack_from("<hhhh", data, 0)
    out = []
    for c in range(w):
        p = struct.unpack_from("<I", data, 8 + 4 * c)[0]
        while p < len(data) and data[p] != 0xFF:
            n = data[p + 1]
            out.extend(data[p + 3:p + 3 + n])
            p += n + 4
    return out


class Colours:
    def __init__(self, wad):
        self.wad, self.pal = wad, Palette(wad)
        self.flats = wad.between({"F_START", "FF_START"}, {"F_END", "FF_END"})
        self.patches = wad.between({"P_START", "PP_START"}, {"P_END", "PP_END"})
        pn = wad.get("PNAMES")
        n = struct.unpack_from("<i", pn, 0)[0]
        self.pnames = [name8(pn[4 + 8 * i:12 + 8 * i]) for i in range(n)]
        self.textures = {}
        for tl in ("TEXTURE1", "TEXTURE2"):
            d = wad.get(tl)
            if d is None:
                continue
            nt = struct.unpack_from("<i", d, 0)[0]
            for k in range(nt):
                o = struct.unpack_from("<i", d, 4 + 4 * k)[0]
                tname = name8(d[o:o + 8])
                npatch = struct.unpack_from("<h", d, o + 20)[0]
                pl = [struct.unpack_from("<hhhhh", d, o + 22 + 10 * j)[2] for j in range(npatch)]
                self.textures[tname] = pl
        self.cache = {}

    def flat(self, name):
        key = ("F", name)
        if key not in self.cache:
            i = self.flats.get(name)
            self.cache[key] = self.pal.mean(list(self.wad.lump(i))) if i is not None else (0.5, 0.5, 0.5)
        return self.cache[key]

    def texture(self, name):
        key = ("T", name)
        if key not in self.cache:
            px = []
            for p in self.textures.get(name, []):
                pi = self.patches.get(self.pnames[p]) if p < len(self.pnames) else None
                if pi is None:
                    pi = self.wad.index(self.pnames[p]) if p < len(self.pnames) else -1
                if pi is not None and pi >= 0:
                    try:
                        px.extend(picture_pixels(self.wad.lump(pi)))
                    except struct.error:
                        pass
            self.cache[key] = self.pal.mean(px) if px else (0.5, 0.5, 0.5)
        return self.cache[key]


def lit(rgb, light):
    """Average texture colour scaled by the sector light level (lifted a bit, the
    viewers' lighting is darker than Doom's)."""
    f = 0.55 + 0.45 * max(0, min(255, light)) / 255.0
    return tuple(min(1.0, (c ** 0.7) * f * 1.3) for c in rgb)


# ----------------------------------------------------------------- geometry
def clip(poly, a, b, c):
    """Keep the part of convex polygon `poly` where a*x + b*y + c >= 0."""
    out = []
    n = len(poly)
    for i in range(n):
        p, q = poly[i], poly[(i + 1) % n]
        dp = a * p[0] + b * p[1] + c
        dq = a * q[0] + b * q[1] + c
        if dp >= 0:
            out.append(p)
        if (dp >= 0) != (dq >= 0):
            t = dp / (dp - dq)
            out.append((p[0] + t * (q[0] - p[0]), p[1] + t * (q[1] - p[1])))
    return out


def subsector_polygons(m):
    """Exact convex polygon of every subsector, via the BSP tree."""
    xs = [v[0] for v in m.verts]
    ys = [v[1] for v in m.verts]
    pad = 64
    start = [(min(xs) - pad, min(ys) - pad), (max(xs) + pad, min(ys) - pad),
             (max(xs) + pad, max(ys) + pad), (min(xs) - pad, max(ys) + pad)]
    result = {}
    stack = [(len(m.nodes) - 1, start)]
    while stack:
        n, poly = stack.pop()
        nd = m.nodes[n]
        nx, ny, dx, dy = nd[:4]
        # right (front) side: dy*(x-nx) - dx*(y-ny) >= 0
        for child, sgn in ((nd[12], 1), (nd[13], -1)):
            sub = clip(poly, sgn * dy, -sgn * dx, sgn * (dx * ny - dy * nx))
            if len(sub) < 3:
                continue
            if child & 0x8000:
                ss = child & 0x7FFF
                cnt, first = m.ssectors[ss]
                p = sub
                for s in m.segs[first:first + cnt]:
                    (x1, y1), (x2, y2) = m.verts[s[0]], m.verts[s[1]]
                    sdx, sdy = x2 - x1, y2 - y1
                    p = clip(p, sdy, -sdx, sdx * y1 - sdy * x1)
                    if len(p) < 3:
                        break
                if len(p) >= 3:
                    result[ss] = (m.seg_sector(m.segs[first]), p)
            else:
                stack.append((child, sub))
    return result


def as_polys(geom):
    if geom.is_empty:
        return []
    if isinstance(geom, Polygon):
        return [geom]
    if isinstance(geom, MultiPolygon):
        return list(geom.geoms)
    return [g for g in getattr(geom, "geoms", []) if isinstance(g, Polygon)]


def clean(geom, eps=0.05, open_eps=0.0):
    """Close hairline gaps (and optionally remove slivers), drop near-collinear vertices."""
    g = geom.buffer(eps, join_style=2).buffer(-eps, join_style=2)
    if open_eps:
        g = g.buffer(-open_eps, join_style=2).buffer(open_eps, join_style=2)
    out = []
    for p in as_polys(g.simplify(0.05)):
        if p.area > 1.0 and p.is_valid:
            out.append(p)
    return out


# ------------------------------------------------------------------- GDML
class Gdml:
    def __init__(self, offset):
        self.ox, self.oy = offset
        self.defines, self.solids, self.volumes, self.placements = [], [], [], []
        self.nv = 0
        self.defects = 0

    def xy(self, x, y):
        return ((x - self.ox) * SCALE, (y - self.oy) * SCALE)

    def g4(self, x, y, z):
        """Doom map coordinates (units) -> Geant4 coordinates (mm), y up."""
        X, Y = self.xy(x, y)
        return (X, z * SCALE, -Y)

    def vertex(self, x, y, z):
        X, Y, Z = self.g4(x, y, z)
        name = f"v{self.nv}"
        self.nv += 1
        self.defines.append(f'<position name="{name}" x="{X:.3f}" y="{Y:.3f}" z="{Z:.3f}" unit="mm"/>')
        return name

    def slab(self, name, poly, z0, z1):
        """Closed tessellated prism of shapely polygon (with holes) between z0 and z1."""
        poly = orient(poly, sign=1.0)  # exterior CCW, holes CW
        rings = [list(poly.exterior.coords)[:-1]] + [list(r.coords)[:-1] for r in poly.interiors]
        rings = [r for r in rings if len(r) >= 3]
        tris = []
        # constrained Delaunay (GEOS) is robust for sectors with many holes, unlike earcut
        for t in as_polys(constrained_delaunay_triangles(Polygon(rings[0], rings[1:]))):
            corners = [(round(x, 6), round(y, 6)) for x, y in list(t.exterior.coords)[:3]]
            (ax, ay), (bx, by), (cx, cy) = corners
            if abs((bx - ax) * (cy - ay) - (by - ay) * (cx - ax)) < 1e-6:
                continue  # no area (a ring vertex twice): Geant4 rejects such facets
            tris.append(corners)
        # GEOS adds a point where a ring touches itself or another one. On a ring edge it must go
        # into the ring too, or the side wall misses it and the solid has a crack (T-junction).
        known = {(round(x, 6), round(y, 6)) for r in rings for x, y in r}
        extra = {c for t in tris for c in t} - known
        if extra:
            for j, r in enumerate(rings):
                out = []
                for i, (x0, y0) in enumerate(r):
                    x1, y1 = r[(i + 1) % len(r)]
                    out.append((x0, y0))
                    dx, dy = x1 - x0, y1 - y0
                    ll = dx * dx + dy * dy
                    on = []
                    for px, py in extra:
                        u = ((px - x0) * dx + (py - y0) * dy) / ll
                        if 1e-9 < u < 1 - 1e-9 and abs((px - x0) * dy - (py - y0) * dx) / math.sqrt(ll) < 1e-4:
                            on.append((u, (px, py)))
                    out += [q for _, q in sorted(on)]
                rings[j] = out
        points = [p for r in rings for p in r]
        index = {}
        for i, (x, y) in enumerate(points):
            index.setdefault((round(x, 6), round(y, 6)), i)
        tri = []
        for t in tris:
            for c in t:
                if c not in index:  # an interior point: a vertex of the caps only
                    index[c] = len(points)
                    points.append(c)
            tri.append([index[c] for c in t])
        flat = np.array(points, dtype=np.float64)
        bot = [self.vertex(x, y, z0) for x, y in flat]
        top = [self.vertex(x, y, z1) for x, y in flat]
        facets = []
        for a, b, c in tri:
            pa, pb, pc = flat[a], flat[b], flat[c]
            area = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pb[1] - pa[1]) * (pc[0] - pa[0])
            if area < 0:
                b, c = c, b
            facets.append(("t", top[a], top[b], top[c]))
            facets.append(("t", bot[a], bot[c], bot[b]))
        k = 0
        for r in rings:
            n = len(r)
            for i in range(n):
                # a vertex where rings touch appears twice: the walls share it with the caps
                a, b = (index[(round(points[k + j][0], 6), round(points[k + j][1], 6))] for j in (i, (i + 1) % n))
                facets.append(("q", bot[a], bot[b], top[b], top[a]))
            k += n
        # watertightness check: every directed edge must be matched by its reverse
        edges = {}
        for f in facets:
            vs = f[1:]
            for i in range(len(vs)):
                e = (vs[i], vs[(i + 1) % len(vs)])
                edges[e] = edges.get(e, 0) + 1
        bad = sum(1 for (a, b), c in edges.items() if edges.get((b, a)) != c)  # twice both ways where rings touch
        if bad:
            self.defects += 1
            print(f"  warning: {name} has {bad} unmatched edges", file=sys.stderr)
        lines = [f'<tessellated name="{name}" lunit="mm" aunit="deg">']
        for f in facets:
            if f[0] == "t":
                lines.append(f'  <triangular vertex1="{f[1]}" vertex2="{f[2]}" vertex3="{f[3]}" type="ABSOLUTE"/>')
            else:
                lines.append(f'  <quadrangular vertex1="{f[1]}" vertex2="{f[2]}" vertex3="{f[3]}" vertex4="{f[4]}" type="ABSOLUTE"/>')
        lines.append("</tessellated>")
        self.solids.append("\n".join(lines))

    def volume(self, name, solid, material, rgb, pos=(0, 0, 0), rotx=0, angle=None):
        r, g, b = rgb
        facing = f'  <auxiliary auxtype="Angle" auxvalue="{angle}"/>\n' if angle is not None else ''
        self.volumes.append(
            f'<volume name="{name}">\n  <materialref ref="{material}"/>\n  <solidref ref="{solid}"/>\n'
            f'  <auxiliary auxtype="Color" auxvalue="{r:.3f} {g:.3f} {b:.3f} 1"/>\n{facing}</volume>')
        self.placements.append(
            f'  <physvol name="{name}_pv">\n    <volumeref ref="{name}"/>\n'
            f'    <position name="{name}_pos" x="{pos[0]:.3f}" y="{pos[1]:.3f}" z="{pos[2]:.3f}" unit="mm"/>\n'
            + (f'    <rotation name="{name}_rot" x="{rotx}" y="0" z="0" unit="deg"/>\n' if rotx else '')
            + '  </physvol>')

    def write(self, path, world_half):
        hx, hy, hz = world_half
        with open(path, "w") as f:
            f.write('<?xml version="1.0" encoding="UTF-8"?>\n')
            f.write('<gdml xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" '
                    'xsi:noNamespaceSchemaLocation="http://service-spi.web.cern.ch/service-spi/app/releases/GDML/schema/gdml.xsd">\n')
            f.write("<define>\n" + "\n".join(self.defines) + "\n</define>\n")
            f.write("<materials/>\n")
            f.write("<solids>\n")
            f.write(f'<box name="WorldBox" x="{2 * hx:.3f}" y="{2 * hy:.3f}" z="{2 * hz:.3f}" lunit="mm"/>\n')
            f.write("\n".join(self.solids) + "\n</solids>\n")
            f.write("<structure>\n" + "\n".join(self.volumes) + "\n")
            f.write('<volume name="World">\n  <materialref ref="G4_AIR"/>\n  <solidref ref="WorldBox"/>\n')
            f.write("\n".join(self.placements) + "\n</volume>\n</structure>\n")
            f.write('<setup name="Default" version="1.0">\n  <world ref="World"/>\n</setup>\n</gdml>\n')


# ------------------------------------------------------------------- main
def convert(wadpath, mapname, out, skill=2):
    wad = Wad(wadpath)
    m = Map(wad, mapname)
    col = Colours(wad)

    xs = [v[0] for v in m.verts]
    ys = [v[1] for v in m.verts]
    offset = ((min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2)
    base = min(s["floor"] for s in m.sectors) - 16
    top = max(s["ceil"] for s in m.sectors) + 16
    zc = (base + top) / 2
    base -= zc
    top -= zc  # z also re-centred

    g = Gdml(offset)

    # sectors -------------------------------------------------------------
    pieces = {}
    for ss, (sec, poly) in subsector_polygons(m).items():
        pieces.setdefault(sec, []).append(Polygon(poly))
    sector_shapes = {sec: clean(robust_union(ps)) for sec, ps in pieces.items()}
    nslab = 0
    for sec, polys in sector_shapes.items():
        s = m.sectors[sec]
        fz, cz = s["floor"] - zc, s["ceil"] - zc
        fpic, cpic = s["floorpic"], s["ceilpic"]
        fmat = "G4_WATER" if fpic.startswith(LIQUIDS) else "G4_CONCRETE"
        for k, p in enumerate(polys):
            name = f"S{sec}_{k}"
            g.slab(f"{name}_floor_s", p, base, fz)
            g.volume(f"{name}_floor", f"{name}_floor_s", fmat, lit(col.flat(fpic), s["light"]))
            nslab += 1
            if cpic != "F_SKY1" and top > cz:
                g.slab(f"{name}_ceil_s", p, cz, top)
                g.volume(f"{name}_ceil", f"{name}_ceil_s", "G4_CONCRETE", lit(col.flat(cpic), s["light"]))
                nslab += 1

    # outer walls -----------------------------------------------------------
    all_sectors = robust_union([p for ps in sector_shapes.values() for p in ps]).buffer(0.01)
    placed = Polygon()
    nwall = 0
    for i, (v1, v2, flags, special, tag, rs, ls) in enumerate(m.lines):
        if ls != 0xFFFF or rs == 0xFFFF:
            continue
        side = m.sides[rs]
        s = m.sectors[side["sector"]]
        (x1, y1), (x2, y2) = m.verts[v1], m.verts[v2]
        L = math.hypot(x2 - x1, y2 - y1)
        if L == 0:
            continue
        nx, ny = -(y2 - y1) / L * WALL_THICK, (x2 - x1) / L * WALL_THICK  # left = outside
        quad = Polygon([(x1, y1), (x2, y2), (x2 + nx, y2 + ny), (x1 + nx, y1 + ny)])
        geom = quad.difference(all_sectors).difference(placed)
        rgb = lit(col.texture(side["mid"]), s["light"])
        z0, z1 = s["floor"] - zc, s["ceil"] - zc
        if s["ceilpic"] != "F_SKY1":
            z1 = top
        z0 = base
        for k, p in enumerate(clean(geom, open_eps=0.25)):
            name = f"L{i}_{k}"
            g.slab(f"{name}_s", p, z0, z1)
            g.volume(f"{name}_wall", f"{name}_s", "G4_CONCRETE", rgb)
            nwall += 1
        placed = placed.union(quad)

    # things ----------------------------------------------------------------
    start = None
    nthing = 0
    skill_bit = {1: 1, 2: 1, 3: 2, 4: 4, 5: 4}.get(skill, 2)
    for i, (x, y, ang, typ, flags) in enumerate(m.things):
        if typ == 1:
            start = (x, y, ang)
        if typ not in THING_TYPES or not (flags & skill_bit) or (flags & 16):
            continue
        tname, r, h, shape, rgb, zoff = THING_TYPES[typ]
        fz = m.sectors[m.point_sector(x, y)]["floor"] - zc
        name = f"T{i}_{tname}"
        if shape == "orb":
            g.solids.append(f'<orb name="{name}_s" r="{r * SCALE:.3f}" lunit="mm"/>')
            g.volume(name, f"{name}_s", "G4_WATER", rgb, g.g4(x, y, fz + zoff + r), angle=ang)
        else:  # tube axis along z, rotated to stand upright (G4 y)
            g.solids.append(f'<tube name="{name}_s" rmin="0" rmax="{r * SCALE:.3f}" z="{h * SCALE:.3f}" '
                            f'deltaphi="360" aunit="deg" lunit="mm"/>')
            g.volume(name, f"{name}_s", "G4_WATER", rgb, g.g4(x, y, fz + h / 2), rotx=90, angle=ang)
        nthing += 1

    # the map itself (sectors, lines, specials, textures), for doomviewer's doors, lifts and
    # switches, and its textured view
    import doom_surfaces
    level = out.rsplit(".", 1)[0] + "_level.txt"
    rgb = (Palette(wad).rgb * 255).round().astype(np.uint8)
    nflat, ntex = doom_surfaces.export(wad, m, mapname, sector_shapes, offset, zc, level, picture_rgba, write_png, rgb,
                                       skill_bit)
    print(f"{nflat} flat triangles, {ntex} textures -> {level}, textures/")

    hx = (max(xs) - min(xs)) / 2 + 64
    hy = (max(ys) - min(ys)) / 2 + 64
    # headroom above and below: doors raise ceiling slabs, lifts lower floor slabs (by at most
    # the level's height range), and a placement must stay inside the world
    hz = (top - base) / 2 + 16 + (top - base)
    world_half = (hx * SCALE, hz * SCALE, hy * SCALE)  # G4 axes: y up
    g.write(out, world_half)

    meta = {"map": mapname, "scale_mm_per_unit": SCALE, "offset_units": offset, "z_offset_units": zc,
            "axes": "G4 x = Doom x, G4 y = up, G4 z = -Doom y", "world_half_mm": world_half}
    if start:
        x, y, ang = start
        fz = m.sectors[m.point_sector(x, y)]["floor"] - zc
        meta["player_start"] = {"eye_mm": g.g4(x, y, fz + EYE_HEIGHT), "angle_deg": ang}
    print(f"{mapname}: {len(sector_shapes)} sectors -> {nslab} slabs, {nwall} wall pieces, {nthing} things; "
          f"{g.nv} vertices -> {out}")
    return meta


def extract_sounds(wad, outdir):
    """Write every DS* sound lump as an 8-bit mono WAV file (outdir/dsname.wav) for doomviewer.
    Doom sound lumps: u16 format (3), u16 sample rate, u32 sample count, then unsigned 8-bit PCM."""
    import os
    import wave
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for name, pos, size in wad.lumps:
        if not name.startswith("DS") or size < 8:
            continue
        fmt, rate, count = struct.unpack_from("<HHI", wad.data, pos)
        if fmt != 3:
            continue
        with wave.open(os.path.join(outdir, name.lower() + ".wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(1)  # WAV 8-bit is unsigned, like Doom's
            w.setframerate(rate)
            w.writeframes(wad.data[pos + 8:pos + 8 + min(count, size - 8)])
        n += 1
    return n



def mus2mid(mus):
    """Doom's MUS music format as a type-0 MIDI file (the id WADs use MUS; Freedoom ships MIDI).
    MUS: 140 ticks a second, MUS channel 15 is percussion (MIDI 9), and a note-on may omit its
    velocity (the channel's last one is used)."""
    score_len, score_start = struct.unpack_from("<HH", mus, 4)
    controllers = {1: 0x00, 2: 0x01, 3: 0x07, 4: 0x0A, 5: 0x0B, 6: 0x5B, 7: 0x5D, 8: 0x40, 9: 0x43}
    system = {10: 0x78, 11: 0x7B, 12: 0x7E, 13: 0x7F, 14: 0x79}
    track = bytearray(b"\x00\xff\x51\x03\x07\xa1\x20")  # tempo: 0.5 s a beat, 70 ticks a beat
    channel_map, velocity = {}, [127] * 16
    delay = 0

    def varlen(v):
        out = [v & 0x7F]
        while v > 0x7F:
            v >>= 7
            out.insert(0, 0x80 | (v & 0x7F))
        return bytes(out)

    def emit(status, *data):
        nonlocal delay
        track.extend(varlen(delay) + bytes([status]) + bytes(data))
        delay = 0

    def midi_channel(c):
        if c == 15:
            return 9
        if c not in channel_map:  # in order of first use, skipping the percussion channel
            used = set(channel_map.values())
            channel_map[c] = next(m for m in range(16) if m != 9 and m not in used)
            emit(0xB0 | channel_map[c], 0x07, 127)
        return channel_map[c]

    p = score_start
    while p < len(mus):
        desc = mus[p]
        p += 1
        kind, ch = (desc >> 4) & 7, midi_channel(desc & 15)
        if kind == 0:  # release note
            emit(0x80 | ch, mus[p] & 0x7F, 64)
            p += 1
        elif kind == 1:  # play note, maybe with a new velocity
            note = mus[p]
            p += 1
            if note & 0x80:
                velocity[ch] = mus[p] & 0x7F
                p += 1
            emit(0x90 | ch, note & 0x7F, velocity[ch])
        elif kind == 2:  # pitch bend: 0..255, 128 centred
            bend = mus[p] << 6
            p += 1
            emit(0xE0 | ch, bend & 0x7F, bend >> 7)
        elif kind == 3:
            if mus[p] in system:
                emit(0xB0 | ch, system[mus[p]], 0)
            p += 1
        elif kind == 4:
            ctrl, value = mus[p], min(mus[p + 1], 127)
            p += 2
            if ctrl == 0:
                emit(0xC0 | ch, value)
            elif ctrl in controllers:
                emit(0xB0 | ch, controllers[ctrl], value)
        elif kind == 6:  # score end
            break
        if desc & 0x80:  # a delay follows
            d = 0
            while True:
                b = mus[p]
                p += 1
                d = d * 128 + (b & 0x7F)
                if not b & 0x80:
                    break
            delay += d
    track.extend(varlen(delay) + b"\xff\x2f\x00")
    return b"MThd" + struct.pack(">IHHH", 6, 0, 1, 70) + b"MTrk" + struct.pack(">I", len(track)) + bytes(track)


def extract_music(wad, outdir):
    """Write every D_* music lump as outdir/D_NAME.mid (MUS converted to MIDI) and the OPL
    instrument bank as outdir/GENMIDI.op2, for doomviewer's OPL3 music."""
    import os
    os.makedirs(outdir, exist_ok=True)
    n = 0
    for name, pos, size in wad.lumps:
        data = wad.data[pos:pos + size]
        if name == "GENMIDI":
            with open(os.path.join(outdir, "GENMIDI.op2"), "wb") as f:
                f.write(data)
        elif name.startswith("D_") and size > 4:
            if data[:4] == b"MUS\x1a":
                data = mus2mid(data)
            elif data[:4] != b"MThd":
                continue
            with open(os.path.join(outdir, name + ".mid"), "wb") as f:
                f.write(data)
            n += 1
    return n

# Sprites for doomviewer (prefix per thing type; CPOS is Doom II only): monsters, the barrel's
# explosion, and the keys, weapons, ammo, health and armour lying about
SPRITE_PREFIXES = ("POSS", "SPOS", "CPOS", "TROO", "SARG", "SKUL", "HEAD", "BOSS", "BAR1", "BEXP",
                   "BAL1", "BAL2", "BAL7",  # fireballs
                   "BKEY", "YKEY", "RKEY", "BSKU", "YSKU", "RSKU",  # monsters, barrels, keys
                   "SHOT", "SGN2", "MGUN", "LAUN", "PLAS", "BFUG",  # weapons and ammo to pick up
                   "CLIP", "AMMO", "SHEL", "SBOX", "ROCK", "BROK", "CELL", "CELP", "BPAK",
                   "BON1", "BON2", "STIM", "MEDI", "ARM1", "ARM2", "SOUL", "MEGA")  # health, armour


def extract_sprites(wad, outdir):
    """Write the monster sprite lumps (between S_START and S_END) as outdir/NAME.png, with their
    offsets as outdir/offsets.txt lines "NAME left top" (left: the thing's centre column; top:
    rows above its feet). Names follow Doom: TROOA2A8 is frame A at rotation 2, and mirrored, 8."""
    import os
    os.makedirs(outdir, exist_ok=True)
    rgb = (Palette(wad).rgb * 255).round().astype(np.uint8)
    names = [lump[0] for lump in wad.lumps]
    start = names.index("S_START") if "S_START" in names else 0
    offsets = []
    for name, pos, size in wad.lumps[start:]:
        if name in ("S_END", "SS_END"):
            break
        if not name.startswith(SPRITE_PREFIXES) or size < 8:
            continue
        img, lo, to = picture_rgba(wad.data[pos:pos + size], rgb)
        write_png(os.path.join(outdir, name + ".png"), img)
        offsets.append(f"{name} {lo} {to}\n")
    with open(os.path.join(outdir, "offsets.txt"), "w") as f:
        f.writelines(offsets)
    return len(offsets)


# Status bar graphics for doomviewer's HUD: the bar, its digit fonts, Doomguy's faces, keys,
# the heads-up message font, and the weapon sprites with their muzzle flashes
# and the intermission screens (WI*: episode maps, level names, counters; Doom II's INTERPIC)
HUD_LUMPS = ("STBAR", "STARMS", "STTNUM", "STTPRCNT", "STTMINUS", "STYSNUM", "STGNUM", "STF", "STKEYS", "STCFN",
             "WI", "CWILV", "INTERPIC",
             "PISG", "PISF", "SHTG", "SHTF", "CHGG", "CHGF", "MISG", "MISF", "PLSG", "PLSF", "BFGG", "BFGF")


def picture_rgba(data, rgb):
    """A Doom 'picture' format patch as an RGBA array (transparent where no post covers it),
    plus its left and top offsets."""
    w, h, lo, to = struct.unpack_from("<hhhh", data, 0)
    img = np.zeros((h, w, 4), dtype=np.uint8)
    for c in range(w):
        p = struct.unpack_from("<I", data, 8 + 4 * c)[0]
        while p < len(data) and data[p] != 0xFF:
            top, n = data[p], data[p + 1]
            idx = np.frombuffer(data[p + 3:p + 3 + n], dtype=np.uint8)
            rows = slice(top, min(top + n, h))
            img[rows, c, :3] = rgb[idx[:rows.stop - top]]
            img[rows, c, 3] = 255
            p += n + 4
    return img, lo, to


def write_png(path, img):
    """Minimal RGBA PNG writer (stdlib only)."""
    import zlib
    h, w = img.shape[:2]
    raw = b"".join(b"\0" + img[y].tobytes() for y in range(h))
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def extract_hud(wad, outdir):
    """Write the status bar pictures as outdir/NAME.png, and their offsets (which Doom subtracts
    when drawing a patch) as outdir/offsets.txt lines "NAME left top"."""
    import os
    os.makedirs(outdir, exist_ok=True)
    rgb = (Palette(wad).rgb * 255).round().astype(np.uint8)
    offsets = []
    for name, pos, size in wad.lumps:
        if not name.startswith(HUD_LUMPS) or size < 8:
            continue
        img, lo, to = picture_rgba(wad.data[pos:pos + size], rgb)
        write_png(os.path.join(outdir, name + ".png"), img)
        offsets.append(f"{name} {lo} {to}\n")
    with open(os.path.join(outdir, "offsets.txt"), "w") as f:
        f.writelines(offsets)
    return len(offsets)


def camera_macro(eye, angle_deg, world_half, fov_half=37.0, look_ahead=1000.0):
    """Geant4 vis commands that put the camera at `eye` (G4 mm), looking along Doom angle.

    Geant4 places the camera at target + viewpointVector * (R/sin(fov_half) - dolly),
    R = scene radius, so we aim at a point `look_ahead` in front of the eye and dolly in.
    fov_half: OpenGL/TSG apply it to the vertical half-angle, the RayTracer to the horizontal.
    """
    a = math.radians(angle_deg)
    fx, fz = math.cos(a), -math.sin(a)  # Doom (cos, sin, 0) in G4 axes
    tx, ty, tz = eye[0] + fx * look_ahead, eye[1], eye[2] + fz * look_ahead
    R = math.sqrt(sum(h * h for h in world_half))  # scene extent radius of the world box
    dolly = R / math.sin(math.radians(fov_half)) - look_ahead
    hx, hy, hz = world_half
    return "\n".join([
        "# pin the scene extent to the world box so the dolly maths below is exact",
        f"/vis/scene/add/extent {-hx:.1f} {hx:.1f} {-hy:.1f} {hy:.1f} {-hz:.1f} {hz:.1f} mm",
        f"/vis/viewer/set/projection perspective {fov_half} deg",
        "/vis/viewer/set/upVector 0 1 0",
        f"/vis/viewer/set/viewpointVector {-fx:.6f} 0 {-fz:.6f}",
        f"/vis/viewer/set/targetPoint {tx:.1f} {ty:.1f} {tz:.1f} mm",
        f"/vis/viewer/dollyTo {dolly:.1f} mm",
    ])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wad")
    ap.add_argument("map", nargs="?", default="E1M1",
                    help="E1M1, MAP01, a list (E1M1,E1M2), or all: every map in the WAD (default E1M1)")
    ap.add_argument("-o", "--out", help="the GDML file for one map, or the folder for several (default .)")
    ap.add_argument("--skill", type=int, default=3, help="1-5, which monsters to include (default 3)")
    a = ap.parse_args()
    import os
    import re
    if a.map.lower() == "all":
        maps = [n for n, _, _ in Wad(a.wad).lumps if re.fullmatch(r"E\dM\d|MAP\d\d", n)]
    else:
        maps = [m.strip().upper() for m in a.map.split(",")]
    if len(maps) == 1 and (a.out or "").endswith(".gdml"):
        outs = [a.out]
    else:  # a folder: <map>.gdml for each, sharing sounds/, textures/, ... (doomviewer goes from one
           # map to the next in it)
        os.makedirs(a.out or ".", exist_ok=True)
        outs = [os.path.join(a.out or ".", m.lower() + ".gdml") for m in maps]
    for mapname, out in zip(maps, outs):
        meta = convert(a.wad, mapname, out, a.skill)
        write_map_files(out, meta)
    out = outs[0]
    sounds = os.path.join(os.path.dirname(out) or ".", "sounds")
    print(f"{extract_sounds(Wad(a.wad), sounds)} sounds -> {sounds}/")
    sprites = os.path.join(os.path.dirname(out) or ".", "sprites")
    print(f"{extract_sprites(Wad(a.wad), sprites)} monster sprites -> {sprites}/")
    music = os.path.join(os.path.dirname(out) or ".", "music")
    print(f"{extract_music(Wad(a.wad), music)} songs -> {music}/")
    hud = os.path.join(os.path.dirname(out) or ".", "hud")
    print(f"{extract_hud(Wad(a.wad), hud)} status bar pictures -> {hud}/")
    import physicist_face  # Doomguy, restyled as a totally-not-famous physicist (/doom/face)
    physicist_face.main(hud)
    import lhc_hud  # the status bar as an accelerator magnet (/doom/hudStyle)
    lhc_hud.main(hud)
    import weapon_sprites  # the weapons as particle-physics instruments (/doom/weaponStyle)
    weapon_sprites.main(hud)


def write_map_files(out, meta):
    """<map>.json, and the gun and camera macros at the player start"""
    stem = out.rsplit(".", 1)[0]
    with open(stem + ".json", "w") as f:
        json.dump(meta, f, indent=1)
    if "player_start" in meta:
        ps = meta["player_start"]
        a = math.radians(ps["angle_deg"])
        ex, ey, ez = ps["eye_mm"]
        with open(f"{stem}_gun.mac", "w") as f:  # the "plasma rifle": a particle gun at the player's eyes
            f.write("# particle gun at the player start, aimed where the player looks\n"
                    f"/gun/position {ex:.1f} {ey - 300:.1f} {ez:.1f} mm\n"
                    f"/gun/direction {math.cos(a):.6f} 0 {-math.sin(a):.6f}\n"
                    "/gun/particle proton\n/gun/energy 10 GeV\n")
        for suffix, fov, what in (("camera", 37.0, "OpenGL / Qt / TSG viewers, 4:3 window"),
                                  ("camera_rt", 45.0, "RayTracer (90 deg horizontal, like Doom)")):
            with open(f"{stem}_{suffix}.mac", "w") as f:
                f.write(f"# first-person camera at the player start - {what}\n")
                f.write(camera_macro(ps["eye_mm"], ps["angle_deg"], meta["world_half_mm"], fov) + "\n")


if __name__ == "__main__":
    main()
