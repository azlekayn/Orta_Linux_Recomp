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
- `RECOMP_GL_HIDE=1` — create the SDL window hidden (keeps automated runs off the
  desktop; GL still renders). `run_game.sh` sets it by default.
- `RECOMP_GL_NODEPTH`, `RECOMP_GL_NOCULL`, `RECOMP_GL_NOSCISSOR` — isolate state.
- `RECOMP_NO_COMBINERS` — force the simple `texel*diffuse` path.
- `RECOMP_VBLSTAT=1` — every 10 s, vblanks raised/claimed and title-event counts.
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
geometry, no `[PVIDEO]`, screen black. It is a **hang, not slowness**:
`RECOMP_STALL_EXIT_SECS` fired (exit code 3) after 180 s with no drawing, and
NV2A `PUT == GET` (the executor has nothing to do).

Evidence from a long run (240 s, scripted Start+A to the menu):
- GPU: `flips` climb slowly then stop mattering; `tris` stuck at ~585k;
  `PUT == GET` → the title submits no new work.
- gdb thread stacks during the stall:
  - main thread: `KeWaitForSingleObject` ← `sub_002ADCB0` ← `sub_00068C10` ←
    `sub_000AD510` ← … (the game's main init path waits on a kernel object).
  - threads 2 and 4: same `sub_002ADCB0` wait, from `sub_00234120` /
    `sub_00233CF0`.
  - thread 3: **busy** (no syscall) in `sub_002521F0` ← `sub_00245020` ←
    `sub_00245BD0` ← … (a decoder/loader — likely the resource or FMV path).
  - loader/ack threads idle in `nv2a_ack_thread` / `nv2a_flush_ack_thread`.
- Directory scans look correct (`[QDIR]` finds `rendersettingbinNN.rsb` then
  reports no-more). The game keeps re-opening via guest `0x0022E41E` (the
  resource/cache helper).
- `[UNIMPL] in al, dx` at `0x002B5A7D` repeats (a driver routine reading I/O
  port `0x80C0` then a vblank wait at `0x002B5AB0`: writes `edi+0x600100`
  = PCRTC_INTR_0 and spins while `edi+0x100` (PMC_INTR_0) bit 24 is set). It is
  emulated now (`recomp_manual.c:recomp_unimpl`, `RECOMP_PORT_IN`), but that did
  not unstick it. Note the vblank→ISR path does work early on
  (`[NV2A] vblank -> ISR claimed it`).

So the hang is a game-logic wait: the main thread blocks on a kernel object that
a loader/decoder thread never signals. It happens after difficulty select, at
the transition into the first stage / opening movie.

### Update (2026-10-30, later): it is the **FMV (MPEG-2)**, and it *is* skippable

Solved what the "black" frame actually is:

- The **waits are not a hang.** With `RECOMP_VBLSTAT=1`, vblanks keep firing and
  being claimed (600/10 s) and the three `sub_002ADCB0` waiters **cycle**
  (`title events: 600 sets, 1000+ waits` per 10 s). They wake every vblank and
  re-wait — normal D3D pacing. The frame loop keeps running; the screen is black
  because nothing is drawn, not because the game is stuck.
- The only genuinely stuck thread is the loader, pinned in a **tight loop**
  inside `sub_002521F0` (same host PC `0x14d3b2a` across samples). Call chain:
  `sub_002521F0` ← `sub_00245020` ← `sub_00245BD0` ← `sub_002475B0` ←
  `sub_00247600` ← `sub_0024E210` ← `sub_0024B110` ← `sub_0024B210` ←
  `sub_00248A30` ← `sub_00248B50` ← `sub_00236BC0` ← `sub_00233D60`.
  `sub_002521F0` is a **4-state `00 00 01` start-code scanner** (game_vtable
  category) — i.e. an MPEG-2 elementary-stream parser.
- The `[READ]` log settles it: the last reads are 1 MB chunks of
  **`00 00 01 BA`** (MPEG-2 pack header) into guest buffers `0x019CE600`,
  `0x01ACE600`, `0x01BCE600`, `0x01CCE600`, `0x01DCE600` (~5 MB), then stop. The
  scanner loop sits right on those buffers (`rdx≈0x019B111A`, end `≈0x019C8800`).
  So after difficulty select Orta plays an **FMV** and its own MPEG decoder loops.
- **The movie is skippable by the player.** Adding A/Start presses during the
  black phase (`RECOMP_AUTOPAD=...,a@3120,a@3250,start@3400,...`) carries the
  game past it: `tris` jumps from ~585k to **110 M+**, `surfaces` 2→4,
  `textures` 16→108 — the game starts rendering. So the *engine* is fine; only
  the FMV decode/render path is.
- After the skip the game settles on a **solid green (or cyan) rectangle** and
  freezes (`nonblack` constant for 150 s). That is the **video surface** drawn
  with an unsupported/unconverted format, not decoded video. `nv2a_gpu_gl.c`
  has a PVIDEO overlay path (`nv2a_exec_pvideo`, ~L217/L955) but it is not
  producing the FMV image here.

Next steps (video path):
1. **The movie is `game_files/movie/op_plus.sfd`** — 4,956,160 bytes, and the
   read total matches it exactly (4×1 MB + 761,856). It is an **MPEG-2 program
   stream**, byte-for-byte the same container as the working
   `opening_bs.sfd` (`00 00 01 BA` pack / `00 00 01 BB` system / `00 00 01 BE`
   PES). So the container is not the problem — the game's own decoder spins
   while parsing it.
2. `RECOMP_SKIP_SFD=1` (a diagnostic in `bridge_create_file_impl`) makes `.sfd`
   opens fail: the game then stalls *earlier* (flip ~2400, menu transition), so
   it does not gracefully skip the movie — do not use it as a workaround.
3. Pressing buttons during the black screen (autopad) *does* get past it (the
   player can skip the FMV), proving the engine is otherwise fine; the green
   fullscreen quad afterwards is a separate presentation issue.
4. Next: decide whether the decoder loop is a recompiler bug in the MPEG code
   (SSE2 IDCT / motion compensation) or genuinely waiting for more stream.
   The caller chain is `sub_00245BD0` → `sub_00245020` → `sub_002521F0`;
   `sub_00245BD0` calls the scanner with a **small stack buffer** (`esp+0x18`,
   length `0xCE`), so the spin is likely an **outer parse loop that never
   advances** rather than one huge scan — check the return value the caller uses
   to move through the stream.
   `[UNIMPL] stmxcsr` keeps being hit at guest `0x00269711` — check whether that
   no-op changes decoder results. A good probe: dump the decoder's output
   surface while it runs, and compare the scanner's consumed position over time.
5. Fix the **video presentation** (the solid green/cyan fullscreen quad) seen
   once the movie is skipped. No `[PVIDEO]` and no `[TEXFAIL]` are logged, so it
   is neither the overlay path nor an undecoded texture format — find what
   method draws the movie frame (likely a blit/scaler the executor ignores, or a
   CPU-written surface the GL backend does not re-upload at present).

### Update (2026-10-30, later still): the FMV decoder produces **no frames**

Ran the movie, unskipped, for **15 minutes** (`/tmp/long.txt`): `flips` climb to
26100, `tris` stays flat at ~627k, screen stays black. So it is not slowness.

What was ruled out:

- **Not an `-O3` miscompile.** Rebuilt `recomp_0052.c` (holds `sub_002521F0`) at
  `-O1` and relinked — still stalls. (The object is back at `-O3`.)
- **Not a stuck loop.** A breakpoint at the scan-loop top (`*0x14d3a70`) shows
  the pointer `edx` advancing `0x1970086, 0x1970087, …`. An earlier "frozen
  pointer" reading was a gdb artifact: registers do **not** refresh across
  `shell sleep` in one attach — use two separate attaches.
- **Not the NV2A video overlay.** `RECOMP_PVIDEO_DEBUG=1` (added to
  `nv2a_pb_exec.c:nv2a_exec_pvideo`) prints the PVIDEO registers ~2×/s; during
  the movie they are all zero (`700=0 704=0 928=FFFFFFFF 958=0 …`). The title
  never programs the overlay, so `nv2a_exec_pvideo` never returns a frame. There
  is also **no D3D8 overlay method** in the HLE vtable (`d3d8_device.c:1406`) —
  `d3d8_check_overlay()` is unrelated (a struct-alignment check).
- **Not the guest framebuffer.** Dumped guest phys `0xBA000` and `0x24000`
  (640×480×4) during the movie: pure random garbage (unwritten memory), identical
  across samples. The GPU presents via its own GL surface, so this is expected.
- **GPU is idle during the movie**: `[PB] 0 pushbuffer calls followed`, only
  2 clears/frame, no triangles. Audio does run (`[APU] peak 457`).
- **Platform gap found:** the port's two host-side display paths are both
  `#if defined(_WIN32)` — `video/video_pump.c` (Media Foundation, `.wmv` only,
  behind `RECOMP_FMV_HOST`, off by default) and `video/fb_present.c`
  (GDI framebuffer window, `RECOMP_FB_WINDOW`, off). So on Linux there is **no
  host-side movie playback**; only the title's own decoder. If Windows plays the
  `.sfd`, the difference must be in something the decoder's guest code depends on
  that the HLE provides differently.
- Candidate: **timing.** `xbox_KeQueryPerformanceCounter` maps straight to host
  `QueryPerformanceCounter`, which on POSIX is nanoseconds (`freq = 1e9`); on
  Windows it is ~1e7, and on real Xbox hardware it is 3,375,000 Hz. If the title
  hardcodes the Xbox rate instead of calling `KeQueryPerformanceFrequency`, its
  frame pacing math diverges. **Tried and it did not help:**
  `RECOMP_QPC_XBOX=1` (added in `win32_compat.c` — reports 3,375,000 Hz and
  scales the counter accordingly) still stalls at the movie unchanged.

- **The stall is a busy-wait.** Raising `RECOMP_KERNEL_LOG_BUDGET` shows the hot
  kernel calls during the movie: `NtWaitForSingleObjectEx` (ordinal 234) **59.5M
  calls, all returning `STATUS_TIMEOUT` (0x102)**, `RtlLowerChar` 24M,
  `NtSuspendThread` 6.5M, `RtlEnter/LeaveCriticalSection` ~0.7M each. The
  0-timeout wait poll is already annotated in `kernel_bridge.c:1948` as Orta's
  **sound threads** (`SwitchToThread()` added for it), so the 59M is expected
  noise, not the blocker. `NtSuspendThread` (6.5M) comes from `sub_0022D43C`
  called by the D3D worker family (`sub_00233Cxx`), and the bridge
  (`kernel_bridge.c:5150`) already special-cases **Orta's CRI movie server**
  parking itself. **Tried `RECOMP_NOSUSPEND=1`** (makes every suspend a no-op):
  still stalls. `RtlLowerChar` (24M) is `sub_0022EDFA`, a `strlwr`.

- What still is **not** explained: the decoder thread in `sub_002521F0` runs
  (pointer advances) yet no frame ever reaches the GPU (tris flat for 15 min).
  Either the decode genuinely never completes for this stream, or it completes
  into memory nothing displays.

Practical note: mashing buttons during the black screen **does** get past the FMV
(the engine then renders, `tris` 585k → 110M+), but it settles on a static
green/cyan quad — so even the skip path has a second bug after the movie.

### Update (2026-10-30, final): the decoder stalls on *every* movie, and the
### next state is a sleeping wait

- **ffmpeg decodes the movies fine** (`movie/op_plus.sfd`: MPEG-1, 640×368,
  ~30 fps, ~24 s; audio = CRI ADX). So the streams are valid.
- **Substituting a short clip for `op_plus.sfd` gets the game past that movie**
  — it then opens the *next* movie (`opening_bs.sfd` etc.) and stalls there. So
  the guest decoder stalls on **all** real Sofdec streams, not just one.
- **Shortening *every* `.sfd`** (a throwaway test, originals restored) still
  ends on a **static green rectangle** at ~flip 3000: a centred 16:9 quad.
  Green = the movie area painted with the decoder's **empty frame**
  (YUV(Y=0,U=0,V=0) → RGB(0,135,0)); the video texture never gets real pixels.
- At that green state **all guest threads are asleep** (the main thread in
  `sub_0022D2A6`, a `WaitForSingleObject` with timeout, under
  `sub_000121F0`/`sub_000A19F0`/…/`sub_000BD410`). So the game is *waiting on an
  event that never signals*, not spinning. The movie area is drawn each frame
  (a few hundred tris), the executor runs, but nothing advances it.

New gated diagnostics added this session (all off by default):
`RECOMP_TEXLOG` (distinct texture formats), `RECOMP_FILELOG` (every file open
with its path — this is how the movie layering was mapped), `RECOMP_PVIDEO_DEBUG`
(PVIDEO registers), `RECOMP_NOSUSPEND`, `RECOMP_QPC_XBOX`, `RECOMP_SKIP_SFD`.

**Where this leaves it:** the engine, GPU backend and input all work; two
guest-side things don't — the Softdec/CRI decoder produces no frames, and the
state after it sleeps forever on an event. Fixing the decoder (or making the
title's movie player see an immediately-finished stream) is the crux; a host-side
`.sfd` player alone won't help because the title itself waits on its own decoder.

Things to try next:
1. `RECOMP_VBLSTAT=1`: do vblanks keep being raised/claimed through the hang, or
   does the vblank thread stall too? (Records vblanks, PMC bit, title event
   sets/waits every 10 s.)
2. Identify `sub_002521F0` (the busy thread) and `sub_002ADCB0` (the wait) — dump
   the guest stack at the stall (watchdog) and read the recompiled C: which
   event does `sub_002ADCB0` wait on, and which thread/function signals it?
3. Check the loader override `recomp_manual.c:sub_00014D50` — it only drains
   `pending_list_0x3410A0`; if the hang is a load queued elsewhere, it never
   runs.
4. Watch files opened just before the hang and whether the title is trying to
   start the opening movie (`.sfd`); no `.sfd` open was logged, so it is likely
   still in resource/cache loading, not the movie.
5. `RECOMP_UNIMPL_TRAP=1` / `RECOMP_WATCHDOG_SECS=<n>` to stop/dump right at the
   hang rather than after 180 s.

Session note (2026-10-30):
- `RECOMP_VBLSTAT=1` samples show `vblanks 600 (claimed 600)` (60 Hz, ISR claims
  them) but `PMC 01000000` (PMC_INTR_0 bit 24 set) and `handled 0` (the title's
  vblank counter at `0x2C0430` is *not* advancing). So the vblank wait may be
  stuck because the title ISR is not actually acking/advancing.
- The PCRTC trap already gives `PCRTC_INTR_0` its write-1-to-clear meaning **and
  clears PMC bit 24 on the title's ack**
  (`xbox_memory_layout.c:pcrtc_write_veh`, ~line 1404). Clearing PMC bit 24
  unconditionally after `kernel_raise_interrupt` in `kernel_vblank_tick`
  **regressed the boot** (the game stalled at start-up), so do not do that — the
  ack has to come from the title's write.
- Added to the GL backend (this session): `RECOMP_GL_HIDE` (hidden SDL window for
  unattended runs) and re-upload of surfaces the GPU did not draw this frame
  (`surf_upload` when `touched_frame + 1 < frame`), so CPU-written frames (the
  FMV decoder) can appear.
- No desktop window: `run_game.sh` sets `RECOMP_GL_HIDE=1`. An `Xvfb` display
  would be cleaner still if available.

Update (2026-10-30, later):
- Runs now use **Xvfb** (`Xvfb :99 -screen 0 1280x960x24`, llvmpipe GL 4.6 core);
  set `DISPLAY=:99`. No desktop window. (The on-screen window under llvmpipe
  comes back black; the **surface dump is the reliable signal**.)
- The main thread's hang is a **`KeWaitForSingleObject`** on
  **`*(0x2BD708) + 0x2554`** (waitreason 6, KernelMode, INFINITE) — see
  `sub_002ADCB0` in `orta/src/recomp/gen/recomp_0066.c`. `0x2BD708` is the D3D
  device pointer (`RECOMP_SEMA_DEVPTR`); the object is a D3D device event.
- The runtime signals the D3D fence-interrupt event at **`dev + 0x2564`** by
  default (`nv2a_pb_exec.c` ~L4463, `method==0x0100&&param`, overridable with
  `RECOMP_GPU_INTR_EVENT`). Orta waits at **`+0x2554`** here — a 0x10 mismatch.
  **But** overriding `RECOMP_GPU_INTR_EVENT=0x2554` stalled the *boot* (the NOP
  interrupt then set `dev+0x2554` = `0x2C0404` instead of the default
  `0x2C0414`), so the title uses more than one device event and a blind offset
  swap is wrong. Determine which event each wait is on, and signal the matching
  one (possibly signal both, or find the right device base).
- The `KeSetEvent` used by the vblank routine is thunk `0x30C9E4`
  (ordinal 145); the wait is thunk `0x30C9E8` (ordinal 159).

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
