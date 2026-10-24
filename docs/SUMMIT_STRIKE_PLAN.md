# Ghost Recon 2: Summit Strike — porting plan (prepared 2026-10-23, not started)

Summit Strike (Xbox, 2005) is a standalone expansion on the same Stonewall
engine as Ghost Recon 2. The plan: a second project made from this one at tag
`v1.0`, on the same toolkit branch. Almost everything carries over; what
does not is anything tied to an address in GR2's XBE (a different build moves
every function and global).

## 0. Before starting

- The user's Summit Strike disc, extracted (XISO). Title ID and XDK version
  come from the XBE header (step 2).
- Toolkit: `ElKev118/xboxrecomp`, branch `gr2-port` (tag `gr2-v1.0` = the
  state GR2 v1.0 shipped with). Keep working on that branch or branch
  `ss-port` from it; fixes in the toolkit help both titles.
- GR2 stays buildable: regenerate GR2 only from its own XBE (see step 3 on
  the shared `game_files/`).

## 1. New project from the template

Copy `ghostrecon2` (at `v1.0`, without `build/`, `dist/`, `src/recomp/gen/`)
to `XboxRecompProject\summitstrike` and make it its own git repo
(`ElKev118/summitstrike-recomp`). Then change the title's identity -- best
done first as one header (`src/port/port_title.h`) that every file below
reads, so a third title is one file:

| What | Where (GR2 value) |
|---|---|
| Data folder `%LOCALAPPDATA%\<name>` | `src/port/gr2_paths.c` ("GhostRecon2"), `src/port/vk_glsl.c` (shader cache dir, twice) |
| SPIR-V cache salt | `src/port/vk_glsl.c` `SPV_CACHE_SALT` ("gr2-spv-1...") |
| Settings file name | `src/port/port_settings.c` ("gr2.ini") |
| Window title, thread name | `src/port/vk_window.c` ("Ghost Recon 2", "gr2-window") |
| Menu window title | `src/port/port_menu.cpp` ("Ghost Recon 2 - Port Options") |
| Error box text, entry point | `src/main.c` (`YOUR_GAME_ENTRY_POINT 0x0001DF71`, title id 0x55530005) |
| Old-save migration from `%LOCALAPPDATA%\xboxrecomp` | `gr2_paths.c` `save_dir()` -- drop it for SS (that folder was GR2's) |
| Exe / project name | `CMakeLists.txt` `project(ghostrecon2 ...)` |
| Release texts | `README.md`, `tools/package.ps1` (LEEME, zip name) |

Rename `gr2_*` symbols only where they are title hooks (`gr2_d3d.c`,
`gr2_set_frame_ms`); the generic port code can keep its names.

## 2. Identify the XBE

`python -m tools.xbe_parser <ss>\default.xbe` (toolkit): entry point, title
id, sections, kernel imports, XDK libraries -> write `docs/XBE_SUMMARY.md`
and `docs/KERNEL_IMPORTS.md` for SS as was done for GR2. Check the XDK
version first: if it is GR2's (5849) the D3D/DSound/XAPI library code is
near-identical and `tools.func_id` should recognise most of it; a different
XDK means re-checking the D3D hooks more carefully.

## 3. Regenerate

`tools/regen.sh` reads `xboxrecomp/game_files/default.xbe` and writes into
`../ghostrecon2/src/recomp/gen`. For SS: copy it to the new project, point it
at `game_files_ss/default.xbe` and `../summitstrike/src/recomp/gen`, and keep
the disasm/func_id/abi outputs per title (they are cached by XBE path).

Function lists (`seed_functions.json`, `trace_functions.json`,
`mmio_functions.json`, `manual_functions.json`) hold GR2 addresses: start SS
with empty lists, except `manual_functions.json`, which must name SS's
`D3D_BlockOnTime` once found (step 4). Grow the others the way GR2's grew
(seeds for code the disassembler misses; MMIO functions from the VEH log).

## 4. Title-specific addresses to re-find

All in `src/gr2_d3d.c` (GR2 values; same engine, so look for the same code
shapes):

| GR2 | What | How to find in SS |
|---|---|---|
| `0x0043A1B8` | D3D device pointer global | the most-read global in the D3D section |
| `0x0042F970` | `D3D_BlockOnTime(fence, flags)`, `ret 8` | the fence wait spinning on `*(device+0x30)`; toolkit `docs/technical/d3d8ltcg-device-context.md` |
| `0x0042F8B0` | `KickOff(flags)`, `ret 4` | called first by BlockOnTime when the fence is the newest |
| `0x0042F99F` | return address pushed for KickOff | the instruction after that call inside BlockOnTime |
| `0x0007E19D` / `0x57BF1C` | frame limiter and its float target (ms, 33.3) | the sleep-to-target loop in the main loop; the float read before it |

Not needed: `0x00A17000` in `vk_backend.c` (GR2's grade-table / shadow-map
address) only gates log lines; the lookup-table handling it relates to is
generic (dependent AR/GB reads).

## 5. First boot -- check these first (all were GR2 bugs, most fixed generically)

1. **XMV movies stutter / run at half speed**: vblank interrupt bits must be
   held until the DPC runs (toolkit, generic) -- if movies misbehave, check
   that first (memory note `xmv-vblank-root-cause`).
2. **"A problem has occurred" on loading a second mission**: contiguous
   memory reclaim (`RECOMP_HEAP_RECLAIM`, default on in `main.c`), heap best
   fit, `xbox_SetMapSize(128 MB)` -- all in the template.
3. **No menu music / starved streams**: `NtSetTimerEx` (toolkit, done).
4. **Mission load hangs with audio**: idle-voice round-robin (toolkit, done).
5. **Brief geometry glitches**: fence wait times out on no progress (2 s),
   in `gr2_d3d.c` -- port it with the new addresses.
6. **Cyan / teal colours**: fog colour byte order (generic, done); at high
   internal resolution, lookup tables kept at 1x (generic, done).
7. **Metallic rain / dense sounds**: HRTF on voices sharing a sample
   (`RECOMP_APU_HRTF=dense`, generic, done).

## 6. Same polish as GR2 v1.0 (comes for free with the template)

Options menu, gr2.ini-style settings, internal resolution 1x-6x, aspect,
SMAA / FSR 1 / RCAS / area downscale, shader cache, AppData folder, no
launcher needed, DebugKeys, packaging (`tools/package.ps1 -Zip`).

## 7. Still open for both titles

- Gamepad navigation of the options menu (SDL game controller -> ImGui).
- CPU performance: the game thread and the pushbuffer walk are both ~100 %
  at ~150 FPS (docs/VULKAN_PLAN.md, "Recompiler output"): x87/MMX as locals
  in the recompiler + a cheaper pushbuffer walk, together.
- Optional static CRT (/MT) to drop the VC++ redistributable requirement.
