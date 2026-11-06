# SC3 reference records

What SC3 on BozoEngine is compared with: runs of the current SC3 main build (the PS2Recomp path), recorded with BozoEngine's
formats, `PS2XREF1` (`docs/specs/harness.md`, 80 bytes per game frame) and `PS2XCLG1` (`docs/specs/calllog.md`). `records/`
holds hashes and counts only. The call logs hold game memory: they stay local, at `~/recomp/sc3/bozo-b1/runs/` on the
recording machine, and are identified below by SHA-256.

Recorded 2026-11-06, each from a fresh user folder with blank memory cards.

| Record | Env (`scripts/`) | Frames | Route | Log records | Calls | Log bytes | Log SHA-256 |
|---|---|---|---|---|---|---|---|
| `boot.ref` | `ref_boot.env` | 3000 | No input: boot and title | 20,239,184 | 18,235,708 | 856,287,407 | `2a1b2df038a1bc00b84b973a16440ca23064345fed23f16de1f9bdc62399f365` |
| `menus.ref` | `ref_menus.env` | 7740 | `route_frames.txt` up to the fight's start | 60,955,107 | 52,675,792 | 2,488,938,677 | `585c7af293a684df79e8bbcc127eea383a598acc9d9c1be50e68b74deef701d5` |
| `fight.ref` | `ref_fight.env` | 11700 | The route: title, Mode Select, Single Player, Practice, Free Training, 60 s of attacks, pause | 107,871,653 | 93,940,652 | 5,121,637,383 | `8799bcb60e3ff55142747e060f080bf62ea922436fdb1d088ca0169037e5fa72` |

The ELF's XXH64 in every record is `60984b458cf23f17`. `route_frames.txt` is `scripts/regress.sh`'s route with each of its
waits taken as 60 game frames.

## The build

- SC3: this branch's `runtime/`, `CMakeLists.txt` and `recomp/sc3.toml` (main da8a761 plus `PS2X_DETERMINISTIC` and
  `SC3_CALL_NAMES` in `runtime/sc3_overrides.cpp`, both inert without their variables).
- PS2Recomp: main's pin 27ebdbc4 plus branch `sc3/harness` (bc47c970, local to the recording machine so far): the fork's
  `harness/calllog` commits picked onto the pin, and Linux write tracking for the capture.
- Generated code: `ps2_recomp` of that branch on `sc3.toml` with `capture_hooks = true` added to `[general]`; VU code from
  `ps2_vu_aot` and `vu1_lift.py` (identical to main's); `generated/iop` as main's.
- Clang 23.1.3, `RelWithDebInfo` flags `-O2 -DNDEBUG -g0 -msse4.2`.
- Packs: `sc3_textures.pack` and `sc3_2d.pack` as main builds them. No mesh pack.
- `SC3_CALL_NAMES`: `scripts/sc3_call_names.py`. The recorder's named calls are the addresses main answers as stubs, with the
  names of our own `recomp/sc3_symbols.txt` (`sub_<address>` where the map has none). `sc3.toml`'s names are not used.

## Determinism

Main's default run is not deterministic. These settings make it so, and each one is a difference from what a player runs:

| Setting | What it replaces |
|---|---|
| `PS2X_DETERMINISTIC=1` | Host clock in the frame step, EE, IOP (`PS2X_IOP_WALLCLOCK`) and SPU2 (`PS2X_SPU2_WALLCLOCK`); host dates; host pads |
| `PS2X_GS_SYNC=1` | The GS worker thread, whose timing placed the GS interrupt at a different record from run to run |
| `PS2X_VIF1_THREAD=0` | VIF1 and VU1 on their own thread, whose timing changed the path through the frame kick 0x10F098 |
| `PS2X_SC3_NO_MOVIES=1` | One-shot movies, which `SfdPlayer` plays on a decoder thread and the audio device's clock |

Checked: `boot` and `fight` were each recorded twice, under load and at the same time as other runs. Both pairs are identical on
every field for every frame, and their logs differ only in the run name. `menus.ref` equals the first 7740 records of
`fight.ref`. `boot` and `fight` first differ at frame 1560, the route's first press. `calllog_read.py check` on `boot.clg`:
0 errors.

What these records do not cover:

- **Pictures.** With `PS2X_GS_SYNC=1` the window stays blank, so `drawBatches` and `drawVertices` are 0 on every frame and
  `draws` says nothing about the picture. The route was checked by eye in a separate run without `PS2X_GS_SYNC` (`routecheck.env`), which reached
  Free Training and the attacks. Its record is not kept: that run is not deterministic.
- **Timing against a player's run.** EE cycles and the order of GS handshakes are this mode's, not those of a default run.
- **One-shot movies.** Compare those with pictures and sound. Menu preview movies still start here (`mwPlyStart` files 8-13);
  both `fight` recordings agree across them, but their decoder thread still runs on host time, so a mismatch at a preview is
  the first place to look.
- **Native audio.** `PS2X_NATIVE_AUDIO=1` is not used. The `sound` field is the emulated SPU2's mix on the IOP clock.
- **The fork's self-checks.** `calllog.md` section 9's strict, substitution and replay checks were not run on these logs.
- **Other builds.** The hashes assume this build: another fork commit or changed overrides in `runtime/` can change them.

## Recording

    python3 scripts/sc3_call_names.py recomp/sc3.toml recomp/sc3_symbols.txt <names.txt>
    SC3_REF_RUNNER=<ps2EntryRunner> SC3_ELF=<SLUS_212.16> SC3_REF_PACKS=<packs> SC3_REF_OUT=<out> SC3_REF_CALL_NAMES=<names.txt> \
      reference/scripts/run.sh boot 2400 ref_boot.env

Compare with BozoEngine's `tools/capture/ref_compare.py` and `calllog_read.py <log> compare <other>`.
