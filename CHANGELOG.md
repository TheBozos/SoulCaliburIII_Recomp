# Changelog

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). A release's section is the release notes' "What's new" and the launcher's.

## [Unreleased]

### Changed
- **BozoEngine is now the only recompiler and runtime.** The PS2Recomp submodule, its build, the runtime overrides,
  the VU1 tests and the scripts that built, ran or mapped it are removed; that build stays in main's history
  (`da8a761`). The game has no GS, VIF, VU, DMAC, IOP or SPU2 model and no interpreter.
- The user folder is the same (`SoulCaliburIII_Recomp`, `soulcalibur3_recomp` on Linux), with the same `mc0`/`mc1`
  card folders.

### Added
- The game on BozoEngine: the whole executable recompiled (16,656 functions), every library answered natively, the
  sound modules (CRI_ADXI, NUSOUND, SDRDRV) translated from the disc at build time, libsd and the mixer native.
- The frame in C++: the kick, the ordering table, the packet API and the frame clock. The game's code touches no device
  register on any checked route (`[sc3:acknowledged]` 0).
- Menus, 2D and fights drawn natively from the game's own variables: meshes and textures from packs converted from
  your disc, the stage materials' own mip levels, glow, shadow volumes, the sun flare's depth probe and the display
  copy. All nine modes run with no mesh part lost.
- Movies decoded with FFmpeg from your disc image, on the game's own clock: full-screen movies at 29.95-29.98 pictures
  a second with their soundtracks, and the menu preview loops.
- Windows: `SC3 Launcher.exe` and the build kit. The launcher checks your USA disc image (SLUS-21216), builds the game
  from it on your PC with the kit (no MSYS2, Python or network needed) and starts it.
- `scripts/sc3_build.py`: the same build from the command line, on Windows or Linux.
- `scripts/make_release.py`, `scripts/apply_update.py` and the CI and release workflows.

### Known limits
- Not yet played by a human on this build; sound not checked on a real device; Windows not run on a real GPU or with
  a gamepad.
- Fight stills compare at 20.4-27.7 dB with the PS2Recomp build's (its own run-to-run floor 22.6-35.5 dB).
- Ten rarely used vertex program entries are not written (none was reached in any recorded run); a part needing one
  is not drawn.
- A fault at 0x117808 was seen in 4 of 15 runs early in the migration and not since.
