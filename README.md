<div align="center">

# SoulCaliburIII_Recomp

**A native Linux and Windows port of *Soulcalibur III* (NTSC-U, `SLUS-21216`), made by statically recompiling the game's PlayStation 2 code to C++ on BozoEngine.**<br>
The game's own MIPS R5900 code runs as a PC program. Its 2D, meshes, textures, effects, sound and movies go through
native PC services. There is no GS, VIF, VU, DMAC, IOP or SPU2 model in the build, and no interpreter.

![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-1b8cff)
![Compiler](https://img.shields.io/badge/compiler-GCC-1b8cff)
![Based on](https://img.shields.io/badge/based%20on-BozoEngine-17a35b)
![Status](https://img.shields.io/badge/status-in%20testing-e8a33c)

<img src="docs/screenshots/title_screen.jpg" width="70%" alt="Soulcalibur III's title screen">

<sub>Captured from the earlier PS2Recomp build of the port.</sub>

</div>

> [!IMPORTANT]
> No game files are included: no ELF, IRX modules, AFS archives, or code generated from them.
> You need **your own copy** of the North American release, PS2 ID **`SLUS-21216`**. The port is built against that disc's executable:
>
> | File | SHA-1 |
> |---|---|
> | `SLUS_212.16` | `9bbb67a70137052931bc245855eedeee6bbe5b49` |

## Status

**In testing on BozoEngine, not released yet.** Every check below was made with scripted input; no human play session
has been run on the BozoEngine build yet. The earlier PS2Recomp build (main up to `da8a761`) was played by a human
and through every mode; its history is in [RECOMP_NOTES.md](RECOMP_NOTES.md#the-ps2recomp-build).

What has been run, on Linux unless said:

| | Result |
|---|---|
| Boot: logos, opening movie, title, attract | 7200 frames with movies in a window: no fault, exit 0 |
| The regression route: title, Mode Select, Single Player, Practice, Free Training, 60 s of attacks, pause | 11700 frames headless and in a window: no fault, 0 mesh parts lost |
| Every mode entered from Mode Select: Tales of Souls, Chronicles of the Sword, World Competition, Soul Arena, Museum, Character Creation, Versus, Shop, Options | 7000 frames each: no fault, 0 mesh parts lost, 0 2D pages dropped |
| Full-screen movies (CRI Sofdec) | The boot's MOVIE.AFS file 4: 1051 pictures in 35.10 s (29.95/s), then file 0 at 29.98/s; menu preview loops play |
| Music, voices and effects (CRI ADX through the game's own sound modules) | Route against the PS2Recomp build, per 10 s window: level median 0.64 dB, max 4.21 dB apart; spectrum cosine min 0.885, median 0.984 |
| Device registers touched by the game's code (`[sc3:acknowledged]`) | 0 on the route, the boot and the nine modes |
| Frame rate in a window, development PC (Ryzen 7 7800X3D) | 59.5 to 60.0 game frames a second, held by the frame pacer |
| Windows 11 (VM, 4 cores, Mesa llvmpipe, no GPU) | Built natively with the kit and cross-built from Linux; route, boot and stills agree with Linux (headless game state equal on every frame); 53-56 frames/s in menus, 28-51 in fights on the software renderer |
| Memory cards | BozoEngine's libmc on host card folders (`mc0/`, `mc1/`); a save and reload has not been checked on this build |

How each layer of the game is answered:

| Layer | |
|---|:---:|
| The whole EE executable recompiled by `bozo-recomp` (16,656 functions, 578 jump tables, 0 untranslated) | ✅ |
| Libraries (kernel, libpad, libmc, libcdvd, libgraph, SIF) answered natively by BozoEngine | ✅ |
| Sound modules CRI_ADXI, NUSOUND, SDRDRV translated from your disc at build time; libsd and the mixer native | ✅ |
| The frame's kick, ordering table and packet API in C++: no DMA chain, GIF or VIF packet is built for a device or read | ✅ |
| 2D (menus, text, the fight HUD) built in C++ from the game's variables, textured from a 2D pack of your disc | ✅ |
| Meshes and textures from packs converted from your disc, drawn at the game's own hooks; skinning in C++ | ✅ |
| Glow, shadow volumes, the sun flare's depth probe, the display copy, mip levels: native passes | ✅ |
| Movies decoded with FFmpeg from your disc image, pictures on the renderer, soundtracks in the mixer | ✅ |
| Frame timing from the EE clock (no timer registers); frames paced by the PC's clock | ✅ |
| Windows and Linux builds, the build kit and the launcher | ✅ |
| A human play session, a real sound device, a real GPU on Windows | ⏳ |
| Widescreen | ⏳ |

✅ works · ⏳ not checked or not done

### Known limits

- **Pictures against the PS2Recomp build.** Eleven fight stills at fixed frames compare at 20.42 to 27.71 dB PSNR
  against main's, where main against itself (animation timing) gives 22.63 to 35.54 dB. The remaining contributors
  found: animation phase, the water's scroll phase, sub-pixel edges, texture aliasing.
- **Frame-read sprites** (2D drawn from the frame so far or the glow's half picture) are drawn natively but not
  checked against PCSX2: none were found in three GS dumps.
- **Mesh routines.** Two routines of the game's vertex programs (B:0xD8, A:0x1268) are written from their microcode
  but not yet run on a model part. Ten entries (A:0xDA8, A:0x2098, A:0x2140, A:0x2330, B:0xDD0, B:0x10F0, B:0x21E0,
  B:0x2290, B:0x24F8, B:0x2740) are not written: none was reached in 302 million program calls recorded over every
  mode. A part that needs one is counted in the log (`[sc3:mesh] ... routine`) and not drawn.
- **Sound device.** Every test run used SDL's dummy audio driver; sound was measured from the mixer's output (`--wav`).
  Playback on a real device has not been checked.
- **A fault at 0x117808** (`unhandled device read at 0xfffffffc`, frame 2680 of the route) was seen in 4 of 15 runs
  early in the migration and in none of the more than 100 runs logged since. Not explained; a run that meets it prints
  `[sc3:frame] 0x1177E0 file ... has no entries`.
- **Timing on the EE clock.** The pause menu can show a fading "HIGH" label that main has already hidden (the game
  pauses one step earlier); the silence between the boot's two movie files is 4.6 s here against 4.7-5.0 s on main.
- **`mwPlyPause`** does not freeze a movie's picture.
- **Windows**: not run on a real GPU or with a gamepad; on llvmpipe the fight is below 60 frames a second.

## For players

You build the game yourself, on your PC, from **your own disc**: nothing of the game is in the download. No
release has been published yet.

**Windows.** A release zip (`scripts/make_release.py`) holds `SC3 Launcher.exe`, our code with BozoEngine's, and the
build kit (compiler, CMake, Ninja, a static FFmpeg, Python). Unzip it outside `Program Files` with about 15 GB free,
start the launcher, pick your USA disc image (`.iso`, or `.bin` with its `.cue`), click **Prepare the game** (about
20 minutes on 4 cores, most of it the packs), then **Play**. Requires Windows 10 1903 or newer, a 64-bit CPU and
OpenGL 3.3.

**Linux.** With GCC, CMake 3.21+, Ninja, Python 3, pkg-config and FFmpeg's development libraries (`libavcodec`,
`libavformat`, `libavutil`, `libswresample`, `libswscale`):

```bash
git clone --recurse-submodules https://github.com/TheBozos/SoulCaliburIII_Recomp.git
cd SoulCaliburIII_Recomp
python3 scripts/sc3_build.py --disc "/path/to/Soulcalibur III (USA).iso" --out ~/sc3-build
~/sc3-build/SC3/play_sc3.sh
```

`sc3_build.py` checks the disc (`SLUS_212.16`'s SHA-1), extracts what the build reads (the ELF, `ROOT.OLK`, three sound
modules), builds BozoEngine's recompiler, generates the code, converts the packs, compiles `sc3` and stages a game
folder: `sc3`, `SLUS_212.16`, `sc3_symbols.txt`, `packs/`, `disc.iso` (a hard link to your image when it can) and
`play_sc3.sh` (`Play SC3.cmd` on Windows). Each step is skipped when its inputs did not change; `--check` only checks a
disc. On 8 threads (Ryzen 7 7800X3D) a build from nothing took 649 s, 550 of them the packs; the work folder takes
8.9 GB.

| Key | |
|---|---|
| F2 | Settings: controls, video |
| F3 | Frame rate overlay |
| F11, Alt+Enter | Full screen |

A controller is used when one is connected, otherwise the keyboard; both are rebound in the settings window and kept
in `input.ini`.

### Saves, settings and logs

| | Linux | Windows |
|---|---|---|
| User folder | `$XDG_DATA_HOME/soulcalibur3_recomp` (`~/.local/share/soulcalibur3_recomp`) | `%APPDATA%\SoulCaliburIII_Recomp` |

It holds the memory cards (`mc0/`, `mc1/`), `input.ini`, `video.ini` and the logs. It is the same folder, with the same
card layout, as the PS2Recomp build's.

## Build (developers)

```bash
git submodule update --init BozoEngine
cmake -S . -B build/bozo -G Ninja -DSC3_ELF=<SLUS_212.16> -DSC3_ISO=<disc image>
cmake --build build/bozo                      # sc3, sc3-headless, sc3-check, sc3-irx-check
cmake --build build/bozo --target sc3_packs   # build/bozo/packs: textures, meshes, 2D
```

`scripts/bozo_build.sh` runs the same into `build/bozo` from `game/SLUS_212.16` (`scripts/extract_iso.sh <image>` makes
`game/`), and `scripts/bozo_headless.sh <name> [args]` runs `sc3-headless` with a fresh user folder. The sound modules
are read from `IRX/` beside the ELF (`SC3_IRX_DIR`), the packs' inputs from the ELF's folder (`SC3_DISC_DIR`).

| CMake option | |
|---|---|
| `BOZO_ENGINE_DIR` | Another BozoEngine tree (default: the submodule) |
| `SC3_SYMBOL_MAP` | The library names of the stripped ELF (default `recomp/sc3_symbols.txt`) |
| `SC3_EXTERNALS` | An externals list to generate with (`bozo-calllog-tool externals`: what `sc3-check --pinned` needs) |
| `SC3_WINDOW=OFF` | No `sc3` and no SDL3 |
| `SC3_NATIVE_SOUND=OFF` | No sound modules |
| `SC3_GEN_FILES`, `SC3_GEN_OPT` | How many files the generated code is split into (32), its optimisation (`-O1`) |
| `SC3_HOST_TOOLS`, `SC3_GUI_SUBSYSTEM` | Windows cross build: the native `bozo-externals` and `bozo-recomp`; `sc3.exe` without a console |

Windows from Linux: a mingw-w64 GCC targeting the UCRT with `cmake/mingw-cross.cmake` (the command is at its top),
`scripts/build_win_ffmpeg.sh` for the static FFmpeg, `scripts/make_win_package.sh` for a portable folder that never
leaves the machine that built it. Clang targets (zig c++, llvm-mingw) build a binary that crashes: on Win64 their
`__builtin_longjmp` restores rbp from the frame address instead of the frame pointer, which breaks BozoEngine's stops.

The release: `scripts/make_build_kit.py` (Windows, MSYS2 and the network) assembles the kit at the versions
`scripts/build_kit_packages.json` pins; `launcher/` is `SC3 Launcher.exe`; `scripts/make_release.py` makes the zip
from a commit and a `CHANGELOG.md` section; `.github/workflows/release.yml` runs it on a version tag and makes a draft
release. `tests/test_release_tools.py` checks the build driver and release tools without game data (CI, Linux and
Windows).

### Running

```
sc3 [--iso image] [--elf SLUS_212.16] [--card-dir dir] [--width w --height h] [--fullscreen] [--no-vsync]
    [--render-scale s] [--capture-dir dir] [--capture-frames n,n,...] [--quit-after n] [--pad-script [n=]file]...
    [--print] [--no-sound] [--audio-latency ms] [--wav file]
sc3-headless --frames n [--iso image] [--pad-script file] [--out run.ref] [--dump-ram dir] [--profile-io] [--wav file] ...
```

The disc image is `--iso`, else `SC3_ISO`, else the build's, else the one `.iso` beside the ELF. The packs are the
variables below, else `packs/` beside the program; the symbol map `SC3_SYMBOL_MAP`, else `sc3_symbols.txt` beside it.
`sc3-headless` runs on the game's own clock and is deterministic: two runs give the same `--out` record.

<details>
<summary><b>Environment variables</b></summary>

| Variable | Effect |
|---|---|
| `SC3_TEXTURE_PACK`, `SC3_MESH_PACK`, `SC3_2D_PACK` | The packs. Without the mesh or 2D pack that layer is not drawn; the game still runs |
| `SC3_SYMBOL_MAP`, `SC3_ISO` | The symbol map, the disc image |
| `SC3_NO_MOVIES=1` | Skip the one-shot movies (menu preview loops still play) |
| `SC3_MIPMAPS=0` | Textures at level 0 only (by default stage materials use the game's own mip levels) |
| `SC3_NATIVE_2D=0`, `SC3_NATIVE_MESHES=0` | That layer's hooks not installed: nothing of it is drawn (diagnostic) |
| `SC3_HOLD_FRAMES=n,...` | The game stands 1 s at the end of those frames, so a capture by frame (`--capture-frames`) has that frame's picture |
| `SC3_HIDE_EE=<addr>,...`, `SC3_HIDE_TBP=<tbp>,...` | Leave out the draws of those builders, or of textures at those pages (diagnostic) |
| `SC3_TRACE_CD=1`, `SC3_MESH_DUMP=<frame>`, `SC3_MESH_TRACE_LISTS=1` | Disc reads, one frame's mesh draws, model list relocations, in the log |
| `SC3_TRACE_SCENE=1`, `SC3_TRACE_CLOCKS=<addr>,...` | Each scene the game changes to and each start of those counters, with the frame, in the log (the PCSX2 comparison's anchors, `reference/pcsx2`) |
| `PS2X_USER_DIR=<dir>` | Another user folder |
| `PS2X_LOG_FILE=<file>` | The log there (`0`: no log file) |
| `PS2X_NO_DIALOGS=1` | Errors in the log only, no message box |
| `PS2X_WINDOW_SIZE=WxH`, `PS2X_WINDOW_MODE`, `PS2X_RENDER_SCALE`, `PS2X_FRAME_RATE` | Override `video.ini` |
| `PS2X_SHOW_FPS=1`, `PS2X_FRAME_STATS=1` | Start with the overlay on; frame pacing statistics in the log |
| `PS2X_INPUT_CONFIG`, `PS2X_VIDEO_CONFIG`, `PS2X_GAMEPAD_DB` | Other `input.ini`, `video.ini`, controller database |
| `PS2X_RUMBLE=0` | No vibration |

</details>

Every run prints `[sc3:acknowledged] N hits` at its end: the game's accesses to device registers, which have no device
behind them. It is 0 on every route checked; a non-zero count names a site still to replace (`--profile-io` lists them).

## How it works

```
your disc ──► sc3_build.py / CMake
                │
  SLUS_212.16 ──► bozo-externals (+ recomp/sc3_symbols.txt, sc3_stubs.txt, the port's overrides)
                └► bozo-recomp (+ recomp/sc3_patches.txt, sc3_entries.txt) ──► C++ ──┐
  IRX/CRI_ADXI, NUSOUND, SDRDRV ──► BozoEngine's native_irx ──► C++ ─────────────────┤
  ROOT.OLK ──► scripts/sc3_texture_pack.py, sc3_mesh_pack.py, sc3_2d_pack.py ──► packs/ (read at run time)
                                                                                     │
  bozo/port/*.cpp (SC3's overrides) + BozoEngine (kernel, libraries, renderer, mixer) ┴──► sc3, sc3-headless
```

- The ELF has no symbols. `recomp/sc3_symbols.txt` names its library functions from our own matcher and a hand review
  (`sc3_symbols_reviewed.txt`); BozoEngine answers those natively.
- `bozo/port/` holds SC3's own code: the frame kick and ordering table (`sc3_frame.cpp`, `sc3_ot.cpp`,
  `sc3_packet.cpp`), the clock (`sc3_clock.cpp`), the 2D builders (`sc3_2d.cpp`), meshes, textures and materials
  (`sc3_meshes.cpp`, `sc3_textures.cpp`, `sc3_texture_pack.cpp`, `sc3_material.cpp`), passes (`sc3_offscreen.cpp`),
  movies (`sc3_movies.cpp`), loads (`sc3_loads.cpp`), VU0 (`sc3_vu0.cpp`) and sound wiring (`bozo/sound/`).
- Textures and meshes exist on the disc only as ready-made GS and VIF data: the pack scripts convert them offline from
  your disc into the build tree, and the game's own keys find them at run time. Nothing converted is committed.
- [RECOMP_NOTES.md](RECOMP_NOTES.md) describes each piece, how it was checked, and the history.
  [`reference/`](reference/README.md) holds the hashes and scripts of the PS2Recomp build's reference runs.

## Disc facts

| | |
|---|---|
| Boot ELF | `SLUS_212.16`, entry `0x100008`, stripped |
| Video mode | NTSC (`SYSTEM.CNF` `VER = 1.01`) |
| IOP modules | `IRX/*.IRX`: CRI_ADXI, LIBSD, MCMAN, MCSERV, NUSOUND, PADMAN, SDRDRV, SIO2MAN |
| Data | `ROOT.OLK`, `BGM.AFS`, `SE.AFS`, `VOICE.AFS`, `MOVIE.AFS` (Sofdec) |

## Repository layout

| Path | Contents |
|---|---|
| `BozoEngine/` | The engine (git submodule, at a merged engine commit) |
| `CMakeLists.txt`, `cmake/` | The build (`bozo.cmake`), the sound modules (`sc3_audio.cmake`), the Windows cross build |
| `bozo/` | The programs (`sc3_window.cpp`, `sc3_headless.cpp`, `sc3_check.cpp`), the port (`port/`), sound (`sound/`) |
| `recomp/` | The recompiler's inputs: symbol map, stubs, entries, patches |
| `scripts/` | The build driver, packs, kit and release tools; disassembly and IRX helpers |
| `launcher/` | SC3 Launcher.exe (Windows) |
| `reference/` | The PS2Recomp build's reference records, route and A/B tools |
| `tests/` | The build driver's and release tools' tests |
| `docs/screenshots/` | Pictures of the running port |
| `game/`, `build/` | Created by the scripts, ignored by git |

Everything derived from the game (disc files, generated code, packs, builds, logs, captures) stays outside this
repository. Never commit or redistribute it.

## Credits

- [BozoEngine](https://github.com/TheBozos/BozoEngine) (MIT), the recompiler and runtime.
- *Twisted Metal: Black*'s port ([TheBozos/TMB_Recomp](https://github.com/TheBozos/TMB_Recomp)), whose build kit,
  launcher and release pattern this port follows.
- The earlier build used [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors, through
  [TheBozos/PS2Recomp](https://github.com/TheBozos/PS2Recomp).
- *Soulcalibur III* © Bandai Namco Entertainment, developed by Project Soul. This is an unofficial fan project, not affiliated with or endorsed by them.
