// SPDX-License-Identifier: GPL-2.0-or-later
// doomviewer - load a doom2gdml level into Geant4, colour it, and shoot real particles through it.
//
//   doomviewer e1m1.gdml              interactive (Qt), runs vis.mac
//   doomviewer e1m1.gdml batch.mac    batch
//
// Colours come from the GDML <auxiliary auxtype="Color" auxvalue="r g b a"/> tags.
// The particle gun fires Doom's weapons as real particles (kWeapons); damage is energy deposit.
//
// Gameplay commands (master thread only):
//   /doom/fire [n]            shoot n times with the current weapon from the camera, along the view
//   /doom/weapon 4            take a weapon (Doom's slots: 2 electron gun, 3 pion scattergun,
//                             4 muon chaingun, 5 carbon-ion cannon, 6 proton rifle, 7 antiproton BFG)
//   /doom/idkfa               every weapon, full ammo, all keys
//   /doom/gunDrop 300 mm      how far below the eye the muzzle sits
//   /doom/energyPerHP 20 MeV  energy deposit that counts as one hit point of damage
//   /doom/status              health of every monster
//   /doom/reset               restart the level (as after a death)
//   /doom/monsters true       let the monsters move (Qt session: ten AI steps a second)
//   /doom/think 0.1 s         one monster AI step (for macros)
//   /doom/walk 400 mm         move the camera forward (negative: back), Doom style
//   /doom/strafe 400 mm       move sideways (positive: right)
//   /doom/turn 5 deg          turn left (negative: right)
//   /doom/move 384 0 5 0      all at once: forward, right (mm), turn left, look up (deg)
//   /doom/where               print the eye position
//   /doom/mouseSensitivity 0.15  mouse look, degrees per mouse count (pixel)
//   /doom/god true            the monsters' attacks no longer hurt
//   /doom/crosshair false     hide the HUD crosshair
//   /doom/face physicist      whose face is in the status bar (default doomguy; see physicist_face.py)
//   /doom/hudStyle doom       Doom's own status bar (lhc: an accelerator magnet, see lhc_hud.py)
//   /doom/weaponStyle doom    Doom's weapon sprites (instruments: see weapon_sprites.py)
//   /doom/music off           the music (on, off, or a song: D_E1M2, D_RUNNIN, ...), OPL3 FM synth
//   /doom/musicVolume 0.5     music volume, 0 to 1
//   /doom/use                 open the door or press the switch in front of you
//   /doom/world 1 s           run the doors, lifts and floors (Qt session: every tic)
// Monsters (volumes named T<index>_<type>) are sensitive detectors: the energy deposited in
// them per event is damage. Dead monsters fall: the body becomes a flat dark red heap on the
// floor, low enough to step over and shoot past, as in Doom.
// Monsters wake when they see the player, hear a shot within 25 m or get hurt, then chase the
// player at Doom speeds under the same step/ledge/wall rules, by moving their placements.
// They fight back with Doom's attacks: zombies and shotgun guys shoot (hitscan, with Doom's
// spread), demons bite, imps claw up close and throw fireballs (drawn, and dodgeable) from
// afar. The player has 100 health; at 0 the view drops to the floor, and fire, use or
// /doom/reset restart the level. The Qt session shows Doom's status bar (graphics from the WAD, see DoomHud).
// Walking keeps the eye at Doom's view height above the floor, climbs steps up to 24 units
// and slides along walls and monsters. In the Qt session the keys are bound as
//   W/S or Up/Down: walk   A/D: strafe   Q/E or Left/Right: turn   Space (held): fire
//   2-7: weapons   F (or the right button while captured): use
// whenever no text field has the keyboard focus. Held keys combine (e.g. W+D+Q).
// The level's doors, lifts, moving floors, switches, walk-over lines, keys, teleporters and
// exits work as in Doom (doomworld.hh, from <map>_level.txt): they move the sector slabs.
// Mouse look: the mouse is captured from the start (hidden cursor, free look, left button fires;
// Esc or M releases it, M captures it again); dragging with the left button looks too: on native Wayland through the pointer
// constraints / relative pointer protocols, on X11 by warping the pointer to the centre.

#include "G4AutoLock.hh"
#include "G4Ellipsoid.hh"
#include "G4Event.hh"
#include "G4GDMLParser.hh"
#include "G4GeometryManager.hh"
#include "G4LogicalVolume.hh"
#include "G4LogicalVolumeStore.hh"
#include "G4Navigator.hh"
#include "G4ParticleGun.hh"
#include "G4ParticleTable.hh"
#include "G4PhysListFactory.hh"
#include "G4PVReplica.hh"
#include "G4Polyhedron.hh"
#include "G4PolyhedronArbitrary.hh"
#include "G4Region.hh"
#include "G4RunManager.hh"
#include "G4RunManagerFactory.hh"
#include "G4SDManager.hh"
#include "G4Scene.hh"
#include "G4StateManager.hh"
#include "G4Step.hh"
#include "G4SystemOfUnits.hh"
#include "G4TransportationManager.hh"
#include "G4UIcmdWithABool.hh"
#include "G4UIcmdWithAString.hh"
#include "G4UIcmdWithADouble.hh"
#include "G4UIcmdWithADoubleAndUnit.hh"
#include "G4UIcmdWithAnInteger.hh"
#include "G4UIcmdWithoutParameter.hh"
#include "G4UIdirectory.hh"
#include "G4UIExecutive.hh"
#include "G4UImanager.hh"
#include "G4UImessenger.hh"
#include "G4UnitsTable.hh"
#include "G4UserRunAction.hh"
#include "G4VPhysicalVolume.hh"
#include "G4VSensitiveDetector.hh"
#include "G4VUserActionInitialization.hh"
#include "G4VUserDetectorConstruction.hh"
#include "G4VUserPrimaryGeneratorAction.hh"
#include "G4VUserVisAction.hh"
#include "G4VVisManager.hh"
#include "G4VViewer.hh"
#include "G4VisAttributes.hh"
#include "G4VisExecutive.hh"
#include "G4WorkerThread.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include "doomworld.hh"

#ifdef DOOM_QT
#include "G4UIQt.hh"

#include <QApplication>
#include <QMainWindow>
#include <QCursor>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QPointer>
#include <QWidget>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <GL/gl.h>
#endif

#ifdef DOOM_SOUND
#include <pulse/error.h>
#include <pulse/simple.h>
#include "doommusic.hh"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#endif

#ifdef DOOM_WAYLAND
#include <qpa/qplatformnativeinterface.h>
#include <cstring>
#include <memory>
#include "pointer-constraints-client.h"
#include "relative-pointer-client.h"
#endif

// Doom player dimensions, in map units of 32 mm (see doom2gdml.py)
const G4double kMapUnit = 32. * mm;
const G4double kEyeHeight = 41 * kMapUnit;
// The view's near plane: nothing comes closer than this (walls stay a player radius, 16 units, away)
const G4double kNearPlane = 4 * kMapUnit;
const G4double kPlayerHeight = 56 * kMapUnit;
const G4double kPlayerRadius = 16 * kMapUnit;
const G4double kMaxStepUp = 24 * kMapUnit;
const G4double kMaxPitch = 85. * deg;  // short of straight up/down, where the view degenerates
const G4double kCorpseHeight = 8 * kMapUnit;  // a step anyone can climb over
const G4int kPlayerHealth = 100;
const G4double kDeadEyeHeight = 6 * kMapUnit;  // Doom lowers a dead player's view to the floor

// Doom's game clock and attack geometry (p_enemy.c, p_map.c)
const G4double kTic = 1. / 35 * s;

// Wall-clock seconds, for the sprite animations
G4double Now()
{
  static const auto start = std::chrono::steady_clock::now();
  return std::chrono::duration<G4double>(std::chrono::steady_clock::now() - start).count();
}
const G4double kMeleeRange = 60 * kMapUnit;  // MELEERANGE - 20 + the player's radius, centre to centre
const G4double kBulletSpread = 22.4 * deg;  // (P_Random() - P_Random()) << 20 at most, triangular
const G4double kMissileRadius = 6 * kMapUnit;
const G4double kMissileHeight = 32 * kMapUnit;  // launched this far above the thrower's feet

// Doom thing types (names and shapes as written by doom2gdml.py). Lengths in map units:
// body radius, solid centre above the floor, top above the floor; speed in map units/s.
struct MonsterType
{
  G4double health, radius, centre, top, speed;
};
const std::map<G4String, MonsterType> kMonsterTypes = {
  {"zombieman", {20, 20, 28, 56, 70}},   {"shotgunguy", {30, 20, 28, 56, 93}},
  {"chaingunner", {70, 20, 28, 56, 93}}, {"imp", {60, 20, 28, 56, 93}},
  {"demon", {150, 30, 28, 56, 175}},     {"spectre", {150, 30, 28, 56, 175}},
  {"lostsoul", {100, 16, 32, 48, 93}},   {"cacodemon", {400, 31, 63, 94, 93}},
  {"baron", {1000, 24, 32, 64, 93}},     {"barrel", {20, 10, 21, 42, 0}}};

// Doom's sounds per thing type (info.c): sight, pain, death; one picked at random from each list
struct MonsterSounds
{
  std::vector<G4String> sight, pain, death;
};
const std::map<G4String, MonsterSounds> kMonsterSounds = {
  {"zombieman", {{"dsposit1", "dsposit2", "dsposit3"}, {"dspopain"}, {"dspodth1", "dspodth2", "dspodth3"}}},
  {"shotgunguy", {{"dsposit2"}, {"dspopain"}, {"dspodth2"}}},
  {"chaingunner", {{"dsposit2"}, {"dspopain"}, {"dspodth2"}}},
  {"imp", {{"dsbgsit1", "dsbgsit2"}, {"dspopain"}, {"dsbgdth1", "dsbgdth2"}}},
  {"demon", {{"dssgtsit"}, {"dsdmpain"}, {"dssgtdth"}}},
  {"spectre", {{"dssgtsit"}, {"dsdmpain"}, {"dssgtdth"}}},
  {"lostsoul", {{}, {"dsdmpain"}, {"dsfirxpl"}}},
  {"cacodemon", {{"dscacsit"}, {"dsdmpain"}, {"dscacdth"}}},
  {"baron", {{"dsbrssit"}, {"dsdmpain"}, {"dsbrsdth"}}},
  {"barrel", {{}, {}, {"dsbarexp"}}}};

// Doom's monster attacks (info.c, p_enemy.c). Damage is mult * (1..sides) per bullet, bite
// or fireball. Times in tics: chase is one A_Chase step, when attacks are considered; the
// attack frames wind up to the strike, then recover.
enum RangedAttack { kNoRanged, kHitscan, kMissile };
struct MonsterAttack
{
  G4int meleeMult, meleeSides;
  G4String meleeSound;
  RangedAttack ranged;
  G4int pellets, mult, sides;
  G4double missileSpeed;  // map units per tic
  G4String rangedSound;
  G4Colour missileColour;
  G4int chase, windup, recover;
};
const std::map<G4String, MonsterAttack> kMonsterAttacks = {
  {"zombieman", {0, 0, "", kHitscan, 1, 3, 5, 0, "dspistol", {}, 4, 10, 16}},
  {"shotgunguy", {0, 0, "", kHitscan, 3, 3, 5, 0, "dsshotgn", {}, 3, 10, 20}},
  {"chaingunner", {0, 0, "", kHitscan, 2, 3, 5, 0, "dsshotgn", {}, 3, 10, 8}},  // a burst as one attack
  {"imp", {3, 8, "dsclaw", kMissile, 1, 3, 8, 10, "dsfirsht", {1, 0.45, 0.1}, 3, 16, 6}},
  {"demon", {4, 10, "dssgtatk", kNoRanged, 0, 0, 0, 0, "", {}, 2, 16, 8}},
  {"spectre", {4, 10, "dssgtatk", kNoRanged, 0, 0, 0, 0, "", {}, 2, 16, 8}},
  {"lostsoul", {3, 8, "dssklatk", kNoRanged, 0, 0, 0, 0, "", {}, 6, 10, 10}},  // its charge, as a bite
  {"cacodemon", {10, 6, "", kMissile, 1, 5, 8, 10, "dsfirsht", {0.9, 0.3, 0.9}, 3, 10, 5}},
  {"baron", {10, 8, "dsclaw", kMissile, 1, 8, 8, 15, "dsfirsht", {0.3, 1, 0.3}, 3, 16, 8}}};

// The arsenal: Doom's weapon slots and ammo, firing real particles. Fire rates (tics between
// shots), pellet counts and spread are Doom's; the damage is whatever the particles deposit.
enum AmmoType { kClip, kShell, kCell, kMisl, kAmmoTypes };  // Doom's order (am_clip, ...)
const std::array<G4int, kAmmoTypes> kMaxAmmo = {200, 50, 300, 50};
struct Weapon
{
  G4int slot;
  G4String name, particle;  // a particle name, or "ion Z A" for a nucleus
  G4double energy;
  G4int pellets;     // particles per shot
  G4double spread;   // at most this far left or right (triangular), as Doom's P_GunShot
  G4bool accurate;   // the first shot of a burst goes straight (Doom's pistol and chaingun)
  G4int refire;      // tics between shots while the trigger is held
  AmmoType ammo;
  G4int ammoUse;     // per shot
  G4int charge;      // tics from the trigger to the shot (the BFG's charge-up)
  G4String sound, sprite, flash;  // sprite: the weapon in view (WAD sprite names)
  char fireFrame;                 // the sprite frame shown with the muzzle flash
};
const std::vector<Weapon> kWeapons = {
  {2, "electron gun", "e-", 300 * MeV, 1, 5.6 * deg, true, 14, kClip, 1, 0, "dspistol", "PISG", "PISF", 'B'},
  {3, "pion scattergun", "pi+", 500 * MeV, 7, 5.6 * deg, false, 37, kShell, 1, 0, "dsshotgn", "SHTG", "SHTF", 'A'},
  {4, "muon chaingun", "mu-", 2 * GeV, 1, 5.6 * deg, true, 4, kClip, 1, 0, "dspistol", "CHGG", "CHGF", 'B'},
  // the rocket launcher: a carbon nucleus stops inside the target and dumps it all (Bragg peak)
  {5, "carbon-ion cannon", "ion 6 12", 3 * GeV, 1, 0, true, 20, kMisl, 1, 0, "dsrlaunc", "MISG", "MISF", 'B'},
  {6, "proton rifle", "proton", 1 * GeV, 1, 0, true, 3, kCell, 1, 0, "dsplasma", "PLSG", "PLSF", 'A'},
  // the BFG 9000: a spray of antiprotons, which annihilate into pions in whatever they hit
  {7, "antiproton BFG", "anti_proton", 300 * MeV, 40, 45 * deg, false, 60, kCell, 40, 30, "dsbfg", "BFGG", "BFGF", 'B'}};
// Doom's P_CheckAmmo never switches to these by itself
G4bool Dangerous(const Weapon& w) { return w.slot == 5 || w.slot == 7; }

// Doom's pickups (p_inter.c): weapons (a slot) and ammo, in clips of clipammo; Doom's own
// messages for the ammo, the weapons by their names here
const std::array<G4int, kAmmoTypes> kClipAmmo = {10, 4, 20, 1};
struct Pickup
{
  G4int slot;  // a weapon, 0 for ammo only
  AmmoType ammo;
  G4int clips;  // dropped by a monster: half (a weapon one clip, a clip half a clip)
  G4String message;
};
const std::map<G4int, Pickup> kPickups = {
  {2001, {3, kShell, 2, ""}}, {82, {3, kShell, 2, ""}}, {2002, {4, kClip, 2, ""}},
  {2003, {5, kMisl, 2, ""}},  {2004, {6, kCell, 2, ""}}, {2006, {7, kCell, 2, ""}},
  {2007, {0, kClip, 1, "Picked up a clip."}},           {2048, {0, kClip, 5, "Picked up a box of bullets."}},
  {2008, {0, kShell, 1, "Picked up 4 shotgun shells."}}, {2049, {0, kShell, 5, "Picked up a box of shotgun shells."}},
  {2010, {0, kMisl, 1, "Picked up a rocket."}},          {2046, {0, kMisl, 5, "Picked up a box of rockets."}},
  {2047, {0, kCell, 1, "Picked up an energy cell."}},    {17, {0, kCell, 5, "Picked up an energy cell pack."}}};
const G4int kBackpack = 8;
// Health and armour (P_TouchSpecialThing): health or armour points given, up to a limit;
// armour items also set the armour class. `always`: taken even when it adds nothing (bonuses).
struct Supply
{
  G4int health, healthMax, armor, armorMax, armorType;
  G4bool always;
  G4String message, sound;
};
const std::map<G4int, Supply> kSupplies = {
  {2014, {1, 200, 0, 0, 0, true, "Picked up a health bonus.", "dsitemup"}},
  {2011, {10, 100, 0, 0, 0, false, "Picked up a stimpack.", "dsitemup"}},
  {2012, {25, 100, 0, 0, 0, false, "Picked up a medikit.", "dsitemup"}},
  {2015, {0, 0, 1, 200, 1, true, "Picked up an armor bonus.", "dsitemup"}},
  {2018, {0, 0, 100, 100, 1, false, "Picked up the armor.", "dsitemup"}},
  {2019, {0, 0, 200, 200, 2, false, "Picked up the MegaArmor!", "dsitemup"}},
  {2013, {100, 200, 0, 0, 0, true, "Supercharge!", "dsgetpow"}},
  {83, {200, 200, 200, 200, 2, true, "MegaSphere!", "dsgetpow"}}};
// What a dying monster leaves behind (P_KillMobj)
const std::map<G4String, G4int> kDrops = {{"zombieman", 2007}, {"shotgunguy", 2001}, {"chaingunner", 2002}};

// The current shot's spread, set by the master before /run/beamOn and read by every worker's
// gun: each event (pellet) strays by its own random angle about the vertical
std::atomic<G4double> gSpread{0};
// The muzzle and the aim, set the same way. Not /gun/position: the workers run it before they
// take the new level's geometry from the master, and it checks the vertex against the old
// (deleted) world.
G4ThreeVector gMuzzle, gAim(0, 0, 1);

// A private navigator (sight tests, collisions) on the current world, which changes with the level
void FollowWorld(G4Navigator& nav)
{
  G4VPhysicalVolume* world = G4TransportationManager::GetTransportationManager()->GetNavigatorForTracking()->GetWorldVolume();
  if (nav.GetWorldVolume() != world) nav.SetWorldVolume(world);
}

// The level's moving parts (doors, lifts, floors, switches; doomworld.hh), if <map>_level.txt
// was found. Set once in main before any run.
using DoomWorld = doom::World;
DoomWorld* gWorld = nullptr;

// Every monster in the level: health (shared by all worker threads) and where it stands.
class Bestiary
{
 public:
  struct Monster
  {
    G4VPhysicalVolume* pv;
    G4LogicalVolume* lv;
    G4String type;
    const MonsterType* kind;
    G4double health;
    const G4VisAttributes* alive;
    G4bool painted;
    G4ThreeVector spawn;
    G4bool awake;
    G4VSolid* body;
    G4VSolid* corpse;  // made at the first death, kept for later ones
    G4ThreeVector at;  // where it is now. Placements and solids are per thread in MT: this is
                       // the master's truth, which Sync() copies to the workers.
    // fighting, as in Doom's A_Chase
    G4int reaction = 8;      // chase steps after waking before it may shoot
    G4int moveCount = 0;     // chase steps left in its current direction; shoots only at 0
    G4double nextCheck = 0;  // time to its next chase step
    G4double busy = 0;       // attacking: stands still this much longer
    G4double strikeIn = -1;  // the attack lands in this long; < 0: none under way
    G4bool justHit = false;  // hurt since its last attack: fires back at once
    // for the sprites: Doom's angle (anticlockwise from +x, seen from above), and when (Now())
    // it last stepped, started an attack, got hurt, died
    G4double facing = 0, spawnFacing = 0;
    G4double moved = -1e9, attackStart = -1e9, painUntil = -1e9, died = -1e9;
    G4bool hurt = false;  // since the last LayCorpses: the pain frame comes next
  };

  // What the sprites need to know about a monster
  struct View
  {
    G4String type;
    const MonsterType* kind;
    G4ThreeVector at;
    G4double facing, moved, attackStart, painUntil, died;
    G4bool alive, laid, attacking;
  };

  // One event's damage to one monster, recorded on a worker, voiced by the master after the run
  struct Hit
  {
    G4String type;
    G4double edep;
    G4bool killed, gibbed;
    G4ThreeVector at;
  };

  static Bestiary& Instance()
  {
    static Bestiary bestiary;
    return bestiary;
  }

  // Register pv if it is a monster placement ("T12_imp"). Call after it got its colour.
  void Add(G4VPhysicalVolume* pv)
  {
    G4LogicalVolume* lv = pv->GetLogicalVolume();
    const G4String& name = lv->GetName();
    auto sep = name.find('_');
    if (name.empty() || name[0] != 'T' || sep == std::string::npos) return;
    auto type = kMonsterTypes.find(name.substr(sep + 1));
    if (type == kMonsterTypes.end()) return;
    fMonsters[name] = {pv, lv, type->first, &type->second, type->second.health, lv->GetVisAttributes(),
                       false, pv->GetTranslation(), false, lv->GetSolid(), nullptr, pv->GetTranslation()};
  }

  // Its facing at the start (the THINGS angle, a GDML aux tag)
  void SetFacing(const G4String& name, G4double angle)
  {
    auto it = fMonsters.find(name);
    if (it != fMonsters.end()) it->second.facing = it->second.spawnFacing = angle;
  }

  // How the bodies (the sensitive volumes) are drawn: as solids; or, where sprites stand in for
  // them, hidden, or as wireframes (the "detector view"). sprited(type): sprites for that type.
  void SetLook(G4bool sprites, G4bool hitboxes, std::function<G4bool(const G4String&)> sprited)
  {
    G4AutoLock lock(&fMutex);
    fSprites = sprites;
    fHitboxes = hitboxes;
    fSprited = std::move(sprited);
    for (auto& [name, m] : fMonsters) m.lv->SetVisAttributes(Look(m));
  }
  G4bool SpritesOn() const { return fSprites; }
  G4bool Sprited(const G4String& type) const { return fSprites && fSprited && fSprited(type); }

  // Copy what the sprites need; false (out unchanged) if the lock is taken (see Kills)
  G4bool Snapshot(std::vector<View>& out)
  {
    std::unique_lock<G4Mutex> lock(fMutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;
    out.clear();
    for (const auto& [name, m] : fMonsters)
      out.push_back({m.type, m.kind, m.at, m.facing, m.moved, m.attackStart, m.painUntil, m.died, m.health > 0,
                     m.painted, m.busy > 0});
    return true;
  }

  // Move a monster (master thread, outside a run); the workers follow at the next Sync().
  static void Place(Monster& m, const G4ThreeVector& at)
  {
    m.at = at;
    m.pv->SetTranslation(at);
  }

  // Give this thread's copy of every monster placement and solid the master's state.
  // Each worker runs it at the start of a run, before any event is tracked.
  void Sync()
  {
    G4AutoLock lock(&fMutex);
    for (auto& [name, m] : fMonsters) {
      m.pv->SetTranslation(m.at);
      m.lv->SetSolid(m.painted ? m.corpse : m.body);
    }
  }

  // Call f(name, monster) for every monster. Master thread, outside a run (no worker touches
  // the positions then), but the lock keeps health reads consistent anyway.
  template <class F>
  void ForEach(F f)
  {
    G4AutoLock lock(&fMutex);
    for (auto& [name, m] : fMonsters) f(name, m);
  }

  std::vector<G4LogicalVolume*> Volumes() const
  {
    std::vector<G4LogicalVolume*> out;
    for (const auto& [name, m] : fMonsters) out.push_back(m.lv);
    return out;
  }

  void SetEnergyPerHP(G4double e) { fEnergyPerHP = e; }

  void Damage(const G4LogicalVolume* lv, G4double edep)
  {
    G4AutoLock lock(&fMutex);
    auto it = fMonsters.find(lv->GetName());
    if (it == fMonsters.end() || it->second.health <= 0) return;  // unknown, or already a corpse
    Harm(it->first, it->second, edep / fEnergyPerHP, edep);
  }

  // Damage in hit points without a particle (a barrel's blast). Call with the lock held: from
  // inside ForEach.
  void Harm(const G4String& name, Monster& m, G4double hp) { Harm(name, m, hp, hp * fEnergyPerHP); }

  // A new level: forget every monster (their volumes are about to be deleted)
  void Clear()
  {
    G4AutoLock lock(&fMutex);
    fMonsters.clear();
    fHits.clear();
    fDeaths.clear();
  }

 private:
  void Harm(const G4String& name, Monster& m, G4double hp, G4double edep)
  {
    if (m.health <= 0) return;
    m.health -= hp;
    m.awake = true;  // pain wakes anyone
    m.justHit = true;
    m.hurt = true;
    if (m.health > 0 && hp < 0.5) return;  // shower debris: counts, but not worth a line or a yelp
    fHits.push_back({m.type, edep, m.health <= 0, m.health <= -m.kind->health, m.at});  // gibbed: Doom's
                                                                                       // overkill rule
    std::ostringstream msg;  // own stream: worker G4cout precision may have been changed
    msg.precision(4);
    msg << name << " takes " << std::lround(hp) << " HP (" << G4BestUnit(edep, "Energy") << ")";
    if (m.health > 0)
      msg << ", " << std::ceil(m.health) << "/" << m.kind->health << " left";
    else
      msg << (m.type == "barrel" ? " and explodes!" : " and dies!");
    G4cout << msg.str() << G4endl;
  }

 public:
  // The hits recorded since the last call. Master thread, after a run.
  std::vector<Hit> TakeHits()
  {
    G4AutoLock lock(&fMutex);
    std::vector<Hit> hits;
    hits.swap(fHits);
    return hits;
  }

  // Lay down the monsters killed since the last call: the body becomes a flat heap on the
  // floor. The ones hurt but alive show their pain frame for a moment. Master thread, outside
  // a run; the caller re-closes the geometry and redraws if this returns true.
  G4bool LayCorpses()
  {
    G4AutoLock lock(&fMutex);
    G4bool any = false;
    for (auto& [name, m] : fMonsters) {
      if (m.health > 0) {
        if (m.hurt && fSprites) {
          m.painUntil = Now() + 6 * kTic / s;  // Doom's pain states: about 6 tics
          any = true;
        }
        m.hurt = false;
        continue;
      }
      if (m.painted) continue;
      if (!m.corpse) m.corpse = MakeCorpse(name, m);
      G4ThreeVector at = m.at;
      at.setY(at.y() - m.kind->centre * kMapUnit + kCorpseHeight / 2 + 0.5 * mm);  // just off the floor
      Place(m, at);
      m.lv->SetSolid(m.corpse);
      m.painted = any = true;
      m.died = Now();
      m.lv->SetVisAttributes(Look(m));
      fDeaths.push_back({m.type, at});
    }
    return any;
  }

  // The monsters laid down since the last call: type, and where (for what they drop)
  std::vector<std::pair<G4String, G4ThreeVector>> TakeDeaths()
  {
    G4AutoLock lock(&fMutex);
    std::vector<std::pair<G4String, G4ThreeVector>> deaths;
    deaths.swap(fDeaths);
    return deaths;
  }

  // Full health, back at the spawn spot, asleep. The caller re-closes the geometry.
  void Reset()
  {
    G4AutoLock lock(&fMutex);
    for (auto& [name, m] : fMonsters) {
      m.health = m.kind->health;
      m.painted = false;
      m.awake = false;
      m.reaction = 8;
      m.moveCount = 0;
      m.nextCheck = m.busy = 0;
      m.strikeIn = -1;
      m.justHit = m.hurt = false;
      m.facing = m.spawnFacing;
      m.moved = m.attackStart = m.painUntil = m.died = -1e9;
      m.lv->SetSolid(m.body);
      m.lv->SetVisAttributes(Look(m));
      Place(m, m.spawn);
    }
  }

  // Dead monsters and all monsters, barrels not counted (as Doom's kill tally); {-1, -1} if
  // the lock is taken. For the HUD, whose timer can fire while the GUI thread itself holds
  // the lock (a viewer refresh processes Qt events), so it must never wait for it.
  std::pair<G4int, G4int> Kills()
  {
    std::unique_lock<G4Mutex> lock(fMutex, std::try_to_lock);
    if (!lock.owns_lock()) return {-1, -1};
    G4int dead = 0, all = 0;
    for (const auto& [name, m] : fMonsters) {
      if (m.type == "barrel") continue;
      ++all;
      if (m.health <= 0) ++dead;
    }
    return {dead, all};
  }

  void Status()
  {
    G4AutoLock lock(&fMutex);
    G4int alive = 0;
    for (const auto& [name, m] : fMonsters) {
      G4cout << "  " << name << ": ";
      if (m.health > 0) {
        G4cout << std::ceil(m.health) << "/" << m.kind->health;
        if (m.awake) G4cout << " awake at " << G4BestUnit(m.at, "Length");
        G4cout << G4endl;
        ++alive;
      }
      else
        G4cout << "dead" << G4endl;
    }
    G4cout << alive << " of " << fMonsters.size() << " still standing" << G4endl;
  }

 private:
  Bestiary()
  {
    fCorpse.SetColour(G4Colour(0.4, 0.05, 0.05));
    fCorpse.SetForceSolid(true);
    fWire.SetColour(G4Colour(0.3, 1., 0.55, 0.7));  // a detector display's green
    fWire.SetForceWireframe(true);
  }

  const G4VisAttributes* Look(const Monster& m) const
  {
    if (!fSprites || !fSprited || !fSprited(m.type)) return m.painted ? &fCorpse : m.alive;
    return fHitboxes ? &fWire : &G4VisAttributes::GetInvisible();
  }

  // A flat ellipsoid with the body's footprint. It is placed with the body's rotation (tubes
  // are turned upright), so flatten whichever local axis points up in the level.
  static G4VSolid* MakeCorpse(const G4String& name, const Monster& m)
  {
    const G4ThreeVector up = m.pv->GetObjectRotationValue().inverse() * G4ThreeVector(0, 1, 0);
    const G4double r = m.kind->radius * kMapUnit, h = kCorpseHeight / 2;
    return new G4Ellipsoid(name + "_corpse", std::abs(up.x()) > 0.5 ? h : r, std::abs(up.y()) > 0.5 ? h : r,
                           std::abs(up.z()) > 0.5 ? h : r);
  }

  std::map<G4String, Monster> fMonsters;
  std::vector<Hit> fHits;
  std::vector<std::pair<G4String, G4ThreeVector>> fDeaths;
  G4double fEnergyPerHP = 20. * MeV;
  G4VisAttributes fCorpse, fWire;
  G4bool fSprites = false, fHitboxes = false;
  std::function<G4bool(const G4String&)> fSprited;
  G4Mutex fMutex;
};

// One per thread: sums the energy deposited in each monster during an event.
class MonsterSD : public G4VSensitiveDetector
{
 public:
  MonsterSD() : G4VSensitiveDetector("MonsterSD") {}

  void Initialize(G4HCofThisEvent*) override { fEdep.clear(); }

  G4bool ProcessHits(G4Step* step, G4TouchableHistory*) override
  {
    G4double edep = step->GetTotalEnergyDeposit();
    if (edep > 0) fEdep[step->GetPreStepPoint()->GetPhysicalVolume()->GetLogicalVolume()] += edep;
    return true;
  }

  void EndOfEvent(G4HCofThisEvent*) override
  {
    for (const auto& [lv, edep] : fEdep) Bestiary::Instance().Damage(lv, edep);
  }

 private:
  std::map<const G4LogicalVolume*, G4double> fEdep;
};

class DoomLevel : public G4VUserDetectorConstruction
{
 public:
  explicit DoomLevel(const G4String& file) : fFile(file) {}

  // The next Construct (after /run/reinitializeGeometry) reads this level
  void SetFile(const G4String& file) { fFile = file; }
  const G4String& File() const { return fFile; }

  G4VPhysicalVolume* Construct() override
  {
    fParser = std::make_unique<G4GDMLParser>();  // a fresh one per level: it keeps its names
    fParser->Read(fFile, false);  // false: no schema validation
    G4VPhysicalVolume* world = fParser->GetWorldVolume();
    world->GetLogicalVolume()->SetVisAttributes(G4VisAttributes::GetInvisible());

    for (const auto& [lv, auxList] : *fParser->GetAuxMap()) {
      for (const auto& aux : auxList) {
        if (aux.type != "Color") continue;
        std::istringstream in(aux.value);
        G4double r = 0.5, g = 0.5, b = 0.5, a = 1.;
        in >> r >> g >> b >> a;
        auto* va = new G4VisAttributes(G4Colour(r, g, b, a));
        va->SetForceSolid(true);
        lv->SetVisAttributes(va);
      }
    }
    G4LogicalVolume* worldLV = world->GetLogicalVolume();
    for (std::size_t i = 0; i < worldLV->GetNoDaughters(); ++i) Bestiary::Instance().Add(worldLV->GetDaughter(i));
    for (const auto& [lv, auxList] : *fParser->GetAuxMap())
      for (const auto& aux : auxList)
        if (aux.type == "Angle") Bestiary::Instance().SetFacing(lv->GetName(), std::stod(aux.value) * deg);
    return world;
  }

  void ConstructSDandField() override
  {
    // one per thread, kept from level to level
    auto* sd = G4SDManager::GetSDMpointer()->FindSensitiveDetector("MonsterSD", false);
    if (!sd) {
      sd = new MonsterSD;
      G4SDManager::GetSDMpointer()->AddNewDetector(sd);
    }
    for (auto* lv : Bestiary::Instance().Volumes()) SetSensitiveDetector(lv, sd);
  }

 private:
  G4String fFile;
  std::unique_ptr<G4GDMLParser> fParser;
};

// The weapon's particle gun. Particle and energy come through /gun/ commands (master to
// workers), position and direction from gMuzzle/gAim; the spread is added here, per event.
class PlasmaRifle : public G4VUserPrimaryGeneratorAction
{
 public:
  PlasmaRifle() : fGun(new G4ParticleGun(1))
  {
    fGun->SetParticleDefinition(G4ParticleTable::GetParticleTable()->FindParticle("proton"));
    fGun->SetParticleEnergy(10. * GeV);
  }
  ~PlasmaRifle() override { delete fGun; }

  void GeneratePrimaries(G4Event* event) override
  {
    const G4double spread = gSpread.load();
    fGun->SetParticlePosition(gMuzzle);
    G4ThreeVector aim = gAim;
    if (spread > 0) aim.rotateY((G4UniformRand() - G4UniformRand()) * spread);
    fGun->SetParticleMomentumDirection(aim);
    fGun->GeneratePrimaryVertex(event);
  }

 private:
  G4ParticleGun* fGun;
};

// Placements and solids are per thread in MT: each worker takes the monsters' current
// positions and bodies/corpses from the Bestiary, and the doors' and lifts' slabs from the
// World, before it tracks anything.
class WorkerSync : public G4UserRunAction
{
 public:
  void BeginOfRunAction(const G4Run*) override
  {
    Bestiary::Instance().Sync();
    if (gWorld) gWorld->Sync();
  }
};

// /doom/worker/geometry, sent (broadcast) by the master after it built a new level. A task
// worker sets up the new world at its next run without first copying the new volumes' per-thread
// data (placements, solids, sensitive detectors) from the master, and reads garbage; this command
// runs on each worker before that and makes the copy. A no-op on the master. One instance per
// thread: the master's makes the command known for broadcasting, the workers' run it.
class WorkerGeometry : public G4UImessenger
{
 public:
  WorkerGeometry()
  {
    fDir = new G4UIdirectory("/doom/worker/");
    fDir->SetGuidance("Commands the master sends to the worker threads.");
    fCmd = new G4UIcmdWithoutParameter("/doom/worker/geometry", this);
    fCmd->SetGuidance("Copy a new level's volumes from the master (workers only).");
    fCmd->SetToBeBroadcasted(true);
  }
  ~WorkerGeometry() override
  {
    delete fCmd;
    delete fDir;
  }
  void SetNewValue(G4UIcommand*, G4String) override
  {
    if (G4Threading::IsMasterThread()) return;
    // This thread's per-object arrays are still sized for the objects it knew (they grow in
    // chunks of 512), and Geant4's update reads them for the new ones and copies the master's
    // bigger arrays over them: start them afresh, as copies of the master's
    Recopy(G4LogicalVolume::GetSubInstanceManager());
    Recopy(G4VPhysicalVolume::GetSubInstanceManager());
    Recopy(G4PVReplica::GetSubInstanceManager());
    Recopy(G4Region::GetSubInstanceManager());
    G4WorkerThread::UpdateGeometryAndPhysicsVectorFromMaster();
  }

 private:
  template <class Splitter>
  static void Recopy(const Splitter& splitter)
  {
    auto& own = const_cast<Splitter&>(splitter);
    own.FreeSlave();
    own.SlaveCopySubInstanceArray();
  }

  G4UIdirectory* fDir;
  G4UIcmdWithoutParameter* fCmd;
};

class Actions : public G4VUserActionInitialization
{
 public:
  void Build() const override
  {
    SetUserAction(new PlasmaRifle);
    SetUserAction(new WorkerSync);
    static G4ThreadLocal WorkerGeometry* worker = nullptr;  // lives as long as the thread
    if (!worker) worker = new WorkerGeometry;
  }
};

// Plays a WAD sound: lump name, gain, pitch factor, pan (-1 left .. 1 right)
using SoundPlayer = std::function<void(const G4String&, G4double, G4double, G4double)>;
// Plays a song (a D_* lump name; empty: silence) at a volume (0..1)
using MusicPlayer = std::function<void(const G4String&, G4double)>;

// The level's song, as Doom picks it: D_E1M1 for E1M1, Doom II's names for MAP01..MAP32
G4String SongForMap(G4String map)
{
  for (auto& c : map) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  static const char* const kDoom2[] = {
    "RUNNIN", "STALKS", "COUNTD", "BETWEE", "DOOM",   "THE_DA", "SHAWN",  "DDTBLU", "IN_CIT", "DEAD",   "STLKS2",
    "THEDA2", "DOOM2",  "DDTBL2", "RUNNI2", "DEAD2",  "STLKS3", "ROMERO", "SHAWN2", "MESSAG", "COUNT2", "DDTBL3",
    "AMPIE",  "THEDA3", "ADRIAN", "MESSG2", "ROMER2", "TENSE",  "SHAWN3", "OPENIN", "EVIL",   "ULTIMA"};
  if (map.size() == 5 && map.rfind("MAP", 0) == 0 && std::isdigit(static_cast<unsigned char>(map[3]))
      && std::isdigit(static_cast<unsigned char>(map[4]))) {
    const int n = std::stoi(map.substr(3));
    if (n >= 1 && n <= 32) return G4String("D_") + kDoom2[n - 1];
  }
  return "D_" + map;
}

// A fireball in flight, moved by the monster AI step
struct Missile
{
  G4ThreeVector pos, velocity;     // mm, mm/s
  G4int damage;
  G4String by;                     // who threw it
  const G4VPhysicalVolume* owner;  // it starts inside its thrower
  G4Colour colour;
  std::string sprite;              // Doom's sprite (BAL1: an imp's), for the textured view
  G4double age = 0;
};

// A fireball bursting where it hit, for its explosion frames
struct Burst
{
  G4ThreeVector pos;
  std::string sprite;
  G4double time;  // Now() when it burst
};
// Doom's fireballs by thrower (info.c): imp BAL1, cacodemon BAL2, baron BAL7
const std::map<G4String, std::string> kMissileSprites = {{"imp", "BAL1"}, {"cacodemon", "BAL2"}, {"baron", "BAL7"}};
// Their states: two flying frames of 4 tics, then three bursting frames (C, D, E) of 6 tics
constexpr G4int kMissileFlyTics = 4, kMissileBurstTics = 6;

// Draws the missiles in flight as balls. A run-duration model, so it is redrawn at every
// /vis/viewer/rebuild; add it to the scene with /vis/scene/add/userAction. Not while `hidden`
// says so: the textured view draws them as Doom's sprites.
class MissileVis : public G4VUserVisAction
{
 public:
  MissileVis(const std::vector<Missile>& missiles, std::function<G4bool()> hidden)
    : fMissiles(missiles), fHidden(std::move(hidden))
  {}

  void Draw() override
  {
    G4VVisManager* vis = G4VVisManager::GetConcreteInstance();
    if (!vis || (fHidden && fHidden())) return;
    for (const auto& missile : fMissiles) {
      G4PolyhedronSphere ball(0, kMissileRadius, 0, CLHEP::twopi, 0, CLHEP::pi);
      ball.Transform(G4Translate3D(missile.pos));
      G4VisAttributes look(missile.colour);
      look.SetForceSolid(true);
      ball.SetVisAttributes(look);
      vis->Draw(ball);
    }
  }

 private:
  const std::vector<Missile>& fMissiles;
  std::function<G4bool()> fHidden;
};

#ifdef DOOM_QT
// Doom's monster sprites (sprites/ next to the GDML, written by doom2gdml.py), each picture cut
// into runs of one colour per row, ready to be drawn as flat quads
class SpriteSet
{
 public:
  struct Run
  {
    G4int row, x0, x1;  // pixels x0..x1 of this row
  };
  struct Patch
  {
    G4int width, left, top;  // left: the thing's centre column; top: rows above its feet
    std::vector<std::pair<G4Colour, std::vector<Run>>> colours;
  };

  explicit SpriteSet(const std::filesystem::path& dir)
  {
    std::ifstream in(dir / "offsets.txt");
    std::string name;
    G4int left, top;
    while (in >> name >> left >> top) {
      QImage img;
      if (name.size() < 6 || !img.load(QString::fromStdString((dir / (name + ".png")).string()))) continue;
      img = img.convertToFormat(QImage::Format_ARGB32);
      Patch patch{img.width(), left, top, {}};
      // runs grouped by colour, 4 bits a channel (similar shades merge), each group's mean colour
      std::map<G4int, std::pair<G4double[4], std::vector<Run>>> groups;
      for (G4int y = 0; y < img.height(); ++y) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (G4int x = 0; x < img.width();) {
          if (qAlpha(line[x]) < 128) { ++x; continue; }
          const auto key = [](QRgb c) { return (qRed(c) >> 4) << 8 | (qGreen(c) >> 4) << 4 | qBlue(c) >> 4; };
          const G4int k = key(line[x]);
          G4int x1 = x;
          auto& group = groups[k];
          while (x1 < img.width() && qAlpha(line[x1]) >= 128 && key(line[x1]) == k) {
            group.first[0] += qRed(line[x1]);
            group.first[1] += qGreen(line[x1]);
            group.first[2] += qBlue(line[x1]);
            group.first[3] += 1;
            ++x1;
          }
          group.second.push_back({y, x, x1 - 1});
          x = x1;
        }
      }
      for (auto& [k, group] : groups) {
        const G4double n = group.first[3] * 255;
        patch.colours.push_back({G4Colour(group.first[0] / n, group.first[1] / n, group.first[2] / n), std::move(group.second)});
      }
      fPatches.push_back(std::move(patch));
      const auto index = fPatches.size() - 1;
      fIndex[name.substr(0, 6)] = {index, false};                                    // TROOA2
      if (name.size() >= 8) fIndex[name.substr(0, 4) + name.substr(6, 2)] = {index, true};  // TROOA8: mirrored
      fPrefixes.insert(name.substr(0, 4));
    }
    G4cout << "Sprites: " << fPatches.size() << " pictures from " << dir.string() << G4endl;
  }

  G4bool Has(const G4String& prefix) const { return fPrefixes.count(prefix) > 0; }

  // A frame seen from rotation rot (1..8); falls back to the all-angles picture (rotation 0)
  const Patch* Find(const G4String& prefix, char frame, G4int rot, G4bool& flip, G4int& id) const
  {
    for (char r : {char('0' + rot), '0'}) {
      auto it = fIndex.find(prefix + frame + r);
      if (it != fIndex.end()) {
        flip = it->second.second;
        id = G4int(it->second.first);
        return &fPatches[it->second.first];
      }
    }
    return nullptr;
  }

 private:
  std::vector<Patch> fPatches;
  std::map<G4String, std::pair<std::size_t, G4bool>> fIndex;
  std::set<G4String> fPrefixes;
};

// Which frames each monster type shows (Doom's states in info.c)
struct SpriteFrames
{
  const char *prefix, *walk, *attack, *pain, *death;
  G4bool gone;  // after dying, nothing stays (a lost soul, an exploded barrel)
  const char* deathPrefix = nullptr;  // the death frames come from another sprite (the barrel's BEXP)
};
const std::map<G4String, SpriteFrames> kSpriteFrames = {
  {"zombieman", {"POSS", "ABCD", "EF", "G", "HIJKL", false}},
  {"shotgunguy", {"SPOS", "ABCD", "EF", "G", "HIJKL", false}},
  {"chaingunner", {"CPOS", "ABCD", "EF", "G", "HIJKLMN", false}},
  {"imp", {"TROO", "ABCD", "EFG", "H", "IJKLM", false}},
  {"demon", {"SARG", "ABCD", "EFG", "H", "IJKLMN", false}},
  {"spectre", {"SARG", "ABCD", "EFG", "H", "IJKLMN", false}},
  {"lostsoul", {"SKUL", "AB", "CD", "E", "FGHIJK", true}},
  {"cacodemon", {"HEAD", "A", "BCD", "E", "GHIJKL", false}},
  {"baron", {"BOSS", "ABCD", "EFG", "H", "IJKLMNO", false}},
  {"barrel", {"BAR1", "AB", "", "", "ABCDE", true, "BEXP"}}};

// Draws the monsters as Doom's sprites: flat pictures standing upright, turned to the eye, with
// the frame for what the monster does and the rotation for the side it shows (R_ProjectSprite).
// A run-duration model, like the missiles; Stale() says when a rebuild would change the picture.
class SpriteVis : public G4VUserVisAction
{
 public:
  SpriteVis(G4VisManager* vis, std::shared_ptr<SpriteSet> sprites) : fVis(vis), fSprites(std::move(sprites)) {}

  G4bool Sprited(const G4String& type) const
  {
    auto it = kSpriteFrames.find(type);
    return it != kSpriteFrames.end() && fSprites->Has(it->second.prefix);
  }

  G4bool Stale()
  {
    if (!Bestiary::Instance().SpritesOn()) return !fDrawn.empty();
    std::vector<Pick> picks;
    return Choose(picks) && Signature(picks) != fDrawn;
  }

  void Draw() override
  {
    G4VVisManager* concrete = G4VVisManager::GetConcreteInstance();
    if (!concrete) return;
    std::vector<Pick> picks;
    if (!Bestiary::Instance().SpritesOn() || !Choose(picks)) {
      fDrawn.clear();
      return;
    }
    fDrawn = Signature(picks);
    // one polyhedron per colour for all the monsters together: few, large draws
    std::map<G4int, std::pair<G4Colour, std::vector<G4ThreeVector>>> quads;  // by colour, 5 bits a channel
    for (const auto& p : picks) {
      const G4ThreeVector up(0, 1, 0);
      G4ThreeVector back = fEye - p.base;
      back.setY(0);
      if (back.mag2() == 0) continue;
      const G4ThreeVector right = up.cross(back).unit() * kMapUnit;
      const G4ThreeVector rise = up * kMapUnit;
      for (const auto& [colour, runs] : p.patch->colours) {
        const G4Colour& c = p.dark ? kFuzz : colour;
        const G4int key = p.dark ? -1 : G4int(c.GetRed() * 31 + 0.5) << 10 | G4int(c.GetGreen() * 31 + 0.5) << 5 | G4int(c.GetBlue() * 31 + 0.5);
        auto& [shade, q] = quads[key];
        shade = c;
        for (const auto& run : runs) {
          // pixel columns as offsets from the centre column; a mirrored picture swaps sides
          const G4double a = p.flip ? p.patch->left - 1 - run.x1 : run.x0 - p.patch->left;
          const G4double b = p.flip ? p.patch->left - run.x0 : run.x1 + 1 - p.patch->left;
          const G4double top = p.patch->top - run.row, bottom = top - 1;
          q.push_back(p.base + a * right + bottom * rise);
          q.push_back(p.base + b * right + bottom * rise);
          q.push_back(p.base + b * right + top * rise);
          q.push_back(p.base + a * right + top * rise);
        }
      }
    }
    for (const auto& [key, group] : quads) {
      const auto& [colour, corners] = group;
      const G4int n = G4int(corners.size());
      G4PolyhedronArbitrary poly(n, n / 4);
      for (const auto& c : corners) poly.AddVertex(c);
      for (G4int i = 0; i < n; i += 4) poly.AddFacet(i + 1, i + 2, i + 3, i + 4);
      // no SetReferences(): it pairs up the edges of closed solids and complains at every
      // open quad; drawing needs no neighbours
      G4VisAttributes look(colour);
      look.SetForceSolid(true);
      poly.SetVisAttributes(look);
      concrete->Draw(poly);
    }
  }

 private:
  struct Pick
  {
    const SpriteSet::Patch* patch;
    G4int id;
    G4bool flip, dark;
    G4ThreeVector base;  // the feet
    G4int yaw;           // the direction to the eye, in steps of kYawStep
    G4double radius;     // of the (invisible) body, which stops the sight rays
  };
  static constexpr G4double kYawStep = 10 * deg;  // the billboards turn to the eye in these steps
  static inline const G4double kSightCell = 6 * kMapUnit;  // sight is redone when the eye moves this far
  static constexpr G4int kSightBudget = 6;             // monsters whose sight is redone per call
  struct Sight
  {
    G4bool known = false, visible = false;
    G4ThreeVector cell, at;
  };
  static inline const G4Colour kFuzz{0.06, 0.05, 0.05, 0.45};  // the spectre: nearly invisible

  // The picture for every monster, as Doom would show it now; false if nothing is known yet
  G4bool Choose(std::vector<Pick>& picks)
  {
    G4VViewer* viewer = fVis->GetCurrentViewer();
    G4Scene* scene = fVis->GetCurrentScene();
    if (!viewer || !scene) return false;
    const G4ViewParameters& vp = viewer->GetViewParameters();
    const G4Vector3D back = vp.GetViewpointDirection().unit();
    const G4Point3D eye = scene->GetStandardTargetPoint() + vp.GetCurrentTargetPoint() +
                          vp.GetCameraDistance(scene->GetExtent().GetExtentRadius()) * back;
    fEye.set(eye.x(), eye.y(), eye.z());
    if (!Bestiary::Instance().Snapshot(fViews) && fViews.empty()) return false;  // busy: the last copy
    const G4double now = Now(), tic = kTic / s;
    const G4ThreeVector ahead = -G4ThreeVector(back.x(), back.y(), back.z());
    const G4bool inside = Clearance(fEye, ahead) >= 0;  // else (an overview from outside) no sight tests
    fSight.resize(fViews.size());
    G4int budget = kSightBudget;
    std::size_t i = 0;    for (const auto& v : fViews) {
      Sight& sight = fSight[i++];
      auto it = kSpriteFrames.find(v.type);
      if (it == kSpriteFrames.end() || !fSprites->Has(it->second.prefix)) continue;
      const SpriteFrames& f = it->second;
      std::string prefix = f.prefix;
      char frame;
      if (!v.alive) {
        const std::string death = f.death;
        const G4int k = v.laid ? G4int((now - v.died) / (5 * tic)) : 0;  // 5 tics a frame
        if (k >= G4int(death.size()) && f.gone) continue;
        frame = death[std::min<std::size_t>(k, death.size() - 1)];
        if (f.deathPrefix) prefix = f.deathPrefix;
      }
      else if (now < v.painUntil && *f.pain)
        frame = f.pain[0];
      else if (v.attacking && *f.attack)
        frame = f.attack[std::min<std::size_t>(std::size_t((now - v.attackStart) / (8 * tic)), std::strlen(f.attack) - 1)];
      else if (now - v.moved < 0.3)
        frame = f.walk[std::size_t((now / (4 * tic))) % std::strlen(f.walk)];
      else
        frame = f.walk[std::size_t(now / (10 * tic)) % std::min<std::size_t>(2, std::strlen(f.walk))];
      // R_ProjectSprite: the rotation from the angle at which the viewer sees the thing
      const G4double toThing = std::atan2(-(v.at.z() - fEye.z()), v.at.x() - fEye.x());
      G4double turn = toThing - v.facing + 202.5 * deg;
      turn -= CLHEP::twopi * std::floor(turn / CLHEP::twopi);
      const G4int rot = G4int(turn / (45 * deg)) % 8 + 1;
      Pick p;
      p.patch = fSprites->Find(prefix, frame, rot, p.flip, p.id);
      if (!p.patch) continue;
      p.dark = v.type == "spectre";
      p.radius = v.alive ? v.kind->radius * kMapUnit : 0;
      p.base = v.at;
      p.base.setY(v.laid ? v.at.y() - kCorpseHeight / 2 - 0.5 * mm : v.at.y() - v.kind->centre * kMapUnit);
      p.yaw = G4int(std::floor(std::atan2(fEye.z() - v.at.z(), fEye.x() - v.at.x()) / kYawStep));
      if (!InFront(p, ahead)) continue;  // drawing costs: only what can be seen
      if (inside) {
        // sight rays are dear (tessellated walls), so each result is kept until the eye moves
        // half a step or the monster moves, and only a few are redone per call (by the next)
        const G4ThreeVector cell(std::floor(fEye.x() / kSightCell), std::floor(fEye.y() / kSightCell),
                                 std::floor(fEye.z() / kSightCell));
        if (!sight.known || ((sight.cell != cell || sight.at != p.base) && budget > 0)) {
          sight = {true, InSight(p), cell, p.base};
          --budget;
        }
        if (!sight.visible) continue;
      }
      picks.push_back(p);
    }
    return true;
  }

  G4bool InFront(const Pick& p, const G4ThreeVector& ahead) const
  {
    const G4double height = p.patch->top * kMapUnit, width = p.patch->width * kMapUnit;
    return (p.base + G4ThreeVector(0, height / 2, 0) - fEye).dot(ahead) > -std::max(height, width);
  }

  // Whether any of the picture is in sight: rays to five points of it (three up the middle,
  // two at the sides)
  G4bool InSight(const Pick& p)
  {
    const G4double height = p.patch->top * kMapUnit, half = 0.5 * p.patch->width * kMapUnit;
    const G4ThreeVector mid = p.base + G4ThreeVector(0, height / 2, 0);
    G4ThreeVector side = G4ThreeVector(0, 1, 0).cross(mid - fEye);
    if (side.mag2() > 0) side = side.unit() * 0.8 * half;
    for (const G4ThreeVector& to : {mid, p.base + G4ThreeVector(0, 0.15 * height, 0), p.base + G4ThreeVector(0, 0.85 * height, 0),
                                    mid + side, mid - side}) {
      const G4double dist = (to - fEye).mag();
      if (dist < half) return true;
      // the ray stops at the monster's own (invisible) body: up to its radius short (more than
      // half the picture when it is seen side-on, which hid monsters as they turned), or half the
      // picture short at its sides
      if (Clearance(fEye, to - fEye) >= dist - std::max(half, p.radius) - 4 * kMapUnit) return true;
    }
    return false;
  }

  // Free distance from p along dir to the next solid; -1 if p is inside one
  G4double Clearance(const G4ThreeVector& p, const G4ThreeVector& dir)
  {
    FollowWorld(fNav);
    const G4ThreeVector d = dir.unit();
    G4double safety;
    if (fNav.LocateGlobalPointAndSetup(p, &d, false, false) != fNav.GetWorldVolume()) return -1;
    return fNav.ComputeStep(p, d, kInfinity, safety);
  }

  static std::string Signature(const std::vector<Pick>& picks)
  {
    std::ostringstream sig;
    for (const auto& p : picks) sig << p.id << (p.flip ? '~' : ' ') << p.yaw << ' ' << p.base.x() << ' ' << p.base.z() << ';';
    return sig.str();
  }

  G4VisManager* fVis;
  std::shared_ptr<SpriteSet> fSprites;
  std::vector<Bestiary::View> fViews;
  G4ThreeVector fEye;
  std::string fDrawn;  // the signature of what the last Draw showed
  G4Navigator fNav;    // private: the sight tests must not disturb tracking
  std::vector<Sight> fSight;  // per monster, in Snapshot order
};
#endif

// The /doom/ commands. They act on the master only, so none of them is broadcast to workers.
class DoomMessenger : public G4UImessenger
{
 public:
  explicit DoomMessenger(G4VisManager* vis) : fVis(vis)
  {
    fDir = new G4UIdirectory("/doom/", false);
    fDir->SetGuidance("Doom gameplay: shoot from the camera, monster health.");

    fFire = new G4UIcmdWithAnInteger("/doom/fire", this);
    fFire->SetGuidance("Fire the gun from the current camera along the view direction.");
    fFire->SetParameterName("n", true);
    fFire->SetDefaultValue(1);
    fFire->SetRange("n >= 1");

    fGunDrop = new G4UIcmdWithADoubleAndUnit("/doom/gunDrop", this);
    fGunDrop->SetGuidance("How far below the eye (along the up vector) the muzzle sits.");
    fGunDrop->SetDefaultUnit("mm");

    fEnergyPerHP = new G4UIcmdWithADoubleAndUnit("/doom/energyPerHP", this);
    fEnergyPerHP->SetGuidance("Energy deposit that counts as one hit point of damage.");
    fEnergyPerHP->SetDefaultUnit("MeV");

    fStatus = new G4UIcmdWithoutParameter("/doom/status", this);
    fStatus->SetGuidance("Print the player's health and that of every monster.");

    fReset = new G4UIcmdWithoutParameter("/doom/reset", this);
    fReset->SetGuidance("Restart the level: monsters, doors and items as they were, the player at the start");
    fReset->SetGuidance("with a pistol start (as after a death).");

    fWalk = new G4UIcmdWithADoubleAndUnit("/doom/walk", this);
    fWalk->SetGuidance("Walk forward (negative: back), following the floor and stopping at walls.");
    fWalk->SetDefaultUnit("mm");

    fStrafe = new G4UIcmdWithADoubleAndUnit("/doom/strafe", this);
    fStrafe->SetGuidance("Step sideways (positive: right), following the floor and stopping at walls.");
    fStrafe->SetDefaultUnit("mm");

    fTurn = new G4UIcmdWithADoubleAndUnit("/doom/turn", this);
    fTurn->SetGuidance("Turn left (negative: right) about the vertical.");
    fTurn->SetDefaultUnit("deg");

    fWhere = new G4UIcmdWithoutParameter("/doom/where", this);
    fWhere->SetGuidance("Print the eye position and view direction.");

    fMove = new G4UIcommand("/doom/move", this);
    fMove->SetGuidance("Walk, strafe, turn and look in one step (one redraw):");
    fMove->SetGuidance("  forward [mm] right [mm] turn left [deg] look up [deg]");
    for (const char* name : {"forward", "right", "turn", "pitch"}) {
      auto* par = new G4UIparameter(name, 'd', true);
      par->SetDefaultValue(0.);
      fMove->SetParameter(par);
    }

    fSensitivity = new G4UIcmdWithADouble("/doom/mouseSensitivity", this);
    fSensitivity->SetGuidance("Mouse look speed in degrees per mouse count (pixel); applies at once.");
    fSensitivity->SetParameterName("degPerCount", false);
    fSensitivity->SetRange("degPerCount > 0");

    fMonsters = new G4UIcmdWithABool("/doom/monsters", this);
    fMonsters->SetGuidance("Let the monsters move (in the Qt session, ten AI steps a second).");
    fMonsters->SetParameterName("on", false);

    fThink = new G4UIcmdWithADoubleAndUnit("/doom/think", this);
    fThink->SetGuidance("Run one monster AI step covering this much game time.");
    fThink->SetParameterName("dt", false);
    fThink->SetDefaultUnit("s");
    fThink->SetRange("dt > 0");

    fVolumeCmd = new G4UIcmdWithADouble("/doom/volume", this);
    fVolumeCmd->SetGuidance("Sound effects volume, 0 (off) to 1.");
    fVolumeCmd->SetParameterName("volume", false);
    fVolumeCmd->SetRange("volume >= 0 && volume <= 1");

    fMusicCmd = new G4UIcmdWithAString("/doom/music", this);
    fMusicCmd->SetGuidance("Music, played on an emulated OPL3 FM chip with the WAD's instruments (as Doom");
    fMusicCmd->SetGuidance("on a Sound Blaster): on, off, or a song from music/ (D_E1M2, D_RUNNIN, ...).");
    fMusicCmd->SetParameterName("song", false);

    fMusicVolumeCmd = new G4UIcmdWithADouble("/doom/musicVolume", this);
    fMusicVolumeCmd->SetGuidance("Music volume, 0 (off) to 1.");
    fMusicVolumeCmd->SetParameterName("volume", false);
    fMusicVolumeCmd->SetRange("volume >= 0 && volume <= 1");

    fSoundLog = new G4UIcmdWithABool("/doom/soundLog", this);
    fSoundLog->SetGuidance("Print every sound effect with its gain, pitch and pan (also without audio).");
    fSoundLog->SetParameterName("on", false);

    fGodCmd = new G4UIcmdWithABool("/doom/god", this);
    fGodCmd->SetGuidance("God mode: the monsters' attacks no longer hurt.");
    fGodCmd->SetParameterName("on", false);

    fCrosshairCmd = new G4UIcmdWithABool("/doom/crosshair", this);
    fCrosshairCmd->SetGuidance("Show a crosshair in the HUD (shots go along the view centre).");
    fCrosshairCmd->SetParameterName("on", false);

    fWeaponCmd = new G4UIcmdWithAnInteger("/doom/weapon", this);
    fWeaponCmd->SetGuidance("Take a weapon by its Doom slot:");
    for (const auto& w : kWeapons) fWeaponCmd->SetGuidance(("  " + std::to_string(w.slot) + ": " + w.name).c_str());
    fWeaponCmd->SetParameterName("slot", false);

    fFaceCmd = new G4UIcmdWithAString("/doom/face", this);
    fFaceCmd->SetGuidance("Whose face is in the status bar: doomguy, or physicist (totally not a famous");
    fFaceCmd->SetGuidance("one; made by physicist_face.py into hud/physicist/).");
    fFaceCmd->SetParameterName("face", false);
    fFaceCmd->SetCandidates("doomguy physicist");

    fHudStyleCmd = new G4UIcmdWithAString("/doom/hudStyle", this);
    fHudStyleCmd->SetGuidance("The status bar: doom (the WAD's own), or lhc (an accelerator dipole with");
    fHudStyleCmd->SetGuidance("control-room readouts; made by lhc_hud.py into hud/lhc/).");
    fHudStyleCmd->SetParameterName("style", false);
    fHudStyleCmd->SetCandidates("doom lhc");

    fWeaponStyleCmd = new G4UIcmdWithAString("/doom/weaponStyle", this);
    fWeaponStyleCmd->SetGuidance("The weapons in view: doom (the WAD's sprites), or instruments (an electron gun,");
    fWeaponStyleCmd->SetGuidance("a focusing horn, an RF linac, a cryostat, a cryomodule and a Penning trap; made by");
    fWeaponStyleCmd->SetGuidance("weapon_sprites.py into hud/instruments/).");
    fWeaponStyleCmd->SetParameterName("style", false);
    fWeaponStyleCmd->SetCandidates("doom instruments");

    fSpritesCmd = new G4UIcmdWithABool("/doom/sprites", this);
    fSpritesCmd->SetGuidance("Show the monsters as Doom's sprites (from sprites/), standing in for the water");
    fSpritesCmd->SetGuidance("volumes that the physics sees; false: show the volumes themselves.");
    fSpritesCmd->SetParameterName("on", false);

    fHitboxesCmd = new G4UIcmdWithABool("/doom/hitboxes", this);
    fHitboxesCmd->SetGuidance("Detector view: with sprites on, also draw the volumes the particles hit, as wireframes.");
    fHitboxesCmd->SetParameterName("on", false);

    fTexturesCmd = new G4UIcmdWithABool("/doom/textures", this);
    fTexturesCmd->SetGuidance("Draw the level with Doom's textures and lighting (OpenGL Qt viewers; needs");
    fTexturesCmd->SetGuidance("<map>_level.txt and textures/ from doom2gdml.py). Geant4's flat-coloured level");
    fTexturesCmd->SetGuidance("volumes are hidden meanwhile; the physics sees the same geometry either way.");
    fTexturesCmd->SetParameterName("on", false);

    fAnimateCmd = new G4UIcmdWithoutParameter("/doom/animate", this);
    fAnimateCmd->SetGuidance("Redraw if a sprite's frame has changed (the Qt session does this ten times a second).");

    fIdkfa = new G4UIcmdWithoutParameter("/doom/idkfa", this);
    fIdkfa->SetGuidance("Every weapon, full ammo, and all the keys.");

    fUseCmd = new G4UIcmdWithoutParameter("/doom/use", this);
    fUseCmd->SetGuidance("Use what is in front of you (within 64 map units): open a door, press a switch.");

    fMapCmd = new G4UIcmdWithAString("/doom/map", this);
    fMapCmd->SetGuidance("Go to another level (E1M2, MAP05, ...), converted into the same folder as this");
    fMapCmd->SetGuidance("one (doom2gdml.py WAD all -o folder), as Doom's IDCLEV cheat does.");
    fMapCmd->SetParameterName("map", false);

    fNextCmd = new G4UIcmdWithoutParameter("/doom/next", this);
    fNextCmd->SetGuidance("Finish the level now (as its exit does): the tally, then the next level.");

    fWorldCmd = new G4UIcmdWithADoubleAndUnit("/doom/world", this);
    fWorldCmd->SetGuidance("Run the level's doors, lifts, floors and switches for this much game time");
    fWorldCmd->SetGuidance("(the Qt session does it 35 times a second).");
    fWorldCmd->SetParameterName("dt", false);
    fWorldCmd->SetDefaultUnit("s");
    fWorldCmd->SetRange("dt > 0");

    for (G4UIcommand* cmd : {(G4UIcommand*)fFire, (G4UIcommand*)fGunDrop, (G4UIcommand*)fEnergyPerHP,
                             (G4UIcommand*)fStatus, (G4UIcommand*)fReset, (G4UIcommand*)fWalk,
                             (G4UIcommand*)fStrafe, (G4UIcommand*)fTurn, (G4UIcommand*)fWhere, fMove,
                             (G4UIcommand*)fSensitivity, (G4UIcommand*)fMonsters, (G4UIcommand*)fThink,
                             (G4UIcommand*)fVolumeCmd, (G4UIcommand*)fSoundLog, (G4UIcommand*)fGodCmd,
                             (G4UIcommand*)fCrosshairCmd, (G4UIcommand*)fWeaponCmd, (G4UIcommand*)fIdkfa,
                             (G4UIcommand*)fFaceCmd, (G4UIcommand*)fHudStyleCmd, (G4UIcommand*)fWeaponStyleCmd,
                             (G4UIcommand*)fMusicCmd, (G4UIcommand*)fSpritesCmd, (G4UIcommand*)fHitboxesCmd,
                             (G4UIcommand*)fAnimateCmd, (G4UIcommand*)fTexturesCmd,
                             (G4UIcommand*)fMusicVolumeCmd, (G4UIcommand*)fUseCmd, (G4UIcommand*)fWorldCmd,
                             (G4UIcommand*)fMapCmd, (G4UIcommand*)fNextCmd})
      cmd->SetToBeBroadcasted(false);
    PistolStart();
  }

  // The monster sprites (SpriteVis): whether they are out of date, and which types have them
  void SetSprites(std::function<G4bool()> stale, std::function<G4bool(const G4String&)> sprited)
  {
    fStale = std::move(stale);
    fSprited = std::move(sprited);
    ApplyLook();
  }
  void ApplyLook() { Bestiary::Instance().SetLook(fSpritesOn && fSprited, fHitboxes, fSprited); }

  // The textured level (TexturePass): whether there is one, and whether it is drawn now
  void SetTexturesAvailable(G4bool available) { fTexturesAvailable = available; }
  G4bool TexturesOn() const { return fTexturesOn; }
  void SetTextures(G4bool on)
  {
    if (on && !fTexturesAvailable) {
      G4cout << "/doom/textures: no textured level (needs the Qt build and <map>_level.txt + textures/ next to the GDML)" << G4endl;
      return;
    }
    G4VViewer* viewer = fVis->GetCurrentViewer();
    const G4String system = viewer ? viewer->GetSceneHandler()->GetGraphicsSystem()->GetName() : "";
    const G4bool qtGL = (G4StrUtil::contains(system, "OpenGL") && G4StrUtil::contains(system, "Qt")) ||
                        system == "TOOLSSG_QT_GLES";  // QOpenGLWidgets
    if (on && !qtGL) {
      G4cout << "/doom/textures: only for the Qt OpenGL viewers (TSG_QT_GLES, OGL), not " << system << G4endl;
      return;
    }
    if (on == fTexturesOn) return;
    fTexturesOn = on;
    // the level volumes (sector slabs S*, walls L*; things are T*) are drawn textured instead
    G4LogicalVolume* world =
      G4TransportationManager::GetTransportationManager()->GetNavigatorForTracking()->GetWorldVolume()->GetLogicalVolume();
    for (std::size_t i = 0; i < world->GetNoDaughters(); ++i) {
      G4LogicalVolume* lv = world->GetDaughter(i)->GetLogicalVolume();
      const char c = lv->GetName()[0];
      if (c != 'S' && c != 'L') continue;
      if (on) {
        fLevelLooks[lv] = lv->GetVisAttributes();
        lv->SetVisAttributes(G4VisAttributes::GetInvisible());
      }
      else if (fLevelLooks.count(lv))
        lv->SetVisAttributes(fLevelLooks[lv]);
    }
    if (!on) fLevelLooks.clear();
    G4UImanager::GetUIpointer()->ApplyCommand("/vis/viewer/rebuild");
  }

  // The level's doors, lifts and switches. Their sounds, messages and teleports come back here,
  // and they ask here whether the player or a monster is in the way of a closing door.
  void SetWorld(DoomWorld* world)
  {
    fWorld = world;
    if (!world) return;
    DoomWorld::Hooks hooks;
    hooks.sound = [this](const std::string& lump, double x, double y, double z) {
      G4ThreeVector at = fWorld->ToG4(x, y, std::isnan(z) ? 0 : z);
      Camera cam;
      if (std::isnan(z) && GetCamera(cam, nullptr)) at.setY(cam.eye.y());  // a line: at our height
      Sound(lump, &at);
    };
    hooks.message = [this](const std::string& text) { Say(text); };
    hooks.blocked = [this](int sector, double floor, double ceil) { return Blocked(sector, floor, ceil); };
    hooks.teleport = [this](double x, double y, double z, double angle) {
      fTeleport = true;
      fTeleportEye = G4Point3D(fWorld->ToG4(x, y, z) + G4ThreeVector(0, kEyeHeight, 0));
      fTeleportBack = -G4Vector3D(std::cos(angle * deg), 0, -std::sin(angle * deg));
    };
    hooks.exit = [this](bool secret) { StartIntermission(secret); };
    world->SetHooks(hooks);
  }
  const DoomWorld* Level() const { return fWorld; }

  // The level being played (its GDML; the other levels are the <map>.gdml files next to it),
  // and the detector construction that reads the next one
  void SetLevel(DoomLevel* level, const std::filesystem::path& gdml)
  {
    fLevel = level;
    fLevelDir = std::filesystem::absolute(gdml).parent_path();
    fMap = gdml.stem().string();
    for (auto& c : fMap) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    LoadWorld();
  }
  // Told when the World is replaced (a new level); nullptr while there is none
  void OnWorldChanged(std::function<void(const DoomWorld*)> changed) { fWorldChanged = std::move(changed); }
  const G4String& Map() const { return fMap; }
  // The near plane right (see Aim) after a camera macro, which puts the target 1000 mm ahead
  void SettleCamera()
  {
    Camera cam;
    if (GetCamera(cam, nullptr)) Aim(cam, cam.eye, cam.back);
  }

  // Doom's intermission (wi_stuff.c, single player): the tally counts up (sp_state 1-10, odd
  // states are one-second pauses), then the map with the next level (11), then a short wait (12)
  struct Intermission
  {
    G4bool active = false;
    G4int stage = 0;
    G4String map, next;
    G4int kills = 0, items = 0, secrets = 0, time = 0, par = -1;  // what it counts to (par -1: none)
    G4int cKills = -1, cItems = -1, cSecrets = -1, cTime = -1, cPar = -1;  // shown so far, -1 not yet
    G4int pause = 0, count = 0, tic = 0;
    G4bool pointer = false;  // "you are here" blinks
    bool operator==(const Intermission&) const = default;
  };
  const Intermission& Tally() const { return fInter; }
  // Keys held (bits 0-2 blue/yellow/red cards, 3-5 skulls), for the HUD
  G4int Keys() const { return fWorld ? fWorld->Keys() : 0; }
  // The latest message, for four seconds (Doom's HU_MSGTIMEOUT); empty otherwise
  G4String Message() const { return Now() - fMessageTime < 4 ? fMessage : G4String(); }

  // Where sounds go: lump name, gain, pitch factor, pan (-1 left .. 1 right). Empty: silence.
  void SetSoundPlayer(SoundPlayer play) { fPlay = std::move(play); }
  // Where music goes, and the level's song, which starts at once (unless /doom/music off)
  void SetMusicPlayer(MusicPlayer play, const G4String& song)
  {
    fPlayMusic = std::move(play);
    fSong = song;
    UpdateMusic();
  }

  // For the HUD: health (<= 0: dead), god mode, and all the damage taken so far (for the flash)
  G4int Health() const { return fHealth; }
  G4int Armor() const { return fArmor; }
  // The eye, for the textured level pass; false without a viewer
  G4bool Eye(G4ThreeVector& eye) const
  {
    Camera cam;
    if (!GetCamera(cam, nullptr)) return false;
    eye.set(cam.eye.x(), cam.eye.y(), cam.eye.z());
    return true;
  }
  G4bool God() const { return fGod; }
  G4int DamageTaken() const { return fDamageTaken; }
  // Where the last hit came from: -1 left, 0 ahead (within 45 deg), 1 right (Doomguy's face turns)
  G4int LastHitSide() const { return fHitSide; }
  G4bool Crosshair() const { return fCrosshair; }
  // The face set in the status bar: a subfolder of hud/ (none for Doomguy)
  const G4String& Face() const { return fFace; }
  // The status bar style: also a subfolder of hud/ (none for Doom's own)
  const G4String& HudStyle() const { return fHudStyle; }
  // The weapon sprites: a subfolder of hud/ too (none for Doom's own)
  const G4String& WeaponStyle() const { return fWeaponStyle; }
  // The weapon in hand, its ammo, and how many shots it has fired (for the muzzle flash)
  const Weapon& CurrentWeapon() const { return kWeapons[fWeapon]; }
  G4int Ammo(G4int type) const { return fAmmo[type]; }
  G4int MaxAmmo(G4int type) const { return fMaxAmmo[type]; }
  // Whether the player has the weapon in this Doom slot
  G4bool Owns(G4int slot) const
  {
    const std::size_t i = WeaponIndex(slot);
    return i < kWeapons.size() && fOwned[i];
  }
  G4int ShotsFired() const { return fShots; }
  // Tics since the trigger while a charged weapon (the BFG) charges up, else -1
  G4double ChargeTics() const
  {
    if (!fCharging) return -1;
    return std::chrono::duration<G4double>(std::chrono::steady_clock::now() - fChargeStart).count() / (kTic / s);
  }
  // Whether the charge is up, so the next /doom/fire lets the shot go
  G4bool ChargeDue() const { return fCharging && ChargeTics() >= CurrentWeapon().charge; }
  // Charge-ups need someone to fire again when they are due (DoomInput in the Qt session);
  // without one (batch), charged weapons fire at once
  void SetChargeDeferred(G4bool on) { fChargeDeferred = on; }
  // Seconds between shots while the trigger is held
  G4double Refire() const { return CurrentWeapon().refire * kTic / s; }

  // The fireballs in flight, for MissileVis
  const std::vector<Missile>& Missiles() const { return fMissiles; }
  const std::vector<Burst>& Bursts() const { return fBursts; }

  // Mouse look speed, deg per mouse count: set here, read by the Qt input filter
  G4double MouseSensitivity() const { return fMouseSensitivity; }
  // Whether the Qt input filter should run the monster AI
  G4bool MonstersActive() const { return fMonstersActive; }

  ~DoomMessenger() override
  {
    gWorld = nullptr;
    delete fNextCmd;
    delete fMapCmd;
    delete fWorldCmd;
    delete fUseCmd;
    delete fAnimateCmd;
    delete fTexturesCmd;
    delete fHitboxesCmd;
    delete fSpritesCmd;
    delete fWeaponStyleCmd;
    delete fHudStyleCmd;
    delete fFaceCmd;
    delete fIdkfa;
    delete fWeaponCmd;
    delete fCrosshairCmd;
    delete fGodCmd;
    delete fSoundLog;
    delete fMusicVolumeCmd;
    delete fMusicCmd;
    delete fVolumeCmd;
    delete fThink;
    delete fMonsters;
    delete fSensitivity;
    delete fMove;
    delete fWhere;
    delete fTurn;
    delete fStrafe;
    delete fWalk;
    delete fReset;
    delete fStatus;
    delete fEnergyPerHP;
    delete fGunDrop;
    delete fFire;
    delete fDir;
  }

  void SetNewValue(G4UIcommand* cmd, G4String value) override
  {
    if (cmd == fFire)
      Fire(fFire->GetNewIntValue(value));
    else if (cmd == fGunDrop)
      fDrop = fGunDrop->GetNewDoubleValue(value);
    else if (cmd == fEnergyPerHP)
      Bestiary::Instance().SetEnergyPerHP(fEnergyPerHP->GetNewDoubleValue(value));
    else if (cmd == fStatus) {
      G4cout << "  you: " << (fHealth > 0 ? std::to_string(fHealth) + "% health" : G4String("dead"))
             << ", " << fArmor << "% " << (fArmorType == 2 ? "blue " : fArmorType == 1 ? "green " : "") << "armour"
             << (fGod ? " (god mode)" : "") << ", " << CurrentWeapon().name << ", ammo";
      for (G4int a : fAmmo) G4cout << " " << a;
      G4cout << G4endl;
      Bestiary::Instance().Status();
    }
    else if (cmd == fReset)
      Restart();
    else if (cmd == fGodCmd)
      fGod = fGodCmd->GetNewBoolValue(value);
    else if (cmd == fCrosshairCmd)
      fCrosshair = fCrosshairCmd->GetNewBoolValue(value);
    else if (cmd == fFaceCmd)
      fFace = value;
    else if (cmd == fHudStyleCmd)
      fHudStyle = value;
    else if (cmd == fWeaponStyleCmd)
      fWeaponStyle = value;
    else if (cmd == fSpritesCmd || cmd == fHitboxesCmd) {
      (cmd == fSpritesCmd ? fSpritesOn : fHitboxes) = G4UIcmdWithABool::GetNewBoolValue(value);
      if (cmd == fSpritesCmd && fSpritesOn && !fSprited) G4cout << "/doom/sprites: no sprites/ next to the level" << G4endl;
      ApplyLook();
      G4UImanager::GetUIpointer()->ApplyCommand("/vis/viewer/rebuild");
    }
    else if (cmd == fTexturesCmd)
      SetTextures(fTexturesCmd->GetNewBoolValue(value));
    else if (cmd == fAnimateCmd) {
      if (fStale && fStale()) G4UImanager::GetUIpointer()->ApplyCommand("/vis/viewer/rebuild");
    }
    else if (cmd == fWeaponCmd)
      Select(fWeaponCmd->GetNewIntValue(value));
    else if (cmd == fIdkfa) {
      fOwned.assign(kWeapons.size(), true);
      fAmmo = fMaxAmmo;
      if (fWorld) fWorld->GiveKeys();
      Say("Very happy ammo added");
    }
    else if (cmd == fUseCmd)
      Use();
    else if (cmd == fMapCmd) {
      G4String map = value;
      for (auto& c : map) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      LoadLevel(map);
    }
    else if (cmd == fNextCmd)
      StartIntermission(false);
    else if (cmd == fWorldCmd)
      AdvanceWorld(fWorldCmd->GetNewDoubleValue(value));
    else if (cmd == fMonsters)
      fMonstersActive = fMonsters->GetNewBoolValue(value);
    else if (cmd == fThink)
      Think(fThink->GetNewDoubleValue(value));
    else if (cmd == fVolumeCmd)
      fVolume = fVolumeCmd->GetNewDoubleValue(value);
    else if (cmd == fMusicCmd) {
      if (value == "on" || value == "off")
        fMusicOn = value == "on";
      else {
        fSong = value;
        fMusicOn = true;
      }
      UpdateMusic();
    }
    else if (cmd == fMusicVolumeCmd) {
      fMusicVolume = fMusicVolumeCmd->GetNewDoubleValue(value);
      UpdateMusic();
    }
    else if (cmd == fSoundLog)
      fLogSounds = fSoundLog->GetNewBoolValue(value);
    else if (cmd == fWalk)
      Move(fWalk->GetNewDoubleValue(value), 0., 0.);
    else if (cmd == fStrafe)
      Move(0., fStrafe->GetNewDoubleValue(value), 0.);
    else if (cmd == fTurn)
      Move(0., 0., fTurn->GetNewDoubleValue(value));
    else if (cmd == fMove) {
      std::istringstream in(value);
      G4double forward = 0, right = 0, turn = 0, pitch = 0;
      in >> forward >> right >> turn >> pitch;
      Move(forward * mm, right * mm, turn * deg, pitch * deg);
    }
    else if (cmd == fWhere)
      Where();
    else if (cmd == fSensitivity)
      fMouseSensitivity = fSensitivity->GetNewDoubleValue(value);
  }

 private:
  // The current viewer's camera. Same model as the camera macros:
  // eye = target + viewpointVector * cameraDistance, target = standard target + relative target.
  struct Camera
  {
    G4VViewer* viewer;
    G4ViewParameters vp;
    G4Point3D standardTarget, eye;
    G4Vector3D back;  // from the target towards the eye
    G4double distance;
    G4double radius;  // the scene's extent radius
  };

  G4bool GetCamera(Camera& cam, const char* cmd) const  // cmd: for the error message; nullptr: quiet
  {
    cam.viewer = fVis->GetCurrentViewer();
    G4Scene* scene = fVis->GetCurrentScene();
    if (!cam.viewer || !scene) {
      if (cmd) G4cerr << cmd << ": no current viewer" << G4endl;
      return false;
    }
    cam.vp = cam.viewer->GetViewParameters();
    cam.standardTarget = scene->GetStandardTargetPoint();
    cam.back = cam.vp.GetViewpointDirection().unit();
    cam.radius = scene->GetExtent().GetExtentRadius();
    cam.distance = cam.vp.GetCameraDistance(cam.radius);
    cam.eye = cam.standardTarget + cam.vp.GetCurrentTargetPoint() + cam.distance * cam.back;
    return true;
  }


  // Move the eye and view direction, keeping the camera distance (dolly, field of view).
  void SetCamera(Camera& cam, const G4Point3D& eye, const G4Vector3D& back)
  {
    Aim(cam, eye, back);
    Redraw(false);
  }

  // The same without the redraw. It also keeps the target kNearPlane beyond the scene's radius:
  // Geant4's near plane is the camera distance minus that radius (at least 1e-6 of it), so with
  // the camera macros' 1000 mm it was 0.1 mm against a far plane of 100 m, and the depth buffer
  // could not tell distant surfaces (or a monster's feet and the floor) apart.
  static void Aim(Camera& cam, const G4Point3D& eye, const G4Vector3D& back)
  {
    const G4double want = cam.radius + kNearPlane;
    if (cam.vp.GetFieldHalfAngle() > 0 && std::abs(cam.distance - want) > 1 * mm) {
      cam.vp.SetDolly(cam.radius / std::sin(cam.vp.GetFieldHalfAngle()) - want);
      cam.distance = want;
    }
    cam.vp.SetViewAndLights(back);
    cam.vp.SetCurrentTargetPoint(G4Point3D(eye - cam.distance * back - cam.standardTarget));
    cam.viewer->SetViewParameters(cam.vp);
  }

  // Show the changes: volumes that moved or changed need a rebuild, and so do sprites that turn
  // to the eye and show another side; a new camera alone takes only a refresh. With the textured
  // level the sector slabs and walls are hidden, so the level moving needs no rebuild.
  void Redraw(G4bool volumesChanged, G4bool levelMoved = false)
  {
    const G4bool rebuild = volumesChanged || (levelMoved && !fTexturesOn) || (fStale && fStale());
    G4UImanager::GetUIpointer()->ApplyCommand(rebuild ? "/vis/viewer/rebuild" : "/vis/viewer/refresh");
  }

  // Free distance from p along dir to the next solid; -1 if p is inside a solid. The volume
  // `ignore` counts as air: a monster probing from inside its own body.
  G4double Clearance(const G4Point3D& p, const G4Vector3D& dir, const G4VPhysicalVolume* ignore = nullptr)
  {
    FollowWorld(fNav);
    G4ThreeVector pos(p.x(), p.y(), p.z());
    const G4ThreeVector d = G4ThreeVector(dir.x(), dir.y(), dir.z()).unit();
    const G4double nudge = 1 * um;  // past the boundary, to locate what lies beyond it
    G4double travelled = 0, safety;
    G4VPhysicalVolume* pv = fNav.LocateGlobalPointAndSetup(pos, &d, false, false);
    if (pv && pv == ignore) {  // walk out of our own body first
      const G4double out = fNav.ComputeStep(pos, d, kInfinity, safety) + nudge;
      travelled += out;
      pos += out * d;
      pv = fNav.LocateGlobalPointAndSetup(pos, &d, false, false);
    }
    if (pv != fNav.GetWorldVolume()) return travelled > 0 ? travelled - nudge : -1;  // body touches it
    return travelled + fNav.ComputeStep(pos, d, kInfinity, safety);
  }

  // One AI step of dt for every monster: wake on sight, then chase the player.
  void Think(G4double dt)
  {
    if (fInter.active) return;
    Camera cam;
    if (!GetCamera(cam, "/doom/think")) return;
    if (Clearance(cam.eye, G4Vector3D(0, -1, 0)) < 0) return;  // the camera is not in the level
    const G4ThreeVector player(cam.eye.x(), cam.eye.y(), cam.eye.z());
    const G4bool flying = MoveMissiles(dt, player);
    G4bool moved = false;
    Bestiary::Instance().ForEach([&](const G4String& name, Bestiary::Monster& m) {
      if (m.health <= 0 || m.kind->speed == 0) return;
      G4ThreeVector centre = m.at;
      if (!m.awake && Sees(m, centre, player)) {
        m.awake = true;
        G4cout << name << " sees you!" << G4endl;
        Sound(Pick(kMonsterSounds.at(m.type).sight), &centre);
      }
      if (!m.awake || fHealth <= 0) return;  // asleep, or nobody left to fight
      const auto attack = kMonsterAttacks.find(m.type);
      if (attack != kMonsterAttacks.end() && Fight(name, m, attack->second, dt, player)) return;  // stands still
      G4ThreeVector to = player - centre;
      to.setY(0);
      const G4double reach = m.kind->radius * kMapUnit + kPlayerRadius + 8 * kMapUnit;  // melee range
      if (to.mag() <= reach) return;
      const G4double dist = std::min(m.kind->speed * kMapUnit / s * dt, to.mag() - reach);
      // straight at the player, else try around the obstacle like Doom's direction picking
      const G4ThreeVector before = centre;
      G4bool stepped = false;
      for (G4double turn : {0., 45., -45., 90., -90.}) {
        G4ThreeVector dir = to.unit();
        dir.rotateY(turn * deg);
        if (MonsterStep(m, centre, dir, dist, player)) {
          Bestiary::Place(m, centre);
          m.facing = std::atan2(-dir.z(), dir.x());  // Doom's angle: G4 z is -Doom y
          m.moved = Now();
          moved = stepped = true;
          break;
        }
      }
      if (!fWorld) return;
      double x0, y0, z0, x1, y1, z1;
      fWorld->FromG4(before, x0, y0, z0);
      fWorld->FromG4(centre, x1, y1, z1);
      if (stepped)  // walk-over lines that monsters trigger (lifts, some doors)
        fWorld->Cross(x0, y0, x1, y1, false);
      else  // stuck: a door in the way? Monsters open the plain ones (P_Move)
        fWorld->MonsterUse(x0, y0, std::atan2(-to.z(), to.x()) / deg, m.kind->radius + dist / kMapUnit + 8);
    });
    if (fFalling) Fall();  // not inside ForEach: the redraw processes Qt events (the HUD timer)
    if (moved)
      GeometryMoved();
    else if ((flying && !fTexturesOn) || (fStale && fStale()))  // textured: TexturePass repaints them
      G4UImanager::GetUIpointer()->ApplyCommand("/vis/viewer/rebuild");  // redraws the missiles, sprites
  }

  // Doom's attack logic for an awake monster (A_Chase, then the attack frames). Every chase
  // step it bites if the player is in melee range. It considers shooting only at the end of a
  // walk in one direction (moveCount 0), after its reaction time, on a roll that favours short
  // range, or at once when just hurt. Returns true while attacking, when it does not walk.
  G4bool Fight(const G4String& name, Bestiary::Monster& m, const MonsterAttack& a, G4double dt,
               const G4ThreeVector& player)
  {
    if (m.busy > 0) {
      m.busy -= dt;
      if (m.strikeIn >= 0 && (m.strikeIn -= dt) < 0) Strike(name, m, a, player);
      if (m.busy > 0) return true;
      m.nextCheck = a.chase * kTic;  // MF_JUSTATTACKED: one chase step before the next attack,
      m.moveCount = fRandom() & 15;  // then a walk in a new direction
      return false;
    }
    for (m.nextCheck -= dt; m.nextCheck <= 0; m.nextCheck += a.chase * kTic) {
      if (m.reaction > 0) --m.reaction;
      G4bool attack = a.meleeSides > 0 && InMelee(m, player) && Sees(m, m.at, player);
      if (!attack && a.ranged != kNoRanged) {
        if (m.moveCount > 0)
          --m.moveCount;
        else {
          attack = (m.justHit || (m.reaction == 0 && MissileRoll(m, a, player))) && Sees(m, m.at, player);
          if (!attack) m.moveCount = fRandom() & 15;  // walks on, in a new direction
        }
      }
      if (attack) {
        m.justHit = false;
        m.facing = std::atan2(-(player.z() - m.at.z()), player.x() - m.at.x());  // A_FaceTarget
        m.attackStart = Now();
        m.strikeIn = a.windup * kTic;
        m.busy = (a.windup + a.recover) * kTic;
        m.nextCheck = 0;
        return true;
      }
    }
    return false;
  }

  G4bool InMelee(const Bestiary::Monster& m, const G4ThreeVector& player) const
  {
    return std::hypot(player.x() - m.at.x(), player.z() - m.at.z()) < kMeleeRange;
  }

  // P_CheckMissileRange: always within 64 units (192 without a melee attack), never certain
  G4bool MissileRoll(const Bestiary::Monster& m, const MonsterAttack& a, const G4ThreeVector& player)
  {
    G4double dist = std::hypot(player.x() - m.at.x(), player.z() - m.at.z()) / kMapUnit - 64;
    if (a.meleeSides == 0) dist -= 128;  // no melee attack, so fire more
    return static_cast<G4double>(fRandom() % 256) >= std::min(dist, 200.);
  }

  G4int Roll(G4int sides) { return 1 + static_cast<G4int>(fRandom() % sides); }

  // The attack frame: bite or claw in melee range, else shoot or throw a fireball
  void Strike(const G4String& name, const Bestiary::Monster& m, const MonsterAttack& a, const G4ThreeVector& player)
  {
    const G4ThreeVector& centre = m.at;
    const G4bool sees = Sees(m, centre, player);
    if (a.meleeSides > 0 && (InMelee(m, player) || a.ranged == kNoRanged)) {
      Sound(a.meleeSound, &centre);
      if (sees && InMelee(m, player)) Hurt(a.meleeMult * Roll(a.meleeSides), name, centre);  // else too late
      return;
    }
    Sound(a.rangedSound, &centre);
    if (a.ranged == kHitscan) {
      if (!sees) return;  // the bullets hit a wall
      // each pellet strays by Doom's triangular spread; it hits if it passes within our radius
      const G4double half = std::atan2(kPlayerRadius, std::hypot(player.x() - centre.x(), player.z() - centre.z()));
      G4int damage = 0;
      for (G4int i = 0; i < a.pellets; ++i) {
        const G4double stray = (G4double(fRandom() % 256) - G4double(fRandom() % 256)) / 255 * kBulletSpread;
        if (std::abs(stray) < half) damage += a.mult * Roll(a.sides);
      }
      if (damage > 0) Hurt(damage, name, centre);
    }
    else if (a.ranged == kMissile) {  // at the middle of the player's body, from where it stands
      const G4ThreeVector from(centre.x(), centre.y() - m.kind->centre * kMapUnit + kMissileHeight, centre.z());
      const G4ThreeVector target(player.x(), player.y() - kEyeHeight + kPlayerHeight / 2, player.z());
      fMissiles.push_back({from, (target - from).unit() * a.missileSpeed * kMapUnit / kTic,
                           a.mult * Roll(a.sides), name, m.pv, a.missileColour,
                           kMissileSprites.count(m.type) ? kMissileSprites.at(m.type) : "BAL1"});
    }
  }

  // Fly the fireballs for dt. They burst on walls, floors and monsters, or on the player. Returns
  // whether any was in flight (to redraw).
  G4bool MoveMissiles(G4double dt, const G4ThreeVector& eye)
  {
    std::erase_if(fBursts, [](const Burst& b) { return Now() - b.time > 3 * kMissileBurstTics * kTic / s; });
    if (fMissiles.empty()) return false;
    for (auto it = fMissiles.begin(); it != fMissiles.end();) {
      const G4ThreeVector dir = it->velocity.unit();
      G4bool burst = (it->age += dt) > kMissileLife;
      for (G4double left = it->velocity.mag() * dt; !burst && left > 0;) {
        const G4double step = std::min(left, 8 * kMapUnit);
        const G4double free = Clearance(G4Point3D(it->pos), G4Vector3D(dir), it->owner);
        const G4ThreeVector next = it->pos + std::clamp(free, 0., step) * dir;
        if (fHealth > 0 && HitsPlayer(it->pos, next, eye)) {
          Hurt(it->damage, it->by, it->pos);
          burst = true;
        }
        else if (free < step)
          burst = true;
        it->pos = next;
        left -= step;
      }
      if (burst) {
        Sound("dsfirxpl", &it->pos);
        fBursts.push_back({it->pos, it->sprite, Now()});
        it = fMissiles.erase(it);
      }
      else
        ++it;
    }
    return true;
  }

  // Whether the segment a-b passes through the player's body (a cylinder) at a missile's radius
  static G4bool HitsPlayer(const G4ThreeVector& a, const G4ThreeVector& b, const G4ThreeVector& eye)
  {
    const G4double dx = b.x() - a.x(), dz = b.z() - a.z(), len2 = dx * dx + dz * dz;
    const G4double t =
      len2 > 0 ? std::clamp(((eye.x() - a.x()) * dx + (eye.z() - a.z()) * dz) / len2, 0., 1.) : 0.;
    const G4ThreeVector p = a + t * (b - a);
    const G4double feet = eye.y() - kEyeHeight;
    return std::hypot(p.x() - eye.x(), p.z() - eye.z()) < kPlayerRadius + kMissileRadius
           && p.y() > feet - kMissileRadius && p.y() < feet + kPlayerHeight + kMissileRadius;
  }

  // The player takes damage (no armour, as at the start of a Doom level)
  void Hurt(G4int damage, const G4String& by, const G4ThreeVector& from)
  {
    if (fHealth <= 0) return;
    Camera cam;
    if (GetCamera(cam, nullptr)) {  // which way Doomguy looks: towards the attacker
      const G4ThreeVector to = from - G4ThreeVector(cam.eye.x(), cam.eye.y(), cam.eye.z());
      const G4double ahead = -(to.x() * cam.back.x() + to.z() * cam.back.z());
      const G4double right = to.x() * cam.back.z() - to.z() * cam.back.x();
      const G4double angle = std::atan2(right, ahead);
      fHitSide = std::abs(angle) < 45 * deg ? 0 : angle > 0 ? 1 : -1;
    }
    if (fGod) {
      G4cout << by << " hits you for " << damage << ", but you are a god" << G4endl;
      return;
    }
    // P_DamageMobj: armour takes a third (green) or half (blue) of the damage while it lasts
    G4int saved = 0;
    if (fArmorType > 0) {
      saved = fArmorType == 1 ? damage / 3 : damage / 2;
      if (fArmor <= saved) {
        saved = fArmor;
        fArmorType = 0;
      }
      fArmor -= saved;
      damage -= saved;
    }
    fHealth -= damage;
    fDamageTaken += damage;
    G4cout << by << " hits you for " << damage << ": " << std::max(fHealth, 0) << "% health";
    if (saved > 0) G4cout << " (the armour took " << saved << ", " << fArmor << "% left)";
    G4cout << G4endl;
    if (fHealth > 0)
      Sound("dsplpain");
    else
      Die();
  }

  // Doom's death: a scream (a worse one for a big overkill), then the view sinks to the floor
  // at the end of the AI step
  void Die()
  {
    Sound(fHealth < -50 ? "dspdiehi" : "dspldeth");
    fCharging = false;
    G4cout << "You died! Fire (or /doom/reset) to play again." << G4endl;
    fFalling = true;
  }

  void Fall()
  {
    fFalling = false;
    Camera cam;
    if (!GetCamera(cam, nullptr)) return;
    const G4double below = Clearance(cam.eye, G4Vector3D(0, -1, 0));
    if (below < 0) return;
    const G4Point3D eye(cam.eye.x(), cam.eye.y() - below + kDeadEyeHeight, cam.eye.z());
    SetCamera(cam, eye, cam.back);
  }

  // /doom/reset, or fire while dead (Doom restarts on a key press too)
  // Play the level again from the start, as Doom does after a death (G_DoReborn): monsters,
  // doors, items and the clock as they were, the player at the start with a pistol start
  void Restart()
  {
    Bestiary::Instance().Reset();
    fMissiles.clear();
    fBursts.clear();
    fCharging = fTeleport = fFalling = false;
    PistolStart();
    if (fWorld) {  // doors shut, lifts up, keys back where they were
      fWorld->Reset();
      Ride(fWorld->UpdateGeometry());
    }
    const auto start = fLevelDir / (Lower(fMap) + "_camera.mac");
    if (!fMap.empty() && std::filesystem::exists(start)) {
      fHealth = kPlayerHealth;
      fArmor = fArmorType = 0;
      G4UImanager::GetUIpointer()->ApplyCommand("/control/execute " + start.string());
      SettleCamera();
    }
    else
      Respawn();  // no camera macro: get up where we fell
    GeometryMoved();
  }

  // Back to life where the player fell, at full health
  void Respawn()
  {
    if (fHealth <= 0) {
      Camera cam;
      if (GetCamera(cam, nullptr)) {
        const G4double below = Clearance(cam.eye, G4Vector3D(0, -1, 0));
        if (below >= 0)
          SetCamera(cam, G4Point3D(cam.eye.x(), cam.eye.y() - below + kEyeHeight, cam.eye.z()), cam.back);
      }
    }
    fHealth = kPlayerHealth;
    fArmor = fArmorType = 0;
  }

  // Line of sight from the monster's eyes (3/4 of its height, as in Doom) to the player's eye
  G4bool Sees(const Bestiary::Monster& m, const G4ThreeVector& centre, const G4ThreeVector& player)
  {
    const G4double floor = centre.y() - m.kind->centre * kMapUnit;
    const G4Point3D eye(centre.x(), floor + 0.75 * m.kind->top * kMapUnit, centre.z());
    const G4Vector3D toPlayer(player.x() - eye.x(), player.y() - eye.y(), player.z() - eye.z());
    return Clearance(eye, toPlayer, m.pv) >= toPlayer.mag();
  }

  // Move a monster's centre by dist along the horizontal dir if it fits there, by Doom's rules:
  // no climbing more than 24 units, no stepping off a ledge deeper than that, not into the player.
  G4bool MonsterStep(const Bestiary::Monster& m, G4ThreeVector& centre, const G4ThreeVector& dir, G4double dist,
                     const G4ThreeVector& player)
  {
    const G4Vector3D up(0, 1, 0), ahead(dir.x(), 0, dir.z()), side = ahead.cross(up);
    const G4double radius = m.kind->radius * kMapUnit;
    const G4double floor = centre.y() - m.kind->centre * kMapUnit;
    const G4Point3D probe(centre.x(), floor + kMaxStepUp + 1 * mm, centre.z());  // above any step
    // walls and other monsters: along the middle and both flanks of the body
    if (Clearance(probe, ahead, m.pv) < dist + radius) return false;
    for (G4double flank : {-0.9, 0.9})
      if (Clearance(probe + flank * radius * side, ahead, m.pv) < dist + 0.5 * radius) return false;
    const G4Point3D next = probe + dist * ahead;
    const G4double below = Clearance(next, -up, m.pv);
    if (below < 0) return false;
    const G4double newFloor = next.y() - below;
    if (std::abs(newFloor - floor) > kMaxStepUp) return false;
    if (Clearance(G4Point3D(next.x(), newFloor + 1 * mm, next.z()), up, m.pv) < m.kind->top * kMapUnit) return false;
    if (std::hypot(next.x() - player.x(), next.z() - player.z()) < radius + kPlayerRadius) return false;
    centre.set(next.x(), newFloor + m.kind->centre * kMapUnit, next.z());
    return true;
  }

  // Monsters were moved: rebuild the world's navigation voxels now (shared by all threads, and
  // used by our own probes) and redraw. The workers copy the new placements at the start of the
  // next run (WorkerSync). Geant4 11's task run manager configures parallel voxel optimisation:
  // CloseGeometry then only queues the work, and the workers build the voxels at the start of
  // the next run while other workers already track (segfault in G4VoxelNavigation::ComputeStep
  // once a run has several events). So switch it off and build serially here, for the world
  // only. Do NOT flag the run manager (/run/geometryModified) either: that re-voxelises everything
  // on the workers the same way.
  void GeometryMoved(G4bool redraw = true)
  {
    G4VPhysicalVolume* world = G4TransportationManager::GetTransportationManager()->GetNavigatorForTracking()->GetWorldVolume();
    G4GeometryManager* geometry = G4GeometryManager::GetInstance();
    geometry->RequestParallelOptimisation(false, false);  // build the voxels now, here (see above)
    geometry->OpenGeometry(world);
    geometry->CloseGeometry(true, false, world);  // this volume only: serial
    if (redraw) G4UImanager::GetUIpointer()->ApplyCommand("/vis/viewer/rebuild");
  }

  // ---- the level's moving parts (doomworld.hh)

  // A message in the HUD (and the log)
  void Say(const G4String& text)
  {
    G4cout << text << G4endl;
    fMessage = text;
    fMessageTime = Now();
  }

  // Whether the player or a monster stands where sector's floor and ceiling would leave too
  // little room (a door closing on them, a lift rising into a low ceiling). Doom crushes the
  // dead and stops (or reverses) for the living.
  G4bool Blocked(int sector, double floor, double ceil)
  {
    double x, y, z;
    Camera cam;
    if (fHealth > 0 && GetCamera(cam, nullptr)) {
      fWorld->FromG4(G4ThreeVector(cam.eye.x(), cam.eye.y(), cam.eye.z()), x, y, z);
      if (ceil - floor < kPlayerHeight / kMapUnit && fWorld->Touches(sector, x, y, kPlayerRadius / kMapUnit)) return true;
    }
    G4bool blocked = false;
    Bestiary::Instance().ForEach([&](const G4String&, Bestiary::Monster& m) {
      if (blocked || m.health <= 0 || ceil - floor >= m.kind->top) return;
      fWorld->FromG4(m.at, x, y, z);
      blocked = fWorld->Touches(sector, x, y, m.kind->radius);
    });
    return blocked;
  }

  // The player's position and view angle in Doom's map (units, degrees anticlockwise from +x)
  G4bool DoomPosition(const Camera& cam, double& x, double& y, double& z, double& angle) const
  {
    if (!fWorld) return false;
    fWorld->FromG4(G4ThreeVector(cam.eye.x(), cam.eye.y(), cam.eye.z()), x, y, z);
    angle = std::atan2(cam.back.z(), -cam.back.x()) / deg;  // look = -back; Doom y = -G4 z
    return true;
  }

  // P_UseLines: open the door or press the switch ahead
  void Use()
  {
    if (fInter.active) {
      Accelerate();
      return;
    }
    if (fHealth <= 0) {  // as fire does: play again
      Restart();
      return;
    }
    Camera cam;
    double x, y, z, angle;
    if (!GetCamera(cam, "/doom/use")) return;
    if (!DoomPosition(cam, x, y, z, angle)) {
      G4cout << "/doom/use: no <map>_level.txt next to the GDML, so nothing in the level can be used" << G4endl;
      return;
    }
    fWorld->Use(x, y, angle);
  }

  // Run the level's thinkers for dt of game time (whole tics), then move their slabs, the
  // player and the monsters standing on them, and redraw
  void AdvanceWorld(G4double dt)
  {
    fWorldTime += dt;
    G4int tics = static_cast<G4int>(fWorldTime / kTic);
    fWorldTime -= tics * kTic;
    if (fInter.active) {
      for (; tics > 0 && fInter.active; --tics) IntermissionTic();
      return;
    }
    if (!fWorld) return;
    G4bool moved = false;
    for (; tics > 0; --tics) {
      moved |= fWorld->Tic();
      FloorSpecials();
    }
    if (moved) {
      const G4bool monsters = Ride(fWorld->UpdateGeometry());
      GeometryMoved(false);
      Redraw(monsters, true);
    }
    if (fFalling) Fall();
  }

  // Doom's sector specials under the player's feet: secrets, and floors that hurt (nukage,
  // lava) every 32 tics (P_PlayerInSpecialSector, no radiation suit here)
  void FloorSpecials()
  {
    Camera cam;
    double x, y, z, angle;
    if (fHealth <= 0 || !GetCamera(cam, nullptr) || !DoomPosition(cam, x, y, z, angle)) return;
    const G4int sector = fWorld->SectorAt(x, y);
    if (sector < 0 || z - kEyeHeight / kMapUnit > fWorld->Sectors()[sector].floor + 1) return;  // not on its floor
    fWorld->FindSecret(sector);
    static const std::map<G4int, G4int> kDamage = {{5, 10}, {7, 5}, {16, 20}, {4, 20}, {11, 20}};
    auto it = kDamage.find(fWorld->Sectors()[sector].special);
    if (it != kDamage.end() && (fWorld->LevelTime() & 31) == 0) Hurt(it->second, "The floor", G4ThreeVector(cam.eye.x(), cam.eye.y(), cam.eye.z()));
  }

  // Things ride the floors that moved: the player's eye and the monsters (alive or dead) whose
  // centre is over one of these sectors go to its new height. The camera is aimed, not redrawn.
  // Returns whether a monster moved.
  G4bool Ride(const std::vector<G4int>& sectors)
  {
    if (!fWorld || sectors.empty()) return false;
    const std::set<G4int> moved(sectors.begin(), sectors.end());
    double x, y, z;
    G4bool monsters = false;
    Bestiary::Instance().ForEach([&](const G4String&, Bestiary::Monster& m) {
      fWorld->FromG4(m.at, x, y, z);
      const G4int sector = fWorld->SectorAt(x, y);
      if (sector < 0 || !moved.count(sector)) return;
      G4ThreeVector at = m.at;
      at.setY(fWorld->ToG4(x, y, fWorld->Sectors()[sector].floor).y()
              + (m.painted ? kCorpseHeight / 2 + 0.5 * mm : m.kind->centre * kMapUnit));
      if (at == m.at) return;
      Bestiary::Place(m, at);
      monsters = true;
    });
    Camera cam;
    if (GetCamera(cam, nullptr)) {
      fWorld->FromG4(G4ThreeVector(cam.eye.x(), cam.eye.y(), cam.eye.z()), x, y, z);
      const G4int sector = fWorld->SectorAt(x, y);
      if (sector >= 0 && moved.count(sector)) {
        const G4double eyeY = fWorld->ToG4(x, y, fWorld->Sectors()[sector].floor).y()
                              + (fHealth > 0 ? kEyeHeight : kDeadEyeHeight);
        Aim(cam, G4Point3D(cam.eye.x(), eyeY, cam.eye.z()), cam.back);
      }
    }
    return monsters;
  }

  // Gunfire is loud: it wakes every monster within earshot, walls or not.
  void Noise(const G4Point3D& where)
  {
    const G4ThreeVector p(where.x(), where.y(), where.z());
    Bestiary::Instance().ForEach([&](const G4String& name, Bestiary::Monster& m) {
      if (m.health > 0 && !m.awake && m.kind->speed > 0 && (m.at - p).mag() < kEarshot) {  // not barrels
        m.awake = true;
        G4cout << name << " hears the shot!" << G4endl;
        Sound(Pick(kMonsterSounds.at(m.type).sight), &m.at);
      }
    });
  }

  // Play a WAD sound (a no-op without the Qt sound build). `from`: where it happens, nullptr
  // for the player's own sounds. Doom's falloff: full volume within 200 map units, silent
  // beyond 1200; panned by the direction it comes from.
  void Sound(const G4String& lump, const G4ThreeVector* from = nullptr, G4double gain = 1, G4double pitch = 1)
  {
    if ((!fPlay && !fLogSounds) || lump.empty()) return;
    G4double pan = 0;
    Camera cam;
    if (from && GetCamera(cam, nullptr)) {
      G4ThreeVector to = *from - G4ThreeVector(cam.eye.x(), cam.eye.y(), cam.eye.z());
      const G4double dist = to.mag(), close = 200 * kMapUnit, clip = 1200 * kMapUnit;
      if (dist >= clip) return;
      if (dist > close) gain *= (clip - dist) / (clip - close);
      const G4ThreeVector right(cam.back.z(), 0, -cam.back.x());  // view direction x up, level
      to.setY(0);
      if (right.mag2() > 0 && to.mag2() > 0) pan = to.unit().dot(right.unit());
    }
    if (fLogSounds) {
      std::ostringstream msg;
      msg.precision(2);
      msg << std::fixed << "[sound] " << lump << " gain " << gain << " pitch " << pitch << " pan " << pan;
      G4cout << msg.str() << G4endl;
    }
    if (fPlay) fPlay(lump, gain * fVolume, pitch, pan);
  }

  G4String Pick(const std::vector<G4String>& lumps)
  {
    return lumps.empty() ? G4String() : lumps[fRandom() % lumps.size()];
  }

  // Pain or death cries for the shot's hits, shaped by the energy deposited: louder and deeper
  // the more it took, so a hadronic shower sounds unlike a proton just ionising its way through.
  void VoiceHits()
  {
    for (const auto& hit : Bestiary::Instance().TakeHits()) {
      const auto& sounds = kMonsterSounds.at(hit.type);
      const G4double x = std::log10(hit.edep / (100. * MeV));  // ~0.4 ionising pass, ~1.5 shower
      const G4double gain = std::clamp(0.4 + 0.4 * x, 0.25, 1.), pitch = std::clamp(1.25 - 0.25 * x, 0.7, 1.4);
      const G4String lump =
        hit.gibbed && hit.type != "barrel" ? G4String("dsslop") : Pick(hit.killed ? sounds.death : sounds.pain);
      Sound(lump, &hit.at, gain, pitch);
    }
  }

  // Turn left by angle and look up by pitch, then step forward/right (along the floor),
  // sliding along walls like Doom does.
  void Move(G4double forward, G4double right, G4double angle, G4double pitch = 0)
  {
    if (fHealth <= 0 || fInter.active) return;  // the dead do not walk
    Camera cam;
    if (!GetCamera(cam, "/doom/move")) return;
    G4Vector3D back = cam.back;
    back.rotateY(angle);  // positive = left, with y up
    if (pitch != 0) back = Pitched(back, pitch);
    G4Point3D eye = cam.eye;
    if (forward != 0 || right != 0) {
      if (Clearance(eye, G4Vector3D(0, -1, 0)) < 0) {
        G4cerr << "/doom/move: the camera is not inside the level; use e1m1_camera.mac first" << G4endl;
        return;
      }
      // the whole step, else its parts: slides along a wall hit at an angle
      G4bool moved = Step(eye, back, forward, right) || (right != 0 && Step(eye, back, forward, 0))
                     || (forward != 0 && Step(eye, back, 0, right));
      if (!moved && !fBumped) {  // once per bump, not per key repeat
        G4cout << "*oof*" << G4endl;
        Sound("dsoof");
      }
      fBumped = !moved;
      if (!moved && angle == 0) return;
      if (moved && fWorld) {  // walk-over lines, keys picked up, teleporters
        double x0, y0, z0, x1, y1, z1;
        fWorld->FromG4(G4ThreeVector(cam.eye.x(), cam.eye.y(), cam.eye.z()), x0, y0, z0);
        fWorld->FromG4(G4ThreeVector(eye.x(), eye.y(), eye.z()), x1, y1, z1);
        fWorld->Cross(x0, y0, x1, y1, true);
        fWorld->Touch(x1, y1, z1 - kEyeHeight / kMapUnit, [this](const doom::Thing& t) { return Give(t); });
        if (fTeleport) {
          fTeleport = false;
          const G4ThreeVector from(eye.x(), eye.y(), eye.z());
          eye = fTeleportEye;
          back = fTeleportBack;
          Sound("dstelept", &from);
          Sound("dstelept");
        }
      }
    }
    SetCamera(cam, eye, back);
  }

  // The view direction (as viewpoint vector, pointing back) tilted up by pitch, clamped
  static G4Vector3D Pitched(const G4Vector3D& back, G4double pitch)
  {
    const G4Vector3D look = -back;
    const G4Vector3D level(look.x(), 0, look.z());
    if (level.mag2() == 0) return back;
    const G4double p = std::clamp(std::asin(std::clamp(look.y(), -1., 1.)) + pitch, -kMaxPitch, kMaxPitch);
    return -(std::cos(p) * level.unit() + std::sin(p) * G4Vector3D(0, 1, 0));
  }

  // Move eye by forward/right relative to the view, if the player fits there. The eye then
  // sits at Doom's view height above the new floor.
  G4bool Step(G4Point3D& eye, const G4Vector3D& back, G4double forward, G4double right)
  {
    const G4Vector3D up(0, 1, 0);  // the GDML is y-up
    G4Vector3D ahead = -back;
    ahead.setY(0);
    if (ahead.mag2() == 0) return false;  // looking straight up or down
    ahead = ahead.unit();
    G4Vector3D move = forward * ahead + right * ahead.cross(up);
    const G4double length = move.mag();
    if (length == 0) return false;
    move /= length;

    // walls and monsters at eye height, then the floor and headroom at the new spot
    const G4Point3D next = eye + length * move;
    if (Clearance(eye, move) < length + kPlayerRadius) return false;
    const G4double nextBelow = Clearance(next, -up);
    const G4double floor = next.y() - nextBelow;
    if (nextBelow < 0 || floor - (eye.y() - Clearance(eye, -up)) > kMaxStepUp) return false;
    const G4Point3D newEye(next.x(), floor + kEyeHeight, next.z());
    if (Clearance(newEye, up) < kPlayerHeight - kEyeHeight) return false;
    eye = newEye;
    return true;
  }

  void UpdateMusic()
  {
    if (fPlayMusic) fPlayMusic(fMusicOn ? fSong : G4String(), fMusicVolume);
  }

  void Where() const
  {
    Camera cam;
    if (!GetCamera(cam, "/doom/where")) return;
    G4cout << "eye " << cam.eye.x() << " " << cam.eye.y() << " " << cam.eye.z() << " mm, looking "
           << -cam.back.x() << " " << -cam.back.y() << " " << -cam.back.z() << G4endl;
    double x, y, z, angle;
    if (DoomPosition(cam, x, y, z, angle)) {
      const G4int sector = fWorld->SectorAt(x, y);
      G4cout << "map " << x << " " << y << ", eye height " << z << ", angle " << angle << " deg, sector " << sector;
      if (sector >= 0) G4cout << " (floor " << fWorld->Sectors()[sector].floor << ", ceiling " << fWorld->Sectors()[sector].ceil << ")";
      G4cout << G4endl;
    }
  }

  // Take the weapon in this Doom slot
  void Select(G4int slot)
  {
    if (fCharging) return;  // the BFG is going off: no switching now (Doom's weapon is not ready)
    const std::size_t i = WeaponIndex(slot);
    if (i >= kWeapons.size())
      G4cout << "No weapon in slot " << slot << G4endl;
    else if (!fOwned[i])
      G4cout << "You have no " << kWeapons[i].name << " yet" << G4endl;
    else {
      fWeapon = i;
      G4cout << "You take the " << kWeapons[i].name << G4endl;
    }
  }

  static std::size_t WeaponIndex(G4int slot)
  {
    return std::find_if(kWeapons.begin(), kWeapons.end(), [slot](const Weapon& w) { return w.slot == slot; })
           - kWeapons.begin();
  }

  // Doom's start: the pistol (our electron gun) and 50 bullets
  void PistolStart()
  {
    fOwned.assign(kWeapons.size(), false);
    fWeapon = WeaponIndex(2);
    fOwned[fWeapon] = true;
    fMaxAmmo = kMaxAmmo;
    fAmmo = {50, 0, 0, 0};
  }

  // P_GiveAmmo: clips of this ammo (0: half a clip), up to the most we can carry. A first
  // round for an empty weapon switches from the pistol to a better gun that uses it.
  G4bool GiveAmmo(AmmoType ammo, G4int clips)
  {
    if (fAmmo[ammo] >= fMaxAmmo[ammo]) return false;
    const G4int old = fAmmo[ammo];
    fAmmo[ammo] = std::min(fMaxAmmo[ammo], old + (clips ? clips * kClipAmmo[ammo] : kClipAmmo[ammo] / 2));
    if (old == 0 && CurrentWeapon().slot == 2) {
      if (ammo == kShell && Owns(3)) Select(3);
      if (ammo == kCell && Owns(6)) Select(6);
    }
    return true;
  }

  // P_TouchSpecialThing for weapons and ammo: whether the thing is taken
  G4bool Give(const doom::Thing& t)
  {
    if (t.type == kBackpack) {  // twice the ammo you can carry, and a clip of each
      if (fMaxAmmo == kMaxAmmo)
        for (G4int a = 0; a < kAmmoTypes; ++a) fMaxAmmo[a] *= 2;
      for (G4int a = 0; a < kAmmoTypes; ++a) GiveAmmo(AmmoType(a), 1);
      Say("Picked up a backpack full of ammo!");
      Sound("dsitemup");
      return true;
    }
    if (auto supply = kSupplies.find(t.type); supply != kSupplies.end()) return Supplies(t.type, supply->second);
    auto it = kPickups.find(t.type);
    if (it == kPickups.end()) return false;
    const Pickup& p = it->second;
    if (!p.slot) {
      if (!GiveAmmo(p.ammo, t.dropped ? 0 : p.clips)) return false;
      Say(p.message);
      Sound("dsitemup");
      return true;
    }
    const std::size_t i = WeaponIndex(p.slot);  // P_GiveWeapon: the gun and its ammo
    const G4bool ammo = GiveAmmo(p.ammo, t.dropped ? 1 : p.clips), weapon = !fOwned[i];
    if (!ammo && !weapon) return false;
    fOwned[i] = true;
    Say("You got the " + kWeapons[i].name + "!");
    Sound("dswpnup");
    if (weapon) Select(p.slot);  // a new gun goes straight into your hands
    return true;
  }

  // Health and armour as Doom gives them
  G4bool Supplies(G4int type, const Supply& p)
  {
    if (fHealth <= 0) return false;
    if (type == 2018 || type == 2019) {  // armour suits replace a weaker one, never a stronger one
      if (fArmor >= p.armorMax) return false;
      fArmor = p.armor;
      fArmorType = p.armorType;
    }
    else if (p.armor > 0) {  // bonus, megasphere: added up to the limit
      fArmor = std::min(fArmor + p.armor, p.armorMax);
      if (fArmorType == 0 || type == 83) fArmorType = p.armorType;
    }
    if (p.health > 0) {
      if (!p.always && fHealth >= p.healthMax) return false;
      const G4bool needed = fHealth < 25;
      fHealth = type == 83 ? p.health : std::min(fHealth + p.health, p.healthMax);
      if (type == 2012 && needed) {
        Say("Picked up a medikit that you REALLY need!");
        Sound(p.sound);
        return true;
      }
    }
    Say(p.message);
    Sound(p.sound);
    return true;
  }

  // After a shot: the cries, the dead laid down, what they drop (zombies their clip, shotgun
  // guys their shotgun), and barrels going off, which may kill more (barrels too)
  void Aftermath()
  {
    G4bool laid = false;
    for (G4int round = 0; round < 32; ++round) {
      VoiceHits();
      laid |= Bestiary::Instance().LayCorpses();
      const auto deaths = Bestiary::Instance().TakeDeaths();
      if (deaths.empty()) break;
      for (const auto& [type, at] : deaths) {
        if (type == "barrel") Blast(at);
        auto drop = kDrops.find(type);
        if (!fWorld || drop == kDrops.end()) continue;
        double x, y, z;
        fWorld->FromG4(at, x, y, z);
        fWorld->Drop(drop->second, x, y);
      }
    }
    if (laid) GeometryMoved();
  }

  // P_RadiusAttack: 128 damage at the barrel, one less per map unit of distance (the larger of
  // dx and dy, less the target's radius), to everyone it can see
  void Blast(const G4ThreeVector& corpse)
  {
    const G4ThreeVector spot(corpse.x(), corpse.y() - kCorpseHeight / 2 + 21 * kMapUnit, corpse.z());  // its middle
    auto damage = [&](const G4ThreeVector& target, G4double radius) {
      const G4double dist = std::max(0., std::max(std::abs(target.x() - spot.x()), std::abs(target.z() - spot.z())) / kMapUnit - radius);
      if (dist >= 128) return 0.;
      const G4ThreeVector to = target - spot;
      if (to.mag() > 1 * mm && Clearance(G4Point3D(spot), G4Vector3D(to), nullptr) < to.mag() - (radius + 1) * kMapUnit) return 0.;
      return 128 - dist;
    };
    Bestiary::Instance().ForEach([&](const G4String& name, Bestiary::Monster& m) {
      if (m.health <= 0) return;
      const G4double hp = damage(m.at, m.kind->radius);
      if (hp > 0) Bestiary::Instance().Harm(name, m, hp);
    });
    Camera cam;
    if (fHealth > 0 && GetCamera(cam, nullptr)) {
      const G4ThreeVector body(cam.eye.x(), cam.eye.y() - kEyeHeight + kPlayerHeight / 2, cam.eye.z());
      const G4double hp = damage(body, kPlayerRadius / kMapUnit);
      if (hp > 0) Hurt(G4int(hp), "The barrel", spot);
    }
  }

  // ---- levels

  static G4String Lower(G4String s)
  {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }

  // The World of the current map, from <map>_level.txt
  void LoadWorld()
  {
    const auto file = fLevelDir / (Lower(fMap) + "_level.txt");
    std::unique_ptr<DoomWorld> world;
    if (std::filesystem::exists(file)) {
      world = std::make_unique<DoomWorld>(file.string());
      world->Bind(G4TransportationManager::GetTransportationManager()->GetNavigatorForTracking()->GetWorldVolume()->GetLogicalVolume());
      G4cout << "Level " << fMap << ": " << world->Sectors().size() << " sectors, " << world->Lines().size() << " lines" << G4endl;
    }
    else
      G4cout << "No " << file.string() << ": doors and switches will not work (run doom2gdml.py again)" << G4endl;
    gWorld = world.get();
    SetWorld(world.get());
    if (fWorldChanged) fWorldChanged(world.get());
    fWorldOwned = std::move(world);  // the old one goes now, nobody points to it any more
  }

  // The maps converted next to this one (<map>.gdml), in Doom's order
  std::vector<G4String> AvailableMaps() const
  {
    std::vector<G4String> maps;
    static const std::regex name("E[1-9]M[1-9]|MAP[0-9][0-9]");
    for (const auto& entry : std::filesystem::directory_iterator(fLevelDir)) {
      if (entry.path().extension() != ".gdml") continue;
      G4String map = entry.path().stem().string();
      for (auto& c : map) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      if (std::regex_match(map, name)) maps.push_back(map);
    }
    std::sort(maps.begin(), maps.end());
    return maps;
  }

  // G_DoCompleted: the next map. Secret exits lead to E?M9 (MAP31, then MAP32), which return to
  // E1M4, E2M6, E3M7, E4M3 (MAP16); after E?M8 comes the next episode. A map that was not
  // converted: the next one that was, round to the first again.
  G4String NextMap(G4bool secret) const
  {
    const auto maps = AvailableMaps();
    auto has = [&](const G4String& m) { return std::find(maps.begin(), maps.end(), m) != maps.end(); };
    auto em = [](G4int e, G4int m) { return "E" + std::to_string(e) + "M" + std::to_string(m); };
    auto map = [](G4int n) { return G4String(n < 10 ? "MAP0" : "MAP") + std::to_string(n); };
    G4String want;
    G4int e = 0, m = 0;
    if (std::sscanf(fMap.c_str(), "E%dM%d", &e, &m) == 2) {
      static const G4int kSecretReturn[] = {1, 4, 6, 7, 3};
      if (secret && has(em(e, 9)))
        want = em(e, 9);
      else if (m == 9)
        want = em(e, e >= 1 && e <= 4 ? kSecretReturn[e] : 1);
      else
        want = m == 8 ? em(e + 1, 1) : em(e, m + 1);
    }
    else if (std::sscanf(fMap.c_str(), "MAP%d", &m) == 1) {
      if (secret && m == 15 && has(map(31)))
        want = map(31);
      else if (secret && m == 31 && has(map(32)))
        want = map(32);
      else
        want = m == 31 || m == 32 ? map(16) : map(m + 1);
    }
    if (has(want)) return want;
    std::vector<G4String> order;  // the secret levels only through their exits
    for (const auto& mm : maps)
      if (mm.back() != '9' || mm[0] != 'E') {
        if (mm != "MAP31" && mm != "MAP32") order.push_back(mm);
      }
    for (const auto& mm : order)
      if (mm > fMap) return mm;
    return order.empty() ? fMap : order.front();
  }

  // Play another level: the geometry is rebuilt from its GDML (the old volumes are deleted),
  // with its monsters, World and camera. The player keeps health, armour, weapons and ammo; the
  // keys stay behind, as in Doom.
  void LoadLevel(const G4String& map)
  {
    const auto gdml = fLevelDir / (Lower(map) + ".gdml");
    if (!fLevel || !std::filesystem::exists(gdml)) {
      G4cout << "No level " << map << " (" << gdml.string() << "): convert it with doom2gdml.py" << G4endl;
      return;
    }
    G4UImanager* ui = G4UImanager::GetUIpointer();
    fInter.active = false;
    // nothing may draw or point into the old level while it is replaced
    ui->ApplyCommand("/vis/disable");
    fMissiles.clear();
    fBursts.clear();
    fCharging = fTeleport = fFalling = false;
    Bestiary::Instance().Clear();
    gWorld = nullptr;
    SetWorld(nullptr);
    if (fWorldChanged) fWorldChanged(nullptr);
    fWorldOwned.reset();
    fLevelLooks.clear();
    fLevel->SetFile(gdml.string());
    // true: delete the old volumes first. Workers get the flag with the next run's commands and
    // copy the new volumes' per-thread data from the master then. Not /run/initialize: the
    // task run manager's BeamOn(0) makes the workers set up the new world before that copy
    // (GeomNav0002 "Volume must be centered on the origin").
    ui->ApplyCommand("/run/reinitializeGeometry true");
    G4RunManager::GetRunManager()->InitializeGeometry();
    ui->ApplyCommand("/doom/worker/geometry");
    fMap = map;
    LoadWorld();
    ApplyLook();
    GeometryMoved(false);
    // a new scene with the new world, the sprites and missiles, the tracks, and the camera at
    // the new player start
    ui->ApplyCommand("/vis/drawVolume");
    ui->ApplyCommand("/vis/scene/add/userAction");
    ui->ApplyCommand("/vis/scene/add/trajectories smooth");
    ui->ApplyCommand("/vis/scene/endOfEventAction accumulate");
    ui->ApplyCommand("/control/execute " + (fLevelDir / (Lower(map) + "_camera.mac")).string());
    SettleCamera();
    ui->ApplyCommand("/vis/enable");
    if (fTexturesOn) {
      fTexturesOn = false;
      SetTextures(true);  // hides the new level's volumes, rebuilds
    }
    else
      ui->ApplyCommand("/vis/viewer/rebuild");
    fSong = SongForMap(map);
    UpdateMusic();
    Say("Entering " + map);
  }

  // Doom's par times (seconds): E1-E3, and Doom II's MAP01-MAP32
  G4int ParTime(const G4String& map) const
  {
    static const G4int kPars[3][9] = {{30, 75, 120, 90, 165, 180, 180, 30, 165},
                                      {90, 90, 90, 120, 90, 360, 240, 30, 170},
                                      {90, 45, 90, 150, 90, 90, 165, 30, 135}};
    static const G4int kCPars[32] = {30,  90,  120, 120, 90,  150, 120, 120, 270, 90,  210,
                                     150, 150, 150, 210, 150, 420, 150, 210, 150, 240, 150,
                                     180, 150, 150, 300, 330, 420, 300, 180, 120, 30};
    G4int e = 0, m = 0;
    if (std::sscanf(map.c_str(), "E%dM%d", &e, &m) == 2) return e >= 1 && e <= 3 && m >= 1 && m <= 9 ? kPars[e - 1][m - 1] : -1;
    if (std::sscanf(map.c_str(), "MAP%d", &m) == 1 && m >= 1 && m <= 32) return kCPars[m - 1];
    return -1;
  }

  // The level is done (its exit, or /doom/next): the tally
  void StartIntermission(G4bool secret)
  {
    if (fInter.active) return;
    fInter = Intermission();
    fInter.active = true;
    fInter.stage = 1;
    fInter.pause = 35;
    fInter.map = fMap;
    fInter.next = NextMap(secret);
    const auto [kills, monsters] = Bestiary::Instance().Kills();
    fInter.kills = std::max(kills, 0) * 100 / std::max(monsters, 1);
    if (fWorld) {
      const auto [found, total] = fWorld->Items();
      fInter.items = found * 100 / std::max(total, 1);
      fInter.secrets = fWorld->SecretsFound() * 100 / std::max(fWorld->SecretsTotal(), 1);
      fInter.time = fWorld->LevelTime() / 35;
    }
    fInter.par = ParTime(fMap);
    fCharging = false;
    G4cout << fMap << " finished: kills " << fInter.kills << "%, items " << fInter.items << "%, secrets "
           << fInter.secrets << "%, time " << fInter.time << " s; next " << fInter.next
           << " (fire or use to go on)" << G4endl;
    fSong = fMap[0] == 'E' ? "D_INTER" : "D_DM2INT";
    UpdateMusic();
  }

  // One tic of WI_updateStats / WI_updateShowNextLoc / WI_updateNoState
  void IntermissionTic()
  {
    Intermission& w = fInter;
    ++w.tic;
    auto count = [&](G4int& shown, G4int target, G4int step) {
      if (!(w.tic & 3)) Sound("dspistol");
      shown = std::min(shown + step, target);
      if (shown < target) return;
      Sound("dsbarexp");
      ++w.stage;
    };
    if (w.stage < 10 && (w.stage & 1)) {  // a pause before the next line
      if (--w.pause <= 0) {
        ++w.stage;
        w.pause = 35;
      }
    }
    else if (w.stage == 2)
      count(w.cKills, w.kills, 2);
    else if (w.stage == 4)
      count(w.cItems, w.items, 2);
    else if (w.stage == 6)
      count(w.cSecrets, w.secrets, 2);
    else if (w.stage == 8) {
      if (!(w.tic & 3)) Sound("dspistol");
      w.cTime = std::min(w.cTime + 3, w.time);
      if (w.par >= 0) w.cPar = std::min(w.cPar + 3, w.par);
      if (w.cTime >= w.time && w.cPar >= w.par) {
        Sound("dsbarexp");
        w.stage = 10;
      }
    }
    else if (w.stage == 11) {  // the map, "you are here" blinking; on by itself after 4 s
      w.pointer = (w.count & 31) < 20;
      if (--w.count <= 0) {
        w.stage = 12;
        w.count = 10;
      }
    }
    else if (w.stage == 12 && --w.count <= 0)
      LoadLevel(w.next);
  }

  // Fire or use during the tally: finish counting, else go on
  void Accelerate()
  {
    Intermission& w = fInter;
    if (w.stage < 10) {
      w.cKills = w.kills;
      w.cItems = w.items;
      w.cSecrets = w.secrets;
      w.cTime = w.time;
      w.cPar = w.par;
      w.stage = 10;
      Sound("dsbarexp");
    }
    else if (w.stage == 10) {
      Sound("dssgcock");
      if (w.map[0] == 'E') {  // Doom's episodes show the map with the next level
        w.stage = 11;
        w.count = 4 * 35;
      }
      else {
        w.stage = 12;
        w.count = 10;
      }
    }
    else if (w.stage == 11) {
      w.stage = 12;
      w.count = 10;
    }
  }

  // Doom's P_CheckAmmo: out of ammo, change to the best weapon that still has some
  G4bool Reload()
  {
    if (fAmmo[CurrentWeapon().ammo] >= CurrentWeapon().ammoUse) return true;
    for (std::size_t i = kWeapons.size(); i-- > 0;)
      if (fOwned[i] && !Dangerous(kWeapons[i]) && fAmmo[kWeapons[i].ammo] >= kWeapons[i].ammoUse) {
        Select(kWeapons[i].slot);
        return true;
      }
    Say("Out of ammo! Find some (or /doom/idkfa)");
    return false;
  }

  // n shots with the weapon in hand: its particles, at its energy, pellets and spread, from
  // the camera along the view direction
  void Fire(G4int n)
  {
    if (fInter.active) {  // the tally: a key press hurries it on, a held trigger does not
      const auto now = std::chrono::steady_clock::now();
      if (now - fLastShot > std::chrono::duration<G4double>(1.5 * Refire())) Accelerate();
      fLastShot = now;
      return;
    }
    if (fHealth <= 0) {  // as in Doom, a key press restarts
      Restart();
      return;
    }
    // Doom's BFG: the trigger starts the sound, and the shot leaves `charge` tics later
    G4bool charged = false;
    if (fCharging) {
      if (!ChargeDue()) return;  // still charging: the trigger does nothing
      fCharging = false;
      n = fChargeShots;
      charged = true;
    }
    else if (fChargeDeferred && CurrentWeapon().charge > 0 && fAmmo[CurrentWeapon().ammo] >= CurrentWeapon().ammoUse) {
      fCharging = true;
      fChargeStart = std::chrono::steady_clock::now();
      fChargeShots = n;
      Sound(CurrentWeapon().sound);
      return;
    }
    if (!Reload()) return;
    Camera cam;
    if (!GetCamera(cam, "/doom/fire")) return;
    const G4Vector3D back = cam.back;
    const G4Point3D muzzle = cam.eye - fDrop * cam.vp.GetUpVector().unit();

    auto* world = G4TransportationManager::GetTransportationManager()->GetNavigatorForTracking()->GetWorldVolume();
    if (world->GetLogicalVolume()->GetSolid()->Inside(G4ThreeVector(muzzle.x(), muzzle.y(), muzzle.z())) == kOutside) {
      G4cerr << "/doom/fire: the camera is outside the level; move in first" << G4endl;
      return;
    }

    const Weapon& w = CurrentWeapon();
    const G4int shots = std::min(n, fAmmo[w.ammo] / w.ammoUse);
    fAmmo[w.ammo] -= shots * w.ammoUse;
    // a held trigger (the last shot just one refire ago) loses Doom's first-shot accuracy
    const auto now = std::chrono::steady_clock::now();
    const G4bool burst = now - fLastShot < std::chrono::duration<G4double>(1.5 * Refire());
    fLastShot = now;
    gSpread = w.accurate && !burst && n == 1 ? 0. : w.spread;

    std::ostringstream particle, energy;
    if (w.particle.rfind("ion ", 0) == 0)  // a nucleus: /gun/particle ion, then /gun/ion Z A
      particle << "/gun/particle ion\n/gun/" << w.particle;
    else
      particle << "/gun/particle " << w.particle;
    energy << "/gun/energy " << w.energy / MeV << " MeV";
    gMuzzle = muzzle;
    gAim = -back.unit();
    G4UImanager* ui = G4UImanager::GetUIpointer();
    std::istringstream commands(particle.str() + "\n" + energy.str());
    for (std::string command; std::getline(commands, command);) ui->ApplyCommand(command);
    if (!charged) Sound(w.sound);
    ui->ApplyCommand("/run/beamOn " + std::to_string(shots * w.pellets));
    fShots += shots;
    Aftermath();
    Noise(cam.eye);
    Reload();  // Doom changes weapon as soon as the ammo runs out
  }

  G4VisManager* fVis;
  G4double fDrop = 300. * mm;
  G4UIdirectory* fDir;
  G4UIcmdWithAnInteger* fFire;
  G4UIcmdWithADoubleAndUnit* fGunDrop;
  G4UIcmdWithADoubleAndUnit* fEnergyPerHP;
  G4UIcmdWithoutParameter* fStatus;
  G4UIcmdWithoutParameter* fReset;
  G4UIcmdWithADoubleAndUnit* fWalk;
  G4UIcmdWithADoubleAndUnit* fStrafe;
  G4UIcmdWithADoubleAndUnit* fTurn;
  G4UIcmdWithoutParameter* fWhere;
  G4UIcommand* fMove;
  G4UIcmdWithADouble* fSensitivity;
  G4double fMouseSensitivity = 0.15;
  G4UIcmdWithABool* fMonsters;
  G4UIcmdWithADoubleAndUnit* fThink;
  G4bool fMonstersActive = false;
  G4UIcmdWithADouble* fVolumeCmd;
  G4double fVolume = 0.7;
  G4UIcmdWithABool* fSoundLog;
  G4bool fLogSounds = false;
  G4UIcmdWithAString* fMusicCmd;
  G4UIcmdWithADouble* fMusicVolumeCmd;
  G4String fSong;
  G4bool fMusicOn = true;
  G4double fMusicVolume = 0.5;
  MusicPlayer fPlayMusic;
  G4UIcmdWithABool* fGodCmd;
  G4bool fGod = false;
  G4int fHealth = kPlayerHealth;
  G4int fArmor = 0, fArmorType = 0;  // armour points, and 1 green (saves 1/3) or 2 blue (1/2)
  G4int fDamageTaken = 0;
  G4bool fFalling = false;
  G4int fHitSide = 0;
  G4UIcmdWithABool* fCrosshairCmd;
  G4bool fCrosshair = true;
  G4UIcmdWithAnInteger* fWeaponCmd;
  G4UIcmdWithoutParameter* fIdkfa;
  G4UIcmdWithAString* fFaceCmd;
  G4String fFace = "doomguy";
  G4UIcmdWithAString* fHudStyleCmd;
  G4String fHudStyle = "lhc";
  G4UIcmdWithAString* fWeaponStyleCmd;
  G4String fWeaponStyle = "doom";  // instruments: a baseline, not the default yet
  G4UIcmdWithABool* fSpritesCmd;
  G4UIcmdWithABool* fHitboxesCmd;
  G4UIcmdWithoutParameter* fAnimateCmd;
  G4UIcmdWithABool* fTexturesCmd;
  G4bool fTexturesOn = false, fTexturesAvailable = false;
  std::map<G4LogicalVolume*, const G4VisAttributes*> fLevelLooks;  // while textured: the hidden volumes' own
  G4bool fSpritesOn = true, fHitboxes = false;
  std::function<G4bool()> fStale;                     // a rebuild would change the sprites
  std::function<G4bool(const G4String&)> fSprited;    // that monster type has sprites
  // set by PistolStart (constructor, restarts): the weapon in hand, the ones owned, the ammo
  std::size_t fWeapon = 0;
  std::vector<G4bool> fOwned;
  std::array<G4int, kAmmoTypes> fAmmo{}, fMaxAmmo = kMaxAmmo;  // fMaxAmmo: doubled by a backpack
  G4int fShots = 0;
  std::chrono::steady_clock::time_point fLastShot;
  G4bool fChargeDeferred = false, fCharging = false;
  std::chrono::steady_clock::time_point fChargeStart;
  G4int fChargeShots = 0;
  std::vector<Missile> fMissiles;
  std::vector<Burst> fBursts;
  static constexpr G4double kMissileLife = 10. * s;
  SoundPlayer fPlay;
  std::minstd_rand fRandom;  // sound variants and monster attacks; keeps the physics random engine untouched
  static constexpr G4double kEarshot = 25. * m;
  G4Navigator fNav;  // our own, so collision checks never disturb the tracking navigator
  G4bool fBumped = false;
  G4UIcmdWithoutParameter* fUseCmd;
  G4UIcmdWithADoubleAndUnit* fWorldCmd;
  DoomWorld* fWorld = nullptr;
  G4double fWorldTime = 0;  // game time not yet run as a whole tic
  G4String fMessage;
  G4double fMessageTime = -1e9;
  G4UIcmdWithAString* fMapCmd;
  G4UIcmdWithoutParameter* fNextCmd;
  DoomLevel* fLevel = nullptr;
  std::filesystem::path fLevelDir;  // where the levels are
  G4String fMap;               // the one being played, e.g. "E1M1"
  std::unique_ptr<DoomWorld> fWorldOwned;
  std::function<void(const DoomWorld*)> fWorldChanged;
  Intermission fInter;
  G4bool fTeleport = false;  // a teleporter fired during the step: go here instead
  G4Point3D fTeleportEye;
  G4Vector3D fTeleportBack;
};

#ifdef DOOM_SOUND
// Software mixer for the WAD sounds (sounds/*.wav, written by doom2gdml.py): voices with gain,
// pan and pitch (by resampling), written to a PulseAudio stream by its own thread, so audio
// keeps flowing while the GUI thread is busy tracking a shot.
class DoomSound
{
 public:
  explicit DoomSound(const std::filesystem::path& dir, const std::filesystem::path& musicDir)
    : fMusicDir(musicDir)
  {
    const bool music = fMusic.LoadInstruments(musicDir / "GENMIDI.op2");
    std::error_code ec;
    for (const auto& file : std::filesystem::directory_iterator(dir, ec)) {
      Sample sample;
      if (file.path().extension() == ".wav" && LoadWav(file.path(), sample))
        fSamples[file.path().stem().string()] = std::move(sample);
    }
    if (fSamples.empty())
      G4cout << "No sounds in " << dir << ": run doom2gdml.py to extract them from the WAD, and copy "
             << "the sounds/ folder next to the GDML file." << G4endl;
    if (!music)
      G4cout << "No music instruments (" << musicDir / "GENMIDI.op2" << "): run doom2gdml.py, and copy the "
             << "music/ folder next to the GDML file." << G4endl;
    if (fSamples.empty() && !music) return;
    // Through PulseAudio (or PipeWire's pulse server), not Qt Multimedia: Qt starts its audio
    // threads with a 1 MiB stack, smaller than Geant4's static TLS (1.2 MB, mostly
    // libG4processes_hadronic), so glibc refuses them and QMediaDevices waits forever.
    const pa_sample_spec spec{PA_SAMPLE_FLOAT32NE, static_cast<std::uint32_t>(kRate), 2};
    pa_buffer_attr buffer;
    buffer.maxlength = buffer.prebuf = buffer.minreq = buffer.fragsize = static_cast<std::uint32_t>(-1);
    buffer.tlength = pa_usec_to_bytes(50000, &spec);  // 50 ms: low latency, no underruns
    int error = 0;
    fStream = pa_simple_new(nullptr, "doomviewer", PA_STREAM_PLAYBACK, nullptr, "Doom sound effects", &spec,
                            nullptr, &buffer, &error);
    if (!fStream) {
      G4cout << "No audio output (" << pa_strerror(error) << "): playing without sound." << G4endl;
      return;
    }
    fThread = std::thread([this] {  // default stack size, unlike Qt's audio threads
      std::vector<float> block(2 * 256);  // ~6 ms of stereo frames
      while (!fStop) {
        Mix(block.data(), block.size() / 2);
        if (pa_simple_write(fStream, block.data(), block.size() * sizeof(float), nullptr) < 0) break;
      }
    });
    G4cout << "Sound: " << fSamples.size() << " WAD sounds" << (music ? ", OPL3 music" : "") << ", " << kRate
           << " Hz output" << G4endl;
  }

  ~DoomSound()
  {
    fStop = true;
    if (fThread.joinable()) fThread.join();
    if (fStream) pa_simple_free(fStream);
  }

  // Play a song from the music folder (e.g. "D_E1M1"), looping; empty: stop. GUI thread.
  void Music(const G4String& song, double volume)
  {
    fMusicGain = static_cast<float>(volume);
    if (song == fSong) return;
    if (song.empty())
      fMusic.Stop();
    else if (!fMusic.Load(fMusicDir / (song + ".mid"))) {
      G4cout << "No song " << fMusicDir / (song + ".mid") << G4endl;
      return;
    }
    fSong = song;
  }

  // Start a sound. Any thread; in practice the GUI (master) thread.
  void Play(const G4String& lump, double gain, double pitch, double pan)
  {
    auto it = fSamples.find(lump);
    if (!fStream || it == fSamples.end()) return;
    const float left = gain * std::sqrt(0.5 * (1 - pan)), right = gain * std::sqrt(0.5 * (1 + pan));  // equal power
    std::lock_guard<std::mutex> lock(fMutex);
    if (fVoices.size() >= kMaxVoices) fVoices.erase(fVoices.begin());  // the oldest makes way
    fVoices.push_back({&it->second, 0., it->second.rate / kRate * pitch, left, right});
  }

 private:
  struct Sample
  {
    std::vector<float> data;  // mono, -1..1
    double rate = 0;
  };

  struct Voice
  {
    const Sample* sample;
    double pos, step;  // in source samples; step = source rate / output rate * pitch
    float left, right;
  };

  // Mix the playing voices into frames of interleaved stereo floats
  void Mix(float* out, std::size_t frames)
  {
    std::fill(out, out + 2 * frames, 0.f);
    fMusic.Render(out, frames, fMusicGain);  // its own lock: a song change does not hold up sounds
    std::lock_guard<std::mutex> lock(fMutex);
    for (std::size_t f = 0; f < frames; ++f) {
      float left = out[2 * f], right = out[2 * f + 1];
      for (auto& v : fVoices) {
        const auto& s = v.sample->data;
        const std::size_t i = static_cast<std::size_t>(v.pos);
        if (i + 1 >= s.size()) continue;
        const float x = s[i] + (s[i + 1] - s[i]) * static_cast<float>(v.pos - i);  // linear interpolation
        left += x * v.left;
        right += x * v.right;
        v.pos += v.step;
      }
      out[2 * f] = std::clamp(left, -1.f, 1.f);
      out[2 * f + 1] = std::clamp(right, -1.f, 1.f);
    }
    fVoices.erase(std::remove_if(fVoices.begin(), fVoices.end(),
                                 [](const Voice& v) { return v.pos + 1 >= v.sample->data.size(); }),
                  fVoices.end());
  }

  // Mono 8-bit (Doom's) or 16-bit PCM WAV
  static bool LoadWav(const std::filesystem::path& file, Sample& out)
  {
    std::ifstream in(file, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4))
      return false;
    std::uint16_t channels = 0, bits = 0;
    for (std::size_t p = 12; p + 8 <= bytes.size();) {
      std::uint32_t size;
      std::memcpy(&size, &bytes[p + 4], 4);
      size = std::min<std::size_t>(size, bytes.size() - p - 8);
      const char* body = &bytes[p + 8];
      if (!std::memcmp(&bytes[p], "fmt ", 4) && size >= 16) {
        std::uint32_t rate;
        std::memcpy(&channels, body + 2, 2);
        std::memcpy(&rate, body + 4, 4);
        std::memcpy(&bits, body + 14, 2);
        out.rate = rate;
      }
      else if (!std::memcmp(&bytes[p], "data", 4) && channels == 1) {
        if (bits == 8)
          for (std::size_t i = 0; i < size; ++i) out.data.push_back((static_cast<std::uint8_t>(body[i]) - 128) / 128.f);
        else if (bits == 16)
          for (std::size_t i = 0; i + 1 < size; i += 2) {
            std::int16_t s;
            std::memcpy(&s, body + i, 2);
            out.data.push_back(s / 32768.f);
          }
        return out.data.size() > 1 && out.rate > 0;
      }
      p += 8 + size + (size & 1);
    }
    return false;
  }

  static constexpr std::size_t kMaxVoices = 16;
  std::map<G4String, Sample> fSamples;  // by lump name, e.g. "dsplasma"; read-only once loaded
  std::vector<Voice> fVoices;
  std::mutex fMutex;  // fVoices: Play on the GUI thread, Mix on the audio thread
  static constexpr double kRate = 44100;
  DoomMusic fMusic{kRate};
  std::filesystem::path fMusicDir;
  G4String fSong;  // playing now (GUI thread)
  std::atomic<float> fMusicGain{0.5f};
  pa_simple* fStream = nullptr;
  std::thread fThread;
  std::atomic<bool> fStop{false};
};
#endif

#ifdef DOOM_WAYLAND
// Pointer lock on native Wayland (zwp_pointer_constraints_v1 + zwp_relative_pointer_v1), which
// Qt does not expose. Our objects live on a private event queue: Qt's reader thread fills it
// along with its own, and Dispatch() runs our handlers on the GUI thread, once per tick.
class WaylandLock
{
 public:
  // nullptr if the window has no Wayland surface or the compositor lacks the protocols
  static std::unique_ptr<WaylandLock> Create(QWidget* view)
  {
    auto* app = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    QWindow* window = view->window()->windowHandle();
    if (!app || !window || !app->pointer()) return nullptr;
    auto* surface = static_cast<wl_surface*>(
      QGuiApplication::platformNativeInterface()->nativeResourceForWindow("surface", window));
    auto lock = std::make_unique<WaylandLock>(app->display());
    if (!surface || !lock->fConstraints || !lock->fRelative) return nullptr;
    lock->fLock = zwp_pointer_constraints_v1_lock_pointer(lock->fConstraints, surface, app->pointer(), nullptr,
                                                          ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
    lock->fMotion = zwp_relative_pointer_manager_v1_get_relative_pointer(lock->fRelative, app->pointer());
    zwp_locked_pointer_v1_add_listener(lock->fLock, &kLockListener, lock.get());
    zwp_relative_pointer_v1_add_listener(lock->fMotion, &kMotionListener, lock.get());
    wl_display_flush(app->display());
    return lock;
  }

  explicit WaylandLock(wl_display* display) : fDisplay(display), fQueue(wl_display_create_queue(display))
  {
    auto* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), fQueue);
    fRegistry = wl_display_get_registry(wrapper);  // on our queue, and so is all it binds
    wl_proxy_wrapper_destroy(wrapper);
    wl_registry_add_listener(fRegistry, &kRegistryListener, this);
    wl_display_roundtrip_queue(fDisplay, fQueue);
  }

  ~WaylandLock()
  {
    if (fMotion) zwp_relative_pointer_v1_destroy(fMotion);
    if (fLock) zwp_locked_pointer_v1_destroy(fLock);
    if (fRelative) zwp_relative_pointer_manager_v1_destroy(fRelative);
    if (fConstraints) zwp_pointer_constraints_v1_destroy(fConstraints);
    wl_registry_destroy(fRegistry);
    wl_display_flush(fDisplay);
    wl_event_queue_destroy(fQueue);
  }

  // Run the handlers for what arrived since the last call; returns the motion summed up.
  QPointF Dispatch()
  {
    wl_display_dispatch_queue_pending(fDisplay, fQueue);
    const QPointF delta = fDelta;
    fDelta = QPointF();
    return delta;
  }

  // The compositor ended the lock (focus moved away); a one-shot lock does not come back.
  bool Lost() const { return fLost; }

 private:
  static void Global(void* data, wl_registry* registry, uint32_t name, const char* iface, uint32_t)
  {
    auto* self = static_cast<WaylandLock*>(data);
    if (std::strcmp(iface, zwp_pointer_constraints_v1_interface.name) == 0)
      self->fConstraints = static_cast<zwp_pointer_constraints_v1*>(
        wl_registry_bind(registry, name, &zwp_pointer_constraints_v1_interface, 1));
    else if (std::strcmp(iface, zwp_relative_pointer_manager_v1_interface.name) == 0)
      self->fRelative = static_cast<zwp_relative_pointer_manager_v1*>(
        wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
  }
  static void GlobalRemove(void*, wl_registry*, uint32_t) {}
  static void Locked(void*, zwp_locked_pointer_v1*) {}
  static void Unlocked(void* data, zwp_locked_pointer_v1*) { static_cast<WaylandLock*>(data)->fLost = true; }
  static void Motion(void* data, zwp_relative_pointer_v1*, uint32_t, uint32_t, wl_fixed_t, wl_fixed_t,
                     wl_fixed_t dx, wl_fixed_t dy)  // unaccelerated, like games use
  {
    static_cast<WaylandLock*>(data)->fDelta += QPointF(wl_fixed_to_double(dx), wl_fixed_to_double(dy));
  }

  static constexpr wl_registry_listener kRegistryListener = {Global, GlobalRemove};
  static constexpr zwp_locked_pointer_v1_listener kLockListener = {Locked, Unlocked};
  static constexpr zwp_relative_pointer_v1_listener kMotionListener = {Motion};

  wl_display* fDisplay;
  wl_event_queue* fQueue;
  wl_registry* fRegistry = nullptr;
  zwp_pointer_constraints_v1* fConstraints = nullptr;
  zwp_relative_pointer_manager_v1* fRelative = nullptr;
  zwp_locked_pointer_v1* fLock = nullptr;
  zwp_relative_pointer_v1* fMotion = nullptr;
  QPointF fDelta;
  bool fLost = false;
};
#endif

#ifdef DOOM_QT
// Doom keys for the Qt session. Installed on the whole application, so it works whichever
// viewer widget has the focus; keys pass through untouched while a text field is focused.
// Held keys are tracked from press/release (auto-repeat only repeats the last key), mouse
// movement is summed, and a timer turns both into one /doom/move per tick, so W+D+Q and
// mouse look all work together without queueing up redraws.
class DoomInput : public QObject
{
 public:
  DoomInput(const DoomMessenger* doom, QObject* parent) : QObject(parent), fDoom(doom)
  {
    fThinkTimer = startTimer(kThinkMs);
    fWorldTimer = startTimer(kWorldMs);
    fClock.start();
    fWorldClock.start();
  }

 protected:
  bool eventFilter(QObject* obj, QEvent* event) override
  {
    const auto type = event->type();
    if (type == QEvent::ApplicationDeactivate) {  // no stuck keys or grabbed mouse after alt-tab
      fHeld.clear();
      fTrigger = false;
      Release();
    }
    if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonRelease
        || type == QEvent::MouseButtonDblClick || type == QEvent::MouseMove)
      return Mouse(obj, static_cast<QMouseEvent*>(event));
    if (type != QEvent::KeyPress && type != QEvent::KeyRelease) return QObject::eventFilter(obj, event);
    QWidget* focus = QApplication::focusWidget();
    if (focus && focus->testAttribute(Qt::WA_InputMethodEnabled)) return false;  // typing a command
    auto* key = static_cast<QKeyEvent*>(event);
    if (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) return false;
    if (key->key() == Qt::Key_M || (key->key() == Qt::Key_Escape && fCaptured)) {
      if (type == QEvent::KeyPress && !key->isAutoRepeat()) {
        fAutoCapture = !fCaptured;  // released by hand: stays free until M again
        if (fCaptured)
          Release();
        else
          Capture();
      }
      return true;
    }
    if (key->key() == Qt::Key_Space) {
      if (!key->isAutoRepeat()) Trigger(type == QEvent::KeyPress);
      return true;
    }
    if (key->key() == Qt::Key_F) {  // use: doors, switches
      if (type == QEvent::KeyPress && !key->isAutoRepeat() && Idle()) G4UImanager::GetUIpointer()->ApplyCommand("/doom/use");
      return true;
    }
    if (key->key() >= Qt::Key_1 && key->key() <= Qt::Key_7) {  // Doom's weapon slots
      if (type == QEvent::KeyPress && !key->isAutoRepeat() && Idle())
        G4UImanager::GetUIpointer()->ApplyCommand("/doom/weapon " + std::to_string(key->key() - Qt::Key_0));
      return true;
    }
    if (!kMoveKeys.count(key->key())) return false;
    if (key->isAutoRepeat()) return true;  // we repeat by ourselves
    if (type == QEvent::KeyPress) {
      fHeld.insert(key->key());
      if (!fTimer) {
        Tick();  // a tap moves at once
        fTimer = startTimer(kTickMs);
      }
    }
    else
      fHeld.erase(key->key());
    return true;
  }

  void timerEvent(QTimerEvent* timer) override
  {
    if (timer->timerId() == fWorldTimer) {  // doors, lifts, floors: Doom's 35 tics a second
      const double dt = std::min(fWorldClock.restart() / 1000., 0.25);
      if (Idle() && (fDoom->Level() || fDoom->Tally().active)) {
        std::ostringstream cmd;
        cmd << "/doom/world " << dt << " s";
        G4UImanager::GetUIpointer()->ApplyCommand(cmd.str());
      }
      // mouse look from the start, as in Doom, and again on coming back to the window
      if (fAutoCapture && !fCaptured && Idle() && QApplication::activeWindow()) Capture(true);
      return;
    }
    if (timer->timerId() == fThinkTimer) {
      // real elapsed time, so slow redraws slow the frame rate, not the monsters
      const double dt = std::min(fClock.restart() / 1000., 0.25);
      if (fDoom->ChargeTics() >= 0 && !fTimer) fTimer = startTimer(kTickMs);  // e.g. /doom/fire typed
      if (fDoom->MonstersActive() && Idle()) {
        std::ostringstream cmd;
        cmd << "/doom/think " << dt << " s";
        G4UImanager::GetUIpointer()->ApplyCommand(cmd.str());
      }
      else if (Idle())
        G4UImanager::GetUIpointer()->ApplyCommand("/doom/animate");  // idle and pain frames
      return;
    }
    if (fHeld.empty() && fYaw == 0 && fPitch == 0 && !Locked() && !fTrigger && fDoom->ChargeTics() < 0) {
      killTimer(fTimer);
      fTimer = 0;
    }
    else
      Tick();
  }

 private:
  static constexpr int kTickMs = 33;          // ~30 moves per second
  static constexpr double kStepMm = 384.;     // 12 map units per tick
  static constexpr double kTurnDeg = 5.;
  static constexpr int kThinkMs = 100;  // monster AI steps
  static constexpr int kWorldMs = 28;   // the level's thinkers, every tic or so

  // Left-drag in the 3D view looks around (instead of Geant4's orbit); other buttons and the
  // wheel stay Geant4's. While captured, plain movement looks and the left button fires.
  bool Mouse(QObject* obj, QMouseEvent* mouse)
  {
    auto* view = qobject_cast<QWidget*>(obj);
    if (!view || !view->inherits("QOpenGLWidget")) return false;
    const QPointF pos = mouse->globalPosition();
    if (fCaptured) {
      if ((mouse->type() == QEvent::MouseButtonPress || mouse->type() == QEvent::MouseButtonRelease)
          && mouse->button() == Qt::LeftButton)
        Trigger(mouse->type() == QEvent::MouseButtonPress);
      else if (mouse->type() == QEvent::MouseButtonPress && mouse->button() == Qt::RightButton && Idle())
        G4UImanager::GetUIpointer()->ApplyCommand("/doom/use");
      else if (mouse->type() == QEvent::MouseMove && !Locked()) {  // X11: warp back to the centre
        const QPoint centre = fCaptured->mapToGlobal(fCaptured->rect().center());
        if (pos.toPoint() != centre) {  // ignore the move our own warp causes
          Look(pos - QPointF(centre));
          QCursor::setPos(centre);
        }
      }
      return true;
    }
    if (mouse->button() != Qt::LeftButton && !(mouse->buttons() & Qt::LeftButton)) return false;
    if (mouse->type() == QEvent::MouseMove) Look(pos - fLast);
    fLast = pos;
    return true;
  }

  void Look(const QPointF& delta)
  {
    AddLook(delta);
    if (!fTimer) fTimer = startTimer(kTickMs);
  }

  void AddLook(const QPointF& delta)
  {
    fYaw -= delta.x() * fDoom->MouseSensitivity();    // mouse left = turn left
    fPitch -= delta.y() * fDoom->MouseSensitivity();  // mouse up = look up
  }

  // The 3D view: under the cursor, else the first one in the active window
  static QWidget* FindView()
  {
    QWidget* view = QApplication::widgetAt(QCursor::pos());
    if (view && view->inherits("QOpenGLWidget")) return view;
    if (QWidget* top = QApplication::activeWindow())
      for (QWidget* w : top->findChildren<QWidget*>())
        if (w->inherits("QOpenGLWidget") && w->isVisible()) return w;
    return nullptr;
  }

  // quiet: the automatic capture, which tries again later and gives up without a pointer lock
  void Capture(bool quiet = false)
  {
    QWidget* view = FindView();
    if (!view) {
      if (!quiet) G4cout << "Point the mouse at the 3D view and press M again." << G4endl;
      return;
    }
    if (QGuiApplication::platformName() == "wayland") {
#ifdef DOOM_WAYLAND
      fLock = WaylandLock::Create(view);
#endif
      if (!Locked()) {
        fAutoCapture = false;
        G4cout << "This Wayland compositor (or build) has no pointer lock: drag with the left\n"
                  "button to look, or start doomviewer with QT_QPA_PLATFORM=xcb." << G4endl;
        return;
      }
      QApplication::setOverrideCursor(Qt::BlankCursor);
      if (!fTimer) fTimer = startTimer(kTickMs);  // the tick collects the relative motion
    }
    else {
      fHadTracking = view->hasMouseTracking();
      view->setMouseTracking(true);  // moves without a button held
      view->grabMouse(Qt::BlankCursor);
      QCursor::setPos(view->mapToGlobal(view->rect().center()));
    }
    fCaptured = view;
    if (!quiet || !fToldCapture)
      G4cout << "Mouse captured: move to look, left button fires, right button uses, Esc or M releases\n"
                "(for the command line and menus; M captures again)." << G4endl;
    fToldCapture = true;
  }

  void Release()
  {
    if (!fCaptured) return;
    if (Locked()) {
#ifdef DOOM_WAYLAND
      fLock.reset();
#endif
      QApplication::restoreOverrideCursor();
    }
    else {
      fCaptured->releaseMouse();
      fCaptured->setMouseTracking(fHadTracking);
    }
    fCaptured = nullptr;
  }

  bool Locked() const
  {
#ifdef DOOM_WAYLAND
    return fLock != nullptr;
#else
    return false;
#endif
  }
  inline static const std::set<int> kMoveKeys = {Qt::Key_W, Qt::Key_S, Qt::Key_A, Qt::Key_D, Qt::Key_Q,
                                                 Qt::Key_E, Qt::Key_Up, Qt::Key_Down, Qt::Key_Left, Qt::Key_Right};

  // G4UIQt keeps the event loop running during a run: no moving until it is over
  static bool Idle() { return G4StateManager::GetStateManager()->GetCurrentState() == G4State_Idle; }

  int Held(int a, int b = 0) const { return fHeld.count(a) || (b && fHeld.count(b)) ? 1 : 0; }

  // Trigger pulled or released: fire at once, then again every refire while it is held
  void Trigger(bool pulled)
  {
    fTrigger = pulled;
    if (!pulled) return;
    Shoot();
    if (!fTimer) fTimer = startTimer(kTickMs);
  }

  // A shot, if the weapon is ready (tapping fires no faster than holding)
  void Shoot()
  {
    if (!Idle() || (fShotClock.isValid() && fShotClock.elapsed() < fDoom->Refire() * 1000)) return;
    fShotClock.start();
    G4UImanager::GetUIpointer()->ApplyCommand("/doom/fire");
  }

  void Tick()
  {
    if (fDoom->ChargeDue() && Idle()) G4UImanager::GetUIpointer()->ApplyCommand("/doom/fire");  // BFG away
    if (fTrigger) Shoot();
    const int forward = Held(Qt::Key_W, Qt::Key_Up) - Held(Qt::Key_S, Qt::Key_Down);
    const int right = Held(Qt::Key_D) - Held(Qt::Key_A);
    const int left = Held(Qt::Key_Q, Qt::Key_Left) - Held(Qt::Key_E, Qt::Key_Right);
#ifdef DOOM_WAYLAND
    if (fLock) {
      AddLook(fLock->Dispatch());
      if (fLock->Lost()) Release();
    }
#endif
    const double yaw = fYaw, pitch = fPitch;
    fYaw = fPitch = 0;  // mouse moves made during a run are dropped, not replayed as a jump
    if ((!forward && !right && !left && yaw == 0 && pitch == 0) || !Idle()) return;
    std::ostringstream cmd;
    cmd << "/doom/move " << forward * kStepMm << " " << right * kStepMm << " " << left * kTurnDeg + yaw
        << " " << pitch;
    G4UImanager::GetUIpointer()->ApplyCommand(cmd.str());
  }

  const DoomMessenger* fDoom;
  std::set<int> fHeld;
  int fTimer = 0;
  int fThinkTimer = 0, fWorldTimer = 0;
  QElapsedTimer fClock, fWorldClock;
  QElapsedTimer fShotClock;
  bool fTrigger = false;  // fire held (Space, or the left button while captured)
  double fYaw = 0, fPitch = 0;  // mouse look not yet applied, deg
  QPointF fLast;                // drag look: previous cursor position
  QPointer<QWidget> fCaptured;  // captured look: the 3D view holding the mouse
  bool fAutoCapture = true;      // capture whenever the window is active (M/Esc turn it off)
  bool fToldCapture = false;
  bool fHadTracking = false;
#ifdef DOOM_WAYLAND
  std::unique_ptr<WaylandLock> fLock;  // captured look on native Wayland
#endif
};

// One status bar picture from the WAD (hud/NAME.png, extracted by doom2gdml.py) and the
// offsets Doom subtracts when it draws it
struct HudPatch
{
  QImage image;
  int left = 0, top = 0;
};
using HudPatches = std::map<QString, HudPatch>;

// What the HUD shows
struct HudState
{
  int health = kPlayerHealth, armor = 0, kills = 0, monsters = 0;
  bool god = false, crosshair = true;
  int red = 0;   // Doom's red palette, 0..8
  QString face;  // Doomguy's face, e.g. "STFST01"
  QString skin;  // the face set
  QString style;  // the status bar style: "doom" or "lhc"
  QString weapon, flashSprite;  // the weapon's sprites, e.g. "CHGG", "CHGF"
  QChar fireFrame;
  bool flash = false;    // just fired
  int ammo = 0;          // of the weapon in hand
  std::array<int, kAmmoTypes> ammoAll{}, maxAll{};
  std::array<bool, 6> owned{};  // weapon slots 2..7
  QStringList messages;  // top left, in Doom's font
  int keys = 0;          // bits 0-2 blue/yellow/red keycards, 3-5 skull keys
  DoomMessenger::Intermission inter;  // the level-end screen, instead of all the above
  bool operator==(const HudState&) const = default;
};

// Doom's status bar, painted by Qt on top of the 3D view with the WAD's own graphics, laid out
// as st_stuff.c does on Doom's 320x200 screen (pixels 1.2 times taller than wide), plus the red
// damage palette over the whole view and messages in Doom's heads-up font. Without the
// graphics it falls back to plain text. A transparent child widget of the QOpenGLWidget, so it
// never makes Geant4 redraw the scene, and mouse events pass through to the view.
class HudOverlay : public QWidget
{
 public:
  HudOverlay(QWidget* view, const HudPatches& patches) : QWidget(view), fPatches(patches)
  {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
  }

  void Show(const HudState& state)
  {
    if (state == fState) return;
    fState = state;
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override
  {
    QPainter p(this);
    if (fState.inter.active) {
      PaintIntermission(p);
      return;
    }
    // Doom's red palettes shift the whole screen towards red in eight steps
    if (fState.red > 0) p.fillRect(rect(), QColor(255, 0, 0, 255 * fState.red / 9));
    if (fPatches.count("STBAR"))
      PaintDoom(p);
    else
      PaintPlain(p);
  }

 private:
  // Status bar positions in Doom's 320x200 screen (st_stuff.c)
  static constexpr int kBarY = 168, kNumY = 171, kHealthX = 90, kArmorX = 221;
  static constexpr int kArmsBgX = 104, kArmsX = 111, kArmsY = 172, kFaceX = 143, kAmmoX = 44;
  static constexpr int kAmmoNowX = 288, kAmmoMaxX = 314;
  static constexpr int kKeyX = 239;
  static constexpr std::array<int, 3> kKeyY = {171, 181, 191};
  static constexpr std::array<int, kAmmoTypes> kAmmoRowY = {173, 179, 191, 185};  // clip, shell, cell, misl

  void PaintDoom(QPainter& p)
  {
    // Doom's 320 columns across the view, the bar's 32 rows at the bottom; never more than a
    // fifth of the view's height (centred then, like a source port's status bar on a wide screen)
    fSx = width() / 320.;
    fSy = fSx * 1.2;
    if (32 * fSy > 0.2 * height()) {
      fSy = 0.2 * height() / 32;
      fSx = fSy / 1.2;
    }
    fX0 = (width() - 320 * fSx) / 2;
    fBarTop = height() - 32 * fSy;
    if (fX0 > 0) p.fillRect(QRectF(0, fBarTop, width(), 32 * fSy), Qt::black);

    // the weapon in hand (R_DrawPSprite: at sx 1, sy 32 in a 168-row view), with its muzzle flash
    if (fState.health > 0 && !fState.weapon.isEmpty()) {
      InView(p, fState.weapon + (fState.flash ? fState.fireFrame : QChar('A')) + "0", 1, 16);
      if (fState.flash) InView(p, fState.flashSprite + "A0", 1, 16);
    }

    Patch(p, "STBAR", 0, kBarY);
    Number(p, fState.ammo, kAmmoX, kNumY, "STTNUM");
    Number(p, std::max(fState.health, 0), kHealthX, kNumY, "STTNUM");
    Patch(p, "STTPRCNT", kHealthX, kNumY);
    Patch(p, "STARMS", kArmsBgX, kBarY);
    for (int i = 0; i < 6; ++i)  // weapons 2..7: yellow if we have it
      Patch(p, QString(fState.owned[i] ? "STYSNUM%1" : "STGNUM%1").arg(i + 2), kArmsX + (i % 3) * 12, kArmsY + (i / 3) * 10);
    Patch(p, fState.face, kFaceX, kBarY);
    Number(p, fState.armor, kArmorX, kNumY, "STTNUM");
    Patch(p, "STTPRCNT", kArmorX, kNumY);
    for (int i = 0; i < 3; ++i)  // the key boxes: a skull key shows over a card of its colour
      if (fState.keys & (9 << i)) Patch(p, QString("STKEYS%1").arg(fState.keys & (8 << i) ? i + 3 : i), kKeyX, kKeyY[i]);
    for (int a = 0; a < kAmmoTypes; ++a) {  // every ammo type: what we have / the most we can carry
      Number(p, fState.ammoAll[a], kAmmoNowX, kAmmoRowY[a], "STYSNUM");
      Number(p, fState.maxAll[a], kAmmoMaxX, kAmmoRowY[a], "STYSNUM");
    }

    // heads-up messages, in Doom's font from the top left; the kill tally at the top right
    for (int i = 0; i < fState.messages.size(); ++i) Message(p, fState.messages[i], 0, 9 * i);
    const QString kills = QString(fState.style == "lhc" ? "EVENTS %1/%2" : "KILLS %1/%2")
                            .arg(fState.kills).arg(fState.monsters);
    Message(p, kills, 320 - MessageWidth(kills) - 2, 0);

    if (fState.crosshair && fState.health > 0) {  // not Doom's, but shots go along the view centre
      p.setPen(QPen(QColor(255, 255, 255, 170), std::max(1., fSx * 0.7)));
      const double cx = width() / 2., cy = height() / 2., c = 3 * fSx;
      p.drawLine(QPointF(cx - c, cy), QPointF(cx + c, cy));
      p.drawLine(QPointF(cx, cy - c), QPointF(cx, cy + c));
    }
  }

  // Doom's intermission (wi_stuff.c) on the whole view: the episode's map (or INTERPIC), the
  // finished level's name and the tally, then the map with splats on the levels done and the
  // "you are here" pointer at the next one, under "entering" and its name
  void PaintIntermission(QPainter& p)
  {
    const auto& w = fState.inter;
    fSx = std::min(width() / 320., height() / 240.);
    fSy = fSx * 1.2;
    fX0 = (width() - 320 * fSx) / 2;
    fY0 = (height() - 200 * fSy) / 2;
    p.fillRect(rect(), Qt::black);
    if (!fPatches.count("WIOSTK")) {  // hud/ from before the intermission graphics: plain text
      PaintIntermissionText(p);
      return;
    }
    int e = 0, m = 0;
    const bool episode = std::sscanf(w.map.c_str(), "E%dM%d", &e, &m) == 2;
    int ne = 0, nm = 0;
    std::sscanf(w.next.c_str(), "E%dM%d", &ne, &nm);
    if (!episode) {
      std::sscanf(w.map.c_str(), "MAP%d", &m);
      std::sscanf(w.next.c_str(), "MAP%d", &nm);
    }
    auto name = [&](int ep, int map) {  // the level-name patch: WILV<e-1><m-1>, Doom II CWILV<nn>
      return episode ? QString("WILV%1%2").arg(ep - 1).arg(map - 1) : QString("CWILV%1").arg(map - 1, 2, 10, QChar(u'0'));
    };
    const bool mapScreen = episode && e >= 1 && e <= 3;
    Screen(p, mapScreen ? QString("WIMAP%1").arg(e - 1) : QString("INTERPIC"), 0, 0);
    if (w.stage <= 10) {
      // WI_drawLF: the level, "finished"
      const QString last = name(e, m);
      Screen(p, last, (320 - PatchWidth(last)) / 2, 2);
      Screen(p, "WIF", (320 - PatchWidth("WIF")) / 2, 2 + 5 * PatchHeight(last) / 4);
      // WI_drawStats
      const int lh = 3 * PatchHeight("WINUM0") / 2;
      Screen(p, "WIOSTK", 50, 50);
      Percent(p, 270, 50, w.cKills);
      Screen(p, "WIOSTI", 50, 50 + lh);
      Percent(p, 270, 50 + lh, w.cItems);
      Screen(p, "WISCRT2", 50, 50 + 2 * lh);
      Percent(p, 270, 50 + 2 * lh, w.cSecrets);
      Screen(p, "WITIME", 16, 168);
      Time(p, 160 - 16, 168, w.cTime);
      if (w.par >= 0) {
        Screen(p, "WIPAR", 160 + 16, 168);
        Time(p, 320 - 16, 168, w.cPar);
      }
      return;
    }
    // WI_drawShowNextLoc: splats on the levels done, the pointer on the next
    if (mapScreen && ne == e) {
      static constexpr int kNodes[3][9][2] = {
        {{185, 164}, {148, 143}, {69, 122}, {209, 102}, {116, 89}, {166, 55}, {71, 56}, {135, 29}, {71, 24}},
        {{254, 25}, {97, 50}, {188, 64}, {128, 78}, {214, 92}, {133, 130}, {208, 136}, {148, 140}, {235, 158}},
        {{156, 168}, {48, 154}, {174, 95}, {265, 75}, {130, 48}, {279, 23}, {198, 48}, {140, 25}, {281, 136}}};
      const auto& nodes = kNodes[e - 1];
      const int last = m == 9 ? nm - 1 : m;  // back from the secret level: up to the one before
      for (int i = 0; i < last && i < 9; ++i) Screen(p, "WISPLAT", nodes[i][0], nodes[i][1]);
      if (m == 9) Screen(p, "WISPLAT", nodes[8][0], nodes[8][1]);
      if ((w.pointer || w.stage == 12) && nm >= 1 && nm <= 9) {
        // WI_drawOnLnode: the first pointer that fits on the screen
        const QString yah = FitsOnScreen("WIURH0", nodes[nm - 1][0], nodes[nm - 1][1]) ? "WIURH0" : "WIURH1";
        Screen(p, yah, nodes[nm - 1][0], nodes[nm - 1][1]);
      }
    }
    // WI_drawEL: "entering", the next level
    if (!nm) return;
    const QString next = name(episode ? ne : 0, nm);
    Screen(p, "WIENTER", (320 - PatchWidth("WIENTER")) / 2, 2);
    Screen(p, next, (320 - PatchWidth(next)) / 2, 2 + 5 * PatchHeight(next) / 4);
  }

  // The intermission without the WAD's graphics
  void PaintIntermissionText(QPainter& p)
  {
    const auto& w = fState.inter;
    p.setRenderHint(QPainter::Antialiasing);
    const double cx = width() / 2., h = height(), size = std::max(14., h / 18);
    const QColor red(205, 30, 30), grey(200, 200, 200);
    auto time = [](int t) { return t < 0 ? QString() : QString("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar(u'0')); };
    auto pct = [](int v) { return v < 0 ? QString() : QString("%1%").arg(v); };
    if (w.stage <= 10) {
      Text(p, QString::fromStdString(w.map) + " FINISHED", cx, h * 0.15, size * 1.4, red, Qt::AlignHCenter);
      const QStringList lines = {"KILLS " + pct(w.cKills), "ITEMS " + pct(w.cItems), "SECRET " + pct(w.cSecrets),
                                 "TIME " + time(w.cTime) + (w.par >= 0 ? "   PAR " + time(w.cPar) : QString())};
      for (int i = 0; i < lines.size(); ++i) Text(p, lines[i], cx, h * (0.35 + 0.12 * i), size, grey, Qt::AlignHCenter);
    }
    else {
      Text(p, "ENTERING", cx, h * 0.4, size, grey, Qt::AlignHCenter);
      Text(p, QString::fromStdString(w.next), cx, h * 0.4 + size * 1.8, size * 1.4, red, Qt::AlignHCenter);
    }
  }

  // V_DrawPatch on the intermission's full 320x200 screen
  void Screen(QPainter& p, const QString& name, double x, double y)
  {
    auto it = fPatches.find(name);
    if (it == fPatches.end()) return;
    const HudPatch& patch = it->second;
    p.drawImage(QRectF(fX0 + (x - patch.left) * fSx, fY0 + (y - patch.top) * fSy, patch.image.width() * fSx,
                       patch.image.height() * fSy),
                patch.image);
  }

  int PatchWidth(const QString& name) const
  {
    auto it = fPatches.find(name);
    return it == fPatches.end() ? 0 : it->second.image.width();
  }
  int PatchHeight(const QString& name) const
  {
    auto it = fPatches.find(name);
    return it == fPatches.end() ? 0 : it->second.image.height();
  }

  bool FitsOnScreen(const QString& name, int x, int y) const
  {
    auto it = fPatches.find(name);
    if (it == fPatches.end()) return false;
    const HudPatch& patch = it->second;
    const int left = x - patch.left, top = y - patch.top;
    return left >= 0 && top >= 0 && left + patch.image.width() < 320 && top + patch.image.height() < 200;
  }

  // WI_drawNum: digits leftwards from x (at least `digits` of them, zero-padded); returns the
  // left end
  int WiNumber(QPainter& p, int x, int y, int n, int digits)
  {
    if (digits < 0) {
      digits = 0;
      for (int t = n; t || !digits; t /= 10) ++digits;
    }
    const int w = PatchWidth("WINUM0");
    while (digits--) {
      x -= w;
      Screen(p, QString("WINUM%1").arg(n % 10), x, y);
      n /= 10;
    }
    return x;
  }

  // WI_drawPercent: nothing until it starts counting
  void Percent(QPainter& p, int x, int y, int value)
  {
    if (value < 0) return;
    Screen(p, "WIPCNT", x, y);
    WiNumber(p, x, y, value, -1);
  }

  // WI_drawTime: m:ss (h:mm:ss) ending at x; "sucks" past 61 minutes
  void Time(QPainter& p, int x, int y, int t)
  {
    if (t < 0) return;
    if (t > 61 * 59) {
      Screen(p, "WISUCKS", x - PatchWidth("WISUCKS"), y);
      return;
    }
    int div = 1;
    do {
      x = WiNumber(p, x, y, (t / div) % 60, 2) - PatchWidth("WICOLON");
      div *= 60;
      if (div == 60 || t / div) Screen(p, "WICOLON", x, y);
    } while (t / div);
  }

  // Draw a patch at Doom screen position (x, y), minus its offsets (V_DrawPatch). Rows from
  // kBarY down belong to the bar, the others count from the top of the view.
  void Patch(QPainter& p, const QString& name, double x, double y)
  {
    auto it = fPatches.find(name);
    if (it == fPatches.end()) return;
    const HudPatch& patch = it->second;
    const double top = y >= kBarY ? fBarTop + (y - kBarY - patch.top) * fSy : (y - patch.top) * fSy;
    p.drawImage(QRectF(fX0 + (x - patch.left) * fSx, top, patch.image.width() * fSx, patch.image.height() * fSy),
                patch.image);
  }

  // A weapon sprite: Doom screen position (x, y) in its 3D view, the 168 rows above the bar
  void InView(QPainter& p, const QString& name, double x, double y)
  {
    auto it = fPatches.find(name);
    if (it == fPatches.end()) return;
    const HudPatch& patch = it->second;
    p.drawImage(QRectF(fX0 + (x - patch.left) * fSx, fBarTop + (y - patch.top - kBarY) * fSy,
                       patch.image.width() * fSx, patch.image.height() * fSy),
                patch.image);
  }

  // STlib_drawNum: digits leftwards from x, the right end
  void Number(QPainter& p, int value, int x, int y, const char* font)
  {
    auto zero = fPatches.find(QString(font) + "0");
    if (zero == fPatches.end()) return;
    const int w = zero->second.image.width();
    do {
      x -= w;
      Patch(p, QString("%1%2").arg(font).arg(value % 10), x, y);
      value /= 10;
    } while (value);
  }

  // Doom's heads-up font (STCFN033 = '!' ... STCFN095 = '_'); a space is 4 pixels
  void Message(QPainter& p, const QString& text, double x, double y)
  {
    for (QChar c : text.toUpper()) {
      const QString name = QString("STCFN%1").arg(int(c.unicode()), 3, 10, QChar(u'0'));
      auto it = fPatches.find(name);
      if (c == ' ' || it == fPatches.end()) {
        x += 4;
        continue;
      }
      Patch(p, name, x, y);
      x += it->second.image.width();
    }
  }

  int MessageWidth(const QString& text) const
  {
    int w = 0;
    for (QChar c : text.toUpper()) {
      auto it = fPatches.find(QString("STCFN%1").arg(int(c.unicode()), 3, 10, QChar(u'0')));
      w += c == ' ' || it == fPatches.end() ? 4 : it->second.image.width();
    }
    return w;
  }

  // Without the WAD graphics: outlined text
  void PaintPlain(QPainter& p)
  {
    p.setRenderHint(QPainter::Antialiasing);
    const double w = width(), h = height(), margin = h / 30;
    const double big = std::max(18., h / 10), small = big * 0.38;
    const QColor red(205, 30, 30), gold(230, 190, 40), grey(200, 200, 200);
    if (fState.health <= 0) {
      Text(p, "YOU DIED", w / 2, h / 2, big * 1.2, red, Qt::AlignHCenter);
      Text(p, "fire to play again", w / 2, h / 2 + big * 0.7, small * 1.2, grey, Qt::AlignHCenter);
    }
    else if (fState.crosshair) {
      p.setPen(QPen(QColor(255, 255, 255, 170), std::max(1.5, h / 400)));
      const double c = std::max(6., h / 60);
      p.drawLine(QPointF(w / 2 - c, h / 2), QPointF(w / 2 + c, h / 2));
      p.drawLine(QPointF(w / 2, h / 2 - c), QPointF(w / 2, h / 2 + c));
    }
    const double base = h - margin;
    Text(p, "HEALTH", margin, base - big * 0.95, small, grey, Qt::AlignLeft);
    Text(p, QString("%1%").arg(std::max(fState.health, 0)), margin, base, big, red, Qt::AlignLeft);
    if (fState.god) Text(p, "GOD", margin + big * 3.4, base, small * 1.3, gold, Qt::AlignLeft);
    Text(p, QString("AMMO %1").arg(fState.ammo), w / 2, base, small * 1.3, grey, Qt::AlignHCenter);
    Text(p, "KILLS", w - margin, base - big * 0.95, small, grey, Qt::AlignRight);
    Text(p, QString("%1/%2").arg(fState.kills).arg(fState.monsters), w - margin, base, big * 0.7, red,
         Qt::AlignRight);
  }

  // Bold text with a black outline; (x, baseline) is its left, centre or right end by align
  static void Text(QPainter& p, const QString& text, double x, double baseline, double size,
                   const QColor& colour, Qt::Alignment align)
  {
    QFont font;
    font.setPixelSize(static_cast<int>(size));
    font.setBold(true);
    const double width = QFontMetricsF(font).horizontalAdvance(text);
    if (align & Qt::AlignHCenter) x -= width / 2;
    if (align & Qt::AlignRight) x -= width;
    QPainterPath path;
    path.addText(QPointF(x, baseline), font, text);
    p.strokePath(path, QPen(Qt::black, std::max(2., size / 12), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.fillPath(path, colour);
  }

  const HudPatches& fPatches;
  HudState fState;
  double fSx = 1, fSy = 1.2, fX0 = 0, fY0 = 0, fBarTop = 0;  // Doom screen -> widget
};

// The level as Doom draws it: textured walls, floors and ceilings, with Doom's sector light
// fading with distance, animated flats, scrolling walls, the sky, and the keys lying about.
// Geant4's vis has no textures (each solid has one colour), so this draws the surfaces itself,
// in OpenGL, into Geant4's own Qt view: a paint-event filter lets Geant4 paint (monsters,
// sprites, tracks), then draws the level into the same framebuffer, against the same depth
// buffer, with the camera set up as G4OpenGLViewer::SetView does. The surfaces come from the
// World (doomworld.hh) as it is now, so doors and lifts move in the picture too. /doom/textures
// hides Geant4's flat-coloured level volumes; the physics geometry is unchanged.
class TexturePass : public QObject
{
 public:
  TexturePass(const DoomMessenger* doom, G4VisManager* vis, const DoomWorld* world, const std::filesystem::path& dir,
              QObject* parent)
    : QObject(parent), fDoom(doom), fVis(vis), fDir(dir)
  {
    SetWorld(world);
    // the items' sprites
    std::ifstream offsets(dir / "sprites" / "offsets.txt");
    std::string name;
    int left, top;
    while (offsets >> name >> left >> top) {
      const G4bool item = std::any_of(kItemSprites.begin(), kItemSprites.end(),
                                      [&](const auto& kv) { return kv.second == name.substr(0, 4); })
                          || std::any_of(kMissileSprites.begin(), kMissileSprites.end(),
                                         [&](const auto& kv) { return kv.second == name.substr(0, 4); });
      if (!item || name.size() != 6) continue;
      Texture t;
      t.image = QImage(QString::fromStdString((dir / "sprites" / (name + ".png")).string())).convertToFormat(QImage::Format_RGBA8888);
      t.left = left;
      t.top = top;
      if (!t.image.isNull()) fSprites[name] = std::move(t);
    }
    startTimer(1000 / 35);
  }

  // A new level (nullptr while it is being replaced): its textures are loaded now and go to
  // OpenGL at the next paint
  void SetWorld(const DoomWorld* world)
  {
    fWorld = world;
    fOld.clear();
    for (const Texture& t : fTextures)
      if (t.id) fOld.push_back(t.id);
    if (fSky.id) fOld.push_back(fSky.id);
    fTextures.clear();
    fSky = Texture();
    fVertices.clear();
    fBuilt = 0;
    fUploaded = false;
    if (!world) return;
    auto load = [this](const std::string& file) {
      return QImage(QString::fromStdString((fDir / "textures" / file).string())).convertToFormat(QImage::Format_RGBA8888);
    };
    for (const auto& info : world->Textures()) {
      Texture t;
      t.image = load(info.file);
      fTextures.push_back(std::move(t));
    }
    if (!world->SkyFile().empty()) fSky.image = load(world->SkyFile());
    fSky.height = fSky.image.isNull() ? 128 : fSky.image.height();
    fScrollers = world->HasScrollers();
    G4cout << "Textured level: " << fTextures.size() << " textures" << G4endl;
  }

  G4bool Loaded() const { return fWorld && fWorld->Loaded() && !fTextures.empty(); }

 protected:
  void timerEvent(QTimerEvent*) override
  {
    for (QWidget* top : QApplication::topLevelWidgets())
      for (QWidget* w : top->findChildren<QWidget*>())
        if (auto* gl = qobject_cast<QOpenGLWidget*>(w); gl && gl->isVisible() && gl != fView) {
          if (fView) fView->removeEventFilter(this);
          fView = gl;
          gl->installEventFilter(this);
        }
    // repaint (Geant4 replays its display lists) when the picture changes: animated flats every
    // 8 tics, blinking keys every 10, scrolling walls, switches and doors in the World
    if (fView && fWorld && fDoom->TexturesOn() && (Frame() != fDrawn || Fireballs())) fView->update();
  }

  // Geant4 paints its view into the widget's framebuffer, then we draw on top, before Qt shows it
  bool eventFilter(QObject* watched, QEvent* event) override
  {
    if (event->type() != QEvent::Paint || watched != fView || fPainting || !fDoom->TexturesOn() || !fWorld) return false;
    fPainting = true;
    QCoreApplication::sendEvent(watched, event);  // QOpenGLWidget::paintEvent: Geant4's paintGL
    fPainting = false;
    Draw();
    return true;
  }

 private:
  struct Texture
  {
    QImage image;
    GLuint id = 0;
    int left = 0, top = 0;  // sprites: Doom's offsets
    int height = 128;       // the sky
  };

  // thing type -> sprite name: keys (which blink), weapons and ammo
  inline static const std::map<G4int, std::string> kItemSprites = {
    {5, "BKEY"},    {6, "YKEY"},    {13, "RKEY"},   {40, "BSKU"},   {39, "YSKU"},   {38, "RSKU"},
    {2001, "SHOT"}, {82, "SGN2"},   {2002, "MGUN"}, {2003, "LAUN"}, {2004, "PLAS"}, {2006, "BFUG"},
    {2007, "CLIP"}, {2048, "AMMO"}, {2008, "SHEL"}, {2049, "SBOX"}, {2010, "ROCK"}, {2046, "BROK"},
    {2047, "CELL"}, {17, "CELP"},   {8, "BPAK"},   {2014, "BON1"}, {2011, "STIM"}, {2012, "MEDI"},
    {2015, "BON2"}, {2018, "ARM1"}, {2019, "ARM2"}, {2013, "SOUL"}, {83, "MEGA"}};
  // Doom's animated items (info.c states): frames, tics per frame, which frames are bright
  struct Animation
  {
    const char* frames;
    G4int tics;
    const char* bright;
  };
  inline static const std::map<std::string, Animation> kAnimations = {
    {"BKEY", {"AB", 10, "B"}}, {"YKEY", {"AB", 10, "B"}}, {"RKEY", {"AB", 10, "B"}},
    {"BSKU", {"AB", 10, "B"}}, {"YSKU", {"AB", 10, "B"}}, {"RSKU", {"AB", 10, "B"}},
    {"BON1", {"ABCDCB", 6, ""}}, {"BON2", {"ABCDCB", 6, ""}}, {"ARM1", {"AB", 7, "B"}},
    {"ARM2", {"AB", 7, "B"}}, {"SOUL", {"ABCDCB", 6, "ABCD"}}, {"MEGA", {"ABCD", 6, "ABCD"}}};

  static G4int Tic() { return G4int(Now() / (kTic / s)); }

  // What the picture depends on besides the camera
  std::array<G4int, 4> Frame() const
  {
    // animated items: their frames change every 6, 7 or 10 tics; one number for all of them
    G4int items = 0;
    for (const auto& thing : fWorld->Things()) {
      auto item = kItemSprites.find(thing.type);
      if (thing.taken || item == kItemSprites.end()) continue;
      auto anim = kAnimations.find(item->second);
      if (anim != kAnimations.end()) items = items * 31 + Tic() / anim->second.tics;
    }
    return {Tic() / 8, items, G4int(fWorld->Version()), fScrollers ? fWorld->LevelTime() / 2 : 0};
  }

  void Draw()
  {
    if (!fView) return;
    const int w = fView->width() * fView->devicePixelRatio(), h = fView->height() * fView->devicePixelRatio();
    fView->makeCurrent();
    if (!fProgram && !Init()) return;
    if (!fUploaded) Upload();
    G4Point3D eye;
    if (!SetView(w, h, eye)) return;
    fDrawn = Frame();
    if (fBuilt != fWorld->Version() || (fScrollers && fBuiltTime != fWorld->LevelTime())) {
      fWorld->Surfaces(fVertices);  // cheap: a few thousand vertices
      fBuilt = fWorld->Version();
      fBuiltTime = fWorld->LevelTime();
    }
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    glPushClientAttrib(GL_CLIENT_ALL_ATTRIB_BITS);
    glDisable(GL_LIGHTING);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glEnable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    fProgram->bind();
    fProgram->setUniformValue("tex", 0);
    fProgram->setUniformValue("mapUnit", GLfloat(kMapUnit));
    fProgram->setUniformValue("fullbright", 0.f);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    const G4int animFrame = Tic() / 8;
    const auto& infos = fWorld->Textures();
    for (std::size_t i = 0; i < fVertices.size() && i < fTextures.size(); ++i) {
      const auto& v = fVertices[i];
      if (v.empty()) continue;
      const auto& anim = infos[i].anim;
      const Texture& shown = anim.empty() ? fTextures[i]
                                          : fTextures[anim[(infos[i].animPos + animFrame) % anim.size()]];
      Arrays(v, shown.id);
    }
    DrawItems(eye);
    DrawFireballs(eye);
    if (fSky.id) DrawSky(eye);
    fProgram->release();
    glPopClientAttrib();
    glPopAttrib();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    Snapshot(w, h);
  }

  static void Arrays(const std::vector<GLfloat>& v, GLuint texture)
  {
    glBindTexture(GL_TEXTURE_2D, texture);
    glVertexPointer(3, GL_FLOAT, 6 * sizeof(GLfloat), v.data());
    glTexCoordPointer(3, GL_FLOAT, 6 * sizeof(GLfloat), v.data() + 3);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(v.size() / 6));
  }

  // The keys, weapons and ammo not yet taken: upright pictures turned to the eye (1 map unit a
  // pixel), in the sector's light, animated as in Doom (keys blink, bonuses shimmer; bright frames
  // ignore the light)
  void DrawItems(const G4Point3D& eye)
  {
    for (const auto& thing : fWorld->Things()) {
      auto item = kItemSprites.find(thing.type);
      if (thing.taken || item == kItemSprites.end() || thing.sector < 0) continue;
      char frame = 'A';
      G4bool bright = false;
      if (auto anim = kAnimations.find(item->second); anim != kAnimations.end()) {
        const std::string frames = anim->second.frames;
        frame = frames[Tic() / anim->second.tics % frames.size()];
        bright = std::strchr(anim->second.bright, frame) != nullptr;
      }
      auto sprite = fSprites.find(item->second + frame + "0");
      if (sprite == fSprites.end()) continue;
      const Texture& t = sprite->second;
      const auto& sector = fWorld->Sectors()[thing.sector];
      Billboard(t, fWorld->ToG4(thing.x, thing.y, sector.floor), eye,
                bright ? 1. : std::clamp(sector.light, 0, 255) / 255.);
    }
  }

  // A sprite standing at `foot`, turned to the eye, 1 map unit a pixel, in this light (0..1)
  void Billboard(const Texture& t, const G4ThreeVector& foot, const G4Point3D& eye, double light)
  {
    G4ThreeVector right(eye.z() - foot.z(), 0, foot.x() - eye.x());  // across the line of sight
    if (right.mag2() == 0) return;
    right = right.unit() * kMapUnit;
    const G4ThreeVector up(0, kMapUnit, 0);
    const G4double x0 = -t.left, x1 = t.image.width() - t.left, y1 = t.top, y0 = t.top - t.image.height();
    std::vector<GLfloat> v;
    auto corner = [&](G4double x, G4double y, G4double u, G4double tv) {
      const G4ThreeVector p = foot + x * right + y * up;
      for (G4double f : {p.x(), p.y(), p.z(), u, tv, light}) v.push_back(GLfloat(f));
    };
    corner(x0, y0, 0, 1);
    corner(x1, y0, 1, 1);
    corner(x1, y1, 1, 0);
    corner(x0, y0, 0, 1);
    corner(x1, y1, 1, 0);
    corner(x0, y1, 0, 0);
    Arrays(v, t.id);
  }

  // Whether fireballs are flying or bursting: then every tic is a new picture
  G4bool Fireballs() const
  {
    if (!fDoom->Missiles().empty()) return true;
    return std::any_of(fDoom->Bursts().begin(), fDoom->Bursts().end(),
                       [](const Burst& b) { return Now() - b.time < 3 * kMissileBurstTics * kTic / s; });
  }

  // The fireballs as Doom's sprites, full bright: two frames in flight, three as they burst. The
  // picture stands 4 units below the missile's centre, on the bottom of Doom's 8-unit-high mobj.
  void DrawFireballs(const G4Point3D& eye)
  {
    auto draw = [&](const G4ThreeVector& pos, const std::string& name) {
      auto sprite = fSprites.find(name);
      if (sprite == fSprites.end()) return;
      const G4ThreeVector foot = pos - G4ThreeVector(0, 4 * kMapUnit, 0);
      Billboard(sprite->second, foot, eye, 1.);
    };
    fProgram->setUniformValue("fullbright", 1.f);
    for (const Missile& m : fDoom->Missiles()) draw(m.pos, m.sprite + char('A' + Tic() / kMissileFlyTics % 2) + "0");
    for (const Burst& b : fDoom->Bursts()) {
      const G4int frame = G4int((Now() - b.time) / (kTic / s)) / kMissileBurstTics;
      if (frame < 3) draw(b.pos, b.sprite + char('C' + frame) + "0");
    }
    fProgram->setUniformValue("fullbright", 0.f);
  }

  // Doom's sky: 1024 texels round the horizon (the 256-wide texture four times), one texel a
  // screen pixel up and down with the horizon at row 100 (skytexturemid). A cylinder round the
  // eye, at the far plane, so it only fills what nothing else covers.
  void DrawSky(const G4Point3D& eye)
  {
    const G4double r = 1000 * kMapUnit;
    std::vector<GLfloat> v;
    const G4int n = 64;
    auto corner = [&](G4int i, G4double hFraction) {
      const G4double a = CLHEP::twopi * i / n;  // Doom angle, anticlockwise from +x seen from above
      const G4double h = hFraction * r;
      for (G4double f : {eye.x() + r * std::cos(a), eye.y() + h, eye.z() - r * std::sin(a),
                         4. * i / n, (100 - 160 * hFraction) / fSky.height, 1.})
        v.push_back(GLfloat(f));
    };
    for (G4int i = 0; i < n; ++i)  // two triangles per side
      for (auto [di, hf] : {std::pair{0, -2.}, {1, -2.}, {1, 2.}, {0, -2.}, {1, 2.}, {0, 2.}}) corner(i + di, hf);
    fProgram->setUniformValue("fullbright", 1.f);
    glDepthRange(1, 1);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    Arrays(v, fSky.id);
    glDepthRange(0, 1);
  }

  static void UploadTexture(Texture& t, G4bool repeatS, G4bool repeatT)
  {
    if (t.image.isNull()) return;
    glGenTextures(1, &t.id);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);  // Doom's big pixels
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeatS ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeatT ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.image.width(), t.image.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 t.image.constBits());
  }

  // The level's textures to OpenGL, the last level's freed (in the view's context)
  void Upload()
  {
    if (!fOld.empty()) glDeleteTextures(GLsizei(fOld.size()), fOld.data());
    fOld.clear();
    for (Texture& t : fTextures) UploadTexture(t, true, true);
    UploadTexture(fSky, true, false);
    fUploaded = true;
  }

  G4bool Init()
  {
    for (auto& [name, t] : fSprites) UploadTexture(t, false, false);
    fProgram = new QOpenGLShaderProgram(this);
    // GLSL 1.20, the compatibility profile Geant4's OpenGL viewer runs in. Doom's light
    // diminishing (r_main.c): colormap = (15 - light/16) * 4 - 1280 / distance in map units,
    // clamped to 0..31; each colormap step is 1/32 darker.
    const G4bool ok =
      fProgram->addShaderFromSourceCode(QOpenGLShader::Vertex,
        "#version 120\n"
        "varying vec3 uvl; varying float depth;\n"
        "void main() { gl_Position = ftransform(); uvl = gl_MultiTexCoord0.xyz;\n"
        "  depth = -(gl_ModelViewMatrix * gl_Vertex).z; }\n") &&
      fProgram->addShaderFromSourceCode(QOpenGLShader::Fragment,
        "#version 120\n"
        "uniform sampler2D tex; uniform float mapUnit; uniform float fullbright;\n"
        "varying vec3 uvl; varying float depth;\n"
        "void main() { vec4 c = texture2D(tex, uvl.xy); if (c.a < 0.5) discard;\n"
        "  float startmap = (15.0 - clamp(floor(uvl.z * 255.0 / 16.0), 0.0, 15.0)) * 4.0;\n"
        "  float level = clamp(startmap - 1280.0 / max(depth / mapUnit, 1.0), 0.0, 31.0);\n"
        "  gl_FragColor = vec4(c.rgb * mix(1.0 - level / 32.0, 1.0, fullbright), 1.0); }\n") &&
      fProgram->link();
    if (!ok) {
      G4cerr << "Textured level: shader failed: " << fProgram->log().toStdString() << G4endl;
      return false;
    }
    return true;
  }

  // Geant4 resets its matrices after drawing, so set up the same camera again, as
  // G4OpenGLViewer::SetView does (same near and far planes, so the depth buffer still fits)
  G4bool SetView(int w, int h, G4Point3D& camera)
  {
    G4VViewer* viewer = fVis->GetCurrentViewer();
    G4Scene* scene = fVis->GetCurrentScene();
    if (!viewer || !scene || w <= 0 || h <= 0) return false;
    const G4ViewParameters& vp = viewer->GetViewParameters();
    G4double radius = scene->GetExtent().GetExtentRadius();
    if (radius <= 0) radius = 1;
    const G4Point3D target = scene->GetStandardTargetPoint() + vp.GetCurrentTargetPoint();
    const G4double distance = vp.GetCameraDistance(radius);
    camera = target + distance * vp.GetViewpointDirection().unit();
    const G4double pnear = vp.GetNearDistance(distance, radius), pfar = vp.GetFarDistance(distance, pnear, radius);
    const G4double ratioX = h > w ? G4double(h) / w : 1, ratioY = w > h ? G4double(w) / h : 1;
    const G4double right = vp.GetFrontHalfHeight(pnear, radius) * ratioY, top = vp.GetFrontHalfHeight(pnear, radius) * ratioX;
    glViewport(0, 0, w, h);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    const G4Vector3D scale = vp.GetScaleFactor();
    glScaled(scale.x(), scale.y(), scale.z());
    if (vp.GetFieldHalfAngle() == 0)
      glOrtho(-right, right, -top, top, pnear, pfar);
    else
      glFrustum(-right, right, -top, top, pnear, pfar);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    const G4Point3D look = distance > 1e-6 * radius ? target : G4Point3D(target - radius * vp.GetViewpointDirection().unit());
    // gluLookAt
    const G4Vector3D f = (look - camera).unit(), side = f.cross(vp.GetUpVector()).unit(), up = side.cross(f);
    const GLdouble m[16] = {side.x(), up.x(), -f.x(), 0, side.y(), up.y(), -f.y(), 0,
                            side.z(), up.z(), -f.z(), 0, 0, 0, 0, 1};
    glMultMatrixd(m);
    glTranslated(-camera.x(), -camera.y(), -camera.z());
    return true;
  }

  // debugging: DOOM_VIEW_SNAPSHOT=prefix saves the finished 3D view about once a second
  void Snapshot(int w, int h)
  {
    static const char* snapshot = std::getenv("DOOM_VIEW_SNAPSHOT");
    if (!snapshot || Tic() / 35 == fSnapshotSecond) return;
    fSnapshotSecond = Tic() / 35;
    QImage image(w, h, QImage::Format_RGBA8888);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
    image.mirrored().save(QString("%1_%2.png").arg(snapshot).arg(fSnapshots++, 3, 10, QChar(u'0')));
  }

  const DoomMessenger* fDoom;
  G4VisManager* fVis;
  std::filesystem::path fDir;
  const DoomWorld* fWorld = nullptr;
  std::vector<GLuint> fOld;  // the last level's textures, to free
  G4bool fUploaded = false;
  std::vector<Texture> fTextures;  // by the World's texture ids
  std::map<std::string, Texture> fSprites;
  Texture fSky;
  std::vector<std::vector<GLfloat>> fVertices;  // per texture: x y z (mm), u v, light (0..1)
  unsigned fBuilt = 0;
  G4int fBuiltTime = -1;
  G4bool fScrollers = false;
  std::array<G4int, 4> fDrawn{};
  QPointer<QOpenGLWidget> fView;
  QOpenGLShaderProgram* fProgram = nullptr;
  G4int fSnapshotSecond = -1, fSnapshots = 0;
  bool fPainting = false;
};

// Keeps a HudOverlay on the visible 3D view (made by /vis/open, replaced with new viewers and
// tabs) and feeds it the game state 35 times a second, Doom's tic rate. Runs Doomguy's face
// as ST_updateFaceWidget does: five pain levels by health, glancing around every half second,
// turning towards an attacker (or "ouch" for more than 20 damage) for a second after a hit.
class DoomHud : public QObject
{
 public:
  DoomHud(const DoomMessenger* doom, const std::filesystem::path& dir, QObject* parent)
    : QObject(parent), fDoom(doom)
  {
    fBase = Load(dir);
    // status bar styles and face sets: hud/<name>/ replaces the pictures it has
    for (const char* skin : {"lhc", "instruments", "physicist"}) {
      HudPatches pictures = Load(dir / skin);
      if (!pictures.empty()) fSkins[skin] = pictures;
    }
    fPatches = fBase;
    if (!fPatches.count("STBAR"))
      G4cout << "No status bar graphics in " << dir << " (run doom2gdml.py, and copy the hud/ folder "
             << "next to the GDML file): plain HUD." << G4endl;
    startTimer(kTicMs);
  }

  ~DoomHud() override { delete fOverlay; }  // if its view still exists

  // The pictures listed in dir/offsets.txt
  static HudPatches Load(const std::filesystem::path& dir)
  {
    HudPatches patches;
    std::ifstream offsets(dir / "offsets.txt");
    std::string name;
    int left, top;
    while (offsets >> name >> left >> top) {
      QImage image(QString::fromStdString((dir / (name + ".png")).string()));
      if (!image.isNull()) patches[QString::fromStdString(name)] = {image, left, top};
    }
    return patches;
  }

 protected:
  void timerEvent(QTimerEvent*) override
  {
    QWidget* view = VisibleView();
    if (!view) return;
    if (!fOverlay)
      fOverlay = new HudOverlay(view, fPatches);
    else if (fOverlay->parentWidget() != view)
      fOverlay->setParent(view);
    if (fOverlay->geometry() != view->rect()) fOverlay->setGeometry(view->rect());
    if (!fOverlay->isVisible()) fOverlay->show();
    fOverlay->raise();

    HudState state;
    state.health = fDoom->Health();
    state.armor = fDoom->Armor();
    state.god = fDoom->God();
    state.skin = QString::fromStdString(fDoom->Face());
    state.style = QString::fromStdString(fDoom->HudStyle());
    if (!fSkins.count(state.style)) state.style = "doom";  // no hud/lhc/: Doom's own bar
    const QString weapons = QString::fromStdString(fDoom->WeaponStyle());
    if (state.skin + "/" + state.style + "/" + weapons != fSkin) {
      // the overlay draws from fPatches: the base set, then the bar style, weapons and face set
      fSkin = state.skin + "/" + state.style + "/" + weapons;
      fPatches = fBase;
      for (const QString& set : {state.style, weapons, state.skin}) {
        auto skin = fSkins.find(set);
        if (skin != fSkins.end())
          for (const auto& [name, patch] : skin->second) fPatches[name] = patch;
      }
    }
    state.crosshair = fDoom->Crosshair();
    const auto [kills, monsters] = Bestiary::Instance().Kills();
    if (kills >= 0) {
      fKills = kills;
      fMonsters = monsters;
    }
    state.kills = fKills;
    state.monsters = fMonsters;
    // Doom: damagecount grows by the damage, drops by one a tic; red palette (count + 7) / 8
    const int taken = fDoom->DamageTaken(), hit = taken - fTaken;
    fTaken = taken;
    fDamageCount = std::min(100, std::max(0, fDamageCount - 1) + hit);
    state.red = std::min(8, (fDamageCount + 7) / 8);
    state.face = Face(hit);

    const Weapon& weapon = fDoom->CurrentWeapon();
    state.weapon = QString::fromStdString(weapon.sprite);
    state.flashSprite = QString::fromStdString(weapon.flash);
    state.fireFrame = QChar(weapon.fireFrame);
    if (state.weapon != fWeapon) {  // its name, for two seconds, as Doom shows pickups
      fWeapon = state.weapon;
      if (fNameTics >= 0) fNameTics = 70;  // not for the one we start with
    }
    fNameTics = std::max(0, fNameTics - 1);
    const int shots = fDoom->ShotsFired();
    if (shots > fShots) fFlashTics = 4;
    fShots = shots;
    const G4double charge = fDoom->ChargeTics();  // the BFG flashes for its last 10 tics of charging
    state.flash = fFlashTics > 0 || (charge >= 0 && charge >= weapon.charge - 10);
    fFlashTics = std::max(0, fFlashTics - 1);
    for (int a = 0; a < kAmmoTypes; ++a) {
      state.ammoAll[a] = fDoom->Ammo(a);
      state.maxAll[a] = fDoom->MaxAmmo(a);
    }
    for (int i = 0; i < 6; ++i) state.owned[i] = fDoom->Owns(i + 2);
    state.ammo = state.ammoAll[weapon.ammo];
    state.keys = fDoom->Keys();
    state.inter = fDoom->Tally();
    if (const G4String message = fDoom->Message(); !message.empty()) state.messages << QString::fromStdString(message);
    if (state.health <= 0)
      state.messages << (state.style == "lhc" ? "BEAM DUMP - PRESS FIRE TO REINJECT" : "PRESS FIRE TO RESTART");
    if (fNameTics > 0) state.messages << QString::fromStdString(weapon.name).toUpper();
    if (state.god) state.messages << "DEGREELESSNESS MODE ON";
    fOverlay->Show(state);

    // debugging: DOOM_HUD_SNAPSHOT=prefix saves the HUD alone (on grey) once a second
    static const char* snapshot = std::getenv("DOOM_HUD_SNAPSHOT");
    if (snapshot && ++fTics % 35 == 0) {
      QImage image(fOverlay->size(), QImage::Format_ARGB32_Premultiplied);
      image.fill(QColor(90, 90, 90));
      fOverlay->render(&image);
      image.save(QString("%1_%2.png").arg(snapshot).arg(fTics / 35, 2, 10, QChar(u'0')));
    }
    // DOOM_WINDOW_SNAPSHOT=prefix saves the whole Geant4 window once a second, plus where the 3D
    // view is in it (x y w h in a .txt), to paste the view and HUD snapshots into
    static const char* window = std::getenv("DOOM_WINDOW_SNAPSHOT");
    if (window && fTics % 35 == 0) {
      const QString name = QString("%1_%2").arg(window).arg(fTics / 35, 2, 10, QChar(u'0'));
      fOverlay->window()->grab().save(name + ".png");
      const QPoint at = fOverlay->mapTo(fOverlay->window(), QPoint(0, 0));
      std::ofstream(name.toStdString() + ".txt")
          << at.x() << ' ' << at.y() << ' ' << fOverlay->width() << ' ' << fOverlay->height() << '\n';
    }
  }

 private:
  static constexpr int kTicMs = 1000 / 35;

  QString Face(int hit)
  {
    const int health = fDoom->Health();
    if (health <= 0) return "STFDEAD0";
    const int pain = (100 - std::min(health, 100)) * 5 / 101;  // ST_calcPainOffset
    if (hit > 0) {  // the newest hit decides, for a second
      const int side = fDoom->LastHitSide();
      fHurtFace = hit > 20 ? QString("STFOUCH%1").arg(pain)
                  : side == 0 ? QString("STFKILL%1").arg(pain)  // head-on
                  : QString(side > 0 ? "STFTR%1" : "STFTL%1").arg(pain) + "0";  // not "%10": that is arg 10
      fHurtTics = 35;
    }
    if (fHurtTics > 0) {
      --fHurtTics;
      return fHurtFace;
    }
    if (fDoom->God()) return "STFGOD0";
    if (--fLookTics <= 0) {  // a new glance every half second
      fLook = static_cast<int>(fRandom() % 3);
      fLookTics = 17;
    }
    return QString("STFST%1%2").arg(pain).arg(fLook);
  }

  // The 3D view on screen: OGL Qt and TSG Qt GLES views are QOpenGLWidgets
  static QWidget* VisibleView()
  {
    for (QWidget* top : QApplication::topLevelWidgets())
      for (QWidget* w : top->findChildren<QWidget*>())
        if (w->inherits("QOpenGLWidget") && w->isVisible()) return w;
    return nullptr;
  }

  const DoomMessenger* fDoom;
  HudPatches fBase, fPatches;  // fPatches: what the overlay draws, fBase plus the style and face set
  std::map<QString, HudPatches> fSkins;  // styles and face sets, by name
  QString fSkin;  // "face/style/weapons" that fPatches was made for
  QPointer<HudOverlay> fOverlay;
  int fTaken = 0, fKills = 0, fMonsters = 0, fDamageCount = 0;
  QString fHurtFace;
  int fHurtTics = 0, fLook = 0, fLookTics = 0, fTics = 0;
  QString fWeapon;
  int fNameTics = -1, fFlashTics = 0, fShots = 0;  // fNameTics -1: no weapon seen yet
  std::minstd_rand fRandom;
};
#endif

int main(int argc, char** argv)
{
  if (argc < 2) {
    G4cerr << "usage: " << argv[0] << " level.gdml [macro.mac]" << G4endl;
    return 1;
  }
  G4UIExecutive* session = (argc == 2) ? new G4UIExecutive(argc, argv) : nullptr;

  auto* runManager = G4RunManagerFactory::CreateRunManager(G4RunManagerType::Default);
  auto* doomLevel = new DoomLevel(argv[1]);
  new WorkerGeometry;  // the master's copy of /doom/worker/geometry (kept to the end)
  runManager->SetUserInitialization(doomLevel);
  runManager->SetUserInitialization(G4PhysListFactory().GetReferencePhysList("FTFP_BERT"));
  runManager->SetUserInitialization(new Actions);

  auto* vis = new G4VisExecutive;
  vis->Initialize();
  auto* doom = new DoomMessenger(vis);

  G4UImanager* ui = G4UImanager::GetUIpointer();
  ui->ApplyCommand("/run/initialize");
  // the fireballs, drawn once vis.mac adds them to the scene (/vis/scene/add/userAction). Their
  // extent is a token box at the centre, always inside the world: the actions outlive the level,
  // and a level's extent would spoil the next, smaller one's camera maths (the scene extent
  // decides where dollyTo puts the eye).
  const G4VisExtent actionExtent(-1 * mm, 1 * mm, -1 * mm, 1 * mm, -1 * mm, 1 * mm);
  vis->RegisterRunDurationUserVisAction("Doom missiles", new MissileVis(doom->Missiles(), [doom] { return doom->TexturesOn(); }), actionExtent);
  // the doors, lifts and switches (<map>_level.txt from doom2gdml.py), and the other levels
  const auto level = std::filesystem::path(argv[1]);
  doom->SetLevel(doomLevel, level);
#ifdef DOOM_QT
  // the monsters' sprites, likewise (/doom/sprites)
  const auto spriteDir = std::filesystem::path(argv[1]).parent_path() / "sprites";
  if (std::filesystem::exists(spriteDir / "offsets.txt")) {
    auto* sprites = new SpriteVis(vis, std::make_shared<SpriteSet>(spriteDir));
    vis->RegisterRunDurationUserVisAction("Doom sprites", sprites, actionExtent);
    doom->SetSprites([sprites] { return sprites->Stale(); },
                     [sprites](const G4String& type) { return sprites->Sprited(type); });
  }
#endif
  if (session) {
#ifdef DOOM_QT
    DoomInput* input = nullptr;
    DoomHud* hud = nullptr;
    if (session->IsGUI() && qApp) {
      if (auto* qt = dynamic_cast<G4UIQt*>(session->GetSession())) qt->GetMainWindow()->setWindowTitle("Geant4Doom");
      qApp->installEventFilter(input = new DoomInput(doom, qApp));
      doom->SetChargeDeferred(true);  // DoomInput lets the BFG's charged shot go
      hud = new DoomHud(doom, std::filesystem::path(argv[1]).parent_path() / "hud", qApp);
      auto* textures = new TexturePass(doom, vis, doom->Level(), level.parent_path(), qApp);
      doom->SetTexturesAvailable(textures->Loaded());
      doom->OnWorldChanged([textures](const DoomWorld* world) { textures->SetWorld(world); });
    }
#endif
#ifdef DOOM_SOUND
    std::unique_ptr<DoomSound> sound;
    if (session->IsGUI()) {
      sound = std::make_unique<DoomSound>(level.parent_path() / "sounds", level.parent_path() / "music");
      doom->SetSoundPlayer([&sound](const G4String& lump, G4double gain, G4double pitch, G4double pan) {
        sound->Play(lump, gain, pitch, pan);
      });
      doom->SetMusicPlayer([&sound](const G4String& song, G4double volume) { sound->Music(song, volume); },
                           SongForMap(level.stem().string()));
    }
#endif
    // vis.mac from the working directory, or else the one next to the binary
    std::filesystem::path visMac = "vis.mac";
    if (!std::filesystem::exists(visMac)) visMac = std::filesystem::canonical("/proc/self/exe").parent_path() / "vis.mac";
    ui->ApplyCommand("/control/alias level " + (std::filesystem::absolute(level).parent_path() / level.stem()).string());
    ui->ApplyCommand("/control/execute " + visMac.string());
    doom->SettleCamera();
    session->SessionStart();
#ifdef DOOM_SOUND
    doom->SetSoundPlayer(nullptr);
    doom->SetMusicPlayer(nullptr, "");
    sound.reset();
#endif
#ifdef DOOM_QT
    delete hud;
    delete input;  // while Qt's Wayland connection, which a pointer lock uses, still exists
#endif
    delete session;
  }
  else {
    ui->ApplyCommand(G4String("/control/execute ") + argv[2]);
  }
  delete doom;
  delete vis;
  delete runManager;
  return 0;
}
