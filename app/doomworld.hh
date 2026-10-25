// SPDX-License-Identifier: GPL-2.0-or-later
// doomworld.hh: the moving parts of a Doom level for doomviewer: doors, lifts, floors,
// switches, walk-over lines, keys, teleporters and exits, as Doom runs them (p_doors.c,
// p_plats.c, p_floor.c, p_switch.c, p_spec.c, p_map.c), on the map that doom_surfaces.py
// writes (<map>_level.txt).
//
// A sector's floor and ceiling are the GDML slabs "S<sector>_<k>_floor" and "..._ceil", so
// a moving floor or ceiling translates those placements (vertically, by the height change).
// Placements are per thread in MT Geant4: the master moves them, and every worker copies the
// translations at the start of a run (Sync, from the WorkerSync run action), as for the
// monsters. The world box has room above and below for the slabs (doom2gdml.py).
//
// The walls drawn with textures depend on the sector heights, so they are built here too, from
// the current state (Surfaces), with Doom's texture alignment rules (r_segs.c).
//
// Everything is in Doom map units and tics; Geant4 coordinates only at the edges (ToG4/FromG4).
#ifndef DOOMWORLD_HH
#define DOOMWORLD_HH

#include "G4AutoLock.hh"
#include "G4LogicalVolume.hh"
#include "G4ThreeVector.hh"
#include "G4VPhysicalVolume.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace doom
{
constexpr int kSkyTexture = -2;
constexpr int kMLSecret = 32, kMLDontPegTop = 8, kMLDontPegBottom = 16;
constexpr int kTicsPerSecond = 35;

struct TextureInfo
{
  std::string file, name;
  int width = 64, height = 64;
  std::vector<int> anim;  // the sequence it cycles in (8 tics a frame), and its place there
  int animPos = 0;
  int partner = -1;  // the other half of a switch (SW1xxx <-> SW2xxx)
};

struct Sector
{
  double floor, ceil;
  int floorTex, ceilTex, light, special, tag;
  double floor0, ceil0;  // at the start: restored by Reset
  int floorTex0, special0;
  std::vector<int> lines;
  std::vector<std::pair<G4VPhysicalVolume*, bool>> slabs;  // the GDML slabs: (placement, is ceiling)
  double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
  int mover = -1;      // the thinker moving it (Doom's specialdata), or -1
  bool moved = false;  // since the geometry was last updated
};

struct Side
{
  int sector;
  double xoff, yoff;
  int upper, lower, mid;
  int upper0, lower0, mid0;
};

struct Line
{
  double x1, y1, x2, y2;
  int flags, special, tag, front, back;
  int special0;
};

struct Thing
{
  int type;
  double x, y, angle;
  int sector;
  bool taken = false;
  bool dropped = false;  // by a dying monster: half the ammo, and gone after a reset
};

struct Flat
{
  int sector;
  double x[3], y[3];
};

class World
{
 public:
  // What the world needs from the game: positional sounds (Doom coordinates), messages,
  // whether a thing is in the way of a sector's new floor/ceiling, a teleport for the
  // player, and the exit
  struct Hooks
  {
    std::function<void(const std::string& lump, double x, double y, double z)> sound;
    std::function<void(const std::string& text)> message;
    std::function<bool(int sector, double floor, double ceil)> blocked;
    std::function<void(double x, double y, double z, double angleDeg)> teleport;
    std::function<void(bool secret)> exit;
  };

  explicit World(const std::string& file)
  {
    std::ifstream in(file);
    std::string line, kind;
    while (std::getline(in, line)) {
      std::istringstream w(line);
      if (!(w >> kind) || kind[0] == '#') continue;
      if (kind == "origin")
        w >> fOx >> fOy >> fZc >> fScale;
      else if (kind == "texture") {
        TextureInfo t;
        int id;
        w >> id >> t.file >> t.width >> t.height >> t.name;
        fTextures.push_back(t);
      }
      else if (kind == "anim") {
        std::vector<int> seq;
        for (int id; w >> id;) seq.push_back(id);
        for (std::size_t i = 0; i < seq.size(); ++i)
          if (seq[i] >= 0 && seq[i] < int(fTextures.size())) {
            fTextures[seq[i]].anim = seq;
            fTextures[seq[i]].animPos = int(i);
          }
      }
      else if (kind == "switch") {
        int a, b;
        w >> a >> b;
        if (a >= 0 && b >= 0 && a < int(fTextures.size()) && b < int(fTextures.size())) {
          fTextures[a].partner = b;
          fTextures[b].partner = a;
        }
      }
      else if (kind == "sky")
        w >> fSkyFile;
      else if (kind == "sector") {
        Sector s;
        int i;
        w >> i >> s.floor >> s.ceil >> s.floorTex >> s.ceilTex >> s.light >> s.special >> s.tag;
        s.floor0 = s.floor;
        s.ceil0 = s.ceil;
        s.floorTex0 = s.floorTex;
        s.special0 = s.special;
        fSectors.push_back(s);
      }
      else if (kind == "side") {
        Side s;
        int i;
        w >> i >> s.sector >> s.xoff >> s.yoff >> s.upper >> s.lower >> s.mid;
        s.upper0 = s.upper;
        s.lower0 = s.lower;
        s.mid0 = s.mid;
        fSides.push_back(s);
      }
      else if (kind == "line") {
        Line l;
        int i;
        w >> i >> l.x1 >> l.y1 >> l.x2 >> l.y2 >> l.flags >> l.special >> l.tag >> l.front >> l.back;
        l.special0 = l.special;
        fLines.push_back(l);
      }
      else if (kind == "flat") {
        Flat f;
        w >> f.sector;
        for (int k = 0; k < 3; ++k) w >> f.x[k] >> f.y[k];
        fFlats.push_back(f);
      }
      else if (kind == "thing") {
        Thing t;
        w >> t.type >> t.x >> t.y >> t.angle >> t.sector;
        fThings.push_back(t);
      }
    }
    fSectorFlats.resize(fSectors.size());
    for (std::size_t i = 0; i < fFlats.size(); ++i)
      if (fFlats[i].sector >= 0 && fFlats[i].sector < int(fSectors.size())) fSectorFlats[fFlats[i].sector].push_back(int(i));
    for (std::size_t i = 0; i < fLines.size(); ++i) {
      const Line& l = fLines[i];
      for (int side : {l.front, l.back}) {
        if (side < 0 || side >= int(fSides.size())) continue;
        Sector& s = fSectors[fSides[side].sector];
        s.lines.push_back(int(i));
        s.minX = std::min({s.minX, l.x1, l.x2});
        s.maxX = std::max({s.maxX, l.x1, l.x2});
        s.minY = std::min({s.minY, l.y1, l.y2});
        s.maxY = std::max({s.maxY, l.y1, l.y2});
      }
    }
  }

  G4bool Loaded() const { return !fSectors.empty(); }
  void SetHooks(Hooks hooks) { fHooks = std::move(hooks); }

  // Find the sector slabs among the world's daughters ("S12_0_floor", "S12_1_ceil")
  void Bind(G4LogicalVolume* world)
  {
    for (std::size_t i = 0; i < world->GetNoDaughters(); ++i) {
      G4VPhysicalVolume* pv = world->GetDaughter(i);
      const std::string name = pv->GetLogicalVolume()->GetName();
      if (name.size() < 2 || name[0] != 'S' || !std::isdigit(static_cast<unsigned char>(name[1]))) continue;
      const int sector = std::stoi(name.substr(1));
      const G4bool ceiling = name.size() > 5 && name.compare(name.size() - 5, 5, "_ceil") == 0;
      if (sector < 0 || sector >= int(fSectors.size())) continue;
      fSectors[sector].slabs.push_back({pv, ceiling});
      fSlabs.push_back({pv, 0.});
      fSlabIndex[pv] = fSlabs.size() - 1;
      fSectorOfVolume[pv->GetLogicalVolume()] = sector;
    }
  }

  // The sector whose floor or ceiling slab this is; -1 for anything else
  int SectorOfVolume(const G4LogicalVolume* lv) const
  {
    auto it = fSectorOfVolume.find(lv);
    return it == fSectorOfVolume.end() ? -1 : it->second;
  }

  // ---- where things are (Doom map units)
  G4bool InSector(int s, double x, double y) const
  {
    const Sector& sec = fSectors[s];
    if (x < sec.minX || x > sec.maxX || y < sec.minY || y > sec.maxY) return false;
    for (int i : fSectorFlats[s]) {
      const Flat& f = fFlats[i];
      G4bool inside = true;
      for (int k = 0; k < 3 && inside; ++k) {  // anticlockwise: left of every edge (or on it)
        const int n = (k + 1) % 3;
        inside = (f.x[n] - f.x[k]) * (y - f.y[k]) - (f.y[n] - f.y[k]) * (x - f.x[k]) >= -1e-6;
      }
      if (inside) return true;
    }
    return false;
  }
  // The sector at a point; -1 outside the level
  int SectorAt(double x, double y) const
  {
    for (std::size_t s = 0; s < fSectors.size(); ++s)
      if (InSector(int(s), x, y)) return int(s);
    return -1;
  }
  // Whether a thing's box (radius r round x, y) reaches into sector s
  G4bool Touches(int s, double x, double y, double r) const
  {
    for (auto [dx, dy] : {std::pair{0., 0.}, {-r, -r}, {r, -r}, {r, r}, {-r, r}, {0., -r}, {0., r}, {-r, 0.}, {r, 0.}})
      if (InSector(s, x + dx, y + dy)) return true;
    return false;
  }

  // ---- coordinates
  G4ThreeVector ToG4(double x, double y, double z) const
  {
    return {(x - fOx) * fScale, (z - fZc) * fScale, -(y - fOy) * fScale};
  }
  void FromG4(const G4ThreeVector& p, double& x, double& y, double& z) const
  {
    x = p.x() / fScale + fOx;
    y = -p.z() / fScale + fOy;
    z = p.y() / fScale + fZc;
  }

  // ---- state for the game and the drawing
  const std::vector<Sector>& Sectors() const { return fSectors; }
  const std::vector<Side>& Sides() const { return fSides; }
  const std::vector<Line>& Lines() const { return fLines; }
  const std::vector<Thing>& Things() const { return fThings; }
  const std::vector<TextureInfo>& Textures() const { return fTextures; }
  const std::string& SkyFile() const { return fSkyFile; }
  int LevelTime() const { return fLevelTime; }
  int Keys() const { return fKeys; }  // bits 0-2 blue/yellow/red cards, 3-5 skull keys
  void GiveKeys() { fKeys = 63; }
  // Changes that the drawing must follow: heights, textures, things taken
  unsigned Version() const { return fVersion; }
  G4bool HasScrollers() const
  {
    for (const Line& l : fLines)
      if (l.special == 48) return true;
    return false;
  }
  G4bool Busy() const { return !fMovers.empty() || !fButtons.empty(); }

  // ---- Doom's actions

  // P_UseLines: the player at (x, y) uses whatever lies ahead within 64 units. Lines without a
  // special are passed if they leave an opening (a two-sided line), else "no way" (dsnoway).
  void Use(double x, double y, double angleDeg)
  {
    const double a = angleDeg * M_PI / 180, x2 = x + 64 * std::cos(a), y2 = y + 64 * std::sin(a);
    std::vector<std::pair<double, int>> hits;
    for (std::size_t i = 0; i < fLines.size(); ++i) {
      double t;
      if (Crosses(x, y, x2, y2, fLines[i], t)) hits.push_back({t, int(i)});
    }
    std::sort(hits.begin(), hits.end());
    for (const auto& [t, i] : hits) {
      const Line& l = fLines[i];
      if (l.special == 0) {
        if (Opening(l) <= 0) {
          Sound("dsnoway", x, y);
          return;
        }
        continue;
      }
      UseSpecial(i, PointSide(x, y, l), true);
      return;
    }
  }

  // A monster at (x, y) walking along angleDeg is blocked within `reach`: it opens a plain door
  // in its way (P_Move -> P_UseSpecialLine; monsters only use special 1, and not secret lines)
  G4bool MonsterUse(double x, double y, double angleDeg, double reach)
  {
    const double a = angleDeg * M_PI / 180, x2 = x + reach * std::cos(a), y2 = y + reach * std::sin(a);
    for (std::size_t i = 0; i < fLines.size(); ++i) {
      const Line& l = fLines[i];
      double t;
      if (l.special == 1 && !(l.flags & kMLSecret) && Crosses(x, y, x2, y2, l, t)) {
        if (PointSide(x, y, l) == 0) return UseSpecial(int(i), 0, false);
      }
    }
    return false;
  }

  // P_CrossSpecialLine: a thing moved from (x0, y0) to (x1, y1); walk-over lines it crossed fire
  void Cross(double x0, double y0, double x1, double y1, G4bool player)
  {
    for (std::size_t i = 0; i < fLines.size(); ++i) {
      const Line& l = fLines[i];
      if (l.special == 0) continue;
      double t;
      if (!Crosses(x0, y0, x1, y1, l, t)) continue;
      CrossSpecial(int(i), PointSide(x0, y0, l), player);
    }
  }

  // The player at (x, y, feet z) picks up what it touches (P_TouchSpecialThing): keys here,
  // anything else if give(thing) takes it (weapons and ammo, which the game keeps)
  void Touch(double x, double y, double z, const std::function<G4bool(const Thing&)>& give)
  {
    static const std::map<int, std::pair<int, const char*>> keys = {
      {5, {0, "Picked up a blue keycard."}},     {6, {1, "Picked up a yellow keycard."}},
      {13, {2, "Picked up a red keycard."}},     {40, {3, "Picked up a blue skull key."}},
      {39, {4, "Picked up a yellow skull key."}}, {38, {5, "Picked up a red skull key."}}};
    for (Thing& t : fThings) {
      if (t.taken || t.sector < 0 || t.type == 14) continue;  // 14: a teleport destination
      const double floor = fSectors[t.sector].floor;
      if (std::abs(t.x - x) >= 16 + 20 || std::abs(t.y - y) >= 16 + 20 || z > floor + 16 || z + 56 < floor) continue;
      auto key = keys.find(t.type);
      if (key == keys.end()) {
        if (give && give(t)) {
          t.taken = true;
          ++fVersion;
        }
        continue;
      }
      {
        t.taken = true;
        fKeys |= 1 << key->second.first;
        ++fVersion;
        Message(key->second.second);
        Sound("dsitemup", x, y);
      }
    }
  }

  // A thing dropped by a dying monster (P_KillMobj): a clip, a shotgun, a chaingun
  void Drop(int type, double x, double y)
  {
    Thing t;
    t.type = type;
    t.x = x;
    t.y = y;
    t.angle = 0;
    t.sector = SectorAt(x, y);
    t.dropped = true;
    if (t.sector < 0) return;
    fThings.push_back(t);
    ++fVersion;
  }

  // Doom's sector specials under the player: a secret (9) is found once
  G4bool FindSecret(int sector)
  {
    if (sector < 0 || fSectors[sector].special != 9) return false;
    fSectors[sector].special = 0;
    ++fSecretsFound;
    Message("A secret is revealed!");
    return true;
  }
  int SecretsFound() const { return fSecretsFound; }
  // Doom's item count (MF_COUNTITEM): the health and armour bonuses and the spheres; found, total
  std::pair<int, int> Items() const
  {
    int found = 0, total = 0;
    for (const Thing& t : fThings)
      if (!t.dropped && (t.type == 2014 || t.type == 2015 || t.type == 2013 || t.type == 83)) {
        ++total;
        found += t.taken;
      }
    return {found, total};
  }
  int SecretsTotal() const
  {
    int n = 0;
    for (const Sector& s : fSectors) n += s.special0 == 9;
    return n;
  }

  // One game tic: every thinker moves, switches pop back. Returns whether any floor or
  // ceiling moved (then call UpdateGeometry).
  G4bool Tic()
  {
    ++fLevelTime;
    for (std::size_t i = 0; i < fMovers.size(); ++i)
      if (fMovers[i].active) Think(int(i));
    fMovers.erase(std::remove_if(fMovers.begin(), fMovers.end(), [](const Mover& m) { return !m.active; }),
                  fMovers.end());
    for (std::size_t i = 0; i < fMovers.size(); ++i) fSectors[fMovers[i].sector].mover = int(i);  // re-index
    for (auto it = fButtons.begin(); it != fButtons.end();) {
      if (--it->timer > 0) {
        ++it;
        continue;
      }
      SwapSwitch(it->line);
      const Line& l = fLines[it->line];
      Sound("dsswtchn", (l.x1 + l.x2) / 2, (l.y1 + l.y2) / 2);
      it = fButtons.erase(it);
    }
    G4bool any = false;
    for (const Sector& s : fSectors) any |= s.moved;
    return any;
  }

  // Move the slabs of the sectors that moved (master thread, outside a run). Returns the
  // sectors moved, for the things riding on them.
  std::vector<int> UpdateGeometry()
  {
    std::vector<int> moved;
    G4AutoLock lock(&fMutex);
    for (std::size_t i = 0; i < fSectors.size(); ++i) {
      Sector& s = fSectors[i];
      if (!s.moved) continue;
      s.moved = false;
      moved.push_back(int(i));
      for (auto& [pv, ceiling] : s.slabs) {
        const double dy = ((ceiling ? s.ceil - s.ceil0 : s.floor - s.floor0)) * fScale;
        pv->SetTranslation(G4ThreeVector(0, dy, 0));
        fSlabs[fSlabIndex[pv]].second = dy;
      }
    }
    if (!moved.empty()) ++fVersion;
    return moved;
  }

  // Every worker, at the start of a run: this thread's slab placements as the master has them
  void Sync()
  {
    G4AutoLock lock(&fMutex);
    for (auto& [pv, dy] : fSlabs) pv->SetTranslation(G4ThreeVector(0, dy, 0));
  }

  // Back to the level as it started (/doom/reset)
  void Reset()
  {
    for (Sector& s : fSectors) {
      s.moved = s.floor != s.floor0 || s.ceil != s.ceil0;
      s.floor = s.floor0;
      s.ceil = s.ceil0;
      s.floorTex = s.floorTex0;
      s.special = s.special0;
      s.mover = -1;
    }
    for (Side& s : fSides) {
      s.upper = s.upper0;
      s.lower = s.lower0;
      s.mid = s.mid0;
    }
    for (Line& l : fLines) l.special = l.special0;
    fThings.erase(std::remove_if(fThings.begin(), fThings.end(), [](const Thing& t) { return t.dropped; }),
                  fThings.end());
    for (Thing& t : fThings) t.taken = false;
    fMovers.clear();
    fButtons.clear();
    fKeys = 0;
    fSecretsFound = 0;
    fLevelTime = 0;
    ++fVersion;
  }

  // ---- the textured surfaces, as Doom draws them now. Per texture: x y z (mm), u v (in
  // texture sizes), light (0..1), for GL_TRIANGLES.
  void Surfaces(std::vector<std::vector<float>>& out) const
  {
    out.assign(fTextures.size(), {});
    auto vertex = [&](std::vector<float>& v, double x, double y, double z, double u, double t, double light) {
      const G4ThreeVector p = ToG4(x, y, z);
      for (double f : {p.x(), p.y(), p.z(), u, t, light}) v.push_back(float(f));
    };
    for (const Flat& f : fFlats) {
      const Sector& s = fSectors[f.sector];
      const double light = std::clamp(s.light, 0, 255) / 255.;
      if (s.floorTex >= 0) {
        auto& v = out[s.floorTex];
        for (int k = 0; k < 3; ++k) vertex(v, f.x[k], f.y[k], s.floor, f.x[k] / 64, -f.y[k] / 64, light);
      }
      if (s.ceilTex >= 0) {  // seen from below: the other way round
        auto& v = out[s.ceilTex];
        for (int k : {0, 2, 1}) vertex(v, f.x[k], f.y[k], s.ceil, f.x[k] / 64, -f.y[k] / 64, light);
      }
    }
    for (const Line& l : fLines) {
      for (const G4bool front : {true, false}) {
        const int side = front ? l.front : l.back, other = front ? l.back : l.front;
        if (side < 0) continue;
        const Side& sd = fSides[side];
        const double x1 = front ? l.x1 : l.x2, y1 = front ? l.y1 : l.y2;
        const double x2 = front ? l.x2 : l.x1, y2 = front ? l.y2 : l.y1;
        const double length = std::hypot(x2 - x1, y2 - y1);
        if (length == 0) continue;
        const Sector& f = fSectors[sd.sector];
        const int contrast = y1 == y2 ? -16 : x1 == x2 ? 16 : 0;  // Doom's fake contrast
        const double light = std::clamp(f.light + contrast, 0, 255) / 255.;
        const double u0 = sd.xoff + (front && l.special == 48 ? fLevelTime : 0);  // scrolling walls
        auto quad = [&](int tex, double zb, double zt, double vtop) {
          if (tex < 0 || zt <= zb) return;
          const TextureInfo& t = fTextures[tex];
          auto& v = out[tex];
          const double ua = u0 / t.width, ub = (u0 + length) / t.width;
          const double vb = (vtop - zb) / t.height, vt = (vtop - zt) / t.height;
          vertex(v, x1, y1, zb, ua, vb, light);
          vertex(v, x2, y2, zb, ub, vb, light);
          vertex(v, x2, y2, zt, ub, vt, light);
          vertex(v, x1, y1, zb, ua, vb, light);
          vertex(v, x2, y2, zt, ub, vt, light);
          vertex(v, x1, y1, zt, ua, vt, light);
        };
        auto height = [&](int tex) { return tex >= 0 ? fTextures[tex].height : 0; };
        if (other < 0) {  // one-sided: the middle texture, floor to ceiling
          const double vtop = (l.flags & kMLDontPegBottom ? f.floor + height(sd.mid) : f.ceil) + sd.yoff;
          quad(sd.mid, f.floor, f.ceil, vtop);
          continue;
        }
        const Sector& b = fSectors[fSides[other].sector];
        if (f.ceil > b.ceil && !(f.ceilTex == kSkyTexture && b.ceilTex == kSkyTexture)) {
          const double vtop = (l.flags & kMLDontPegTop ? f.ceil : b.ceil + height(sd.upper)) + sd.yoff;
          quad(sd.upper, b.ceil, f.ceil, vtop);
        }
        if (f.floor < b.floor) {
          const double vtop = (l.flags & kMLDontPegBottom ? f.ceil : b.floor) + sd.yoff;
          quad(sd.lower, f.floor, b.floor, vtop);
        }
        if (sd.mid >= 0) {  // see-through: drawn once, not tiled
          const double lo = std::max(f.floor, b.floor), hi = std::min(f.ceil, b.ceil), h = height(sd.mid);
          const double top = (l.flags & kMLDontPegBottom ? lo + h : hi) + sd.yoff;
          quad(sd.mid, std::max(top - h, lo), std::min(top, hi), top);
        }
      }
    }
  }

 private:
  enum class Kind { Door, Plat, Floor };
  enum class Door { Normal, Close30ThenOpen, Close, Open, BlazeRaise, BlazeOpen, BlazeClose };
  enum class Plat { DownWaitUpStay, BlazeDWUS, RaiseToNearestAndChange, RaiseAndChange };
  enum class Floor { Lower, LowerToLowest, TurboLower, Raise, RaiseCrush, RaiseToNearest, RaiseTurbo, Raise24,
                     Raise24AndChange };
  enum class Result { Ok, Crushed, PastDest };

  struct Mover
  {
    Kind kind;
    int sector;
    G4bool active = true;
    double speed = 2;
    int direction = 1;  // 1 up, -1 down, 0 waiting (doors); plats: 1 up, -1 down, 0 waiting
    // doors
    Door door = Door::Normal;
    double topHeight = 0;
    int topWait = 150, countdown = 0;
    // plats
    Plat plat = Plat::DownWaitUpStay;
    double low = 0, high = 0;
    int wait = 0;
    // floors
    Floor floor = Floor::Lower;
    double dest = 0;
  };

  struct Button
  {
    int line, timer;
  };

  // ---- geometry helpers
  static int PointSide(double x, double y, const Line& l)  // P_PointOnLineSide: 0 front (right)
  {
    const double dx = x - l.x1, dy = y - l.y1, ldx = l.x2 - l.x1, ldy = l.y2 - l.y1;
    return dy * ldx >= ldy * dx ? 1 : 0;
  }

  // Whether the segment (ax,ay)-(bx,by) crosses the line; t: where along the segment
  static G4bool Crosses(double ax, double ay, double bx, double by, const Line& l, double& t)
  {
    const double rx = bx - ax, ry = by - ay, sx = l.x2 - l.x1, sy = l.y2 - l.y1;
    const double d = rx * sy - ry * sx;
    if (d == 0) return false;
    const double qx = l.x1 - ax, qy = l.y1 - ay;
    t = (qx * sy - qy * sx) / d;
    const double u = (qx * ry - qy * rx) / d;
    return t >= 0 && t <= 1 && u >= 0 && u <= 1;
  }

  double Opening(const Line& l) const  // P_LineOpening: the gap a two-sided line leaves
  {
    if (l.front < 0 || l.back < 0) return 0;
    const Sector& a = fSectors[fSides[l.front].sector];
    const Sector& b = fSectors[fSides[l.back].sector];
    return std::min(a.ceil, b.ceil) - std::max(a.floor, b.floor);
  }

  // The sectors next to s (through its two-sided lines)
  template <class F>
  void Neighbours(int s, F f) const
  {
    for (int i : fSectors[s].lines) {
      const Line& l = fLines[i];
      if (l.front < 0 || l.back < 0) continue;
      const int a = fSides[l.front].sector, b = fSides[l.back].sector;
      const int n = a == s ? b : a;
      if (n != s) f(fSectors[n]);
    }
  }
  double LowestFloorAround(int s) const
  {
    double h = fSectors[s].floor;
    Neighbours(s, [&](const Sector& n) { h = std::min(h, n.floor); });
    return h;
  }
  double HighestFloorAround(int s) const
  {
    double h = -500;
    Neighbours(s, [&](const Sector& n) { h = std::max(h, n.floor); });
    return h;
  }
  double NextHighestFloor(int s, double current) const
  {
    double h = 1e9;
    Neighbours(s, [&](const Sector& n) {
      if (n.floor > current) h = std::min(h, n.floor);
    });
    return h == 1e9 ? current : h;
  }
  double LowestCeilingAround(int s) const
  {
    double h = 1e9;
    Neighbours(s, [&](const Sector& n) { h = std::min(h, n.ceil); });
    return h == 1e9 ? fSectors[s].ceil : h;
  }

  // ---- the line specials
  G4bool UseSpecial(int i, int side, G4bool player)
  {
    Line& l = fLines[i];
    if (side == 1 && l.special != 124) return false;  // switches and doors work from the front
    if (!player && l.special != 1) return false;
    const int sp = l.special;
    switch (sp) {
      case 1: case 26: case 27: case 28: case 31: case 32: case 33: case 34: case 117: case 118:
        return VerticalDoor(i, player);
      case 11:
      case 51:
        SwapSwitch(i, sp == 11 ? "dsswtchx" : "dsswtchn");
        l.special = 0;
        if (fHooks.exit) fHooks.exit(sp == 51);
        return true;
      default:
        break;
    }
    // switches: S1 once (the switch stays on), SR again after a second
    static const std::map<int, std::pair<std::function<G4bool(World&, int)>, G4bool>> kSwitches = {
      {29, {[](World& w, int l) { return w.DoDoor(l, Door::Normal); }, false}},
      {50, {[](World& w, int l) { return w.DoDoor(l, Door::Close); }, false}},
      {103, {[](World& w, int l) { return w.DoDoor(l, Door::Open); }, false}},
      {111, {[](World& w, int l) { return w.DoDoor(l, Door::BlazeRaise); }, false}},
      {112, {[](World& w, int l) { return w.DoDoor(l, Door::BlazeOpen); }, false}},
      {113, {[](World& w, int l) { return w.DoDoor(l, Door::BlazeClose); }, false}},
      {42, {[](World& w, int l) { return w.DoDoor(l, Door::Close); }, true}},
      {61, {[](World& w, int l) { return w.DoDoor(l, Door::Open); }, true}},
      {63, {[](World& w, int l) { return w.DoDoor(l, Door::Normal); }, true}},
      {114, {[](World& w, int l) { return w.DoDoor(l, Door::BlazeRaise); }, true}},
      {115, {[](World& w, int l) { return w.DoDoor(l, Door::BlazeOpen); }, true}},
      {116, {[](World& w, int l) { return w.DoDoor(l, Door::BlazeClose); }, true}},
      {21, {[](World& w, int l) { return w.DoPlat(l, Plat::DownWaitUpStay, 0); }, false}},
      {62, {[](World& w, int l) { return w.DoPlat(l, Plat::DownWaitUpStay, 0); }, true}},
      {122, {[](World& w, int l) { return w.DoPlat(l, Plat::BlazeDWUS, 0); }, false}},
      {123, {[](World& w, int l) { return w.DoPlat(l, Plat::BlazeDWUS, 0); }, true}},
      {20, {[](World& w, int l) { return w.DoPlat(l, Plat::RaiseToNearestAndChange, 0); }, false}},
      {68, {[](World& w, int l) { return w.DoPlat(l, Plat::RaiseToNearestAndChange, 0); }, true}},
      {14, {[](World& w, int l) { return w.DoPlat(l, Plat::RaiseAndChange, 32); }, false}},
      {15, {[](World& w, int l) { return w.DoPlat(l, Plat::RaiseAndChange, 24); }, false}},
      {66, {[](World& w, int l) { return w.DoPlat(l, Plat::RaiseAndChange, 24); }, true}},
      {67, {[](World& w, int l) { return w.DoPlat(l, Plat::RaiseAndChange, 32); }, true}},
      {18, {[](World& w, int l) { return w.DoFloor(l, Floor::RaiseToNearest); }, false}},
      {23, {[](World& w, int l) { return w.DoFloor(l, Floor::LowerToLowest); }, false}},
      {55, {[](World& w, int l) { return w.DoFloor(l, Floor::RaiseCrush); }, false}},
      {71, {[](World& w, int l) { return w.DoFloor(l, Floor::TurboLower); }, false}},
      {101, {[](World& w, int l) { return w.DoFloor(l, Floor::Raise); }, false}},
      {102, {[](World& w, int l) { return w.DoFloor(l, Floor::Lower); }, false}},
      {131, {[](World& w, int l) { return w.DoFloor(l, Floor::RaiseTurbo); }, false}},
      {45, {[](World& w, int l) { return w.DoFloor(l, Floor::Lower); }, true}},
      {60, {[](World& w, int l) { return w.DoFloor(l, Floor::LowerToLowest); }, true}},
      {64, {[](World& w, int l) { return w.DoFloor(l, Floor::Raise); }, true}},
      {65, {[](World& w, int l) { return w.DoFloor(l, Floor::RaiseCrush); }, true}},
      {69, {[](World& w, int l) { return w.DoFloor(l, Floor::RaiseToNearest); }, true}},
      {70, {[](World& w, int l) { return w.DoFloor(l, Floor::TurboLower); }, true}},
      {132, {[](World& w, int l) { return w.DoFloor(l, Floor::RaiseTurbo); }, true}}};
    auto it = kSwitches.find(sp);
    if (it == kSwitches.end()) {
      NotDone(sp);
      return false;
    }
    if (!it->second.first(*this, i)) return false;  // nothing to start (it is moving already)
    SwapSwitch(i);
    if (it->second.second)
      fButtons.push_back({i, kTicsPerSecond});  // BUTTONTIME
    else
      l.special = 0;
    return true;
  }

  void CrossSpecial(int i, int oldSide, G4bool player)
  {
    Line& l = fLines[i];
    const int sp = l.special;
    if (!player && sp != 4 && sp != 10 && sp != 88) return;  // monsters: these, and teleports (not done)
    // W1 (once): the line's special is cleared; WR (repeatable) stays
    static const std::map<int, std::pair<std::function<void(World&, int)>, G4bool>> kWalk = {
      {2, {[](World& w, int l) { w.DoDoor(l, Door::Open); }, false}},
      {3, {[](World& w, int l) { w.DoDoor(l, Door::Close); }, false}},
      {4, {[](World& w, int l) { w.DoDoor(l, Door::Normal); }, false}},
      {16, {[](World& w, int l) { w.DoDoor(l, Door::Close30ThenOpen); }, false}},
      {108, {[](World& w, int l) { w.DoDoor(l, Door::BlazeRaise); }, false}},
      {109, {[](World& w, int l) { w.DoDoor(l, Door::BlazeOpen); }, false}},
      {110, {[](World& w, int l) { w.DoDoor(l, Door::BlazeClose); }, false}},
      {75, {[](World& w, int l) { w.DoDoor(l, Door::Close); }, true}},
      {76, {[](World& w, int l) { w.DoDoor(l, Door::Close30ThenOpen); }, true}},
      {86, {[](World& w, int l) { w.DoDoor(l, Door::Open); }, true}},
      {90, {[](World& w, int l) { w.DoDoor(l, Door::Normal); }, true}},
      {105, {[](World& w, int l) { w.DoDoor(l, Door::BlazeRaise); }, true}},
      {106, {[](World& w, int l) { w.DoDoor(l, Door::BlazeOpen); }, true}},
      {107, {[](World& w, int l) { w.DoDoor(l, Door::BlazeClose); }, true}},
      {10, {[](World& w, int l) { w.DoPlat(l, Plat::DownWaitUpStay, 0); }, false}},
      {121, {[](World& w, int l) { w.DoPlat(l, Plat::BlazeDWUS, 0); }, false}},
      {88, {[](World& w, int l) { w.DoPlat(l, Plat::DownWaitUpStay, 0); }, true}},
      {120, {[](World& w, int l) { w.DoPlat(l, Plat::BlazeDWUS, 0); }, true}},
      {22, {[](World& w, int l) { w.DoPlat(l, Plat::RaiseToNearestAndChange, 0); }, false}},
      {95, {[](World& w, int l) { w.DoPlat(l, Plat::RaiseToNearestAndChange, 0); }, true}},
      {5, {[](World& w, int l) { w.DoFloor(l, Floor::Raise); }, false}},
      {19, {[](World& w, int l) { w.DoFloor(l, Floor::Lower); }, false}},
      {36, {[](World& w, int l) { w.DoFloor(l, Floor::TurboLower); }, false}},
      {38, {[](World& w, int l) { w.DoFloor(l, Floor::LowerToLowest); }, false}},
      {56, {[](World& w, int l) { w.DoFloor(l, Floor::RaiseCrush); }, false}},
      {58, {[](World& w, int l) { w.DoFloor(l, Floor::Raise24); }, false}},
      {59, {[](World& w, int l) { w.DoFloor(l, Floor::Raise24AndChange); }, false}},
      {119, {[](World& w, int l) { w.DoFloor(l, Floor::RaiseToNearest); }, false}},
      {130, {[](World& w, int l) { w.DoFloor(l, Floor::RaiseTurbo); }, false}},
      {82, {[](World& w, int l) { w.DoFloor(l, Floor::LowerToLowest); }, true}},
      {83, {[](World& w, int l) { w.DoFloor(l, Floor::Lower); }, true}},
      {91, {[](World& w, int l) { w.DoFloor(l, Floor::Raise); }, true}},
      {92, {[](World& w, int l) { w.DoFloor(l, Floor::Raise24); }, true}},
      {93, {[](World& w, int l) { w.DoFloor(l, Floor::Raise24AndChange); }, true}},
      {94, {[](World& w, int l) { w.DoFloor(l, Floor::RaiseCrush); }, true}},
      {98, {[](World& w, int l) { w.DoFloor(l, Floor::TurboLower); }, true}},
      {128, {[](World& w, int l) { w.DoFloor(l, Floor::RaiseToNearest); }, true}},
      {129, {[](World& w, int l) { w.DoFloor(l, Floor::RaiseTurbo); }, true}}};
    if (sp == 39 || sp == 97) {  // teleports, from the front only (EV_Teleport)
      if (oldSide == 0 && Teleport(i) && sp == 39) l.special = 0;
      return;
    }
    if (sp == 52 || sp == 124) {
      l.special = 0;
      if (fHooks.exit) fHooks.exit(sp == 124);
      return;
    }
    auto it = kWalk.find(sp);
    if (it == kWalk.end()) return;  // not a walk-over line (a door, a switch, a scrolling wall)
    it->second.first(*this, i);
    if (!it->second.second) l.special = 0;
  }

  // EV_VerticalDoor: a door used directly; it is the sector behind the line
  G4bool VerticalDoor(int i, G4bool player)
  {
    Line& l = fLines[i];
    const int sp = l.special;
    const int need = sp == 26 || sp == 32 ? 0 : sp == 27 || sp == 34 ? 1 : sp == 28 || sp == 33 ? 2 : -1;
    if (need >= 0) {
      if (!player) return false;
      if (!(fKeys & (1 << need | 8 << need))) {
        static const char* const colour[] = {"blue", "yellow", "red"};
        Message(std::string("You need a ") + colour[need] + " key to open this door");
        Sound("dsoof", (l.x1 + l.x2) / 2, (l.y1 + l.y2) / 2);
        return false;
      }
    }
    if (l.back < 0) return false;
    const int s = fSides[l.back].sector;
    Sector& sec = fSectors[s];
    if (sec.mover >= 0) {  // already moving: DR doors reverse
      Mover& m = fMovers[sec.mover];
      if (m.kind != Kind::Door) return false;
      if (sp == 1 || sp == 26 || sp == 27 || sp == 28 || sp == 117) {
        if (m.direction == -1)
          m.direction = 1;  // closing: go back up
        else {
          if (!player) return false;  // monsters never close doors
          m.direction = -1;
        }
        return true;
      }
      return false;
    }
    const G4bool blaze = sp == 117 || sp == 118;
    Sound(blaze ? "dsbdopn" : "dsdoropn", s);
    Mover m;
    m.kind = Kind::Door;
    m.sector = s;
    m.direction = 1;
    m.speed = blaze ? 8 : 2;
    m.door = sp == 117 ? Door::BlazeRaise : sp == 118 ? Door::BlazeOpen
             : (sp >= 31 && sp <= 34) ? Door::Open : Door::Normal;
    if (sp >= 31 && sp <= 34) l.special = 0;
    if (sp == 118) l.special = 0;
    m.topHeight = LowestCeilingAround(s) - 4;
    Start(m);
    return true;
  }

  // EV_DoDoor: the doors of every sector tagged like the line
  G4bool DoDoor(int i, Door type)
  {
    G4bool any = false;
    for (std::size_t s = 0; s < fSectors.size(); ++s) {
      Sector& sec = fSectors[s];
      if (sec.tag != fLines[i].tag || sec.mover >= 0) continue;
      any = true;
      Mover m;
      m.kind = Kind::Door;
      m.sector = int(s);
      m.door = type;
      m.speed = 2;
      switch (type) {
        case Door::BlazeClose:
        case Door::Close:
          m.topHeight = LowestCeilingAround(int(s)) - 4;
          m.direction = -1;
          m.speed = type == Door::BlazeClose ? 8 : 2;
          Sound(type == Door::BlazeClose ? "dsbdcls" : "dsdorcls", int(s));
          break;
        case Door::Close30ThenOpen:
          m.topHeight = sec.ceil;
          m.direction = -1;
          Sound("dsdorcls", int(s));
          break;
        default:  // raise, open (blazing or not)
          m.direction = 1;
          m.topHeight = LowestCeilingAround(int(s)) - 4;
          m.speed = type == Door::BlazeRaise || type == Door::BlazeOpen ? 8 : 2;
          if (m.topHeight != sec.ceil) Sound(m.speed > 2 ? "dsbdopn" : "dsdoropn", int(s));
      }
      Start(m);
    }
    return any;
  }

  // EV_DoPlat
  G4bool DoPlat(int i, Plat type, int amount)
  {
    G4bool any = false;
    const Line& l = fLines[i];
    for (std::size_t s = 0; s < fSectors.size(); ++s) {
      Sector& sec = fSectors[s];
      if (sec.tag != l.tag || sec.mover >= 0) continue;
      any = true;
      Mover m;
      m.kind = Kind::Plat;
      m.sector = int(s);
      m.plat = type;
      switch (type) {
        case Plat::RaiseToNearestAndChange:
        case Plat::RaiseAndChange:
          m.speed = 0.5;
          if (l.front >= 0) sec.floorTex = fSectors[fSides[l.front].sector].floorTex;
          m.high = type == Plat::RaiseAndChange ? sec.floor + amount : NextHighestFloor(int(s), sec.floor);
          m.wait = 0;
          m.direction = 1;
          if (type == Plat::RaiseToNearestAndChange) sec.special = 0;
          ++fVersion;
          Sound("dsstnmov", int(s));
          break;
        default:  // down, wait, up, stay
          m.speed = type == Plat::BlazeDWUS ? 8 : 4;
          m.low = std::min(LowestFloorAround(int(s)), sec.floor);
          m.high = sec.floor;
          m.wait = 3 * kTicsPerSecond;
          m.direction = -1;
          Sound("dspstart", int(s));
      }
      Start(m);
    }
    return any;
  }

  // EV_DoFloor
  G4bool DoFloor(int i, Floor type)
  {
    G4bool any = false;
    const Line& l = fLines[i];
    for (std::size_t s = 0; s < fSectors.size(); ++s) {
      Sector& sec = fSectors[s];
      if (sec.tag != l.tag || sec.mover >= 0) continue;
      any = true;
      Mover m;
      m.kind = Kind::Floor;
      m.sector = int(s);
      m.floor = type;
      m.speed = 1;
      switch (type) {
        case Floor::Lower:
          m.direction = -1;
          m.dest = HighestFloorAround(int(s));
          break;
        case Floor::LowerToLowest:
          m.direction = -1;
          m.dest = LowestFloorAround(int(s));
          break;
        case Floor::TurboLower:
          m.direction = -1;
          m.speed = 4;
          m.dest = HighestFloorAround(int(s));
          if (m.dest != sec.floor) m.dest += 8;
          break;
        case Floor::Raise:
        case Floor::RaiseCrush:
          m.direction = 1;
          m.dest = std::min(LowestCeilingAround(int(s)), sec.ceil) - (type == Floor::RaiseCrush ? 8 : 0);
          break;
        case Floor::RaiseTurbo:
          m.speed = 4;
          [[fallthrough]];
        case Floor::RaiseToNearest:
          m.direction = 1;
          m.dest = NextHighestFloor(int(s), sec.floor);
          break;
        case Floor::Raise24AndChange:
          if (l.front >= 0) {
            sec.floorTex = fSectors[fSides[l.front].sector].floorTex;
            sec.special = fSectors[fSides[l.front].sector].special;
            ++fVersion;
          }
          [[fallthrough]];
        case Floor::Raise24:
          m.direction = 1;
          m.dest = sec.floor + 24;
          break;
      }
      Start(m);
    }
    return any;
  }

  void Start(const Mover& m)
  {
    fMovers.push_back(m);
    fSectors[m.sector].mover = int(fMovers.size() - 1);
  }

  void Stop(Mover& m)
  {
    m.active = false;
    fSectors[m.sector].mover = -1;
  }

  // T_MovePlane: move a floor or ceiling by speed towards dest; anything in the way of a
  // floor going up or a ceiling coming down stops it (Doom's "crushed", without the crushing)
  Result MovePlane(int s, double speed, double dest, G4bool ceiling, int direction)
  {
    Sector& sec = fSectors[s];
    double& h = ceiling ? sec.ceil : sec.floor;
    const double old = h;
    G4bool past = false;
    double next = h + direction * speed;
    if ((direction < 0 && next <= dest) || (direction > 0 && next >= dest)) {
      next = dest;
      past = true;
    }
    if (!ceiling && direction > 0 && next >= sec.ceil) {  // a floor stops at the ceiling
      next = sec.ceil;
      past = true;
    }
    h = next;
    const G4bool shrinks = ceiling ? next < old : next > old;
    if (shrinks && fHooks.blocked && fHooks.blocked(s, sec.floor, sec.ceil)) {
      h = old;
      return Result::Crushed;
    }
    if (h != old) sec.moved = true;
    return past ? Result::PastDest : Result::Ok;
  }

  void Think(int index)
  {
    Mover& m = fMovers[index];
    Sector& sec = fSectors[m.sector];
    switch (m.kind) {
      case Kind::Door:  // T_VerticalDoor
        if (m.direction == 0) {
          if (--m.countdown > 0) break;
          if (m.door == Door::BlazeRaise) {
            m.direction = -1;
            Sound("dsbdcls", m.sector);
          }
          else if (m.door == Door::Normal) {
            m.direction = -1;
            Sound("dsdorcls", m.sector);
          }
          else if (m.door == Door::Close30ThenOpen) {
            m.direction = 1;
            Sound("dsdoropn", m.sector);
          }
        }
        else if (m.direction == -1) {
          const Result r = MovePlane(m.sector, m.speed, sec.floor, true, -1);
          if (r == Result::PastDest) {
            if (m.door == Door::BlazeRaise || m.door == Door::BlazeClose) Sound("dsbdcls", m.sector);
            if (m.door == Door::Close30ThenOpen) {
              m.direction = 0;
              m.countdown = 30 * kTicsPerSecond;
            }
            else
              Stop(m);
          }
          else if (r == Result::Crushed && m.door != Door::BlazeClose && m.door != Door::Close) {
            m.direction = 1;  // someone in the doorway: back up
            Sound("dsdoropn", m.sector);
          }
        }
        else {
          const Result r = MovePlane(m.sector, m.speed, m.topHeight, true, 1);
          if (r == Result::PastDest) {
            if (m.door == Door::BlazeRaise || m.door == Door::Normal) {
              m.direction = 0;
              m.countdown = m.topWait;
            }
            else
              Stop(m);
          }
        }
        break;
      case Kind::Plat:  // T_PlatRaise
        if (m.direction == 1) {
          const Result r = MovePlane(m.sector, m.speed, m.high, false, 1);
          if ((m.plat == Plat::RaiseToNearestAndChange || m.plat == Plat::RaiseAndChange) && fLevelTime % 8 == 0)
            Sound("dsstnmov", m.sector);
          if (r == Result::Crushed) {
            m.countdown = m.wait;
            m.direction = -1;
            Sound("dspstart", m.sector);
          }
          else if (r == Result::PastDest) {
            Sound("dspstop", m.sector);
            Stop(m);  // all of ours stay up
          }
        }
        else if (m.direction == -1) {
          if (MovePlane(m.sector, m.speed, m.low, false, -1) == Result::PastDest) {
            m.countdown = m.wait;
            m.direction = 0;
            Sound("dspstop", m.sector);
          }
        }
        else if (--m.countdown <= 0) {
          m.direction = sec.floor <= m.low ? 1 : -1;
          Sound("dspstart", m.sector);
        }
        break;
      case Kind::Floor: {  // T_MoveFloor
        const Result r = MovePlane(m.sector, m.speed, m.dest, false, m.direction);
        if (fLevelTime % 8 == 0) Sound("dsstnmov", m.sector);
        if (r == Result::PastDest) {
          Sound("dspstop", m.sector);
          Stop(m);
        }
        break;
      }
    }
  }

  // P_ChangeSwitchTexture: the first switch texture on the line's front side flips
  void SwapSwitch(int i, const char* sound = "dsswtchn")
  {
    const Line& l = fLines[i];
    if (l.front < 0) return;
    Side& s = fSides[l.front];
    for (int* tex : {&s.upper, &s.mid, &s.lower})
      if (*tex >= 0 && fTextures[*tex].partner >= 0) {
        *tex = fTextures[*tex].partner;
        ++fVersion;
        Sound(sound, (l.x1 + l.x2) / 2, (l.y1 + l.y2) / 2);
        return;
      }
  }

  // EV_Teleport: to the teleport destination (thing 14) in the tagged sector
  G4bool Teleport(int i)
  {
    for (const Thing& t : fThings) {
      if (t.type != 14 || t.sector < 0 || fSectors[t.sector].tag != fLines[i].tag) continue;
      if (fHooks.teleport) fHooks.teleport(t.x, t.y, fSectors[t.sector].floor, t.angle);
      return true;
    }
    return false;
  }

  void NotDone(int special)
  {
    if (fNotDone.insert(special).second)
      Message("(line special " + std::to_string(special) + " is not done yet)");
  }

  void Message(const std::string& text)
  {
    if (fHooks.message) fHooks.message(text);
  }
  void Sound(const std::string& lump, double x, double y)
  {
    if (fHooks.sound) fHooks.sound(lump, x, y, std::nan(""));
  }
  void Sound(const std::string& lump, int sector)  // from the middle of the sector
  {
    const Sector& s = fSectors[sector];
    if (fHooks.sound) fHooks.sound(lump, (s.minX + s.maxX) / 2, (s.minY + s.maxY) / 2, (s.floor + s.ceil) / 2);
  }

  double fOx = 0, fOy = 0, fZc = 0, fScale = 32;
  std::vector<TextureInfo> fTextures;
  std::string fSkyFile;
  std::vector<Sector> fSectors;
  std::vector<Side> fSides;
  std::vector<Line> fLines;
  std::vector<Flat> fFlats;
  std::vector<std::vector<int>> fSectorFlats;
  std::vector<Thing> fThings;
  std::vector<Mover> fMovers;
  std::vector<Button> fButtons;
  std::vector<std::pair<G4VPhysicalVolume*, double>> fSlabs;  // every slab and its height offset (mm)
  std::map<G4VPhysicalVolume*, std::size_t> fSlabIndex;
  std::map<const G4LogicalVolume*, int> fSectorOfVolume;
  std::set<int> fNotDone;
  Hooks fHooks;
  int fLevelTime = 0, fKeys = 0, fSecretsFound = 0;
  unsigned fVersion = 1;
  G4Mutex fMutex = G4MUTEX_INITIALIZER;
};
}  // namespace doom

#endif
