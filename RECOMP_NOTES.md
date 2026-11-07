# Recompilation notes

How *Soulcalibur III* (SLUS-21216) runs on BozoEngine: how the stripped ELF is bound, what SC3's port code answers
and why, how each piece was checked, and what is still open. Build steps are in the [README](README.md). The earlier
PS2Recomp build is summarised at the end.

Golden rule: no emulation. The build has no GS, VIF, VU, GIF, DMAC, IOP machine or SPU2 model, and no interpreter.
Textures and meshes that the disc holds only as ready-made GS and VIF data are converted offline from the player's
disc, never read from packets at run time.

## Binding the ELF

- `bozo-recomp` translates the whole `.text`: 16,656 functions, 578 jump tables (11,540 entries, as gotos), 0
  untranslated instructions. Three `jr` stay unresolved and go through the dispatcher: 0x429768 (a longjmp shape),
  0x42A0E8 and 0x42A248 (C++ exception landings).
- The ELF has no symbols. `recomp/sc3_symbols.txt` names its library functions from our own matcher, never from
  PS2Recomp's analyzer; `recomp/sc3_symbols_reviewed.txt` adds the names found by hand (0x420A00 `sceGsSyncVCallback`,
  0x426B28 `sceMcChdir`). Named functions BozoEngine knows are answered natively.
- `bozo-externals` writes the externals list from the ELF, the symbol map, `recomp/sc3_stubs.txt` and the port's
  overrides (`scripts/sc3_overrides_list.py` scans `bozo/port/*.cpp` for `registerFunction(0x...)`).
  `scripts/sc3_externals_all_transfers.py` turns every override into a `stub` kind, so a direct `j` to an override
  reaches it too (bozo-recomp sends a `j` to an `override` to the guest body by design; that had left 32,885 x 3
  device hits on the route).
- `recomp/sc3_patches.txt` (`bozo-recomp --patches`, guarded: the image in RAM is unchanged) turns the device polls
  left inside large guest functions into always-taken branches or calls: the VIF0/VIF1 error checks, CSR, IMR and
  PMODE stores, the 14 reachable `BC0T` interlocks, the waits 0x10F2F8 and 0x1259E8, and the timer reads of the main
  loop's neighbours (redirected to dead guest addresses 0x12D0D8, 0x12D0E8, 0x10E910, 0x10E920, which have no
  caller).
- `recomp/sc3_entries.txt` is empty: since BozoEngine #60 the analysis finds every entry the old hint lists named.

## The frame

- **Kick** (`sc3_frame.cpp`). 0x10F098 is C++: the kick's start state, the ordering-table walk, then the effects the
  FINISH handler had (0x46E5CE = 1, 0x46E60C += 1, 0x5BE782 = 1, 0x467494 = 0, the SIGNAL counters 0x5BE792/93 from
  the frame's counts). The game's skip route (0x10F248: over budget, or nothing to send) clears that frame's mirror
  without drawing. 0x10E4B8 and 0x10E5C8, which only linked DMA tags, are empty. SC3's DMA driver (0x10C890,
  0x10C9D0-0x10CED0, the scratchpad copies 0x10CF10, 0x10CFF8) is answered at its functions.
- **Ordering table** (`sc3_ot.cpp`). A native mirror of the game's table: GIF and VIF1 lists by segment, layer and
  bucket, published with `Renderer::endFrame`/`beginFrame` at the kick. 0x10EA38's two VIF1 lists are both kept (0
  before 1); ZBUF_1's ZMSK is an entry of the list, so depth writes follow the game.
- **Packet API** (`sc3_packet.cpp`). The begin 0x10F4B8 and the closes 0x10D9A0, 0x10DB88, 0x10DC90, 0x10E1F8 and CRI's
  copies 0x3AE058/0x3AE130 are plain memory copies with no channel register. Proved by a temporary differential build
  that ran guest body and C++ on the same state and compared all 32 MB of RAM and the 16 KB scratchpad: 0 differences
  over 2000+ calls of each.
- **Clock** (`sc3_clock.cpp`). The main loop's budget wait and frame code polled Timer0 and Timer3 (18.9 M and 2.6 M
  reads on the route). They now read lines and a frame clock from the EE clock, an unchanged count charging one
  period as the register model did. A differential build read every answer through the register too: 18,727,759
  clock answers and 2,624,695 lines on the route, 0 different. The vblank handler's FIELD read comes from RAM word
  0xFFFF8, written by its override.
- **Frame end** at 0x148098 (from 0x12EA90) and ADXM's sleep 0x3F0F10 (from 0x12E654) end a host frame; the window
  is paced to 60 by BozoEngine's `FramePacer`.
- **Device accesses.** `[sc3:acknowledged]` at a run's end counts every guest access to a device register (GIF/VIF
  status, IMR, SIGLBLID, BC0, channel registers, timers, IPU, D_ENABLE). Before the C++ kick the 3000-frame boot had
  17,488 GIF/VIF status, 5,499 IMR, 2,749 SIGLBLID, 1,572,939 BC0 and 2,526,966 scratchpad CHCR hits. Now it reads 0 on
  the route, the boot with movies, the route without movies and the nine modes, on Linux and Windows. BozoEngine's
  register stand-ins (#58, #59, #61) are no longer reached; #63's scratchpad copy is kept in the engine but SC3 no
  longer reaches it.

## Drawing

- **2D** (`sc3_2d.cpp`, `scripts/sc3_2d_pack.py`). The builders 0x1388E8, 0x124C80, 0x11EA80, 0x123340, 0x1223D0,
  0x1220C0, 0x1280F8, 0x12C690/0x12C7E0, 0x1134A0, 0x110E50, 0x1107E0/0x110258, 0x112148, 0x114B50 and the preview
  picture copies 0x217E28/0x118B68 are C++, emitting `Material` draws in screen space. Their logic comes from the
  PS2Recomp build, where a check mode compared them word for word with the game's own packets. 0x118020 takes a page's size and ST rows from the file in RAM. Screen objects with no file and no ST
  table (0x1107E0, Tales of Souls) are skipped as the game skips them.
- **Meshes** (`sc3_meshes.cpp`, `scripts/sc3_mesh_pack.py`). Model parts are drawn at the game's hooks 0x10A178,
  0x109990, 0x10A4E8, 0x10B970, 0x10B7E0, 0x14A7D8, 0x12FE98 from the mesh pack, with the vertex programs' routines in
  C++ (skinning, lights, fog, culling). Written: A 0xD8, 0x2B0, 0x450, 0x768, 0xA48, 0xC18, 0xF68, 0x1268, 0x1558,
  0x1730, 0x18C0, 0x1AB8, 0x1D28, 0x1F00 and setups A:0x10, A:0x1C80; B 0xD8, 0x350, 0x5B0, 0x910, 0xB78, 0x1388, 0x15E8,
  0x1A40, 0x1CA8, 0x1EF0 and setups B:0x10, B:0x1990. A:0x1268 and B:0xD8 are not yet run on a part. Not written,
  never reached in 302.2 M program calls recorded on the PS2Recomp build over every mode nor on any BozoEngine run:
  A:0xDA8, A:0x2098 (a setup) with A:0x2140 and A:0x2330, B:0xDD0, B:0x10F0, B:0x21E0 (a setup) with B:0x2290, B:0x24F8
  and B:0x2740. `[sc3:mesh] 250 frames: parts drawn by routine` gives the coverage of a run.
- **Textures** (`sc3_textures.cpp`, `sc3_texture_pack.cpp`, `scripts/sc3_texture_pack.py`). Each upload list of a
  model is played offline into a picture of GS memory and every texture a material selects becomes a record, keyed by
  the list's place in `ROOT.OLK` and the material's TEX0 (`.keys`). Stage materials carry the game's own mip levels
  (589 materials, MXL 1-3), sampled with the game's TEX1 (BozoEngine #71); `SC3_MIPMAPS=0` turns them off.
  Characters, weapons and menus are LCM 1, K 0, MXL 0: bilinear, as on the GS.
- **Pack misses.** A 2D page or mesh part the packs lack is built natively and its draw dropped (`[sc3:2d] ... pages
  dropped`, `[sc3:mesh] ... dropped for the packs`); the game's builders run only for structural refusals. So the game
  state does not depend on the packs: a headless route with older packs (7,397 2D pages dropped) and with the current
  ones (0 dropped) gives equal RAM, scratchpad, calls, threads, sound and EE cycles on all 11,700 frames.
- **Passes** (`sc3_offscreen.cpp`). Glow, shadow volumes and their darkening, the sun flare's depth probe (0x13BDF0
  from the renderer's `depthAt`), the frame reads at TBP 0 and 0x2C40, and the display copy: the game's last pass
  copies the frame through TFX Highlight2 with a colour 0x13D130 computes each frame (R x1.00, G x1.09, B x1.12, +6),
  which is the "teal lift" PCSX2 and the PS2Recomp build both show.
- **Materials** (`sc3_material.cpp`). The register values the builders compute become `Material` fields: blend from
  ALPHA, depth from TEST and ZBUF, alpha test, fog, colour mask, Q. No SC3 draw has a depth write without a test
  (counted: 0 of 2.1 M mesh draws in World Competition).

Checked against the PS2Recomp build (`reference/ab`): the route held at the same game frames on both builds, main's
still at n against ours at n-6..n+3, best offset kept. Fight stills: 20.42 to 27.71 dB, against main's own floor of
22.63 to 35.54 dB (two main runs). Contributors found: animation phase (the same tiles as the floor), the water's
scroll phase (8040 to 8360; main against itself 18.7 dB there), sub-pixel fighter edges. The 2D builders that read
the frame have no counterpart in three PCSX2 GS dumps (Tales of Souls character select, stage intro, stage select),
so they are drawn but unchecked.

## Movies

- `sc3_movies.cpp` answers Sofdec at the `mwPly*` level. `mwPlyStartAfs`/`AfsLp` on partition 1 start an FFmpeg
  decoder on `MOVIE.AFS`'s entry, read from the disc image. Full-screen pictures go to `Renderer::setBackdrop`;
  previews go to the 2D builders as RGBA pictures. `mwPlyCreate` 0x3AA698 and `mwPlyDestroy` 0x3AAC38 are C++ slots
  with no decoder, so Sofdec's SFD create path, Timer1 and the IPU are never reached (coverage of 0x3A0000-0x400000:
  826 generated functions reached before, 558 after).
- The clock is the game's frames: a picture every 1001/30000 s of EE time. With a host clock a stall left the
  soundtrack queued while the game moved on, which made the silence after file 4 vary from 3.4 to 4.6 s.
- Measured: file 4, 1051 pictures in 35.10 s (29.95/s), file 0 at 29.98/s. The silence between them is the game's
  own count (scene 0x31: 240 frames, then a 31-frame fade); 4.6 s here, 4.7 to 5.0 s on main.
- FFmpeg needs avformat (mpegps), swresample, swscale, the ADX decoder and the adx/ac3/mpegvideo raw demuxers and
  parsers: without them mpegps cannot probe an SFD's streams. Without FFmpeg every start reports its end.

## Sound

- CRI_ADXI, NUSOUND and SDRDRV are translated from the disc at build time by BozoEngine's `native_irx`
  (`cmake/sc3_audio.cmake`, `scripts/sc3_irx_images.py`) and started by `bozo/sound/sc3_audio_modules.cpp`. Their
  libsd calls go to BozoEngine's native libsd and mixer; there is no LIBSD image. CRI_ADXI is loaded with
  `thpri=39 spucore=2`.
- Fixed on the way (BozoEngine #68): the IOP step could run past an input half when a task's charge had already
  passed it, so one step of about 794 samples covered two halves with no handler. On the route it left 104 and 106
  stale runs of 280-285 samples (left, right); now 0.
- Against the PS2Recomp build, route with no one-shot movies, 11700 frames: 71.3 dB both, per 10 s level median
  0.64 dB, max 4.21 dB, cosine min 0.885, median 0.984. The large windows are alignment (one lag for the whole file).
  Boot with movies: 74.3 dB both, cosine 0.999.
- Not checked: playback on a real device. Test runs use SDL's dummy driver and measure the mixer's output (`--wav`).

## Checks and how they are run

- **Headless state.** `sc3-headless --out run.ref` writes BozoEngine's PS2XREF1 record (RAM, scratchpad, calls,
  threads, sound, EE cycles, draws per frame). Two runs of one binary give byte-identical records. Windows and Linux
  give equal records on every field, `draws` included once both link the same FFmpeg configuration (the system
  FFmpeg's x86 assembly rounds movie pictures differently from the kit's).
- **Reference.** `reference/` holds the hashes of the PS2Recomp build's deterministic recordings (boot, menus, fight)
  and the scripts that made and compare them. Game state was compared at five moments aligned by disc reads: the
  renderer's callback, OT group bitmap, VU batch counts and per-player words agree; the attract fighters differ by
  `rand`'s position, the low bits of some matrices by libm.
- **Routes.** The route (`reference/scripts/route_frames.txt`, 11700 frames) and the nine modes (7000 frames each from
  Mode Select) are checked for: no `FAULT`, `LOST 0` mesh parts, 0 dropped 2D pages, `[sc3:acknowledged]` 0, a clean
  exit. Exit loops (3 boots and a route) have 0 aborts since the shutdown order fix (players stopped before the
  renderer goes).
- **Windows.** A Windows 11 VM (4 cores, Mesa llvmpipe) ran the cross-built and the kit-built game: game state equal
  to Linux on every frame of the route and the boot, WAVs byte-identical headless, window sound against Linux max
  0.02 to 2.14 dB per 10 s window, stills within Linux's own floor, 12 of 12 exits clean.

## Open

- The fault at 0x117808 (`unhandled device read at 0xfffffffc`, frame 2680 of the route, after preview loops 8 and 9
  start): 4 of 15 runs early in the migration, none in the more than 100 runs logged since. In 0x1177E0 a file with
  entry count 0 and table +0xC 0 gives index -1; the caller is unknown (10+ callers). The next occurrence prints
  `[sc3:frame] 0x1177E0 file ... has no entries` with `ra`.
- The pause menu's fading "HIGH" label: the game pauses one step earlier than main (EE timing).
- `mwPlyPause` does not freeze the picture.
- A chain list missing from the mesh pack places one placeholder where a hit places one per batch: two mesh packs
  that disagree on a chain list would give different RAM. Not seen.
- Not done: a human play session, a real sound device, a real GPU and a gamepad on Windows, widescreen.

## The PS2Recomp build

From 2026-10 to 2026-11-06 the port ran on PS2Recomp (TheBozos fork, last pin 27ebdbc4; SC3 main `da8a761`):

- `recomp/sc3.toml` from `ps2_analyzer` (12,851 functions), a headless Ghidra map and hint lists for pointer-only
  entries; about 12,900 functions recompiled.
- The runtime's emulated EE devices, GS and IOP at first, then step by step: the eight IRX modules recompiled, the
  IOP and SPU2 on the wall clock, the GS drawn through an OpenGL back end, the VU1 and VU0 programs lifted to C++,
  their output drawn without GIF packets, then textures, meshes and the 2D from offline packs.
- Every mode played through with scripted input (Tales of Souls to the credits, Chronicles of the Sword battles,
  World Competition, Soul Arena, Museum, Character Creation, Shop, Options, saves loaded back after a restart), and
  long sessions by a human on 2026-11-01. Fights at 58-60 frames a second on a Ryzen 7 7800X3D.
- What carried over to BozoEngine: the pack converters, the 2D builders' logic, the mesh hooks and skinning, the
  texture keys, the Sofdec answers, the frame-end hooks, the route and its checks. What did not: everything that read
  packets, the GS back end, the VU sandbox and the fork's IOP.

Its code, notes and scripts remain in main's history at `da8a761`; `reference/` describes how its reference runs were
recorded.
