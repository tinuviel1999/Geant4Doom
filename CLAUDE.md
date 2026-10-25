# Geant4Doom: project notes

Converts Doom maps (WAD) into Geant4 GDML and renders them with Geant4's vis drivers.
See README.md for usage. This file records what was learned building it.

## Layout
- `doom2gdml.py`: WAD -> GDML converter. Also writes `<map>.json` (offsets, player start),
  `<map>_camera.mac` (OpenGL/Qt/TSG), `<map>_camera_rt.mac` (RayTracer), `<map>_gun.mac`,
  every DS* sound lump as `sounds/<name>.wav`, the D_* songs as `music/D_*.mid` (MUS converted by
  `mus2mid`) plus `music/GENMIDI.op2`, the status bar graphics as `hud/`, and the monster sprites as
  `sprites/` (plus offsets.txt), next to the GDML. The map argument is one map, a comma list or
  `all` (every E#M#/MAP## lump); `-o` is the .gdml for one map, else the output folder (shared
  sounds/music/hud/sprites/textures). `doom1/` (The Ultimate Doom, private: id's assets) and
  `freedoom1/` hold all 36 maps each. HUD_LUMPS include WI*, CWILV* (Doom II names) and INTERPIC.
- `doom_surfaces.py` (called by doom2gdml.py): the map for doomviewer's moving parts and textured
  view, `<map>_level.txt` (origin, `texture` list, `anim` sequences, `switch` SW1/SW2 pairs, `sky`,
  raw `sector`/`side`/`line` records in Doom units, `flat` triangles per sector, and the key and
  teleport-destination `thing`s; format in its header) and `textures/` (wall textures composed from
  PNAMES patches, flats, sky). The walls are built in C++ (`World::Surfaces`) from the live sector
  heights, by Doom's rules: flats on the 64-unit grid (u = x, v = -y); one-sided middle textures top at the
  ceiling or bottom at the floor (lower unpegged, flag 16); upper textures bottom at the back ceiling
  or top at ours (upper unpegged, flag 8), none between two skies; lower textures top at the back
  floor or aligned with our ceiling (lower unpegged); two-sided middle textures once, not tiled; u
  along the line from v1 (v2 on the back side) + sidedef offsets; fake contrast +-16 light for lines
  along y/x; p_spec.c animdefs sequences in lump order; sky SKYn by episode/map (`textures/sky_SKYn.png`,
  as maps share the folder).
  doom2gdml.py makes the world box 3x the level's height so slabs can slide up and down in it.
- `app/doomworld.hh`: `doom::World`, Doom's sector movers and line specials without Qt (see
  "Doors, lifts, switches" below).
- `physicist_face.py`: restyles the extracted Doomguy faces (hud/STF*.png) as a totally-not-famous
  physicist into hud/physicist/ (run by doom2gdml.py too). Per face: flood-fill the dark hair from
  the top (shallower over the forehead, so the "ouch" wrinkles stay skin), recede the very dark
  fringe and an M-shaped hairline into forehead skin, recolour to black with wave bands,
  add a row of volume on top (canvas +1 row, top offset +1), and darken the brows above the eyes.
  doomviewer's `/doom/face physicist|doomguy` (default doomguy; the physicist is still buggy) overlays hud/<face>/ on the base set.
- `lhc_hud.py`: the accelerator status bar into hud/lhc/ (run by doom2gdml.py too): a generated
  STBAR (blue dipole cryostat, steel flanges at the panel borders x 48/142/178/237/249, hazard
  stripes, recessed screens, 3x5-font labels BUNCHES/HEALTH/BEAMS/SHIELD, ammo rows LEP/PI/ION/P at
  bar y 5/11/17/23 matching clip/shell/missile/cell, a detector cross-section around the face at
  (160.5,16.5)) and STARMS, plus STTNUM/STTPRCNT/STTMINUS recoloured green and STCFN amber.
  `/doom/hudStyle lhc|doom` (default lhc, falls back to doom without hud/lhc/). DoomHud layers
  base -> style -> face set; with lhc the tally reads EVENTS and death says BEAM DUMP.
- `weapon_sprites.py`: the weapon sprites as particle-physics instruments into hud/instruments/ (run
  by doom2gdml.py too, ~20 s): electron gun (glass tube, cathode glow, copper anodes), magnetic horn
  (pion flash = curling tracks), copper RF linac (frame B: cells lit), blue cryostat (frost, hazard
  band, gauge, C6+), yellow SRF cryomodule cut open on a 9-cell niobium cavity, Penning trap
  (viewport on a green antiproton cloud, brighter in B/C). Shaded receding tubes (`Canvas.tube`,
  bands along t, `bulge(t)` for cells/horns), 4x supersampled, drawn in design coordinates shown
  1.3x wide and 0.75x tall (foreshortened) with design (160, 205) at screen (160, 177). Placement
  lessons: Doom's weapon tops stay at about row 113-122 (never up to the crosshair; the HUD
  stretches rows 1.2x), the view ends at row 168, so details and hands (`hand_on`, at the tube's
  side, design y 168) must be in rows ~118-165. Offsets: left = 1 - x0, top = 16 - y0 (R_DrawPSprite).
  Only the frames doomviewer shows are made (idle A, fire frame, flash A/B). Preview without the
  viewer by pasting sprite at (1 - left, 16 - top) on a 320x168 view.
  `/doom/weaponStyle instruments|doom` (default doom; the
  instruments are a baseline to improve, e.g. the hands). DoomHud layers base -> bar style ->
  weapon style -> face set.
- `render_g4.py`: geant4_pybind loader, geantino-only physics, no datasets needed.
  `--hide=_ceil` hides ceilings for overviews.
- `flythrough.py`: writes a multi-frame macro (look-around + walk) and assembles GIFs.
- `macros/`: overview, first-person and raytrace macros.
- `app/`: C++ `doomviewer` (CMake, FTFP_BERT, six particle weapons, see below). Reads the
  Color aux tags from GDML.
- `e1m1/`: generated Freedoom E1M1 level. Run scripts from inside it, because the GDML
  and camera macros are resolved relative to the current directory.

## Conversion design
- Subsector polygons come from walking the WAD's own BSP (NODES/SSECTORS/SEGS) and
  clipping. No node builder is needed. Extended/compressed nodes (XNOD/ZNOD) are not supported.
- Per sector: union the pieces (shapely), triangulate with GEOS constrained Delaunay
  (earcut produced holey solids for sectors with several holes), and extrude to closed
  G4TessellatedSolids. Floor slab goes from world bottom to the floor; ceiling slab from
  the ceiling to the top. Sky sectors (F_SKY1) get no ceiling.
- One-sided linedefs become thin wall prisms outside the sector, clipped against the
  sectors and the walls already placed.
- Scale is 1 map unit = 32 mm. Axes: G4 x = Doom x, G4 y = UP, G4 z = -Doom y.
- Colours are the average texture/flat colour from PLAYPAL, scaled by sector light,
  stored as `<auxiliary auxtype="Color" auxvalue="r g b a"/>`.
- The slab() watertightness check reports unmatched edges on stderr. Robustness for the whole
  of Doom/Freedoom (the first `all` runs died on several maps):
  - GEOS's triangulation can add points where a ring touches itself or another. Points on a ring
    edge are inserted into the ring too (else the side wall misses them: a T-junction crack).
    Zero-area triangles are dropped (Geant4: "Facet is too small").
  - A vertex where rings touch appears twice. Walls and caps share one vertex there, so the
    vertical edge is used twice each way; the check accepts that (non-manifold, not a crack).
  - `robust_union`: unary_union, else make_valid + union_all on a 1e-6/1e-3/1e-2 grid, else
    buffer +-1e-3. E3M7 ("unable to assign free hole") and Freedoom maps ("non-noded
    intersection") needed it.

## Geant4 11.4 gotchas found
1. MT RayTracer: `G4RTPrimaryGeneratorAction::SetUp()` does not copy `up`/`headAngle`
   from G4TheMTRayTracer, so it always assumes up = +y. This is why the GDML is y-up.
   Possible upstream bug report.
2. The scene extent covers visible volumes only (the world is invisible). Camera macros add
   `/vis/scene/add/extent` with the world box, so the `dollyTo` maths places the eye exactly.
   Camera position = target + viewpointVector * (R/sin(fov_half) - dolly).
   **Near plane:** Geant4's near distance is cameraDistance - R (at least 1e-6 R), far is
   cameraDistance + R. The camera macros' target 1000 mm ahead gave near 0.1 mm, far 103 m (ratio
   1e6; a 24-bit depth buffer then resolves ~1.5 m at 50 m): distant surfaces flickered and far
   monsters' sprites sank into the TexturePass floors. doomviewer's `Aim` keeps cameraDistance =
   R + kNearPlane (4 units: near 128 mm, far ~204 m, ratio 1600) by adjusting the dolly, and
   `SettleCamera` applies it after every camera macro (startup, LoadLevel, Restart). Code that
   derives the eye from GetCameraDistance is unaffected; a macro that sets targetPoint = eye + 1000 mm
   must re-apply the camera macro's dollyTo first.
3. OpenGL/TSG apply the field half-angle vertically; the RayTracer applies it horizontally.
4. TSG_OFFSCREEN (zb) mangles triangles that cross the near plane, which matters close to walls.
   Use the RayTracer for fly-throughs.
5. RayTracer: one trace per `/vis/viewer/flush`. Do not also call `/vis/viewer/rebuild`,
   which produces duplicate frames.
6. geant4_pybind (pip wheel bundles Geant4 11.4.1, no headers, so it can't build the C++ app):
   - Set `GEANT4_DATA_DIR` to skip the dataset download prompt. `G4ENSDFSTATEDATA` must
     point to a dir with an (empty) ENSDFSTATE.dat.
   - The RayTracer needs an MT run manager plus an action initialization with a generator.
   - Keep Python-side objects (physics constructors, vis attributes) referenced.
   - `G4LogicalVolumeStore` has no size()/indexing; use `parser.GetAuxMap()`.
7. The C++ viewer needs the real physics datasets. Unset the stub `GEANT4_DATA_DIR` first,
   because Geant4 11 reads it too.

## doomviewer gameplay
- `/doom/fire [n]` rebuilds the eye from the current viewer (standard target + relative
  target + viewpointVector * GetCameraDistance(extent radius)), sets the globals `gMuzzle`/`gAim`
  (read by every worker's PlasmaRifle, like `gSpread`), particle and energy via `/gun/` commands,
  then runs `/run/beamOn n`. Not `/gun/position`: after a level change the workers run it before
  their geometry update, and its CheckVertexInsideWorld reads the deleted world (segfault). A plain
  `/run/beamOn` shoots from the last /doom/fire's muzzle (the origin before any).
- Volumes named `T<i>_<type>` are monsters. They share one `MonsterSD` per thread, and a
  mutex-guarded `Bestiary` singleton holds their health (Doom spawn health,
  `/doom/energyPerHP`, default 20 MeV). Corpses are repainted on the master after the run,
  never from inside it. A plain `/run/beamOn` still does damage, but the repaint waits for the next `/doom/fire`.
- `/doom/` commands are master-only (`SetToBeBroadcasted(false)`).
- Moving monsters (`/doom/think dt`; in Qt a 10 Hz timer calls it while `/doom/monsters true`):
  - They wake on line of sight, on a shot within 25 m, or when damaged. Awake monsters chase at
    Doom-like speeds, trying 0/±45/±90° around obstacles, using the player's step/ledge/wall rules.
  - MT gotcha: a placement's translation (G4PVData) and an LV's solid (G4LVData) are **per thread**.
    Setting them on the master does not reach the workers, which do the tracking. The `Bestiary`
    holds the truth (`Monster::at`, body or corpse), and the `WorkerSync` run action copies it
    into each worker's slots at BeginOfRunAction. Verified by shooting at the old spawn point
    (no hit) and at the new position (hit).
  - After each step that moved anything: OpenGeometry/CloseGeometry on the world only, then
    `/vis/viewer/rebuild`. **Parallel voxel optimisation gotcha:** the task run manager configures
    it, so CloseGeometry only *queues* the voxel build (`IsParallelOptimisationFinished()` stays
    false) and the workers do it at the start of the next run while other workers already track:
    segfault in G4VoxelNavigation::ComputeStep, but only once a run has several events (the
    scattergun's 7 pellets). 1-event runs never showed it, which is why it was first blamed on the
    RayTracer. Fix: `RequestParallelOptimisation(false, false)` before closing, so the master builds
    the voxels serially (verified: a reset+7-event loop crashed in 2 of 3 runs, then 0 of 5).
    Likewise never flag the run manager (`/run/geometryModified`, GeometryHasBeenModified).
  - Corpses: on death (master, after the run), the LV solid becomes a flat G4Ellipsoid (8 map units
    high). It is flattened along whichever local axis points up, because tubes are rotated.
  - Probes start inside the monster's own body, so `Clearance(p, dir, ignore)` walks out of it first.
  - Known issue: the RayTracer brings its own worker run actions, so WorkerSync does not run for
    it. After monsters move or die it can trace stale worker geometry. (Its one segfault was most
    likely the voxel race above.)
    Use TSG_OFFSCREEN for headless checks of doomviewer; it renders from the master's state and
    writes a PNG to the cwd on every redraw, so run tests from a scratch directory.
  - `/geometry/test/run` lists the same 55 pairs before and after chases and kills. The untouched
    level already has 55 at default tolerance, more than "Validation" below says.
- Walking (`/doom/walk`, `/doom/strafe`, `/doom/turn`) edits the viewer's G4ViewParameters
  directly, keeping the camera distance, then calls `/vis/viewer/refresh`. Collisions use a private
  G4Navigator: a ray at eye height (wall/monster, plus player radius), a ray down (floor,
  24-unit step-up limit) and a ray up (56-unit headroom). Random-walk tested over 3 floor levels.
- WASD keys and mouse look: a Qt6 application-wide event filter (`DoomInput`, enabled when CMake
  finds Qt6 Widgets). It ignores keys while a text field has focus or a run is in progress.
  It tracks held keys from press/release events, ignoring OS auto-repeat (which repeats only the
  last key), and adds up mouse deltas. A 30 Hz QObject timer sends one
  `/doom/move fwd right turn pitch` per tick, so everything combines. A blocked diagonal step
  falls back to its forward or strafe part (wall sliding).
- Mouse look: left-drag on a widget that inherits `QOpenGLWidget` (the 3D view of both OGL Qt and
  TSG Qt GLES; the TSG ZB `pixwin` is a plain QWidget and is not handled). Pitch is clamped to ±85°.
  Walking uses the horizontal part of the view direction.
- Captured mouse look is on from the start: `fAutoCapture` makes the 28 ms world timer call
  `Capture(true)` (quiet) whenever the app has an active window, is idle and nothing is captured,
  so it also comes back after alt-tab (ApplicationDeactivate releases). M/Esc release it and clear
  fAutoCapture (for Geant4's command line and menus); M sets it again. A Wayland compositor without
  pointer lock clears it after the first try.
- Captured mouse look (M): on X11 it warps the pointer with `QCursor::setPos`, which native Wayland
  refuses. On Wayland, `WaylandLock` (built when CMake finds wayland-client, wayland-scanner,
  wayland-protocols and Qt6::GuiPrivate, which sets `DOOM_WAYLAND`) uses zwp_pointer_constraints_v1
  (a one-shot lock) plus zwp_relative_pointer_v1 directly. They are bound on a private wl_event_queue
  (via a wl_display proxy wrapper), which Qt's reader thread fills, and the 30 Hz tick dispatches it.
  The wl_surface comes from `QPlatformNativeInterface::nativeResourceForWindow("surface")`.
  `DoomInput` is deleted before the G4UIExecutive, while Qt's wl_display still exists.
  Tested on MangoWM, which offers both protocols (checked with a registry probe).
- Monsters fight back (`Fight`/`Strike` in the messenger, `kMonsterAttacks` from info.c/p_enemy.c),
  inside `/doom/think`:
  - Every chase step (`chase` tics) a monster bites if the player is within 60 units (MELEERANGE - 20
    + player radius) and in sight. It considers a ranged attack only when its `moveCount` (Doom's
    movecount, 0..15 steps per direction) reaches 0, after an 8-step reaction time, on
    P_CheckMissileRange's roll (P(fire) = 1 - min(dist - 64 [-128 without melee], 200)/256), or at
    once when just hurt (MF_JUSTHIT). After an attack it picks a new walk count (MF_JUSTATTACKED).
  - The attack lands after `windup` tics and the monster stands still until `recover` ends.
    Hitscan: Doom's triangular ±22.4° spread per pellet, a hit if within atan(16 units / distance).
    Imps claw in melee range and otherwise throw a fireball at 10 units/tic, which flies in 8-unit
    sub-steps (navigator `Clearance`, ignoring its thrower) and bursts on solids or on the player's
    cylinder. Missiles are drawn by `MissileVis`, a run-duration user vis action (vis.mac:
    `/vis/scene/add/userAction`), so a tick with missiles in flight does `/vis/viewer/rebuild`.
  - Player: 100 health, no armour, `/doom/god`. Death sinks the eye to 6 units above the floor and
    blocks moving and firing; fire/use while dead or `/doom/reset` restarts the level (`Restart`,
    G_DoReborn: Bestiary and World reset (keys, secrets, level time too), pistol start, the eye back
    at the start via `<map>_camera.mac`; no GDML reload). Before, it stood up where it fell.
    `Respawn` (in place) is only the fallback without a camera macro. Rates measured headless in god mode over 30 s at 7 m:
    34 fireballs, 18 shotgun blasts, 29 claws from imps that reached melee range.
- HUD (`DoomHud` + `HudOverlay`, Qt only): Doom's status bar from the WAD's own graphics.
  `doom2gdml.py` writes STBAR, STARMS, STTNUM/STTPRCNT, STYSNUM/STGNUM, STF* faces, STKEYS and the
  STCFN message font as `hud/NAME.png` (own stdlib PNG writer), with the patch offsets in
  `hud/offsets.txt`; doomviewer loads `hud/` next to the GDML. Drawn with st_stuff.c coordinates on
  Doom's 320x200 screen (pixels 1.2x tall, nearest-neighbour), capped at 1/5 of the view height.
  Numbers as STlib_drawNum (right end at x). Ammo box blank (no ammo system), armor 0%, ARMS slot 6
  (plasma) lit. Doomguy's face follows ST_updateFaceWidget: 5 pain levels, a glance every 17 tics,
  for 35 tics after a hit a turn towards the attacker (`LastHitSide`; STFKILL head-on) or STFOUCH
  for >20 damage, god face, dead face. (Qt gotcha: `QString("STFTR%10").arg(n)` is placeholder 10, not
  %1 + "0": it gave STFTR3, a missing patch, and the face vanished on side hits.) Red palette = min(8, (damagecount + 7) / 8) / 9 as alpha;
  damagecount += damage, -1 per tic. Messages and the kill tally use STCFN. Falls back to outlined
  text without `hud/`. `/doom/crosshair` toggles the (non-Doom) crosshair. A transparent,
  mouse-transparent child QWidget of the visible QOpenGLWidget (re-found every tick, 35 Hz), so it
  never triggers a Geant4 redraw.
  `DOOM_HUD_SNAPSHOT=prefix` saves the HUD layer alone once a second: use it (with
  QT_QPA_PLATFORM=offscreen) to check the HUD. `DOOM_WINDOW_SNAPSHOT=prefix` grabs the whole Geant4
  window (help tree, output, session line; the GL area comes out blank) plus `prefix_NN.txt` with the
  view's x y w h in it: paste view + HUD there for README screenshots (a big offscreen screen:
  `QT_QPA_PLATFORM=offscreen:configfile=screen.json`).
  **Deadlock gotcha:** a Qt viewer refresh (`/vis/viewer/refresh` from SetCamera) processes Qt
  events, so Qt timers (HUD, input) can run re-entrantly *inside* any /doom/ command. The first HUD
  locked the Bestiary mutex while Think held it inside ForEach (death -> camera drop -> refresh),
  and froze the app. Now: never redraw inside `Bestiary::ForEach` (the death fall is deferred to
  the end of Think), and Qt-timer code only `try_lock`s the Bestiary (`Kills()`).
- Sound (`DoomSound`, built when CMake finds libpulse-simple, which sets `DOOM_SOUND`): loads
  `sounds/*.wav` from the GDML's directory and mixes up to 16 voices (44.1 kHz stereo float) on a
  `std::thread` that blocks in `pa_simple_write` (50 ms target latency; PipeWire's pulse server).
  **Not Qt Multimedia:** Qt starts its audio-device thread with a fixed 1 MiB stack
  (`QThread::setStackSize(0x100000)`), but Geant4's static TLS is 1.25 MB (1.23 MB in
  libG4processes_hadronic alone). glibc refuses a stack smaller than the static TLS (EINVAL,
  "QThread::start: Thread creation error"), and `QMediaDevices::defaultAudioOutput()` then hangs
  forever. Any library thread created with a small explicit stack hits the same problem in a
  Geant4 process. Check with `readelf -lW lib.so | grep TLS`.
  DMX lumps are format 3, a u16 rate, a u32 count, then u8 PCM (with 16 pad bytes either side).
  The messenger only holds a `SoundPlayer` std::function, which is null in batch mode.
  Positional like Doom: full volume within 200 map units, silent past 1200, equal-power pan.
  Events: dsplasma on fire, dsoof once per wall bump, sight sounds when a monster wakes by
  seeing you, the same when it hears a shot, then after the run pain/death per hit, dsslop when
  health <= -spawn health (gibbed), dsbarexp for barrels. Pain and death sounds depend on energy:
  x = log10(edep / 100 MeV), gain = clamp(0.4 + 0.4x, 0.25, 1), pitch = clamp(1.25 - 0.25x, 0.7, 1.4),
  so small hits are quiet and squeaky, showers loud and deep. Lump lists per type follow info.c.
  `/doom/volume` sets master gain; `/doom/soundLog true` prints every sound with its gain, pitch and
  pan, which is the way to test sound headless.
- Music (`app/doommusic.hh`, `DoomMusic`, part of DOOM_SOUND): the WAD's MIDI songs on Nuked-OPL3
  (`app/third_party/nuked-opl3`, LGPL-2.1, downloaded from github.com/nukeykt/Nuked-OPL3 master;
  `OPL3_GenerateResampled` at 44.1 kHz) with GENMIDI instruments, rendered into the same mixer
  thread (music first, then the effects on top). The driver re-implements DMX as documented by
  Chocolate Doom's i_oplmusic.c (consulted, not copied; GPL): Doom 1.9 voice allocation, 18 voices
  (DMX's -opl3 mode, so stereo; the DMX left/right swap is corrected), carrier at max attenuation
  until SetVolume, kVolume curve, double-voice instruments (flag 4, second voice detuned by
  fineTuning/2 - 64), fixed-pitch percussion (flag 1; MIDI channel 9, keys 35-81 -> instruments
  128+), pitch bend MSB only (32 steps per semitone), controllers 7/10/0x7b. DMX's frequency table
  is computed: F-number = round(16.3516 Hz * 2^((index - 64)/384) * 2^20 / 49716) for index < 284,
  then one looped octave with the block in bits 10-12; it matches the table within rounding. Note
  0 is C0 = MIDI 12: GENMIDI's offsets (mostly -12) make up for it, and A4 renders at 440 Hz on
  piano, bass, strings and lead (FFT check). All tracks are merged into one tick-sorted event list;
  the song loops. Song per level: `SongForMap` (E#M# -> D_E#M#, MAP01-32 -> Doom II's names).
  `/doom/music on|off|D_NAME`, `/doom/musicVolume` (default 0.5; peak ~0.6 at gain 1 on D_E1M1).
  Cost: 30 s of music renders in 0.7 s. Headless audio test without the speakers: `pactl
  load-module module-null-sink sink_name=doomtest`, run with `PULSE_SINK=doomtest`, record
  `parecord -d doomtest.monitor`, then `pactl unload-module`.
- Weapons (`kWeapons`, Doom slots, keys 2-7, `/doom/weapon`): electron gun (300 MeV e-, pistol),
  pion scattergun (7 x 500 MeV pi+, shotgun), muon chaingun (2 GeV mu-), carbon-ion cannon
  (3 GeV 12C = 250 MeV/u, rocket launcher; `"ion 6 12"` -> `/gun/particle ion` + `/gun/ion 6 12`),
  proton rifle (1 GeV p, plasma), antiproton BFG (40 x 300 MeV anti_proton, ±45° triangular spray,
  40 cells per shot: `ammoUse`). Doom's refire tics (14/37/4/20/3/60), ±5.6° triangular spread (first shot of a burst exact for
  electron gun and chaingun; burst = last shot within 1.5 refires), ammo pools clip/shell/cell/misl
  (max 200/50/300/50), automatic switch when empty (P_CheckAmmo, only to owned weapons, never to
  slots 5/7: `Dangerous`). Weapons are found by slot (`WeaponIndex`), not by index: inserting
  weapons shifts the indices.
  Pickups (`kPickups`, p_inter.c): pistol start (`PistolStart`: electron gun + 50 clip, also on
  restart/reset); `/doom/idkfa` gives all weapons, the current max ammo and all keys. P_GiveWeapon:
  2 clips of its ammo (1 if dropped), new weapon selected at once, not taken if owned and its
  ammo is full. P_GiveAmmo: clipammo 10/4/20/1, a dropped clip gives half, refused at max; first
  shells/cells into an empty pool switch from the pistol to an owned scattergun/rifle. Backpack (8)
  doubles fMaxAmmo once and gives a clip of each. Doom's ammo messages, "You got the <weapon
  name>!" + dswpnup. `World::Touch(x, y, z, give)` handles keys itself and asks the messenger for
  the rest. Deaths: `Bestiary::TakeDeaths` after LayCorpses -> `kDrops` (zombieman clip,
  shotgunguy shotgun, chaingunner chaingun) -> `World::Drop` (a `dropped` thing, removed on
  Reset). Item things (ITEMS in doom_surfaces.py) and their sprites (SPRITE_PREFIXES) are drawn by
  TexturePass (`kItemSprites`) as billboards, so only in the textured view. Tested headless: shells
  picked up, a shotgun guy killed with the electron gun dropped the scattergun, picking it up gave
  4 shells and switched to it, reset back to pistol start.
  Health and armour (`kSupplies`, `Supplies`): bonuses +1 up to 200 (always taken), stimpack +10 /
  medikit +25 up to 100 (refused at 100; "REALLY need" when below 25 before, not Doom's buggy
  after-check), green/blue armour set 100/200 and class 1/2 unless already at that much,
  soulsphere +100 up to 200 (dsgetpow), megasphere 200/200 blue. Hurt applies P_DamageMobj's armour:
  saved = damage/3 (green) or /2 (blue), capped by the points left (class drops to 0). Respawn
  clears armour. HUD armour number; `/doom/status` prints it. TexturePass `kAnimations` (info.c):
  keys AB/10 tics (B bright), bonuses ABCDCB/6, armour AB/7 (B bright), soulsphere and megasphere
  bright; `Frame()` folds the animated items' frame numbers in, so the view repaints when they
  change. Tested headless: armour picked up (refused again at 100), zombie hits 6 -> 2 saved,
  15 -> 5 saved, health bonus, medikit at 16%. Spread is applied per event in the worker's gun (`gSpread`,
  set by the master before beamOn); particle and energy go through `/gun/` commands on every shot.
  Energies tuned against the imp (water) at 2.5 m, 30 shots each, damage per shot vs Doom:
  e- 11.9 (pistol 10), pi+ 68.5 (shotgun ~70), mu- 14.5 (chaingun 10), p 23.7 at 1 GeV (plasma 22;
  10 GeV gave 120, from hadronic showers). Damage above the kill is not counted (the corpse takes
  none), so tune strong weapons with deposits from immortal monsters (temporarily skip the health
  decrement) and convert edep / 20 MeV: 12C 141 per hit (one target; Doom rocket ~90 + splash);
  BFG ~1100 on the imp plus 4.3 monsters hit per shot (each stopped antiproton ~50 HP; Doom's ball
  100-800 plus tracers). Raising the antiproton energy (1, 3 GeV) or narrowing the spray to ±22.5°
  only concentrates it more. 20 BFG shots take ~5 s. /control/echo markers are broadcast and
  interleave in MT logs: split shots on a master-only line (`You take the ...` from /doom/weapon). Space or the captured left button fires while held, at
  the weapon's refire (DoomInput::Trigger/Shoot). The HUD draws the weapon sprite (R_DrawPSprite
  position: x 1 - leftoffset, y 16 - topoffset in the 168-row view) with the firing frame and
  muzzle flash for 4 tics after a shot, the ammo counts, owned slots in ARMS, and the weapon name
  for 2 s after a switch. Weapon sprites (PISG/PISF, SHTG/SHTF, CHGG/CHGF, MISG/MISF, PLSG/PLSF, BFGG/BFGF) are in hud/.
  BFG charge-up (`Weapon::charge`, 30 tics as Doom's S_BFG1-3): the trigger plays dsbfg and starts
  the charge; `/doom/fire` while charging does nothing until `ChargeDue()`, then lets the shot go (the
  particles leave from wherever the player is by then); weapon switches are refused; death and
  restart cancel it. DoomInput's 30 Hz tick fires it when due (measured 31.1 tics) and keeps
  running while charging (the 10 Hz think timer restarts it for a typed /doom/fire). The HUD flashes
  BFGF for the last 10 tics (A_GunFlash). Only with `SetChargeDeferred(true)` (Qt session); batch
  macros fire at once, so headless tests are not slowed.
- Sprites (`SpriteSet` + `SpriteVis`, Qt builds): doom2gdml.py writes the monster sprite lumps as
  `sprites/NAME.png` + offsets.txt (left = centre column, top = rows above the feet) and each
  thing's angle as an `Angle` aux tag. Each picture is cut into one-colour runs per row (4 bits a
  channel). They are drawn as quads, in one G4PolyhedronArbitrary per colour for all monsters, by a
  run-duration user vis action like MissileVis. Billboards face the eye horizontally (in 10° steps),
  at 1 map unit a pixel. Rotation follows R_ProjectSprite: floor((angle viewer->thing - facing + 202.5°)/45°)
  mod 8, +1. Mirrored lumps come from the second half of the name (TROOA2A8). Frames (`kSpriteFrames`) use
  wall-clock `Now()`: walk if it stepped < 0.3 s ago, attack while busy, pain (set by LayCorpses for
  ~6 tics after a hit), death frames 5 tics each from `died`, idle alternating A/B every 10 tics.
  The bodies get `Bestiary::Look`: invisible, or a green wireframe with `/doom/hitboxes`.
  - Cost: drawing all 51 monsters cost +24 ms per rebuild, so only monsters in front and in sight
    are drawn (5 navigator rays to the picture). The rays are expensive because of the tessellated
    walls (7-13 ms for all), so each result is cached until the eye moves 6 units or the monster
    moves, with at most 6 re-tests per call. A ray counts if it gets within max(half the picture's width, the body's
    radius) + 4 units of its point: it stops at the invisible body, and narrow frames (demons
    head-on/from behind, TROOA4A6, Freedoom's BOSS B/D 2/8: 20 Doom and 24 Freedoom walk frames
    have half-width + 4 < radius) used to fail all five rays, so monsters vanished as they turned.
  - Rebuild only when needed: `Stale()` compares a signature (patch, flip, yaw step, position) with
    the last Draw. SetCamera rebuilds instead of refreshing when stale, Think rebuilds when stale,
    and the Qt 10 Hz timer sends `/doom/animate` when the monsters are off.
    Measured (offscreen OGL Qt, a few monsters in view): rebuild 19.7 vs 18.1 ms without sprites,
    strafe step 21 ms vs 2 ms (it mostly rebuilds).
  - Don't call HepPolyhedron::SetReferences on the open quads: it prints "List N is not empty"
    hundreds of thousands of times. Drawing needs no neighbours.
  - Sprites read the Bestiary with `Snapshot` (try_lock; it keeps the last copy if busy).
  - The RayTracer draws no user vis actions, and the bodies are invisible, so it shows no monsters.
- Textured level (`TexturePass`, Qt; `/doom/textures`, on in vis.mac): Geant4's vis has no
  textures, so the pass draws the surfaces itself with OpenGL into Geant4's view widget.
  - Hook: an event filter on the QOpenGLWidget catches the Paint event, sends it on itself
    (QOpenGLWidget::paintEvent -> Geant4's paintGL into the widget FBO), then draws on top and
    returns true. `aboutToCompose` was tried first but never fires under QT_QPA_PLATFORM=offscreen.
  - The default `/vis/open` here is TOOLSSG_QT_GLES, not OGL; both are QOpenGLWidgets in a
    compatibility context, and both work. After Geant4's paint the fixed-function matrices are
    identity and the viewport 1x1, so the pass rebuilds the camera as G4OpenGLViewer::SetView does
    (glFrustum from GetNearDistance/GetFarDistance/GetFrontHalfHeight with the aspect ratios, then
    gluLookAt), which matches TSG's depth too. Verified by drawing both levels at once: they z-fight
    (textured floors, flat walls in one frame), so occlusion between Geant4's objects (sprites,
    tracks, wireframes) and the textured level is right in both directions.
  - `/doom/textures true` hides the S*/L* level volumes (keeps their vis attributes to restore), so
    Geant4 draws only things and tracks: rebuild 10.8 ms vs 19.4, strafe step 14 vs 28 ms (offscreen).
  - Shader (GLSL 1.20, QOpenGLShaderProgram): Doom's light diminishing, colormap =
    (15 - light/16)*4 - 1280/distance(map units), clamped 0..31, brightness 1 - colormap/32; alpha
    test for grates. Textures GL_NEAREST, mipmapped (GL_GENERATE_MIPMAP). Animated flats switch every
    8 tics; the 35 Hz timer calls update() when the frame changes (Geant4 replays its display lists).
  - Sky: a cylinder round the eye drawn last with glDepthRange(1,1) and GL_LEQUAL, so it fills only
    what nothing covers; u = Doom angle * 1024/360° texels, v = 100 - 160 * tan(elevation) (skytexturemid).
  - `DOOM_VIEW_SNAPSHOT=prefix` saves the finished 3D view (glReadPixels) about once a second: the
    way to check the textured view headless (QT_QPA_PLATFORM=offscreen, null sink for audio).
  - Not for TSG_OFFSCREEN/ZB/RayTracer (no QOpenGLWidget); `/doom/textures` refuses other viewers.
    Opening another viewer while textured leaves the level hidden: `/doom/textures false` first.
  - Sprites are still lit by Geant4 (no distance fading), so far monsters look brighter than walls.
  - Fireballs: TexturePass draws them (`DrawFireballs`, via the shared `Billboard`) as BAL1/BAL2/BAL7
    (imp/cacodemon/baron, `kMissileSprites`; doom2gdml.py exports them), full bright: A/B every 4
    tics in flight, then C/D/E 6 tics each from a `Burst` recorded where it hit (pruned after 18
    tics). Feet 4 units below the missile's centre (Doom's mobj z is its bottom). MissileVis hides its
    balls while textured, Think skips the missile rebuild then, and the 35 Hz timer repaints while
    fireballs fly or burst. Checked offscreen against Doom E1M1's T16_imp.
- Doors, lifts, switches (`doom::World` in doomworld.hh, loaded from `<map>_level.txt` in main;
  `gWorld` for WorkerSync):
  - A moving floor/ceiling translates the sector's slab placements `S<sec>_<k>_floor/_ceil` (placed
    at 0 in the GDML) by (h - h0) * 32 mm in y. Same MT rules as monsters: the master sets them,
    `World::Sync` copies them into each worker at BeginOfRunAction, then GeometryMoved(false)
    recloses the world's voxels serially.
  - Thinkers as p_doors.c/p_plats.c/p_floor.c (speeds per tic, door wait 150, plat wait 105,
    top = lowest neighbour ceiling - 4, DR doors reverse when used while moving, monsters never
    close them). "Crushed" = the `blocked` hook: the player (alive) or a live monster whose box
    touches the sector (`World::Touches`, 9 sample points over the flat triangles) and whose height
    no longer fits; doors reverse (not the close types), plats go back down. No crush damage.
  - `/doom/world dt` runs whole tics (accumulates the rest); DoomInput sends it every 28 ms while
    idle. After a tic that moved slabs: `UpdateGeometry`, `Ride` (the player's eye and monsters
    whose centre's sector moved go to floor + 41 / their centre; the camera is only `Aim`ed),
    voxels, then one `Redraw` (refresh when textured, as the slabs are hidden; rebuild otherwise
    or when monsters moved/sprites are stale). FloorSpecials per tic: secrets (9), damaging floors
    every 32 tics.
  - `/doom/use` (F, or the right button while captured): P_UseLines, 64 units along the view yaw;
    the first line with a special decides (front side only, except 124), a plain line with no
    opening says dsnoway. Keyed doors 26-28/32-34 need card or skull (bits 0-2 / 3-5); a monster
    stuck while chasing calls `MonsterUse` (special 1 only, not ML_SECRET, front side).
  - Walk-over: Move calls `Cross(old, new)` per step (either direction; teleports 39/97 from the
    front only) and `Touch` for keys (36-unit box). Monsters cross only 4/10/88. Teleports set a
    pending eye/back that Move uses instead of the step. Exits (11/51/52/124; 51/124 secret) start the intermission.
  - Unknown specials say "(line special N is not done yet)" once when used.
  - Tested headless on Freedoom E1M1: door open/close/reverse on the player, reset; SR lift down,
    ride up (eye follows); S1 floor lower then dsnoway on reuse; exit; blue door refused, key
    pickup, door opens; W1 tagged door. Textured door snapshots (offscreen Qt) show it rising.
- Keys and messages in the HUD: STKEYS0-5 at (239, 171/181/191) (skull over card), the last
  `Say()` message for 4 s at the top left.
- TexturePass draws from the World: `Surfaces()` is rebuilt when `Version()` changes (heights,
  switch textures, keys taken) or every tic with scrolling walls (special 48, +1 texel a tic),
  and keys (BKEY/YKEY/RKEY/BSKU/YSKU/RSKU, A/B every 10 tics, B bright) are textured billboards.
  Keys are only visible in the textured view.
- Levels (`LoadLevel`, `/doom/map NAME`, exits, `/doom/next`): /vis/disable; clear missiles, the
  Bestiary and the World (hooks off, TexturePass told via OnWorldChanged(nullptr)); DoomLevel::SetFile;
  `/run/reinitializeGeometry true` (old stores deleted); `G4RunManager::InitializeGeometry()` on the
  master (**not** `/run/initialize`: its BeamOn(0) has the task workers set up the new world from
  stale per-thread data: GeomNav0002 "Volume must be centered on the origin"); then the broadcast
  `/doom/worker/geometry` (WorkerGeometry messenger, one per thread; the master's instance only
  makes it broadcastable). On a worker it runs (in the next run's command stack, before the
  worker's own geometry init) FreeSlave + SlaveCopySubInstanceArray for the LV, PV, PVReplica and
  Region splitters, then `G4WorkerThread::UpdateGeometryAndPhysicsVectorFromMaster()`. Without the
  re-copy: segfaults in GetSensitiveDetector / SetSensitiveDetector (dynamic_cast) after 1-4 loads.
  The arrays grow in chunks of 512 per thread, and SlaveReCopySubInstanceArray memcpys the master's
  totalspace into the worker's smaller array. Then LoadWorld, ApplyLook, GeometryMoved, a new scene
  (/vis/drawVolume, userAction, trajectories, `<dir>/<map>_camera.mac`), textures again, the song.
  The player keeps health, armour, weapons and ammo; keys go. Stress tested: all 36 Doom and 36
  Freedoom maps loaded in one process, firing the scattergun in each.
  - Run-duration user vis actions (missiles, sprites) are registered with a token 1 mm extent at the
    origin: they outlive the level, and the first level's extent made a smaller level's scene
    extent too big, so its camera macro's dollyTo put the eye in the wrong place (E4M8 start off
    by 476 units).
  - A HUD folder without the WI* graphics (converted before they were added) gets a plain-text
    tally (`PaintIntermissionText`). A single-map folder made before then plays the tally sounds
    with nothing to draw, and its next map wraps to itself. Play from doom1/ or
    freedoom1/ for transitions.
  - `fLevelDir` is absolute (a relative `e1m1.gdml` gave an empty parent: directory_iterator threw).
  - vis.mac is read from the cwd, else next to the binary (/proc/self/exe); main sets the alias
    `{level}` (absolute path without extension) for `/control/execute {level}_camera.mac`.
- Next map (`NextMap`): Doom's order (G_DoCompleted: secret exit -> E?M9, E?M9 -> E1M4/E2M6/E3M7/
  E4M3, M8 -> next episode, MAP15 -> 31 -> 32, 31/32 -> 16), and if that GDML is not in the folder,
  the next available map after this one (sorted names, secret maps skipped), wrapping to the first.
- Intermission (`StartIntermission`/`IntermissionTic`/`Accelerate`, wi_stuff.c single player):
  stages 1-10 (odd: 1 s pauses; counts +2 a tic, time/par +3; dspistol every 4 tics, dsbarexp at
  each end), 11 the episode map for 4 s (E maps), 12 10 tics then LoadLevel. Fire/use: finish the
  count, then go on; a held trigger does not (burst test like the spread's). Kills exclude barrels.
  Music D_INTER (D_DM2INT for Doom II). The 28 ms world timer also runs while it is active. Drawn by
  HudOverlay::PaintIntermission on a letterboxed 320x200 (1.2 aspect): WIMAP<e-1> (E1-E3, else
  INTERPIC), WILV<e-1><m-1> / CWILV<nn> centred at y 2, WIF below (y + 5h/4), WIOSTK/WIOSTI/WISCRT2
  at (50, 50 + k*lh), lh = 3/2 WINUM height, percents ending at x 270, WITIME (16,168) and time at 144,
  WIPAR (176,168) and par at 304 (WI_drawTime: ":30" style, as Doom); then splats (WISPLAT) on the
  lnodes of the done maps and WIURH0/1 (first that fits on screen) blinking (count & 31) < 20 on
  the next, under WIENTER + its name. No animated backgrounds (WIA*). Checked offscreen against
  the original E1M1 -> E1M2.
- Barrels: their corpses' deaths are collected after each run (`TakeDeaths`); `Blast` does
  P_RadiusAttack from 21 units above the floor: 128 - (max(|dx|,|dy|) - radius), sight-checked with
  Clearance, to monsters (chains to barrels) and the player ("The barrel hits you for 55" at 68 units).
- Headless tests: `/vis/open TSG_OFFSCREEN` plus `/doom/where` (also prints the Doom map position,
  angle and sector). To put the eye at a Doom position: the camera macro's dollyTo (1000 mm to
  the target; doomviewer changes the dolly, see the near plane above), viewpointVector = -look,
  targetPoint = eye + 1000 mm * look. Each move re-renders, so a
  3000-move macro takes several minutes.

## Validation
- `/geometry/test/run` on E1M1 finds only one ~3 mm wall contact and two monsters
  overlapping a step edge.

## Ideas not done yet
- A geantino "fist"; splash damage for the carbon-ion cannon.
- Sprites for the decorations (lamps, columns), which are still volumes.
- Draw the monster sprites in TexturePass too (textured quads with Doom's light fading): cheaper
  than the per-pixel quads, and they would match the walls' lighting.
- Crushers, sector light effects, the remaining line specials, monsters using teleporters,
  telefragging; the chainsaw, invulnerability/berserk/radiation suit and other powerups.
- Doom-accurate monster logic via doomgeneric (attacks, infighting) instead of the simple chase.
- Monster infighting (bullets and fireballs hitting monsters).
- The intermission's animated backgrounds (WIA*), Doom II's text screens between episodes, the
  episode ends (E1M8's "victory" text, the bunny).
- Remaining tessellated-solid defects on a few slabs (the converter still lists them on stderr).
- `--geantino` switch for doomviewer so it starts without datasets.
