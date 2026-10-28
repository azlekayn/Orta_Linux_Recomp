# Orta PC Recomp — Linux port handoff

State as of 2026-10-28. Read this first, then `docs/LINUX_PORT_STATUS.md` and
`LINUX_PORT_SCOPE.md` (the original plan).

## TL;DR

The game **builds, boots, plays audio, and renders** on Linux via a new
OpenGL GPU backend. The **title screen** and **difficulty menu** render
correctly. Selecting a difficulty then **stalls** (black frame, geometry stops)
— that is the main open problem.

Everything below is under `/mnt/SSD Storage/Games/Ports/xbox/`. The disc image
is `Panzer Dragoon Orta (USA).iso` (workspace root); the project is
`Orta_PC_Recomp-0.5.1_Beta/`.

## What works

- Toolchain on Linux: `python3 -m tools.xbe_parser / tools.disasm / tools.func_id
  / tools.abi_analysis / tools.recomp` (17,831 functions, 0 failed). Capstone
  5.x is installed.
- Builds: engine libs + `orta/build64/orta_recomp` (GCC 16).
- Runs: XBE loads, 64 MB Xbox memory maps, kernel bridge + thunks, APU with
  **SDL audio (PipeWire)**, GL 4.6 core context, and the game draws.
- Renders: title screen (logo, PRESS START BUTTON, SEGA/copyright, background)
  and the difficulty menu (EASY/NORMAL/HARD + tooltip), upright, ~55 fps.

## How to build and run (this machine)

### SDL2 CMake package
There is no system SDL2 dev package. A usable one lives in a container overlay;
it was wired up with a small CMake config:

```sh
OVL=/home/workwing/.local/share/containers/storage/overlay/0878fe391723960af59b0ceb93912d5fcd3052b1b34a4adbe4c517dda2766705/diff/usr
mkdir -p /tmp/sdl2real
cat > /tmp/sdl2real/SDL2Config.cmake <<EOF
add_library(SDL2::SDL2 SHARED IMPORTED)
set_target_properties(SDL2::SDL2 PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "$OVL/include/SDL2"
  IMPORTED_LOCATION "/lib64/libSDL2-2.0.so.0")
set(SDL2_FOUND TRUE)
set(SDL2_INCLUDE_DIRS "$OVL/include/SDL2")
EOF
```
(`/lib64/libSDL2-2.0.so.0` is the system runtime; the overlay has the headers.)

### Configure + build
```sh
cd /mnt/SSD\ Storage/Games/Ports/xbox/Orta_PC_Recomp-0.5.1_Beta
cmake -S orta -B orta/build64 -DCMAKE_BUILD_TYPE=Release -DSDL2_DIR=/tmp/sdl2real
cmake --build orta/build64 -j4
```

### Recreate generated code (only if needed)
The ISO was unpacked into `xboxrecomp-main/game_files/` (2,453 files, 2.9 GB).
`default.xbe` and the seeds are there. To regenerate the recompiled C:
```sh
cd xboxrecomp-main
python3 -m tools.xbe_parser game_files/default.xbe --json game_files/default_analysis.json
export XBOXRECOMP_ALIAS_SEEDS="$PWD/game_files/alias_seeds.json"
python3 -m tools.disasm    game_files/default.xbe --text-only \
  --extra-sections 'MMATRIX,D3D,D3DX,DSOUND,WMADEC,PSGSFD00,PSGSFD_I,PSGSFD_B,PSGSFD_P,XGRPH,XPP' \
  --seed-functions game_files/seeds_manual.json --force
python3 -m tools.func_id    game_files/default.xbe
python3 -m tools.abi_analysis game_files/default.xbe
python3 -m tools.recomp     game_files/default.xbe --all --split 250 \
  --gen-dir ../orta/src/recomp/gen --exclude-manual ../orta/src/recomp_manual.c
```

### Run
`run_game.sh` (copy saved next to this file) sets the engine switches and runs
`orta/build64/orta_recomp` on `DISPLAY=:0` with a bounded timeout. The game
looks for `game_files/` next to the exe or in the CWD; `orta/game_files` and
`orta/build64/game_files` are symlinks to `xboxrecomp-main/game_files`.

```sh
bash run_game.sh                     # title screen; no input
RECOMP_AUTOPAD='start@1120,a@1400,a@1700,start@2100,start@2600,a@3000' \
  bash run_game.sh                   # scripted Start+A to the menu + beyond
```

## Debug tooling built in (all env-gated)

- `RECOMP_GL_DUMP=/path.bmp` + `RECOMP_GL_DUMP_FLIP=<flip>` — dump the presented
  surface to a BMP at that flip (convert with `ffmpeg -i x.bmp x.png`).
- `RECOMP_GL_NODEPTH`, `RECOMP_GL_NOCULL`, `RECOMP_GL_NOSCISSOR` — isolate state.
- `RECOMP_NO_COMBINERS` — force the simple `texel*diffuse` path.
- `RECOMP_PORT_IN=<n>` — value returned by the `in al, dx` emulation.
- `RECOMP_UNIMPL_TRAP=1` — abort at the first untranslated instruction.
- `[GPU-GL]` stderr lines: flips/tris/clears/surfaces/textures/present, and the
  first few draws with their first vertex.
- gdb: `SDL`/GL run under `gdb -ex run -ex bt` works (it stops on handled
  APU-traps' SIGSEGV too; `handle SIGSEGV nostop noprint pass` to skip them).
- Screenshots: `ffmpeg -f x11grab -i :0 -frames:v 1 out.png` **comes back black
  here** (the GL window isn't in the X root grab), so prefer the surface dump;
  the live window is what a human sees.

## Architecture notes (important)

- The live rendering path is **not** `d3d8_gl.c`. The push-buffer executor
  `src/kernel/nv2a_pb_exec.c` is a CPU software rasteriser; its hardware path is
  the D3D11 file `src/kernel/nv2a_gpu.c`. On Linux the stubs there were replaced
  with a new **`src/kernel/nv2a_gpu_gl.c`** (added to `src/kernel/CMakeLists.txt`
  on non-Windows, links SDL2 + epoxy). `d3d8_gl.c` is only reached by the menu
  replay + FMV player.
- The executor hands the GPU **screen-space** vertices: x,y in guest pixels, z in
  24-bit depth units (the "screen-space epilogue"). The shader maps x,y → NDC and
  z → clip depth. Do not pass `pos` through raw (z≈2²⁴ clips everything).
- Guest framebuffer is top-down; GL FBO is bottom-up. Present blit samples
  `v_uv = a_p*0.5+0.5` (guest-top at v=1). Write-back reads GL row `h-1-y` into
  guest row `y`. The BMP dump writes GL row `y` to BMP row `y`.
- `depth_func`/alpha func are NV2A values `0x200..0x207`, compare is
  `0x201=LESS .. 0x207=ALWAYS` after subtracting `0x200`.

## Files changed / added (this work)

Engine (`xboxrecomp-main/`):
- `src/platform/xbox_winnt.h` — `NTAPI`; POSIX `PCONTEXT` is a 64-bit struct.
- `src/platform/win32_compat.{h,c}` — real VEH via `sigaction`; missing
  primitives; **`GetExitCodeThread` pseudo-handle fix**; `ExitThread` guard.
- `src/apu/apu_mmio_hook.c`, `src/nv2a/nv2a_mmio_hook.c` — decoders shared with
  POSIX.
- `src/apu/apu_sdl_audio.c` (**new**) + `src/apu/CMakeLists.txt` — SDL2 waveOut.
- `src/kernel/nv2a_gpu_gl.c` (**new**) + `src/kernel/CMakeLists.txt` — the GL
  GPU backend (the big one).
- `src/kernel/nv2a_gpu.c` — POSIX branch is now just `g_water_mode`.
- `src/kernel/kernel_path.c` — partition images, case-insensitive lookup,
  `xbox_path_make_parent`.
- `src/kernel/kernel_file.c` — create a cache file's parent dir.
- `src/kernel/kernel_thunks.c`, `src/kernel/kernel_bridge.c`,
  `src/kernel/recomp_prof.c`, `src/kernel/xbox_memory_layout.c` — POSIX globals,
  rename, `isatty`, watchdog wait-dump, thread-local.
- `src/d3d/d3d8_gl.c` — surface layer (LockRect etc.), overlay hooks, present.

Orta (`orta/`):
- `CMakeLists.txt` — platform guards; GCC forced include; `_GNU_SOURCE`.
- `src/main.c`, `src/launcher.c`, `src/recomp_manual.c` — POSIX ports + SDL pad.
- `src/overlay_gl.cpp` (**new**) + `third_party/imgui/imgui_impl_opengl3.*` — F1
  overlay for GL (not on the critical path; the game uses the GPU backend's own
  window).
- `orta_loop.sh`, `orta_package.sh` — bash pipeline.

## The blocker: stall after the difficulty menu

Symptom: after the scripted menu selection, frames take hundreds of ms to
seconds (`[HITCH] frame N took 3416.8 ms ... idle`), `tris` flatlines, no
geometry, no `[PVIDEO]`, screen black.

Trace: the game loops in a driver function; the last stderr lines are repeated
`[UNIMPL] untranslated instruction REACHED: 'in al, dx' at 0x002B5A7D`
(recompiled at `orta/src/recomp/gen/recomp_0067.c`). Reading that code:

- `0x002B5A79`: `edx=0x80C0; in al, dx` — reads I/O port **0x80C0** (emulated in
  `orta/src/recomp_manual.c:recomp_unimpl`, value via `RECOMP_PORT_IN`, default 0).
- `0x002B5AB0`: `MEM32(edi+0x600100) = ecx;` then spins while
  `MEM32(edi+0x100) & 0x1000000` is set. `0x600100` is PCRTC_INTR_0, `0x100` is
  PMC_INTR_0, bit 24 is the PCRTC interrupt. This is a **vblank wait**.
- So the stall is in the vblank/interrupt modelling (`RECOMP_VBLANK`,
  `RECOMP_PCRTC_TRAP`) or in how the opening movie (Sofdec, game-decoded) is
  presented. The port-I/O emulation alone did not unstick it.

Things to try next:
1. Determine whether the loop is the `loc_002B5AB0` vblank spin (bit 24 stuck
   set) or the caller's loop. `RECOMP_UNIMPL_TRAP=1` stops at the first `in`, not
   the spin; add a counter/log at the spin or use gdb (`break` on the generated
   function, or interrupt and `bt`) during the stall.
2. Check the vblank path: `RECOMP_VBLANK=1`; does the runtime set PMC_INTR_0 bit
   24 each vblank, and does the title's ISR (or our ack thread) clear it? The
   `NV2A: PCRTC_INTR is write-1-to-clear` trap is on (`RECOMP_PCRTC_TRAP=1`).
3. Watch which files the game opens right before the stall (the log's `[OPEN]`
   lines) — is it opening an `.sfd` movie? If so, the movie is decoded by the
   game's own `PSGSFD*`/`WMADEC` code; work out how it presents it (PVIDEO
   overlay vs. a textured quad). The GL backend now composites the overlay
   (`nv2a_exec_pvideo`) but nothing activates it here.

## Other open issues

- **Cyan square artifacts** on the title (a texture/combiner-stage fidelity
  issue); `RECOMP_NO_COMBINERS` did not remove them.
- **GPU vertex programs**: `nv2a_gpu_vsh_enabled()` returns 0, so the executor's
  CPU interpreter runs them. The D3D11 backend runs them on the GPU; that HLSL
  VSH would need a GLSL port for parity/perf.
- Mipmaps, cube textures, bump-env stages, polygon offset, occlusion queries,
  the depth-range drop (`g_zc.z` is forced off).
- Blend factors: `blend_of` maps 0..9; verify the executor's encoding
  (`st->blend_sfactor`) — may need NV2A→GL mapping like `depth_func` did.

## Longer term / nice to have

- **Move the GPU backend from OpenGL to Vulkan.** `nv2a_gpu_gl.c` is a first-cut
  OpenGL 3.3 backend; a Vulkan backend would be the more future-proof target
  (and is the direction modern emulators take). Not urgent — OpenGL works for
  bring-up — but worth planning: the backend is deliberately isolated behind the
  `nv2a_gpu.h` interface (`nv2a_gpu_enabled/begin/tri/clear/flip/sync/...`), so a
  `nv2a_gpu_vk.c` could replace `nv2a_gpu_gl.c` on POSIX without touching the
  executor. Expect the real work to be shader translation (GLSL → SPIR-V), the
  per-surface render targets / depth, the texture upload path, and present.
- `d3d8_gl.c` could eventually share the Vulkan backend too, but it is not on
  the game's live path.

## Where things stand in the status doc

`docs/LINUX_PORT_STATUS.md` records: Phase 0/1/2 done, the GL backend, the
title/menu success, and the post-menu stall. Update it as you go.
