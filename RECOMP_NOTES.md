# Recompilation notes

How *Soulcalibur III* (SLUS-21216) was brought up on PS2Recomp, what the game overrides do and why, the runtime
fixes this game led to, where it stands, and the tools used to debug it. Build steps are in the [README](README.md).

## Bring-up

The ELF is stripped. `recomp/sc3.toml` started as `ps2_analyzer` output (12,851 functions, 636 SCE SDK symbols
matched from its embedded database), then grew by hand:

- **Function map.** A headless Ghidra run with the Emotion Engine extension exported `recomp/ghidra_map.csv`
  (8,901 functions, 910 code labels). The recompiler does not read it (`ghidra_output` is empty); it is kept as
  a reference map of the ELF.
- **Hint lists** (`recomp/*_hints.txt`), folded into `entry_points`:
  - code reached only through data: `scripts/find_code_pointers.py` takes data words that point into `.text`
    right after a function return and are not yet a function start (the `*ptr_hints.txt` and `new_hints.txt`
    lists);
  - every J/JAL target that had no function of its own (`jump_hints.txt`): 63 helpers the analyzer had merged
    into a neighbour, for example 0x3E1820 inside the stub-bound `sceMpegInit`, where the main thread died during
    boot.
- **Stubs.** libcdvd, libdma, libgraph, libpad and libmpeg entry points are bound to the runtime's handlers by
  name (`stubs`), so CD reads, DMA and pad I/O go through the HLE layer.
- **The game's own `rand` (0x42D508) is recompiled, not stubbed.** The runtime's `rand` stub returned
  `std::rand() & 0x7FFF`, while the game's newlib code scales its results by RAND_MAX 0x7FFFFFFF, so every random
  pick came out 0. The recompiled `rand` follows the game's own `srand`.
- **`[mmio]` and `[jump_tables]`** are the analyzer's, kept as generated.

The recompiler emits one C++ file per function (about 12,900 with the entry stubs) and `register_functions.cpp`,
the address-to-function table. `ps2_vu_aot` then finds the VU1 and VU0 microprograms in the ELF itself and compiles
them into `generated/vu1/` (`scripts/recomp.sh` runs both); no run of the game is needed for it.

## Game overrides (`runtime/sc3_overrides.cpp`)

`PS2_REGISTER_GAME_OVERRIDE` matches the ELF by name, entry point (0x100008) and CRC32, then `applySoulcalibur3`
sets up the following.

### Memory

- **Reserved runtime memory at 0x80000-0x100000.** crt0 calls `SetupThread` with the main stack at 0x01FDFF00
  and `SetupHeap(end, -1)`: the heap runs up to the main stack and the game allocates nearly all of it. The
  runtime's HLE pools and invocation stacks move to kernel RAM instead (`configureReservedMemory`).

### Timing and rendering choices

- **Wall-clock catch-up Off.** SC3's audio follows emulated time. Fast-forwarding emulated time after a slow
  stretch ran the game and its music faster than real time, and skipping fields sent it down its late-frame
  paths, whose display lists break: the menu-transition and Chronicles freezes (the ordering-table walk at
  0x10E4B8 looping, the main thread stuck in the frame-end wait 0x12E4F0). With catch-up off, emulated time only
  ever slows down; a late vblank restarts the field timeline instead of leaving a debt (`PS2X_EE_CATCHUP` still
  overrides it).
- **GS SIGNAL stall off** (`setSignalStallEnabled(false)`). A frame chains about 30 SIGNAL handshakes between the
  GIF and VIF1 paths (the GS interrupt handler 0x10E658 restarts both chains when both have signalled). Waiting
  out the modelled GS interrupt latency at each one cost about 20% (Free Training 22 -> 18 fps at the time) and
  the game draws correctly without the stall.
- **Native GS renderer** (`PS2Runtime::requestNativeGsRenderer`). The GS stream becomes OpenGL draws (docs/native.md
  in PS2Recomp). Free Training runs at 58 fps; the vertical seams every 64 pixels, the soft picture and the darkened
  frame were GS back end bugs, fixed on PS2Recomp sc3/native-gs-seams (pixel centres, HIGHLIGHT alpha, Ad blending,
  the Z buffer a draw's ZBUF names). `PS2X_GS_RENDERER=cpu` draws with the CPU reference instead.
- **SPU2 on the wall clock by default** (`PS2X_SPU2_WALLCLOCK=1` in scripts/run.sh, regress.sh and sc3run.sh;
  `=0` keeps the stretched IOP-clock output). The IOP mixes into a queue the audio callback takes from, so SC3's ADX
  stream (CRI ADXI, AutoDMA on core 1, one 256-sample half ahead) gets each half's interrupt before the next half
  plays: regress route 17367 AutoDMA transfers (17353 on the IOP clock), 23 empty halves (24869 before the queue),
  58.24 fps. The SPU2 stays within 512 samples of the IOP clock (PS2Recomp e749e57):
  regress route 17355 transfers, 24 empty halves, 58.24 fps, 6918 frames of silence (`gap=`, mostly while loading)
  in 189 s. SC3's movie soundtracks are host audio mixed into the output; the game never streams PCM on core 0.
- **IOP on the wall clock by default** (`PS2X_IOP_WALLCLOCK=1`, set by the override unless already set; `=0` puts the
  IOP back on the EE cycles the game accounts). Its timers, vblank interrupts and the sound drivers' ticks follow real
  time. It used to cost 4 to 7 fps: the runtime read the host clock at every `EeScheduler::accountCycles`, and
  `clock_gettime`, the duration conversion and `advanceIopEeCycles` took about a quarter of the EE thread in an
  uncapped fight (PS2X_PROFILE: 3150 of 11395 samples). It now reads it once per `kIopWakeEeCycles` (0.16 ms) of EE
  time (PS2Recomp sc3/native-hostclock-iop). Regress route, three runs a side, wall clock against EE clock: 59.79 fps
  against 59.31 capped, 58.62 against 57.81 with the fight uncapped, 6 and 13 frames over 25 ms against 10 and 9,
  0 to 69 empty ADX halves against 0 to 13 (before the fix: 55.87 against 59.79, 51.00 against 57.94, 18 and 57
  frames over 25 ms). Saves were made in Options (create, overwrite) and loaded back after a restart, the setting
  they hold restored at boot and by Load Data.
- **Frame loop on real time** (`SC3_NATIVE_FRAMES`, on unless `=0`; PS2Recomp docs/native.md, layer 1). The main loop
  (0x12DB20) ends each frame with an inline Timer 3 wait: 0x148098 (lines since the last frame) polled from 0x12EA08
  until it reaches the target at sp+0x30, then called once more from 0x12EA88 to record the frame's length. The
  override answers the polling call with the target at once, and at the recording call holds the frame to the
  FramePacer's cap, charges the EE the real time the frame did not account and presents the frame
  (`presentGuestFrame`). SC3 counts frames, so there is no time step to feed. EE time follows real time over the
  run: a frame that accounts more than it took leaves a lead the next frames use up (otherwise the IOP runs ahead and
  the SPU2 drops 44800 samples in the regress route), and a load's long frame is charged in full up to a second
  (the pacer's step stops at 0.1 s, and the ADX stream starved). While a one-shot movie plays (the intro, the
  opening, the Chronicles of the Sword story), the main loop takes another branch (0x12E638) and ends its frame in
  mwPly's vsync wait 0x3AE238: ADXM's 0x3F0F38 sleeps the main thread in 0x3F0F10 until ADXM's vsync thread, woken
  by the vblank interrupt, wakes it. The override answers that sleep (0x3F0F10 returning to 0x3F0F60 for 0x12E654)
  with the same native frame end, so movies are paced by the host as well and nothing runs on the vblank grid. Their
  pictures come from the movie clock (the soundtrack the audio callback took) at each window present: 29.97
  pictures and 60 presents a second, the movie clock 1.000 s per second, the intro 35.12 s against its 35.07 s,
  the opening 243.74 s against 243.68, the Chronicles movie 61.38 s against 61.23, a 330 s run's SPU2 output 330.3 s
  long; the main loop holds 60 frames per second through all of them and Start still skips. Without the override the
  main loop slept until emulated vblanks that no frame end kept on real time: EE time ran at 14% of real time and
  stood 1.8 s ahead when the movie ended (PS2Recomp's idle present, native/renderer ea929440, does not help: the
  movie pictures do not come from the GS). Two PS2Recomp fixes came with it: under native frames an idle wait lasts the EE cycles to its event, not the
  vblank timeline's lead (0.6 s stalls in the title-screen loads, sc3/native-frames-idle), and `PS2X_CAPTURE_DIR`
  saves the frame's own width (movie frames came out 1280x448, sc3/native-movie-scale). Regress route against the
  vblank grid: 59.56 fps against 58.22, game frames 59.65 per second (slowest 5 s 57.6) against 56.67 (46.3 in the
  character select and the stage load), 5 frames over 25 ms against 592, 21 empty ADX halves against 94 (8 to 94
  across runs). `PS2X_FRAME_CAP=0`: 90.3 GS frames and 112.5 game frames per second. The frame end sleeps on the EE
  thread (the game's own wait was a spin, so no other guest thread ran there either). `SC3_FRAME_SLEEP=1` sleeps
  the main thread instead (PS2Recomp `EeScheduler::waitHostDeadline`, the JC2 frame end) while the other guest
  threads run. Regress route, same build, back to back: 59.80 fps against 59.89, 6 frames over 25 ms against 10,
  9 empty ADX halves against 28, but EE time ran up to 2.8 s ahead of real time in the menus (18 ms without), since
  an idle EE skips from event to event while the main thread sleeps; uncapped 71.6 fps against 74.8 and 95 frames
  over 25 ms against 38 (that machine was busy: 90 fps uncapped when idle). So it stays off.
- **IRX modules recompiled** (`PS2X_IOP_NATIVE=1`, set by the override unless already set; `=0` interprets them).
  `scripts/iop_recomp.sh` boots with `PS2X_IOP_DUMP_MODULES` and runs PS2Recomp's `tools/iop_recomp.py` on each
  module into `generated/iop/`: CRI_ADXI 302 functions, MCMAN 202, PADMAN 136, LIBSD 59, SIO2MAN 49, NUSOUND 28,
  MCSERV 24, SDRDRV 13, none left to the interpreter. Regress route on par with the interpreter: 58.20 fps, 17362
  AutoDMA transfers, 14 empty halves (interpreted: 58.13 fps, 17364, 17).
- **VIF1 worker with the GS lag** (`setVif1Threaded(true)`, `setGsLag(true)`). VU1 took about 42% of the game
  thread in fights. VIF1 and the VU1 microcode now run on a worker that runs ahead of the EE on its own EE-cycle
  clock; the EE blocks only when it reaches a point past the busy worker's clock, and GS events raised by worker
  packets are scheduled at the worker's cycle, so GS drawing overlaps VU1. `PS2X_VIF1_THREAD=0` keeps both on the
  EE thread.
- **GS draw rates 8 pixels per GS cycle** (`setDrawRates({256, 32})`: 256 EE cycles per 1024 pixels, 32 per
  primitive). SIGNAL/FINISH latencies follow the estimated GS drawing since the previous event
  (`GsDrawEstimate`). At the default 4 px/cycle the modelled drawing of a fight frame took about 94% of a field
  and the game fell to ~45 fps in emulated time.

### Movies (CRI Sofdec on the host)

Sofdec decodes through libmpeg and the IPU, and the runtime's MPEG emulation is not fed by CRI's bitstream
callbacks: a started player blocks the main thread in `sceMpegGetPicture`. So players are never started:

- all ten `mwPlyStart*` entry points are replaced. `mwPlyStartAfs` / `mwPlyStartAfsLp` on AFS partition 1
  (`MOVIE.AFS`) start an FFmpeg decoder (`SfdPlayer`) on that SFD file for the handle; any other start reports
  its end straight away;
- **full-screen movies** (width >= 512: logos, THX, opening, intro, attract) are the native renderer's held
  backdrop (`Renderer::setBackdrop`), set from the SPU2 mixer at each new picture of the movie clock and cleared when
  the movie ends; their ADX soundtrack is mixed into the SPU2 output, whose length is the movie clock. The game makes
  no picture upload for them (its frame object stays empty). `SC3_NATIVE_MOVIES=0`, or a GS renderer without the
  native renderer, replaces the presented frame through the runtime's presentation override instead;
- **smaller movies** (menu previews, 128x96 and 256x192) are handed to the game by `mwPlyGetCurFrm` (0x3AB630) as
  IPU CSC output, RGBA32 16x16 macroblocks column by column, written into the player's unused work area (handle
  +0x20, the SFD handle at +0x3C marks its end). Alpha movies (handle +8 == 3) carry their alpha as the luma of
  the frame's bottom half. `mwPlyRelCurFrm` and `mwPlyIsNextFrmReady` are answered for host players;
- `mwPlyGetStat` (0x3AE9F8) and the movie task's poll (0x3AE988) report PLAYING until the decoder is drained,
  then PLAYEND (status 4 for handles without a host player, the task's normal exit); `mwPlyStop` (0x3AD870) stops
  the decoder, which is how Start skips a movie;
- **movie task end** (0x218600): a task that never started playing ended without clearing the movie job's
  "playing" word (scratchpad 0x70000030 -> +8), so the job waited forever; the override clears it.

- **Sofdec's decoder set-up** (0x3c25b0, per player handle, and its library init 0x3c2500) runs without libmpeg
  or the IPU: `sceMpegInit`, `sceMpegCreate` and the four `sceMpegAddCallback` only report success (nothing reads
  their results back, libmpeg is never fed), and the IPU context Sofdec saves for a later restore (0x3c2020: the
  toIPU / fromIPU DMA registers, IPU_CTRL, IPU_BP) is the idle one, written without reading the IPU. Before, each
  handle's set-up ran the runtime's MPEG HLE (whose `sceMpegCreate` also writes another game's addresses,
  0x1717bc and 0x171800.., into SC3's memory) and read the IPU three times.

Small movies are drawn by the game itself: the movie task 0x217f78 (flag 0x20, all the menu previews seen) copies
`mwPlyGetCurFrm`'s macroblocks through the scratchpad and sends them as 16x16 GIF image transfers (0x2174c0 ->
0x216e60, channel 2) into the texture a sprite task (0x2161a0, update 0x216060) draws through the menu sprite
builder 0x11f700 -> 0x11ea80. A native path needs that builder native (M4): the host picture then goes to it as a
keyed texture at 0x2174c0, and the macroblock write and the upload go away. Flag 1 (0x217e28 into a texture's
pixels, then 0x118b68) and the full-screen 0x217458 were not reached.

`[sc3:movies]`, one line per movie when it stops: its pictures against the wall clock and the movie clock, the
game's picture uploads (0x217458, 0x2174c0, 0x217e28), the GS image transfers, the IPU register and FIFO accesses
and the libmpeg / libipu / IPU context calls while it played. `SC3_MOVIE_CAPTURE_EVERY=<n>` with
`PS2X_CAPTURE_WINDOW_DIR`: the window showing every n-th picture of a full-screen movie as `movie_<file>_<n>.png`,
to compare two ways of showing it picture for picture (`scripts/imgdiff.py`).

`PS2X_SC3_NO_MOVIES=1` skips the one-shot movies only. Screens built on a looping movie wait for its frames: the
Tales of Souls loading screen before the last episodes stayed on a white NOW LOADING when those were skipped too.

### Native VU1 output (`runtime/sc3_native.cpp`, native layer 5)

On by default with the native GS renderer (`SC3_NATIVE_VU1=0` runs the programs on VU1). PS2Recomp's `Vu1StreamAdapter` reads every VIF1 stream at its DMA kick and decides, for each
MSCAL, whether VU1 runs the program or the adapter's own VU1 (the sandbox, the same AOT code) runs it and draws its
XGKICK packets through `GifToNative` into the GS targets, at the program's point of the GS stream.

- SC3 has two VU1 programs, A (1177 pairs) and B (1352 pairs), both loaded at VU address 0 and swapped 23 times a
  fight frame, so a start address alone does not say which one runs. The override hashes the resident code with the
  hash the VU1 AOT names its fragments by (A `95e81f9a833c1b45`, B `d2fbf7af837a8df4`), again only after an MPG
  (`Vu1StreamAdapter::codeVersion`). Nothing else ever runs on VU1 in the regress route.
- `SC3_NATIVE_VU1=observe`: the adapter sees every chain, every program stays on VU1; `[sc3:vu1]` every 5 s counts
  MSCALs by program and start address. Over the regress route the adapter's total is the state dump's `vu1 mscal`
  less the last unreported 5 s (6071933 against 6077558), character select and loads included, and per frame they
  agree in the fight (974.0 against 974.4). 58.37 fps, as without it.
- `SC3_NATIVE_VU1_HIDE=<A:0x10,B:*,...>` (or `@file`, read at each frame end): those programs skipped and not drawn.
  `REGRESS_PAUSE_HIDE` takes a pause still per entry. What each draws in Free Training:

  | Entry | MSCALs per frame | Draws |
  | --- | --- | --- |
  | A:0x10 | 178 | the stage's main geometry (floor, walls, balustrade); hiding it also loses the fighters |
  | A:0xd8 | 71 | distant stage props (pillars, lamps, background pieces) |
  | A:0x2b0 | 77 | stage props near the camera (balustrade top, shadowed parts) |
  | A:0xc18 | 11 | the sky and far background |
  | A:0x1558 | 8 | a floor layer (the glossy tiles) |
  | A:0x1730 | 13-17 | the floor's reflection (fighters and balustrade mirrored) |
  | A:0x1ab8 | 56 | weapons |
  | A:0x1c80, A:0x1f00 | 20, 37 | the weapons' and pillars' environment-mapped shine |
  | A:0xa48, A:0x1d28 | 7, rare | nothing visible in the paused scene |
  | B:0x1388 | 313 | the fighters' skinned bodies |
  | B:0x10 | 58 | the fighters (hiding it removes them entirely, 0x1388 included) |
  | B:0x15e8 | 60 | fighter parts (heads, sleeves, coat panels) |
  | B:0x350, B:0x5b0, B:0x1990, B:0x1ca8, B:0x1ef0 | 10, 5, 8, 27, 15 | small fighter parts, not separable at this size |

  Hiding a program also skips what it leaves in VU memory for the next ones, so a hidden setup program takes later
  ones with it.
- `SC3_NATIVE_VU1=1` (the default): every program runs off VU1 (in C++ since the ports below; on the sandbox with
  `SC3_NATIVE_VU1_PORT=0`), wherever VU1 would run it (menus, character select, attract
  fights, Character Creation, fights), the draw-only DIRECTHL packets too (none qualify: every DIRECTHL of a fight
  carries register setup). `SC3_NATIVE_VU1_SWITCH=<file>`
  (1 sandbox, 0 VU1, read at each frame end) and `REGRESS_NATIVE_VU1_AB=1` compare the two on the frozen pause:
  `pause_native_psnr` against `pause_floor_psnr` (two VU1 stills of the same pause). `SC3_NATIVE_VU1_ONLY` keeps the
  programs it does not list on VU1.

With the native GS renderer the sandbox's packets go to the GS back end itself (`Config::gsWrites`, TTA's
`tlotr/native-vu1-gs`): each batch carries the register writes its packet made (TEX0 with its CLUT load, CLAMP, TEX1,
ALPHA, TEST), replayed at the group's marker, so textures, CLUTs, regions, mipmaps and blending are the back end's,
in GS stream order. With S, T and Q kept as the packet had them (`sc3/native-gif-q`: U / V had been interpolated in
screen space), the pause still on the sandbox matches VU1: 34.6 dB against a floor of 34.4 dB, the floor strip under
the fighters drawn. Weapon trails, hit sparks and guard flashes show in captures every second over the fight.
`SC3_NATIVE_VU1_GSWRITES=0` draws through the native renderer's own pass instead (20 dB, after the
`sc3/native-gif-blend` fixes: Ad blends, A == B opaque, As stored, TFX / TCC, REGION_REPEAT / REGION_CLAMP windows).
Since the programs draw directly (below), packets and their register replay are only used with
`SC3_NATIVE_VU1_DIRECT=0` or on the sandbox.

The adapter runs on the VIF1 worker (`Config::onVif1Thread`, PS2Recomp `sc3/native-vu1-worker`;
`SC3_NATIVE_VU1_THREAD=0` keeps it on the game thread): VIF1 hands it the stream up to each MSCAL as it gets there, so
the sandbox runs each program at its place among VIF1's commands, on VU1's own memory (no second unpack), and moves
the worker's clock on by the program's cycles as VU1 did; the frame end's switch, hide file, report and publish are
queued behind the frame's transfers. Free Training, 30 s fights:

| Build | Capped | Uncapped fight | EE waits for VIF1 (per 5 s) |
| --- | --- | --- | --- |
| VU1 | 59.6-59.8 | 63.0 (53.7-68.4) | 1.4-1.6 s |
| sandbox on the game thread (before) | 40.9-42.3 | 43.1-43.4 | 1.2 s |
| same, `PS2X_VIF1_SLACK=1e8` | 43.8 | 45.7 | |
| on the VIF1 worker, clock moved at the gates | 37.7 | 40.7 | 3.0 s |
| on the worker, clock moved as each program runs | 53.2 | 54.2 | 2.7 s |
| on the worker, stream handed up to each MSCAL | 58.5 | 55.9 | 2.0 s |
| same, on VU1's own memory | 59.7-59.8 | 54.3-55.1 | 1.9 s |

Uncapped, the sandbox stays about 8 fps under VU1: its GifToNative work (about 1 ms a frame) now runs on the VIF1
worker, where VU1's packets went to the GS thread. The pause still matches VU1 (33.9 dB against a floor of 33.1).
Every screen of the regress route, the title's attract fights and Character Creation look the same on the sandbox
as on VU1 (stills on each side of a `SC3_NATIVE_VU1_SWITCH` flip; menus animate, so compared by eye).

The adapter sees about 300 k fewer MSCALs than VIF1 runs over the route, all in the load after stage select (the
same on the game thread): those programs stay on VU1 and draw as before.

The title screen was the one difference left: on the sandbox its background stayed dark navy, without the blue
glow and the flash of the logo. Programs moved to the sandbox one at a time (`SC3_NATIVE_VU1_ONLY`) put it on A:0x10;
a dump of that program's XGKICK packets showed draws under TEST 0x53001, an alpha test that never passes (ATST
NEVER) with AFAIL RGB_ONLY: the GS writes their colour and keeps alpha and Z. `GifToNative` dropped them as it drops
KEEP. Fixed on PS2Recomp `sc3/native-vu1-title` (ZB_ONLY now goes to the GS too, which masks the colour): over
seconds 21-24 of the title the frame's mean luminance is 168/172/180/194 on both sides (150-153 before) and the
captures' PSNR against a VU1 run equals that of two VU1 runs (26.4/22.0/18.9 dB). With only A:0x1f00 on the sandbox
the screen had turned white; the same fix ended that.

Checked against VU1 with the sandbox on, by stills on each side of a switch and by one capture a second over whole
runs on each side: the Chronicles of the Sword story, briefing, field map, march, the battle (intro, fight, K.O.,
BATTLE RESULT) and the field map after it; three Tales of Souls episodes (Valentine Mansion, Lakeside Coliseum,
Clock Tower: story pages, map, intros, fights, YOU WIN); the boot movies played through (Produced by NAMCO and the
title movie, twice each). Nothing else differs.

Full regress, Free Training, 60 s fights, three runs a side (fight frames over 25 ms counted over the whole run):

| | Capped fps | Uncapped fight fps | Frames over 25 ms | Empty ADX halves | Error lines |
| --- | --- | --- | --- | --- | --- |
| sandbox | 59.45-59.50 | 55.4-56.2 | 4-5 (uncapped 10-33) | 12-25 | 6 (the usual Sofdec / ADX ones) |
| VU1 | 58.65-59.72 | 61.7-63.1 | 5-12 (uncapped 8-17) | 0-18 | 6 |

With the title fix and the sandbox as the default: 59.42 capped, 54.9 uncapped (VU1: 59.45, 61.8).

#### B:0x1388 in C++ (`runtime/sc3_vu1_port.cpp`)

The fighters' skinned bodies, about 313 runs a fight frame of 3-58 vertices (1.9 bones each on average), go
through `Choice::Port` (`SC3_NATIVE_VU1_PORT`, on by default; `=0` the sandbox, `=check` both, compared). What the
program does, from its microcode (`vu_disasm.py` on the code image) and the AOT blocks:

- TOP holds the GIF tag (NLOOP | EOP, PRE with PRIM 0x5c: a textured, Gouraud, alpha-blended strip; NREG 3: ST,
  RGBAQ, XYZF2), then per vertex 1-5 influences of two qwords (position times weight, w the weight; normal times
  weight, w the bone's palette address, bit 15 on the last), the ST qword (w: the vertex's flags, bit 15 ADC) and
  the colour as floats.
- Skinning, in place from TOP + 1: position = sum of bone (four columns, the palette at VU 0) times (x, y, z, w),
  then through vf28-vf31 to GS screen space times w; normal = sum of the bones' 3x3 times n. Four qwords out per
  vertex: ST and flags, colour, normal, position.
- Lighting and projection, one vertex behind its loads (software pipelined; the first iteration's output lands at
  TOP + 0xec and the tag overwrites it): Q = 1 / w, STQ = (ST times vf15, 1) times Q, XYZ = FTOI4 of the position
  times Q; light = vf20 + vf21-23 times max(vf25-27 . n, 0); RGBA = FTOI0 of min(colour times light + vf10 times
  128, vf17.x), vf10 a clamped (vf16 + n'.w) term through vf24; F = 0xff (vi3 = 0xff0). ADC when the vertex's flag
  says so or the guard-band test ((vf18 + xyz / w) times vf19 against +-1, CLIP) fails for any of the last three
  vertices (FCAND 0x3ffff).
- The packet goes at TOP + 0xee, XGKICKed; a packet another program left at TOP + vi6 is kicked first (never in the
  fights recorded).

The C++ keeps the interpreter's float semantics (operands normalized, the VU's operand alignment in additions,
round toward zero, out-of-range results through the same double classification), its Q and clip flag timing
(the DIV lands at the MULq seven pairs on, FCAND sees the CLIP four pairs before it), and leaves the registers,
clip, MAC, status (ZS and SS must already be set, else the sandbox runs it) and cycles (33 + 43 per vertex + 10
per bone, plus 2 per qword of a pending kick) as the program does. Each vertex runs without range checks and again
on the exact path when a value leaves 2^-60..2^120.

Checked bit for bit (packets, VU memory, every register, clip, MAC, status, cycles): 1500 runs recorded from a
fight with `PS2X_VU1_RECORD_PC=0x1388` replayed by `sc3_tests` (`SC3_BUILD_TESTS=ON`, `SC3_VU1_PORT_TRACE`, both
paths), a synthetic batch against the interpreter's digests, and `SC3_NATIVE_VU1_PORT=check` over the whole
regress route: 1,960,411 runs, no difference. The pause still on the port matches the sandbox's in the 3D (the
`REGRESS_NATIVE_VU1_PORT_AB` diff is the blinking pause menu only).

A Soul Arena mission later found a difference the regress route never shows: lights around 1e37 saturate the
colour product to FLT_MAX, and the last MADD adds vf10 times 128 with vf10 at 0. The interpreter decides O on the
long double sum, exactly FLT_MAX, so no overflow; the port set O (MAC 0xe000, status bit 3) for any result that
landed on 0x7f7fffff. It now takes the long double sum too (`vu1-maxflag`). The 4,340 runs recorded in that mission
(`SC3_NATIVE_VU1_RECORD`, check mode records only the mismatches) replay without a difference, and `sc3_tests`
has a synthetic batch with the lights at 1e37 against the interpreter's digest.

Cost: replaying the 1500 runs, 8.1 us a run against the interpreter's 19.5 (10.8 on the exact path). Profile of
the VIF1 thread over 15 s of an uncapped fight (same binary): B:0x1388's AOT blocks 2174 samples on the sandbox,
754 for the C++; the thread from 7807 to 6977 samples while drawing more frames, the EE's waits for it from 2079
to 1851. Regress, Free Training, three or six runs a side:

| | Capped | Uncapped fight |
| --- | --- | --- |
| sandbox | 58.43-58.70 | 52.2-55.3 (mean 53.9) |
| port | 58.97-59.30 | 51.7-56.5 (mean 54.4) |
| VU1 | 59.28 | 58.8 |

Across runs the stage changes and the spread hides the gain; inside one uncapped 90 s fight, switching between the
two every 10 s (`SC3_NATIVE_VU1_PORT=@file`, the first 5 s after each switch left out), three fights: sandbox 53.4 /
54.5 / 54.2 fps, port 55.5 / 56.0 / 55.9, about 1.8 fps more.

What is left between the sandbox and VU1 is not the microprograms: on VU1 the same AOT code runs on the same VIF1
thread, and its packets go to the GS thread. On the sandbox the VIF1 thread also turns them into native batches
(`GifToNative`, about 730 samples) and keeps the texture cache (`TextureCache::uploadLocked` and `WriteCT32`, about
1050). The next step for B:0x1388 is batches straight from the port, without building and reading back its GIF
packet.

#### Programs A and B in C++ (PS2Recomp `tools/vu1_lift.py`, `generated/vu1port/`)

Every MSCAL of A and B runs as C++ by default: A's 20 start pcs (the setups at 0x10, 0x1c80 and 0x2098, which load
the constants and JR to a routine whose address the stream left in VU memory, and the 17 routines) and B's 18 (setups
at 0x10, 0x1990, 0x21e0 and 15 routines). B:0x1388 called directly keeps its hand-written port above while the
status holds sticky Z and S. `SC3_NATIVE_VU1_LOG` names entries with their JR target (`A:0x10>0x2b0`) and prints a
line of MSCALs left on the sandbox whenever there are any: over everything checked below, none.

PS2Recomp's `tools/vu1_lift.py` (first written here as `scripts/vu1_port_gen.py`, now shared with the other ports;
PS2Recomp `native/vu1-lift`) writes the C++ from the microcode `ps2_vu_aot` compiled, for the programs, start pcs and
JR targets `recomp/vu1_lift_spec.json` lists, on its operations header `runtime/ps2_vu1_lifted_ops.h` (namespace
`ps2x::vu1lift`); `scripts/recomp.sh` runs it, so, like `generated/`, nothing derived from the game is in the
repository. The move left the C++ the same byte for byte but for the header and namespace.

- Each instruction becomes the operation it does on the VU registers, held in locals, with the interpreter's float
  rules (operands normalized, the PS2 addition, round toward zero, MAX/MINI on raw bits, FTOI, CLIP). A result or an
  FMAC product outside the normal range goes through the interpreter's own classification (`Vu1Lifted::fmacFlags`,
  PS2Recomp `sc3/native-vu1-all`).
- What VU1Interpreter's pipeline makes depend on timing is worked out by the generator, for every path from every
  start pc, with the pipeline state as part of where a path is: each pair's stall (VF/VI/ACC latencies, the DIV
  unit), when a DIV's Q becomes visible to MULq, which CLIP an FCAND sees and which FMAC an FMAND sees, the old VI a
  branch reads right after an IALU write, the cycles. A loop's first iteration and its steady state become separate
  code where their timing differs: 1675 timed pairs for A's 1177, 1861 for B's 1352.
- XGKICK: a PATH1 transfer of two cycles a qword; the next XGKICK and the program's end wait for it (the only timing
  the data decides). A store while a transfer may still read VU memory (A at 0x1c60 writes the next packet's tag)
  lets it catch up first, as the interpreter does.
- MAC flags are computed only where an FMAND or the end state reads them, sticky Z and S only while the status does
  not hold them yet; U and O only arise on the exact path.

Checked bit for bit (packets, VU memory, registers, Q and I, clip, MAC, status, pc, cycles):

- 15,059 runs recorded from the sandbox over the regress route (`SC3_NATIVE_VU1_RECORD`, replayed by `sc3_tests`
  with `SC3_VU1_TRACE`): no difference.
- `SC3_NATIVE_VU1_PORT=check`, the port and the sandbox on every MSCAL, compared, with no MSCAL left on the sandbox:
  the regress route (5.35 M MSCALs), the attract demo idling on the title (5.80 M), Tales of Souls through three
  episodes (5.95 M), Chronicles of the Sword (6.85 M), VS Modes (4.42 M), the Shop (4.50 M) and Options (0.56 M):
  33.4 M MSCALs, no difference. Entries seen: every start pc of A but 0xda8, 0xf68, 0x2098/0x2140 and 0x2330, of B
  but 0xd8, 0xb78, 0xdd0, 0x10f0, 0x21e0/0x2290, 0x24f8 and 0x2740 (ported all the same, never run in these modes).
- Check mode with both logs over the other modes, driven live through `PS2X_PAD_INPUT` with the Tales of Souls save
  on the memory card: Character Creation (a new character through every tab and most equipment slots), Museum
  (Character Profiles, the character viewer with its stage and camera), Soul Arena (Quick Play through eight
  stages, five missions with two fighters), World Competition (tournament rounds), Tales of Souls to episode 6,
  every character's three costumes and weapons on the character select, and Free Training on every stage of the select grid with a new
  opponent each: 268.8 M MSCALs and 1.79 G VU0 calls. The only difference was B:0x1388's O flag above (4,340 runs,
  one mission); with the fix, 159.4 M MSCALs of those modes again without one. The entries left:

  | Entry | Calls | Reached in |
  | --- | --- | --- |
  | A:0x450 (A:0x10>0x450) | 13,396 | Character Creation's model and job previews, Soul Arena |
  | A:0xf68 (direct and A:0x10>0xf68) | 215,064 | most stages' fights: Soul Arena, World Competition, Free Training |
  | B:0xd8 (direct and B:0x10>0xd8) | 463,170 | Lizardman and others in World Competition, Soul Arena missions, Free Training |
  | B:0xb78 (direct and B:0x10>0xb78) | 239,847 | Nightmare and others, the character select, fights |
  | A:0xda8, A:0x2098/0x2140, A:0x2330 | 0 | never |
  | B:0xdd0, B:0x10f0, B:0x21e0/0x2290, B:0x24f8, B:0x2740 | 0 | never |
  | VU0 0x18, 0x438 | 0 | never |

  B:0x21e0's routines are the 0x1990 family's (fighter parts) with ST from the normal instead of lit colours, an
  environment map; A:0x2098 is a copy of A:0x1c80's setup (the weapons' shine) for 0x2140 and 0x2330. VU0 0x18 is
  the 16-bit angle sine at 0x100a18 (reached from 0x100aa8, cosine), 0x438 a test from the VU0 macro-mode copy of
  the VU1 pipeline at 0x4437c8-0x444450; neither EE path ran. Not tried: Chronicles past its first battles, Tales
  of Souls' last episodes and credits (`PS2X_SC3_AUTOWIN` lost episode 6 to a ring out), unlockable characters and
  Museum items this save does not have, Critical Finish.

Cost, replaying the route's runs on stallman: 2.5-8 us a run for A's entries against the sandbox's 5.3-13.5, 7-16 us
for B's against 13-30. Regress, Free Training, two runs a side, on the same runtime (PS2Recomp 4ebc8e3a), every
program in C++ (VU1 and VU0) against every program on the sandbox and VU0 on its interpreter
(`SC3_NATIVE_VU1_PORT=0 SC3_NATIVE_VU0=0`):

| | Capped | Uncapped fight |
| --- | --- | --- |
| sandbox and VU0 interpreter | 59.02, 59.78 | 55.72, 55.87 |
| C++ | 60.00, 60.02 | 61.43, 62.57 |

On `sc3/native-vu1` e33f7853 (the host-clock IOP and native/renderer 71524c5 merged): 60.00 and 60.02 capped,
60.98 and 60.88 uncapped.

#### Direct draws (`runtime/sc3_draw_sink.cpp`)

The C++ programs above still wrote the GIF packet the microcode would XGKICK; `GifToNative` parsed it into batches and
the GS front end replayed its register writes (`gsWrites`) before the GL back end drew them. They now draw straight
from what they compute (`SC3_NATIVE_VU1_DIRECT`, on by default with the C++ ports; `=0` the packets, `=check` both,
compared; `@file` read at each frame end).

What the packets carried (every MSCAL of the regress route, `SC3_NATIVE_VU1_RECORD`):

| | Where it comes from | What it holds |
| --- | --- | --- |
| Vertex tag | VIF data at TOP + 0, copied by the program to the packet base (TOP + 0xef for A, TOP + 0xee for B) | NLOOP vertices, EOP, PACKED, NREG 3 (ST, RGBAQ, XYZF2), PRE with PRIM: 0x5c (Gouraud, textured, blended strip), 0x7c (fogged), 0x4c (untextured), 0x5d / 0x7d (fans), context 1 always |
| Vertices | computed | per vertex S/T/Q, RGBA and X/Y/Z with F and ADC in the position's w (an ISW), three quadwords apart |
| A+D block | VIF data at TOP + 0x19f (the setup header's word at TOP + 0x1ad + 17 .y gives its offset), kicked from VU memory as is by the setups (A:0x10, A:0x1c80, B:0x10, B:0x1990 and the routines they jump to) before their vertices | TEX1_1 (0x61: bilinear), TEX0_1 (T8 textures, CT32 CLUTs, CLD 1), CLAMP_1, ALPHA_1 (0xa opaque, 0x44 blend, 0x48 additive, 0x58 additive by destination alpha, 0x89 multiply, 0x42 subtract), TEST_1 (0x50000 Z GEQUAL, 0x5002d alpha > 2, 0x53001 alpha NEVER with RGB_ONLY); MIPTBP1_1 for mipmapped stage parts (A:0x10>0x2b0); untextured setups only ALPHA_1 and TEST_1 |

Routines called on their own (A:0xd8, B:0x15e8, ...) kick only vertices: they draw with what the last A+D block set.
The A+D blocks are the game's material data as it keeps it: one 0x80-byte block per material in the stage and
character data loaded into RDRAM (0x01841500 onwards in the route's fight, the same addresses every frame, referenced
by the VIF chain), with ALPHA and TEST words from tables in the ELF's data (A+D quadwords at 0x4740b0 onwards) that its
EE code picks from (it also builds TEST words itself, `lui 5 / ori 0x2d` at 0x1106f8, 0x170554, ...). There is no
higher-level EE material structure they are made from at draw time, so the sink decodes those GS register words
natively (`ps2x::gs::decodeTex0`, no GS front end): the one fallback the native path takes.

How it draws:

- `PS2Recomp/tools/vu1_lift.py` (its sink form, `"sink"` in `recomp/vu1_lift_spec.json`) runs a VI address analysis
  over every timed path (each VI as TOP or a constant plus c + stride * k): the XGKICK at a constant address is the
  packet's base, stores from there to TOP + 0x19f are its quadwords, each a fixed field of a vertex (stride 3) or the
  tag. Programs A and B get a second form, `programA<kSticky, true>`, in which those stores go to
  `DrawSink::put<field, dest>` instead of VU memory and each XGKICK to `DrawSink::kick` (its PATH1 time from the tag);
  the B:0x1388 hand port does the same (`drawSkinnedBody`). Registers, flags, cycles and the rest of VU memory are the
  same as with packets.
- The sink keeps the vertices by field, the PRIM from the vertex tag and the drawing registers from the A+D blocks
  (both contexts, kept from one MSCAL to the next as the GS keeps them); a kick of the program's packet turns strips
  and fans into triangles (ADC vertices draw nothing) with the GS's own numbers (X / 16, Y / 16, 24-bit Z, Q = 1 when
  0) and hands them to `Renderer::drawGs`; a kick of VIF data is an A+D block to decode.
- `Renderer::drawGs` (PS2Recomp `sc3/native-direct-draws`) queues them for the program's group; at the group's GS
  marker the GL back end draws them (`GSRasterBackend::SubmitNative`) into the render target there, with the
  material's TEX0 (its CLUT loaded once, at the first draw after the game set it), TEX1, CLAMP, ALPHA, TEST and MIPTBP
  and the target's FRAME, ZBUF, XYOFFSET, SCISSOR, TEXA, FOGCOL and dithering. No GIF packet, `GifToNative` or
  `GS::writeRegisterUnlocked` in between; the CPU GS renderer draws them the same way.
- Programs without a C++ port would stay on VU1 (none over the modes checked); `gsWrites` and the native DIRECTs are
  off unless the packets are asked for.

Checked:

- `SC3_NATIVE_VU1_DIRECT=check` over the regress route: 5,620,182 MSCALs run both ways, the direct draws (material and
  every vertex, bit for bit) against the packets read back, registers, flags, cycles and VU memory outside the packet:
  no difference; no packet other than the vertex and A+D ones, no register the sink does not know, no other
  primitive.
- Pause still of the same frozen fight, against VU1 (`REGRESS_NATIVE_VU1_AB=1 REGRESS_NATIVE_VU1_DIRECT_AB=1`):
  on the native GL GS renderer, direct draws 33.24 dB, packets 33.23, two VU1 stills 34.94; direct against packets
  85.8 dB (one level off at most); the only difference with VU1 is the blinking pause menu. On the CPU GS reference
  (`PS2X_GS_RENDERER=cpu`, VU1 there too): direct draws 33.75 dB against VU1, 39.2 dB over the 3D below the menu.
- VU1 and direct draws flipped every 3 s (`SC3_NATIVE_VU1_SWITCH`), a frame captured a second, PSNR of consecutive
  captures across a flip against those without one (median): title 12.4 / 13.4, menus 21.8 / 23.1, character select
  20.4 / 20.6, fight 11.8 / 11.6, Chronicles of the Sword field map 26.1 / 26.3: no difference beyond the scenes'
  own motion.

Speed, regress route, Free Training, two runs a side, clean builds (main f1fdb63 against this branch; the same binary
with `SC3_NATIVE_VU1_DIRECT=0` for the packets):

| | Capped | Uncapped fight |
| --- | --- | --- |
| main (packets, GS register replay) | 60.03, 59.97 | 60.93, 60.67 |
| this branch, packets | 60.03, 60.03 | 60.72, 60.45 |
| this branch, direct draws | 60.05, 60.00 | 62.62, 62.42 |

#### Native meshes (`scripts/sc3_mesh_pack.py`, `runtime/sc3_meshes.cpp`, opt-in)

The 3D drawn from the game's part builders and an offline mesh pack instead of from the VIF1 stream (no-GS plan, M2 and
M3). Found with `SC3_RECORD_FRAME` (census): a fight frame calls 0x10A178 312 times (the stage: a REF to the part's raw
VIF data, flags in a2 whose low 16 bits are the routine, program A when bit 0x40000 is set) and 0x10A4E8 145 times
(fighters and weapons: a CALL to a DMA chain whose own MSCALs run 0x1388, 0x15E8, 0x1AB8, ...). Before a part the game
leaves in the scratchpad what goes with it: 0x368 (a header to send: 0x10AD18 copies the renderer's matrices, lights,
fog and guard band from scratchpad 0x100-0x340, 19 quadwords to VU TOP + 0x1AD, the setup at 0x10 loads them into
vf14-vf31 and jumps to the routine), 0x36C / 0x370 (the material A+D block to REF to TOP + 0x19F), 0x350-0x358 (bone
matrices and their indices), 0x384 / 0x388 (the microprogram upload when the program changes). Parts without a header
or material keep the registers and GS state of the last one. Leaves are read whole by sceCdRead and relocated in place
(pointers + the sub-entry's base); a material's TEX0 / MIPTBP get the texture ring's TBP / CBP at run time, so
materials and texture keys are taken from the disc's values.

- `scripts/sc3_mesh_pack.py` (stdlib, `python3 -I`, all cores, about 10 s): every raw part of every leaf (its tag
  UNPACK, then V2-32 ST, V4-32 / S-32 colour, V4-32 position with the vertex flags in w, V3-32 normal, four quadwords a
  vertex) as an object of PS2Recomp's generic mesh pack at its offset in the leaf, the exact words in section `SC3V`;
  every A+D block as a register block (with the GIF tag's REGS word, the texture's used size, as register 0x100);
  `build/sc3_meshes.leaves` maps pack files to ROOT.OLK. 1,647 leaves, 102,839 parts, 434 MB. Checked against the
  recorded frame: the 312 parts at their offsets with the qwc the game passes, the 45 material blocks found.
- At run time (`SC3_NATIVE_MESHES=1`, needs `SC3_TEXTURE_PACK_MODE` on, `keep` for now): leaves are bound
  (`LoadedImages`, size and CRC32) when sceCdRead has read them whole; 0x10A178 (and 0x109990, whose `j 0x10A178`
  skips the table) draws a part of A's routines 0xD8 (lit, fogged), 0x2B0 (lit), 0xC18 (unlit), 0x1558 (unlit, fogged,
  scrolled) and 0x1730 (unlit, scrolled) in plain C++ from the pack's vertices, the header (0x10AD18's copies rewritten
  in C++, kept like the VU's registers), the pack's material and the texture pack (the latest upload group CALLed
  before the part: 0x104AF0 runs a batch's texture CALL right before its draws). The routines' meaning, from their
  microcode: position through vf28-31, Q = 1 / w, STQ = ST times vf14 (plus vf24.xy when scrolled) times Q; lit
  colour vf19 + vf20-23 times max(vf24-26 . n, 0), times the vertex colour, clamped at vf16.x, else the colour times
  vf19; guard band (xyz / w + vf17) times vf18 against 1 over the last three vertices for ADC; fog F from
  (vf17.w + w) times vf18.w clamped to vf15.y..vf15.x, else the vertex's own F. Floats read as the VU reads them (an
  exponent of 255 is the largest finite value: the header carries such words) and products saturate.
- No VIF packet is built for a native part: the scratchpad packet gets a CNT with a VIF MARK (and the program upload
  when the game asked for one, for parts left to VU1). Its draw joins a `MeshViewList` as the game links the packet
  (0x10F8B8: the VIF1 list, in order; 0x10D9A0 / 0x10DC90: an ordering-table bucket); at the frame kick 0x10F098 the
  list is submitted through `MeshSubmitter` (GS-form batches, PS2Recomp `sc3/nogs-m3`), each draw a group, and the
  VIF1 interpreter places each group in the GS stream at its MARK (`setVif1MarkGate`), so the native 3D sorts with
  the 2D, shadows and glow the GS front end still draws until M4-M7. A part not drawn natively stays on VU1, its
  header and material sent (scratchpad 0x368 / 0x36C set) since the native parts before it sent theirs to no one; with
  native meshes on, the VU1 programs' C++ ports are off so that the groups are numbered by the MARKs alone.
- `[sc3:mesh]` every 250 frames: parts drawn natively and their triangles, parts left to VU1 by cause (routine not
  native yet, not in the pack, no material, no pack texture: the texture places listed by TBP / PSM / CBP), chain parts
  left to VU1, MARKs reached, out of the list's order, unknown; leaves bound. `SC3_NATIVE_MESHES=check` also runs each
  native part's routine (through its setup with the same header) on the lifted program and compares the vertices.

- Parts deferred for clipping against the near plane: 0x1088A0 drains them, 0x10B970 clips each on the EE and 0x10B7E0
  sends the strip with the part's routine and no header or material (VU1 keeps the last part's). Drawn natively from the
  clipper's vertices (0x50 bytes a vertex, the four quadwords the routine reads) under the last header and material, the
  routine and GIF tag from the pack's deferred part (0x10B970's caller holds its {flags, address, qwc} entry in s0).
  Left to VU1 only while VU1 holds the last header and material, else counted as not drawn. VU1's header and material
  are tracked apart: a part left to VU1 resends them only after a native part took the game's last one.
- A material without TEX0 (ALPHA / TEST only) is drawn untextured when the part's primitive has no TME.

State (2026-11-01, second pass), Free Training: every raw part native (A:0xD8, 0x2B0, 0xC18, 0x1558, 0x1730, 0xA48,
0x1D28, about 300 a fight frame) and the clipped strips (up to 1,900 per 250 frames, about 0.5 % not drawn); the
0xFFFFFFFB "reserved instruction" was VU1's XGKICK overflow on such a strip run with a stale header, now 0. Bench runs:
3 of 3 reach the pause, mean 58.9 to 59.2 fps, fight 53 to 56 fps against 59.5 with the switch off: with the switch on
the C++ ports of A and B are off, so the fighters' chain parts (0x10A4E8, all on VU1) run on the VU1 recompiler
(PS2X_PROFILE: the VIF1 thread doubles, the EE waits on it). Check: positions, colours, STQ agree with the lifted programs
(rounding only). REGRESS_MESH_AB: the native pause still is 17 dB from the VU1 one (floor 33 dB); per routine
(REGRESS_MESH_AB_EACH) A:0xA48 (16.9 dB) and A:0xD8 (19.7 dB) differ, the floor drawn much darker natively with the same
vertices: the material's interpretation (TFX / TCC / blending) under the GS-form batch, not the arithmetic. Open: that
difference, the fighters and weapons (0x10A4E8's chains: B:0x1388 skinning, B:0x15E8, A:0x1AB8, A:0x1F00 and the shine
setups; raw parts with A:0x1F00 too) and the VIF0 part lists (0x10B3F0). Off by default until those are done and the route is clean.

#### Texture pack (`scripts/sc3_texture_pack.py`, `runtime/sc3_textures.cpp`)

The direct draws took their textures from the GL back end's model of GS memory, filled by the game's uploads through
the GS front end. SC3's textures exist on the disc only as prebuilt GS uploads inside ROOT.OLK's models; they are now
converted offline from the user's disc, and a direct draw binds its texture from the pack.

How the game uploads (found with `SC3_TRACE_FRAMES` and the load tracers below): each model's command list, run by
0x104af0, CALLs texture CALL lists of that model (command 2; 3 through the OT; 20 an upload chain picked from a table
by a float: animated textures). A list is a run of DMA CALL tags into upload units ended by RET; a unit is a CNT with
GIF A+D BITBLTBUF / TRXPOS / TRXREG / TRXDIR and an IMAGE tag, then a REF to the texels, uploaded as PSMCT32 blocks
and sampled as T8 / T4 with CT32 CLUTs. The CALLs and the GIF side's SIGNAL 1 go into the GIF list, the VIF1 side's
SIGNAL 2 into the VIF1 list (a DIRECT); the two halves of the texture area (TBP 0x2c40 and 0x3800) alternate, so a TBP
is no identity. The game reads a leaf of ROOT.OLK whole with CRI's file layer (sceCdRead from 0x3ffad0) and relocates
it in place; a model's DMA addresses are relative to its own header (03 04 04 00), at a leaf's start (cdata) or
inside a container leaf (characters, stages). A material's GIF tag keeps the texture's used size in its spare REGS
bits (bits 32-47 width, 48-63 height); its TEX0 often selects more (TW 10 for a 128 x 64 texture).

The converter (stdlib Python, `python3 -I`, all cores; run by `scripts/build_runtime.sh` when there is no pack yet)
walks ROOT.OLK's sub-archives to their leaves and each leaf to its models, plays each CALL list's uploads into a model
of GS memory (offline only; PSMCT32 / PSMT8 / PSMT4 layouts from PS2Recomp's tables), notes which list entry wrote each
byte, and keeps every material texture of the model whose texels and CLUT the list wrote, at its used size: its
indices (or CT32 texels) and its palette (CT32, index order, alpha 0x80 = 1.0). It writes PS2Recomp's generic texture
pack (`PS2XTEX1`, `tools/native_pack/texture.py`; one file per CALL list, named `<leaf>@<list offset>`) and SC3's key
index beside it (`build/sc3_textures.keys`: where each list sits in ROOT.OLK, its entries and whether every texture
their uploads feed is in the pack, and which records a TEX0 and used size select). On the regress route's disc:
12,267 lists, 37,131 CALL entries, 73,122 records (textures and palettes), 559 MB; 213 upload shapes it does not
convert (short IMAGE data, T4 uploads, chains without an end) keep their GS uploads.

At run time (only on the native GS renderer):

- Where leaves load: ROOT.OLK's sector from the game's first index request (0x150418, a byte position) and the
  sceCdRead serving it; every later sceCdRead into RDRAM maps its buffer to its ROOT.OLK bytes.
- Texture CALLs: 0x10f808 called from command 2 or 20, 0x10d9a0 from command 3 or 20. The target comes from the
  game's command list (its word, or the table entry its float picks, `cvt.w.s` as the EE does), through the load map
  to the pack's entry. With every texture of the entry in the pack, its CALL is not appended to the GIF list (0x10f808's
  bookkeeping without its copy: the scratchpad half flips as it would), so the GS front end never reads those uploads.
  The OT variants still send theirs. Each CALL also records its segment of the GIF / VIF1 handshake (the game's GIF
  sync points before it, 0x10f9d0 with id 1).
- The frame kick 0x10f098 hands the frame's groups to the VIF1 thread; each frame end there takes the next frame's.
- On the VIF1 thread, the DIRECTs the stream carries give the game's own SIGNAL 2 sync points; a textured direct draw
  takes the texture its TEX0 and used size select from the latest group that has it among those whose segment the
  stream has reached, as the GS would sample it then, and hands it to `Renderer::drawGs` (`GsMaterial::texture`,
  PS2Recomp `sc3/nogs-m1`): the GL back end binds it instead of decoding GS memory and loads no CLUT. A draw without
  one samples GS memory as before and is counted by TEX0.
- `[sc3:packets]` and `[sc3:gs]` every 250 frames: texture uploads called, from the pack (left out, sent), not in
  the pack, and the GS image transfers the GS front end still reads from GIF packets (`GS::imageTransfers`); direct
  draws textured from the pack or from GS memory, the most frequent misses by TBP, PSM and CBP.

Upload data the game changes (2026-11-02): the pack holds the disc's textures, but the game may rewrite the upload data
in RDRAM before it sends it. Character Creation does: the created character's leaves get their 1 KB CLUT payloads (the
REF of a 64-qword upload unit) rewritten in place for the colour edits (REF addresses and texels unchanged; found with
`SC3_TEXTURE_DUMP=<dir>`, which writes RDRAM, the ROOT.OLK read map and the frame's groups when `<dir>/request`
appears), so the pack drew the character in the disc's colours (all green). The converter records, per texture, the
leaf offset of each palette entry's upload word and the runs of its texels' upload words with their CRC-32 as on the
disc (`SC3KEYS2`). At the per-part hook a texture is checked against the game's copy of that data, where the group's
leaf was read or copied (the reads next to it that continue it both in RDRAM and in ROOT.OLK included), the palette
once a frame, the texels one frame in eight (a fight: about 110 textures and 165 KB a frame): a changed palette gives the pack's texels decoded under the game's colours (kept
by content, a new native texture key per palette); changed texels are counted (none seen so far) and keep the pack's.
`[sc3:gs]` reports the textures checked, the bytes read, palettes and texels changed, and data outside what the game
read. Creation also CALLs every item's upload lists before drawing any item, the items sharing their TEX0s, so a part
takes its texture from the groups of the leaf (or copy) it sits in first, then from the latest group as before.
State: the colours match the GS path's; the GS path still draws every Creation item in the last item's texture data
(the 33 item lists all write CLUT 0x2c40 and texels 0x2c44 with no sync point between them, and the GS front end
plays them before the items' draws), the native path each item in its own (which of the two the hardware shows is not
checked yet). Chronicles field map: A:0x450, A:0xf68, B:0x910, B:0x1a40 not native yet, about 70 parts a frame without
a pack texture, and two dark ground quads drawn only by the native path. Museum Character Profiles: at the floor.

`SC3_TEXTURE_PACK=<file>` names the pack (the scripts pass `build/sc3_textures.pack` when it exists);
`SC3_TEXTURE_PACK_MODE=1` uses it (off by default: see the state below), `=keep` draws from it with the game's uploads
still sent, `=@file` reads 0 / keep / 1 every 16 frames (`REGRESS_TEXPACK_AB`). `PS2X_NATIVE_TEXTURE_CHECK=1`, with the uploads
sent, compares each pack texture with the texture cache's decode of GS memory where its TEX0 points (meaningful only
where the TBP is not reused within the frame: the cache sees the frame's uploads in DMA order).

State (2026-11-01): off by default, a diagnostic switch. Pause still of the Free Training fight with the pack on and
the covered uploads left out: 22.4 dB against a 32.3 dB floor of two stills without it; the counters show most direct
draws in the pause finding no pack texture (8,000 from the pack, 228,750 from GS memory per 250 frames), so with their
uploads left out they sample stale GS memory. What is missing is a draw's place among the upload groups: the direct
draws come from the VIF1 stream (the game's OT reorders the stage parts against the order the CALLs were built in),
and the GS-order position used here (the game's SIGNAL 2 in the VIF1 stream's DIRECTs, a GIF packet read, acceptable
only as this diagnostic) does not match the groups' segments in the pause. With the 3D drawn from the game's
per-object hooks (M3: 0x104af0's commands run in order, a batch's texture CALL right before its draws) the group is
known at the hook and none of this is needed.

#### VU0 microprograms in C++ (`runtime/sc3_vu0.cpp`)

VU0 runs SC3's math library as microcode (`VCALLMS` from about 15 EE functions): one program, the 172 pairs
`ps2_vu_aot` found at ELF offset 0x367a90 (VU0 code memory hash 5e40f831...), called from 0x0, 0x38, 0x60, 0xf0,
0x360, 0x3c8 and 0x3e8, about 370,000 times a second in a fight. `PS2Runtime::setVu0MicroHook` (PS2Recomp
`sc3/native-vu1-all`) hands each call to `runtime/sc3_vu0.cpp`, which runs the program as C++ written by
`vu1_lift.py` the same way as VU1's (VU0's 4 KB memories and branch range, plus ITOF, MR32, SQRT, RSQRT, WAITQ,
FCGET), from the state VU0 starts at (reset, the registers and flags copied from the EE context).
`SC3_NATIVE_VU0=0` gives every call back to the interpreter, `=check` runs both and compares; `SC3_NATIVE_VU0_LOG`
and `SC3_NATIVE_VU0_RECORD` log and record as for VU1.

Checked: the route's 7,732 recorded runs replayed by `sc3_tests` (`SC3_VU0_TRACE`), 0.15-0.36 us a run, and
`SC3_NATIVE_VU0=check` over the same seven runs as VU1 above: 197.4 M calls, no difference, none left to the
interpreter. 0x18 and 0x438 never ran, in those modes or in the ones above.

### Test harness and tracers

- `PS2X_SC3_AUTOWIN=1`: on each GS interrupt during a fight, the player's health (float at 0x4D36A0) is set to
  full (240) and the opponent's (0x4D36A4) to 1, so the next scripted hit wins. Used to play Tales of Souls to
  the credits with scripted input.
- `PS2X_SC3_TRACE_ENTRY=<addr,...>`: registers, a few stack slots, Timer0 and the EE cycle at each entry.
- `SC3_HIDE_EE=<addr,...>` (`runtime/sc3_census.cpp`): those game functions return at entry (diagnosis);
  `=@file` with `SC3_HIDE_EE_ALL=<the candidates>` reads the list every 16 frames. `REGRESS_PAUSE_HIDE_EE` takes one
  pause still per function (`pause_hide_ee.txt`).
- `SC3_TRACE_LOAD=1`: the file requests (0x150418), sceCdRead calls and copies of 4 KB or more (0x42d2ac);
  `SC3_TRACE_OT=1`: ordering-table insertions per caller every 300 frames; `SC3_TRACE_FRAMES=<frame>,<count>`: for
  those frames (counted at the frame kick 0x10f098), the batches (0x104af0), texture CALL targets, sync points, the GS
  interrupt handler's SIGNAL counts and each direct draw's TEX0 and material tag.
- `PS2X_SC3_TRACE_OBJ=<fn>:<off,...>`: the words at those offsets of the object in `$a0` whenever they change
  (state machines such as the Art Gallery task, state at +0x130).
- `PS2X_SC3_RATE=<addr,...>`: calls per 60 vsyncs of each function.
- `PS2X_SC3_TRACE_PACING` (`_ALL`, `_SPAN`, `_FULL`): the frame limiter's Timer3 waits (0x148098).
- `scripts/regress.sh` ends in the Free Training pause menu and keeps that frozen frame as `pause.png`;
  `scripts/imgdiff.py` compares two frames (PSNR, a diff image) with Python's standard library only. Runs differ in
  stage and poses, so compare stills from one run (`REGRESS_NATIVE_VU1_AB`, `REGRESS_PAUSE_HIDE`), not across runs
  (two VU1 runs' pause stills: 13 dB). `REGRESS_SHOT_SECS=1 REGRESS_KEEP_SHOTS=1` keeps a capture a second.
  `REGRESS_FIGHT_FRAME_CAP=0` uncaps the fight only (`SC3_FRAME_CAP_FILE`): with `PS2X_FRAME_CAP=0` the menus run
  ahead of the route's presses and it ends in Character Creation.
- `SC3_TRACE_FRAMES=1`: every 5 s, the main loop's frames per second, the frames over 25 ms, the longest, EE time's
  lead over real time and the vsync count; each frame over 50 ms on its own.
- `SC3_FRAME_SLEEP=1`: native frames end on `EeScheduler::waitHostDeadline` (other guest threads run while the
  main thread waits) instead of holding the EE thread in the FramePacer.
- `PS2X_SC3_TRACE_GSIRQ=<n>` (`_VSYNC`, `_SPAN`): CSR, SIGLBLID and the VIF1/GIF chain addresses at the GS
  interrupt handler 0x10E658.
- `PS2X_SC3_TRACE_ADXT`, `PS2X_SC3_TRACE_ADXF`: ADX player status changes, AFS partition loading.
- `PS2X_SC3_TRACE_TASKS`, `PS2X_SC3_TRACE_MOVIE`, `PS2X_SC3_TRACE_FADE`: task creation with names (0x128B30),
  movie task and job states and, every 5 s of a full-screen movie, its pictures and the window's presents per second
  against the movie clock, the fade driver (0x1A2960).
- Experiments kept behind variables: `PS2X_SC3_FORCE_TEXT` (start texture-font fades that never started),
  `PS2X_SC3_ADX_PLAYEND` (report PLAYEND for ADX players stuck in DECINFO). Neither is needed today.

Recompiled functions return early at scheduler checkpoints with `ctx->pc` inside their body, so a wrapper around
a guest function may only read state on entry, never `$v0` after the call. Overrides registered with
`registerFunction` only catch dispatched calls, not direct calls between recompiled functions.

## Native target (as of 2026-11-01)

The port runs on the native target the other PS2Recomp ports use. It pins PS2Recomp's `sc3/native-vu1` integration
branch: `native/renderer` plus the SC3 fixes below, on their way into `native/renderer`.

- **Native GS renderer**: PS2Recomp's GS back end translates the GS stream into OpenGL draws (`GSNativeBackend`),
  on its own model of GS memory (see the native layers in the README). SC3 needed these
  fixes in it: pixels sampled at their integer coordinates as the GS does (a one-pixel seam every 64 pixels and a
  soft picture before), HIGHLIGHT and HIGHLIGHT2 adding the vertex alpha at its own scale, blending with Ad at the
  GS's scale, draws testing against the Z buffer their ZBUF names (the shadow pass darkened the whole frame), and
  for the character shadows: blends with COLCLAMP off keeping the low 8 bits (the shadow volumes count by wrapping
  past white), a colour buffer read as Z in the Z format's block order, and untextured draws at their own colour.
- **Higher internal resolution**: `PS2X_GS_SCALE=2` draws at 1280x896. The screenshots above come from it.
- **IOP modules recompiled**: `scripts/iop_recomp.sh` turns the eight IRX modules into native code (813 functions,
  none left to the interpreter); `PS2X_IOP_NATIVE=0` interprets them again.
- **SPU2 on the wall clock** by default (`PS2X_SPU2_WALLCLOCK=1` in the scripts), with an AutoDMA fix so the ADX
  stream gets each half's interrupt in time, and kept within 512 samples of the IOP clock.
- **IOP on the wall clock** by default (`PS2X_IOP_WALLCLOCK=1`, set by the game override; `=0` puts it back on the
  EE cycles the game accounts): its timers, vblank interrupts and sound driver ticks follow real time, at no cost in
  fps (59.8 capped, 58.6 with the fight uncapped, against 59.3 and 57.8 on the EE clock).
- **Frame loop on real time** by default: the main loop's frames are paced by PS2Recomp's FramePacer and shown when
  the game ends them, not on the vblank grid (`SC3_NATIVE_FRAMES=0` goes back to it). One-shot movies too: their loop
  ends its frames in a vsync wait of CRI's ADXM, which the override turns into the same native frame end, so they
  play at their 29.97 pictures a second on the host's clock, with their sound in step. `PS2X_FRAME_CAP` sets the cap
  (60 by default, 0 = unlimited).
- **VU1 microprograms in C++** by default: PS2Recomp's `Vu1StreamAdapter` reads every VIF1 stream and, at each
  MSCAL, runs the program as C++ on its copy of VU1 memory, on the VIF1 thread. The programs draw from what they
  compute: no GIF packet is written, each vertex goes to `runtime/sc3_draw_sink.cpp` as it is computed, the drawing
  registers come from the game's own material blocks in its VIF data, decoded in C++, and the triangles go to the GL
  back end at their place in the GS stream (`Renderer::drawGs`); `SC3_NATIVE_VU1_DIRECT=0` goes back to the packets.
  SC3's two programs, A (stage, props, sky, floor,
  reflection, weapons, shine) and B (fighters), are written as C++ by PS2Recomp's `tools/vu1_lift.py` from the microcode
  `ps2_vu_aot` compiled (`scripts/recomp.sh`, `generated/vu1port/`), with what VU1's pipeline makes timing-dependent
  worked out for every path; the fighters' skinned bodies (B:0x1388) keep their hand-written port
  (`runtime/sc3_vu1_port.cpp`). Every MSCAL leaves exactly what the sandbox (the AOT-compiled microcode on
  PS2Recomp's VU1 interpreter) leaves: packets, VU memory, registers, flags and cycles.
  `SC3_NATIVE_VU1_PORT=0` puts every program back on the sandbox, `=check` runs both and compares,
  `SC3_NATIVE_VU1_PORT_SKIP=<A:0x10,...>` keeps some entries on it, `SC3_NATIVE_VU1=0` gives everything back to the
  emulated VU1. The VU0 microprograms the EE calls with VCALLMS (the math library, about 370,000 calls a second in
  fights) run as C++ written the same way, in the interpreter's place (`runtime/sc3_vu0.cpp`; `SC3_NATIVE_VU0=0` puts
  them back, `=check` compares).
  The regression route holds 60.0 fps capped; uncapped fights run about 62.5 fps drawing directly (61 from the packets), against 55.8 with everything on
  the sandbox and VU0's interpreter.
- **Speed**: the regression route runs at 59.6 fps (58.2 on the vblank grid), with 5 frames over 25 ms instead of
  about 590; uncapped (`PS2X_FRAME_CAP=0`) it reaches 90 GS frames per second, 112 game frames per second.

## Runtime fixes found with this game

All of them are generic and on boubou666's `main`, which the submodule follows; there is no SC3 branch any more.

### EE, kernel and libraries

- **Frame budget calibration (30 fps instead of 60).** SC3 measures at boot how many Timer0 counts pass in 256
  Timer3 (HBLANK) changes, reading Timer3 twice per iteration, and sets its frame budget from that. Reading an
  unchanged timer charged one tick, applied only at the next scheduler checkpoint, so each further poll before it
  added another: the game measured Timer0 at twice its rate, set a two-field budget and ran fights at 30 fps.
  Busy-wait polls now owe a single tick until it is applied (f7a61cdf).
- Timer 3 counts HBLANKs from reset; thread priority 0 accepted; `ChangeThreadPriority` returns the old priority;
  `sceDmaRecvN`.
- **vsprintf**: one 64-bit va_list slot per argument (button help showed "Confirm (null)"); a null format pointer
  formats as the string at address 0 (the Chronicles field map draws a label whose pointer is 0).
- **CD drive timing**: 30/100 ms seeks by distance and a CAV transfer rate; `sceCdSync` reports a read busy for as
  long as the drive takes and a blocking `sceCdSync(0)` waits across vblanks. The Museum's Art Gallery stayed black
  because its loads finished before the Museum's 30-frame fade-out, which then finished last (screen brightness is
  the float at 0x4690D4).

### Recompiler

- MMI variable word shifts (PSLLVW/PSRLVW/PSRAVW) had their operands swapped (broken menus).
- A trailing branch delay slot at a function boundary was dropped when another function started on it (0x1DFF00
  lost `addiu sp,0x30`, crash with ra=1); no function starts in a delay slot any more, which merged 14 former
  split functions (0x200000, 0x1E00F0, ...) into their callers.
- SQRT.S took sqrt($f0) instead of ft (56 sites, among them the game's vector length 0x103000 and distance
  0x103058).
- **VU0 macro VCLIP** put -x/-y/-z in the even clip bits and judged against w instead of |w|. Stage zone culling
  (0x1072B0 -> 0x1087B0, CLIP then `CFC2 vi18 & 0xAA8`) hid every stage part in front of the camera: the first
  Chronicles battle (stage 3, the snow courtyard) and the field map terrain drew only the clear colours.

### IOP and audio

- **Module start**: IRX modules start with loadcore's `start(argc, argv)` and sysclib's `look_ctype_table` is
  implemented. Without them CRI_ADXI never saw `spucore=2` and the game stopped core-0 output every frame: no ADX
  music.
- **CpuSuspendIntr ignored (Chronicles DECISIVE BATTLE stall).** The IOP scheduler delivered interrupts and let
  higher-priority threads run while a thread had interrupts suspended. CRI_ADXI's DTX thread (priority 48) and
  PSM thread (priority 39, woken by the SPU2 ADMA interrupt) share stream-joint chunk lists, and about one
  exchange in 17,000 lost a chunk. After a BGM stop the EE side's restart waits for the whole 0x4000 ring, so the
  player stayed in PREP forever and Chronicles froze on the DECISIVE BATTLE banner. No interrupt dispatch while
  interrupts are suspended, and the masked thread keeps the CPU across slices (64181c70).
- **Audio "frying".** Three causes. The IOP scheduler let a lower-id thread of equal priority preempt the
  running one at every slice end, so CRI thread 7 cut into the ADX mixer thread 10 (both priority 39) and the
  right channel sent stale samples; equal priorities now keep the CPU until they block (389bf5a). Calls the EE ran
  inline on the IOP were never charged against the cycles the EE paid for, so the SPU2 produced 0.9-2.5% more
  audio than real time (fffb0c0). The host time stretcher spliced on every burst of that jitter; it now plays
  contiguous grains while the stream is at pace (0177887). Over the regression route the SPU2 now produces
  48,084 frames per second for 48,112 taken, at a stretch rate of 1.00 throughout.

### GS, DMA and VIF

- **GS interrupt handshake.** SC3 paces its frames on SIGNAL/FINISH interrupts: CSR.FIFO reads empty only once
  the GS has drawn what it was sent, INTC/DMAC causes are latched, the GS interrupt is level-triggered, and
  SIGNAL/FINISH/LABEL are ordered. Their latency follows the estimated drawing time since the previous event
  (`GsDrawEstimate`, scissored primitive area plus a per-primitive cost): a fixed 200,000-cycle latency held the
  heavy attract stage (Chamber of Time) to 20 fps, while shorter fixed ones starved the loader thread and left the
  NOW LOADING overlay over the attract demo. Loading screens draw few huge primitives and keep long latencies.
- **GS event deadlines on the vblank timeline.** GS events were paced by emulated cycles and by a host deadline of
  now plus latency, so the EE slept about 0.9 s in every 5 s of a fight. Their host deadlines now sit on the vblank
  timeline, and GS interrupt checks use an absolute cycle.
- **Wall-clock catch-up** can be bounded to once per field or turned off (`WallClockCatchUp`); with it off a late
  vblank restarts the field timeline (8f08ca89), see the override above.
- **VU1 run-ahead**: the VIF1 worker on its own EE-cycle clock with the GS lag, GS events of worker packets at
  the worker's cycle, a GIF DMA queued under the GS lag completing its channel when PATH3 goes to the worker.
- **REGION_REPEAT drains.** SC3 draws its fighters with 1024x1024 region-repeat textures masked to 256 texels.
  Counted whole, their texel span wrapped through VRAM into the display buffer, so every fighter strip looked like
  a primitive sampling its own target and was drawn serially after a full raster drain (about 17,000 drains per
  5 s in Free Training). A region repeat now samples exactly the texels between FIX and MSK | FIX.
- GIF tag continuation across PATH3 DMA transfers (the frame copy is one REGLIST tag of 280 sprites in 10
  transfers; without it a stale 64-pixel strip showed on the left), CLUT loads deferred to the raster workers.

### VU

- VU1 interpreter split into inline upper/lower/step headers, dead MAC and status flags skipped, FMAC arithmetic
  specialized per instruction; the EFU opcodes were off by one (0x79 is ERSQRT, 0x7C ESIN, 0x7D EATAN, 0x7E EEXP).
- **Compiled microcode: `ps2_vu_aot`.** The port first compiled its microprograms with its own `ps2_vu1_compile`,
  from code images dumped while playing, into single-entry segments with static stall elision (about 5.8 ns per VU
  cycle against ~20 for the interpreter). It now uses main's `ps2_vu_aot`, shared with the other ports, which reads
  the microcode straight from the ELF and compiles blocks with the stalls scheduled at compile time and the VU
  registers in locals. VU0 microprograms are compiled too (their flags stay exact: the EE reads them with CFC2).
- **Sticky-flag elision (3c1da2df, fc88d9b8, e27fba56).** `ps2_vu_aot` skipped the MAC and status writes no reader
  sees only when no microprogram of the ELF can read the sticky status bits. SC3 has FSEQ readers, so all 3,282 of its compiled VU1
  FMACs computed and queued exact flags with a clock advance before each, and they took most of the VU1 thread,
  which is why the port kept its own compiler. Such an FMAC now keeps only what it adds to the sticky bits, folded
  in before any flag write a reader sees, which brought `ps2_vu_aot` to the speed the port needs and let it move
  to plain `main` (with the earlier block-exit, rounding and operand normalization work, e73aba08).
- The UNPACK fast path, XGKICK copying whole elapsed qwords, PATH1 catching up only from where it last did.

### Diagnostics added for this port

`PS2X_STATE_DUMP`, `PS2X_CALL_TRACE`, `PS2X_WATCH`, `PS2X_PAD_SCRIPT` and `PS2X_PAD_INPUT`, `PS2X_FRAME_DUMP`,
`PS2X_EE_PROFILE`, the Linux `PS2X_PROFILE` sampler, `PS2X_GS_DUMP_AT` / `PS2X_GS_DUMP_WHEN`, the IOP tracers
(`PS2X_IOP_TRACE_PC`, `PS2X_IOP_COUNT_RANGE`, `PS2X_IOP_WATCH`), VU1 record and replay, and the EE speed in the
F3 overlay.

## Status

Every mode has been played through with scripted input: Tales of Souls to the credits and the
first save, Chronicles of the Sword (story, editor, briefing, field map, battles, results), World Competition, Soul
Arena, Museum, Character Creation, Shop and Options, with saves loaded back after a restart. Movies play with
sound and skip with Start.

Speed (Ryzen 7 7800X3D, 8 cores): Free Training on a fixed stage runs 298-300 GS frames per 5 s (60 fps) with the
EE at 100% of real time (~1474M cycles per 5 s) and no audio underruns.

Things that look wrong but are the game:

- the Tales of Souls credits run at ~15 fps: the EE runs at full speed with no GS waits, the game paces them;
- Soul Arena fighters tinted in cycling colours, or gold: the Soul Charge and Guard Impact effects;
- the game blends each frame 70/30 with the previous one, which shows as ghosting at low frame rates.

Open:

- The Chronicles field map and its battles dip to ~53-55 fps, with choppy sound when units move on the map.
- Heavy random stages of the attract demo run with the EE at ~89% of real time, bound by the VU1 worker.
- The Single Player menu preview panel shows noise (seen with `PS2X_SC3_NO_MOVIES`; previews are fine with
  movies on).
- The root cause of the ordering-table double link behind the old menu-transition freeze was never found: it no
  longer reproduces since catch-up is off and the GS/CD timing fixes, but other timings may bring it back.
- Not reached yet: widescreen.

The port runs on the native target the other ports use, through PS2Recomp's `sc3/native` integration branch: the
native GS renderer (`GSNativeBackend`, OpenGL) with the SC3 fixes for pixel centres and the 64-pixel seams, HIGHLIGHT
alpha, Ad blending, the Z buffer a draw's ZBUF names, and the character shadows (COLCLAMP off wraps blends, a colour
buffer read as Z keeps the Z block order, untextured draws at their own colour); the SPU2 on the wall clock, kept within 512
samples of the IOP clock; the eight IRX modules recompiled (813 functions, `scripts/iop_recomp.sh`). The regression
route runs at 58.2 fps, and at 58.1 fps with `PS2X_GS_SCALE=2` (1280x896).

## Debugging recipes

Paths below are relative to the repository; `scripts/run.sh` passes extra environment through.

- **State dump**: `PS2X_STATE_DUMP=5` prints every 5 s the EE and IOP threads, GS frames, EE cycles, DMA and VIF
  counters, VU1 cycles, host idle and worker waits, SPU2 state. Benchmark lines: GS frames per 5 s from
  `[state] gs frames`, EE speed from `grep -o '\[state\] ee cycle=[0-9]*'` (differences of ~1474M per 5 s = 100%).
  `[MC]` log lines lack a newline, so anchored `^\[state\]` greps drop some state lines.
- **Live input**: `PS2X_PAD_INPUT=<file>` presses the buttons of each line appended to the file while the game
  runs, one word every 0.35 s (`echo "down down cross" >> file`, `cross+up` presses both); `PS2X_PAD_SCRIPT=t:BUTTON[/ms],...` schedules presses in seconds from start. `PS2X_PAD2_SCRIPT` takes the same format for player 2 (second controller, SIO2 port 1). In zsh,
  write `"${t}:cross"`, not `"$t:cross"` (`:c` is a history modifier). Route to Free Training: Start x4 on the
  title, Cross (Single Player), Down x4 + Cross (Practice), Down + Cross (Free Training), Cross to pick the
  characters and stage (stage select defaults to Random; Right picks a fixed one).
- **Frame dumps**: `PS2X_FRAME_DUMP=<dir> PS2X_FRAME_DUMP_EVERY=<s>` writes the presented frame as raw 640x448
  RGBA (`magick -size 640x448 -depth 8 rgba:<file> out.png`). They fill a disk quickly and cost time themselves.
- **GS dumps**: `PS2X_GS_DUMP_WHEN=<file>` logs the next `PS2X_GS_DUMP_COUNT` primitives (with the VU1 program
  that kicked them) to `PS2X_GS_DUMP_OUT` once that file appears, `PS2X_GS_DUMP_AT=<s>` after a delay; on the CPU
  renderer `PS2X_GS_DUMP=<dir>` also writes decoded textures and VRAM. Comparing a dump of a broken scene with a
  good one found the missing Chronicles stage (VU1 programs and big textured triangles only in the good one).
- **Profiler**: `PS2X_PROFILE=<s>` (`PS2X_PROFILE_DELAY`, `PS2X_PROFILE_OUT`) samples every host thread on its CPU
  clock; `PS2X_EE_PROFILE=1` adds EE cycles by guest resume pc to the state dump (odd keys are timer-poll spins).
- **Watches and traces**: `PS2X_WATCH=addr[:n],...` and `PS2X_IOP_WATCH` print guest words in the state dump;
  `PS2X_CALL_TRACE=1` traces calls including indirect ones; the `PS2X_SC3_TRACE_*` tracers above.
- **VU1**: `PS2X_VU1_RECORD=<file>` records microprogram runs for `ps2_vu1_replay` (checks and times the
  interpreter offline); `PS2X_VU1_EXACT_FLAGS=1` keeps every flag and disables the compiled code.
- **Host backtraces**: `ptrace_scope=1` only allows attaching to children, so start the runner under gdb and
  interrupt it to sample all threads.
- `scripts/sc3run.sh <name> <seconds> [ENV=value...]` runs the game for that long with a state dump, logs to
  `build/runs/<name>.log` and screenshots the window (Hyprland + grim); `scripts/threadcpu.sh`, `gprof_*.py`,
  `dis.sh`, `irxdis.sh`, `irx*.py`, `string_refs.py` are disassembly and profiling helpers.
- `patches/async_vif1*.patch`: the first VIF1-worker experiments, superseded by the runtime's VIF1 worker; kept
  for reference.
