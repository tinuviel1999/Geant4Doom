#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
render_g4.py - load a doom2gdml GDML file into Geant4 (via geant4_pybind) and run vis macros.

    GEANT4_DATA_DIR=/some/empty/dir python3 render_g4.py e1m1.gdml macro.mac [more.mac ...]

Only geantinos + transportation are defined, so no physics datasets are needed:
enough for geometry checks, the TSG/OpenGL viewers and the RayTracer.
"""
import os
import sys
from geant4_pybind import *


class Detector(G4VUserDetectorConstruction):
    def __init__(self, path, hide=()):
        super().__init__()
        self.path = path
        self.hide = hide
        self.parser = G4GDMLParser()

    def Construct(self):
        self.parser.Read(self.path, False)
        world = self.parser.GetWorldVolume()
        world.GetLogicalVolume().SetVisAttributes(G4VisAttributes.GetInvisible())
        n = 0
        self.keep = []  # keep vis attributes alive on the Python side
        for lv, auxlist in self.parser.GetAuxMap().items():
            for aux in auxlist:
                if aux.type == "Color":
                    r, g, b, a = (float(v) for v in aux.value.split())
                    va = G4VisAttributes(G4Colour(r, g, b, a))
                    va.SetForceSolid(True)
                    if any(h in lv.GetName() for h in self.hide):
                        va.SetVisibility(False)
                    lv.SetVisAttributes(va)
                    self.keep.append(va)
                    n += 1
        print(f"render_g4: coloured {n} volumes")
        return world


class Geantinos(G4VPhysicsConstructor):
    def __init__(self):
        super().__init__("geantinos")

    def ConstructParticle(self):
        G4Geantino.GeantinoDefinition()
        G4ChargedGeantino.ChargedGeantinoDefinition()

    def ConstructProcess(self):
        pass  # the modular list adds transportation by itself


class GeantinoPhysics(G4VModularPhysicsList):
    def __init__(self):
        super().__init__()
        self.geantinos = Geantinos()  # keep the Python object alive
        self.RegisterPhysics(self.geantinos)


class NoGun(G4VUserPrimaryGeneratorAction):
    """Placeholder: the RayTracer swaps in its own generator while tracing."""
    def GeneratePrimaries(self, event):
        pass


class Actions(G4VUserActionInitialization):
    def Build(self):
        self.SetUserAction(NoGun())


def main():
    args = sys.argv[1:]
    hide = []
    while args and args[0].startswith("--hide="):
        hide.append(args.pop(0).split("=", 1)[1])  # e.g. --hide=_ceil for a roofless overview
    gdml, macros = args[0], args[1:]
    rm = G4RunManagerFactory.CreateRunManager(G4RunManagerType.MTOnly)  # the RayTracer in an MT build needs an MT run manager
    rm.SetNumberOfThreads(int(os.environ.get("G4THREADS", "4")))
    det = Detector(gdml, hide)
    phys = GeantinoPhysics()
    rm.SetUserInitialization(det)
    rm.SetUserInitialization(phys)
    actions = Actions()
    rm.SetUserInitialization(actions)
    vis = G4VisExecutive("warnings")
    vis.Initialize()
    ui = G4UImanager.GetUIpointer()
    ui.ApplyCommand("/run/initialize")
    for mac in macros:
        ui.ApplyCommand(f"/control/execute {mac}")


if __name__ == "__main__":
    main()
