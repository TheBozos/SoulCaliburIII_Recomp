# Changelog

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). A release's section is the release notes' "What's new" and the launcher's.

## [Unreleased]

### Added
- The Korean disc, SCKA-20059 (v1.00): built from it as from the USA and European discs, by `sc3_build.py`, the
  launcher and CMake (`SC3_REGION=KOR` from the ELF's SHA-1). It runs at 60 frames a second with its Korean menus and
  text; its saves are its own (`BKSCKA-20059SC3`).
- `recomp/kor/`: the Korean executable's address map, symbol map and patches; `bozo/port/sc3_addresses.txt` has a column
  for it, and a row's `reviewed=<column>` names the releases read by hand.

### Fixed
- Kunpaetku Shrine Ruin: no more dark vertical lines every 64 pixels over the water when the game is drawn larger
  than the PS2's picture (#28).
- Custom Character Creation: dialogs and lists no longer show the menus under them through (the memory card dialog
  over the CHARACTERS list, the "All" list over the edit list), and the character stands in the arch instead of the
  middle of the screen (#26). The character select's weapon preview sits above its panel again (#27).

## [0.1.1] - 2026-11-25

### Added
- A Linux download, `SoulCaliburIII_Recomp-<version>-linux.tar.gz`: the launcher for Linux (`sc3-launcher`), the code and
  the SDL3 and Dear ImGui sources. It builds the game with your system's compiler, Python and FFmpeg; **Prepare the
  game** names the packages to install for your distribution when one is missing, and the Update button installs the
  next Linux release.

## [0.1.0] - 2026-11-25

### Changed
- **BozoEngine is now the only recompiler and runtime.** The PS2Recomp submodule, its build, the runtime overrides,
  the VU1 tests and the scripts that built, ran or mapped it are removed; that build stays in main's history
  (`da8a761`). The game has no GS, VIF, VU, DMAC, IOP or SPU2 model and no interpreter.
- The user folder is the same (`SoulCaliburIII_Recomp`, `soulcalibur3_recomp` on Linux), with the same `mc0`/`mc1`
  card folders.

### Added
- Save states: F5 saves, F7 loads, F6 / Shift+F6 pick one of ten slots, also in the settings window (F2); kept in the
  user folder's `states/`. `--save-state` and `--load-state` for scripted runs. A save waits while a movie plays.
- The European disc, SCES-53312 (v2.00): built from it as from the USA disc, by `sc3_build.py`, the launcher and CMake
  (`SC3_REGION` from the ELF's SHA-1). Its PAL50 and PAL60 modes run at 50 and 60 frames a second, and its five
  languages are chosen in the launcher's settings or with `SC3_LANGUAGE`.
- `scripts/sc3_region_map.py`: where an address of the USA executable lives in the European one, and the PAL recomp
  inputs made with it (`recomp/pal/`).
- Texture upscaling, off by default: the game's textures drawn from 2x or 4x copies (edge-directed or smooth filter),
  in the launcher's settings, the Video tab (F2) and `video.ini`.
- The game on BozoEngine: the whole executable recompiled (16,656 functions), every library answered natively, the
  sound modules (CRI_ADXI, NUSOUND, SDRDRV) translated from the disc at build time, libsd and the mixer native.
- The frame in C++: the kick, the ordering table, the packet API and the frame clock. The game's code touches no device
  register on any checked route (`[sc3:acknowledged]` 0).
- Menus, 2D and fights drawn natively from the game's own variables: meshes and textures from packs converted from
  your disc, the stage materials' own mip levels, glow, shadow volumes, the sun flare's depth probe and the display
  copy. All nine modes run with no mesh part lost.
- Movies decoded with FFmpeg from your disc image, on the game's own clock: full-screen movies at 29.95-29.98 pictures
  a second with their soundtracks, and the menu preview loops.
- Windows: `SC3 Launcher.exe` and the build kit. The launcher checks your disc image (USA or European), builds the game
  from it on your PC with the kit (no MSYS2, Python or network needed) and starts it.
- `scripts/sc3_build.py`: the same build from the command line, on Windows or Linux.
- `scripts/make_release.py`, `scripts/apply_update.py` and the CI and release workflows.

### Fixed
- Hair is drawn with its fine see-through strands instead of the flat, opaque shell under them, on every character
  (#15): the strand parts the game skins on VU0 were never drawn. The `LOST` mesh counter now also counts the parts
  left to the game's routine table, which it missed before.
- Fights cost the game's thread less: a Tales of Souls fight (Valentine Mansion) about 5.2 ms a frame instead of
  10.4 on a Ryzen 7 7800X3D (VU0 arithmetic inlined, each mesh part's header decoded once, its buffers kept), which
  should let a slower CPU hold 60 frames a second there (#15).
- On Windows the frame pacer sleeps on a waitable timer: under some Wine builds (Proton Experimental with fsync) its
  sleeps overslept by up to 13 ms and the menus ran at 55 frames a second (#15).
- Saving to the memory card no longer hangs after the icon is written, and neither does formatting a card (#6).
- The draw counters printed every 250 frames are off by default (`SC3_STATS=1` turns them on); writing them to a slow
  console could hold up a frame.

### Known limits
- Not yet played by a human on this build; sound not checked on a real device; Windows not run on a real GPU or with
  a gamepad.
- Fight stills compare at 20.4-27.7 dB with the PS2Recomp build's (its own run-to-run floor 22.6-35.5 dB).
- Ten rarely used vertex program entries are not written (none was reached in any recorded run); a part needing one
  is not drawn.
- A fault at 0x117808 was seen in 4 of 15 runs early in the migration and not since.
- The European disc was played on Linux only, in English, French and German; Spanish, Italian and the Chronicles of
  the Sword mode are not checked yet.
- A save state loaded in a new process can differ slightly from the run that saved it (USA disc).
