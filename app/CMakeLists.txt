# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.16)
project(doomviewer C CXX)

find_package(Geant4 REQUIRED gdml ui_all vis_all)
include(${Geant4_USE_FILE})

add_executable(doomviewer doomviewer.cc)
target_compile_features(doomviewer PRIVATE cxx_std_20)  # std::erase_if, defaulted ==
target_link_libraries(doomviewer ${Geant4_LIBRARIES})

# WASD keys in the Qt session (no moc needed: the key filter has no signals or slots)
find_package(Qt6 QUIET COMPONENTS Widgets OpenGL OpenGLWidgets)
find_package(OpenGL QUIET)
if(Qt6_FOUND)
  target_compile_definitions(doomviewer PRIVATE DOOM_QT)
  target_link_libraries(doomviewer Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets OpenGL::GL)

  # Mouse look without clicking on native Wayland: pointer lock + relative motion protocols.
  # GuiPrivate is needed for the window's wl_surface (QPlatformNativeInterface).
  set(QT_NO_PRIVATE_MODULE_WARNING ON)
  find_package(Qt6 QUIET COMPONENTS GuiPrivate)
  find_package(PkgConfig QUIET)
  find_program(WAYLAND_SCANNER wayland-scanner)
  if(TARGET Qt6::GuiPrivate AND PkgConfig_FOUND AND WAYLAND_SCANNER)
    pkg_check_modules(WAYLAND_CLIENT QUIET IMPORTED_TARGET wayland-client)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
  endif()
  if(WAYLAND_CLIENT_FOUND AND WAYLAND_PROTOCOLS_DIR)
    foreach(proto pointer-constraints relative-pointer)
      set(xml ${WAYLAND_PROTOCOLS_DIR}/unstable/${proto}/${proto}-unstable-v1.xml)
      add_custom_command(OUTPUT ${proto}-client.h ${proto}-protocol.c
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${proto}-client.h
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${proto}-protocol.c
        DEPENDS ${xml})
      target_sources(doomviewer PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${proto}-client.h
                                        ${CMAKE_CURRENT_BINARY_DIR}/${proto}-protocol.c)
    endforeach()
    target_include_directories(doomviewer PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(doomviewer PRIVATE DOOM_WAYLAND)
    target_link_libraries(doomviewer PkgConfig::WAYLAND_CLIENT Qt6::GuiPrivate)
  endif()
endif()

# Doom sound effects (WAVs extracted by doom2gdml.py into sounds/ next to the GDML), played
# through PulseAudio/PipeWire. Not Qt Multimedia: its audio threads' 1 MiB stacks are smaller
# than Geant4's static TLS, so they fail to start and the viewer hangs.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PULSE_SIMPLE QUIET IMPORTED_TARGET libpulse-simple)
endif()
if(PULSE_SIMPLE_FOUND)
  target_compile_definitions(doomviewer PRIVATE DOOM_SOUND)
  target_link_libraries(doomviewer PkgConfig::PULSE_SIMPLE)
  # The music: an emulated OPL3 FM chip (Nuked-OPL3, LGPL-2.1) playing the WAD's MIDI songs
  target_sources(doomviewer PRIVATE third_party/nuked-opl3/opl3.c)
  target_include_directories(doomviewer PRIVATE third_party/nuked-opl3)
endif()

# copy the vis macro next to the binary
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/vis.mac ${CMAKE_CURRENT_BINARY_DIR}/vis.mac COPYONLY)
