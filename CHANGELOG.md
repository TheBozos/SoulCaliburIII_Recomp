# Changelog

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
[Semantic Versioning](https://semver.org/). A release's section is the release notes' "What's new" and the launcher's.

## [Unreleased]

### Added
- The game on BozoEngine: the whole executable recompiled, every library answered natively, the sound modules
  (CRI_ADXI, NUSOUND, SDRDRV) translated from the disc at build time, the movies decoded with FFmpeg, menus, 2D and
  fights drawn natively from the game's own variables.
- Windows: SC3 Launcher.exe and the build kit. The launcher checks your USA disc image (SLUS-21216), builds the game
  from it on your PC with the kit (no MSYS2, Python or network needed) and starts it.
- `scripts/sc3_build.py`: the same build from the command line, on Windows or Linux.
