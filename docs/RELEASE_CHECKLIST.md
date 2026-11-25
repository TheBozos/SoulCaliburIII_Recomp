# Release checklist

Use a scratch user folder for every automated run. Keep game data and generated code out of Git and release zips.
A successful build is not a publication: this checklist records what was actually verified, and the owner publishes the
draft by hand.

v0.1.1 was published on 2026-11-25 (`0bc2a91`, engine `eca63e5`; run 45547629075), the first with a Linux download.
Its draft assets were checked locally before publication:
- the zip, the sources zip and `SoulCaliburIII_Recomp-0.1.1-linux.tar.gz` pass their SHA-256 checks, and both player
  downloads pass `make_release.py --verify-zip` (the archive: one top folder, no ELF but `sc3-launcher`, no game file);
- `sc3-launcher` from the downloaded archive, extracted under a path with spaces and an accent, prints version 0.1.1 and
  the release commit and passes `--self-check` (sc3_build refuses a file of zeros); it needs only libc and libm;
- an archive made locally from the branch before its review fixes (`dd2ee11`), extracted under `Jérôme Games/`, built
  the game from the USA disc with the system's toolchain and the archive's kit alone (659 s; `game/sc3`, `play_sc3.sh`);
- the Linux launcher's GUI selftest on the USA disc passed in a hand-assembled release folder: the build it drives
  (10:36), Settings, Maintenance, French, Play, the game closed with exit code 0 and no crash report;
- the release notes have a Linux section with the archive's hash and the Debian / Ubuntu install command.

v0.1.0 was tagged on 2026-11-25 (`548cdc2`, engine `eca63e5`). The release workflow built its draft (run 45537147547).
The draft assets were checked locally:
- `SoulCaliburIII_Recomp-0.1.0.zip` and `SoulCaliburIII_Recomp-0.1.0-sources.zip` pass their SHA-256 checks;
- `make_release.py --verify-zip` passes on the player zip: no game file, the expected files present. The zip holds
  8,478 files under one `SoulCaliburIII_Recomp/` folder, with `SC3 Launcher.exe`, the build kit, the licences and
  `CHANGELOG.md`; `source/release.ini` is stamped with version 0.1.0, the release commit and the engine commit;
- the sources zip holds the 42 corresponding-source files;
- the release notes are CHANGELOG.md's 0.1.0 section, with its known limits.

## Source and CI

- [x] The only runtime submodule is `BozoEngine`, pinned to a reviewed commit on its `master`.
- [x] No PS2Recomp gitlink, CMake target or runner script remains.
- [x] Release-tool CI passes on Windows and Linux; the tagged release build passes.
- [x] The USA route (`reference/scripts/route_frames.txt`, 9,000 frames) is identical to the previous `master` after
  the European disc changes, and the European disc's save states round-trip with identical RAM.
- [ ] A save state loaded in a new process matches the run that saved it (USA disc; listed in the known limits).

## Timing and rendering

- [x] PAL50 and PAL60 run at 50 and 60 frames a second; NTSC at 60.
- [x] Movies on both discs: 640x448 at 29.97 pictures a second, with their soundtracks.
- [x] #15: hair strands drawn; a Tales of Souls fight costs the game thread about 5.2 ms a frame instead of 10.4
  (Ryzen 7 7800X3D); the Windows pacer sleeps on a waitable timer.
- [ ] A fight holds 60 frames a second on the #15 reporter's slower PC.
- [ ] Human watch/listen acceptance of a full intro and a story movie.
- [ ] F2 Controls/Video, save-state keys and fullscreen switching on the final package.

## Packages

- [x] `SoulCaliburIII_Recomp-<version>.zip` contains one `SoulCaliburIII_Recomp/` folder.
- [x] The player zip passes `make_release.py --verify-zip` and its SHA-256 check. It contains no game file.
- [x] The corresponding-source zip and the licences accompany the release.
- [x] README states Windows 10 1903 or newer, a 64-bit CPU, OpenGL 3.3 and about 15 GB free.
- [ ] Extract the player zip, then build from the USA disc and from the European disc with only its bundled kit,
  offline.
- [ ] Test a path with spaces/non-ASCII, cancellation/resume, Play and restart.
- [x] `SoulCaliburIII_Recomp-<version>-linux.tar.gz` contains one `SoulCaliburIII_Recomp/` folder with `sc3-launcher`
  (0755), passes `make_release.py --verify-zip` and its SHA-256 check, and contains no game file.
- [x] Linux: extract the archive, then build from the USA disc with the system's toolchain and the archive's kit; the
  launcher's selftest drives the build, Play and close.
- [ ] Linux: the European disc built from the archive; the launcher on a distribution other than Arch (Debian, Ubuntu,
  Fedora); the Update button from 0.1.1 to the next Linux release.

## Clean-PC and player acceptance

- [ ] On a fresh standard-user Windows installation: SmartScreen/Mark of the Web, Defender, no MSYS2/Python/git
  installed.
- [ ] A gamepad and 150%/200% display scaling checked on real hardware.
- [ ] Existing memory cards read correctly; movies, menus and fights feel right to the owner.
- [ ] The European disc in Spanish and Italian, and its Chronicles of the Sword mode.
- [x] Remaining limitations are recorded in the release notes before publication.

The development PC can verify the build-kit flow in a VM and under Proton, but cannot prove the fresh-OS, SmartScreen or
real high-DPI checks by itself.
