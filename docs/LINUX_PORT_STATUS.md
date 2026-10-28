# Orta PC Recomp — Linux port status

Companion to [LINUX_PORT_SCOPE.md](../LINUX_PORT_SCOPE.md), which is the
plan. This file records what was actually changed and what is still missing.

## TL;DR

The port is implemented: the engine builds on Linux, the Orta host program
(`orta/`) configures and compiles, and there is a bash build pipeline. What
cannot be claimed as "done" without a legally-owned disc image and a
run is the Phase 0 runtime de-risking (does Orta hit FMV/WMA, does the
OpenGL backend draw correctly).

Audio output over SDL2 and `IDirect3DSurface8` locking are implemented (see
below). See **Remaining gaps** for what is still missing.

## Runtime status — with the disc image (2026-10-27)

The port was run against `Panzer Dragoon Orta (USA).iso`. The whole pipeline
works on Linux and the game boots:

- `tools.xbe_parser` → `tools.disasm` → `tools.func_id` → `tools.abi_analysis`
  → `tools.recomp` all run under `python3` (17,831 functions, 0 failed).
- The executable builds and runs; the XBE loads, the 64 MB Xbox memory layout
  maps, the kernel bridge resolves its thunks, and the APU comes up with real
  **SDL audio output** (PipeWire).
- Several POSIX gaps that stopped the game before it drew anything were found
  and fixed (see below): the `\Device\Harddisk0\Partition0` raw disk image,
  case-insensitive path resolution, and creating a cache file's parent
  directory.

**The blocker for an on-screen game is the rendering path, and the scope's
premise was wrong.** The live pushbuffer executor is
`xboxrecomp-main/src/kernel/nv2a_pb_exec.c`, a **CPU software rasteriser**.
Its hardware path is `nv2a_gpu.c` (a `.c` with a `_WIN32` D3D11 branch and a
POSIX stub returning `nv2a_gpu_enabled() == 0`). On Linux:

- `nv2a_gpu_enabled()` is 0, so the game's 3D is rasterised on the CPU with
  the register-combiner evaluator (`nv2a_rc_eval`). It makes progress but is
  very slow, and it only draws clears and already-screen-space flat geometry;
  vertex programs, texturing and depth are counted and skipped
  (`nv2a_pb_exec.c` says so: "the upgrade path is the D3D11 translator in
  src/nv2a/nv2a_pgraph_d3d11.c").
- `d3d8_gl.c` (the OpenGL backend) is **not on the live path**. It is reached
  only through `nv2a_pgraph_d3d11_method()`, which is called solely by the
  menu-replay harness (`nv2a_pb_replay.c`) and the FMV host player
  (`video_pump.c`). Orta's D3D8 is statically linked in the XBE and talks to
  NV2A directly — the XBE imports no D3D8 from the kernel — so the runtime's
  D3D8 layer is never created for this title on either platform. On Windows
  the game renders through `nv2a_gpu.c`'s D3D11 branch.
- There is also **no presentation window on Linux**: `fb_present.c`'s POSIX
  branch is stubs (the window there is GDI), and the GL window in
  `d3d8_gl.c` is only created on `xbox_Direct3DCreate8`, which never happens
  for this title.

**So the remaining work to see Orta on screen is an OpenGL implementation of
`nv2a_gpu.c`** (mirroring its D3D11 branch). That is a large, self-contained
piece of renderer work — the bulk of what a "Linux GPU backend" for this title
actually needs — and it is not the already-finished `d3d8_gl.c` the scope
assumed.

### OpenGL GPU back end (stage 1) — implemented, in progress

`xboxrecomp-main/src/kernel/nv2a_gpu_gl.c` is a POSIX OpenGL 3.3 back end for
the push-buffer executor, the sibling of `nv2a_gpu.c`'s D3D11 branch:

- SDL window + GL 3.3 core context, created on `RECOMP_GPU=1`.
- One GL render target (colour texture + depth/stencil renderbuffer) per guest
  colour surface, keyed by `(base, pitch, bpp)`; surfaces uploaded from guest
  memory on first use and written back only over the GPU-drawn sub-rect.
- Texture upload via `nv2a_exec_tex_decode` into `GL_TEXTURE_2D_ARRAY` levels.
- An uber fragment shader porting `nv2a_combiner.c`'s register-combiner
  evaluation (the same ICW/OCW/input-mapping/final-combiner rules as the D3D11
  HLSL), plus texture fetch, alpha test and fog; modes 0/1/2.
- Blending, depth test/mask/func, alpha test, cull, scissor from the batch
  state; present blit to the window.
- **Vertex programs still run in the executor's interpreter**
  (`nv2a_gpu_vsh_enabled()` returns 0), exactly the D3D11 branch's "Stage 1".

It initialises and runs (Mesa reports `GL 4.6 (Core Profile)`), draws the game's
geometry, and **renders the game on Linux**: the Panzer Dragoon Orta title
screen (logo, "PRESS START BUTTON", SEGA/copyright text, background) and the
difficulty-select menu (EASY/NORMAL/HARD + tooltip), upright, with depth
testing on and at ~55 fps. The counterexample bugs found and fixed on the way
here:

- The runtime's `GetExitCodeThread` did not resolve the pseudo current-thread
  handle (`-2`), so the warp-path dereferenced it and crashed the game; fixed in
  `win32_compat.c`.
- The executor hands the GPU **screen-space** vertices (x,y in guest pixels, z
  in 24-bit depth units), so the shader maps them to NDC and converts the depth
  value; a raw pass-through clipped every triangle (`z` ≈ 2²⁴).
- `depth_func` is an NV2A value (`0x200`–`0x207`), not a D3D `D3DCMP`, so the
  old mapping returned `GL_NEVER` and discarded every fragment with the depth
  test on.

After the difficulty menu the frame goes **black and the game stalls**
(~3.4 s frames, geometry stops, no `[PVIDEO]` overlay is produced) — the
opening FMV / intro path. The GL backend now composites the NV2A video overlay
(`nv2a_exec_pvideo`) when it is active, but nothing activates it here, so the
next step is to work out how Orta draws its opening movie (its own Sofdec
decoder) and whether it needs the overlay, GPU vertex programs, or a streaming
fix.

Also still to do: the odd cyan blobs/squares (a texture/combiner-stage
fidelity issue), GPU vertex programs (`vsh_enabled` is still 0, so the CPU
interpreter runs them), mipmaps, cube textures, bump-env stages, polygon
offset and occlusion queries.

What runs today: boot, kernel HLE, file/path HLE, APU audio, and an OpenGL
rasterisation path that is close to displaying the game but crashes during
start-up. What is missing: making that path correct and stable, then the
remaining shader/state features.

## Phase 0 — findings

De-risking before the port, done against the tree as shipped:

- **The engine did not build on Linux.** `xboxrecomp-main` had a POSIX
  branch that did not compile. The following were fixed (all are generic
  Win32-on-POSIX gaps, not Orta-specific):
  - `win32_compat.{h,c}`: added `InterlockedAnd/Or/Add`, `YieldProcessor`,
    `GetThreadId`, `SetThreadStackGuarantee`, `GetModuleHandleA`,
    `GetProcAddress`.
  - `xbox_winnt.h`: added `NTAPI`; gave POSIX a real 64-bit `PCONTEXT`.
  - `kernel_thunks.c`: `recomp_diag_quiet()` used `GetStdHandle`/`GetFileType`;
    now `isatty(STDERR_FILENO)` on POSIX.
  - `kernel_bridge.c`: `__declspec(thread)` → `XBOX_THREAD_LOCAL`; the
    `FileRenameInformation` path was Windows-API-only, now has a POSIX
    `rename()`/copy implementation using the open handle's host path.
  - `apu_mmio_hook.c` / `nv2a_mmio_hook.c`: the x86-64 MMIO decoders were
    `#if defined(_WIN32)` only, so APU/GPU register traps were dead on Linux.
    They are now shared: the POSIX sigaction dispatcher (below) presents the
    same register-context layout the decoders already speak.
  - `win32_compat.c`: `AddVectoredExceptionHandler` was a stub that never
    fired. It is now a real VEH chain driven by `sigaction`:
    `SIGSEGV`/`SIGBUS` → `EXCEPTION_ACCESS_VIOLATION`, `SIGTRAP` →
    `EXCEPTION_SINGLE_STEP` (which the AC'97 write trap needs), with the
    ucontext register file mapped to `PCONTEXT`. This makes
    `RECOMP_AC97_READY` (APU traps) and `RECOMP_PCRTC_TRAP` (PCRTC write
    trap) work on Linux.

  After these, `cmake -S xboxrecomp-main && cmake --build` succeeds on Linux
  (verified with GCC 16 on Fedora).

- **FMV / WMA.** `video_player.c`'s non-`_WIN32` branch and `wma_decoder.c`'s
  non-`_WIN32` branch are still stubs returning failure. Whether Orta reaches
  them at runtime could not be determined without the disc image, so Phase 3
  remains conditional. Nothing was implemented for it.

- **SURFACE8 locking.** Resolved in the GL backend. `d3d8_gl.c` now has a
  real `IDirect3DSurface8` implementation: `LockRect`/`UnlockRect` for a
  texture mip level (palette / procedural write-back), an offscreen render
  target (readback through its FBO), an image surface, and the back buffer
  (readback from the scene target — screenshots). `GetSurfaceLevel`,
  `CreateRenderTarget`, `CreateDepthStencilSurface`, `CreateImageSurface`,
  `SetRenderTarget`/`GetRenderTarget`/`GetDepthStencilSurface` and
  `GetBackBuffer` are wired up, and the three extension vtable slots
  (`CreateImageSurface`/`CreateCubeTexture`/`CreateVolumeTexture`) that were
  left NULL are filled. Caveat: the GL backend has no D3D8 format decoder, so
  locking a *palettized* (`P8`) surface hands back the BGRA staging buffer
  rather than raw indices; BGRA8 / 16-bit paths are correct.

- **Audio output.** Resolved. `src/apu/apu_sdl_audio.c` implements the APU
  monitor's `waveOut*` surface over an SDL2 audio device (48 kHz s16 stereo,
  SDL pull callback), so `apu_core.c` is unchanged and its `WHDR_DONE`
  back-pressure still works. The old `win32_compat.h` inert `waveOut*` stubs
  are now prototypes.

## Phase 1 — minimal playable build (implemented)

- **`orta/CMakeLists.txt`** — Windows-only pieces are split:
  `orta.rc` and the Windows SDK libs (`d3d11 dxgi dxguid xinput comctl32
  winmm dbghelp`) are `if(WIN32)`; the `WIN32` subsystem flag and the
  `/FI...` forced include are MSVC/Windows-only; the executable links
  `${CMAKE_DL_LIBS}` on POSIX for the crash handler. The runtime's own
  POSIX/SDL2/GL branches are selected by `add_subdirectory()` as before.
- **`orta/src/main.c`** — POSIX port:
  - native `main()`; the body is shared as `orta_main()`.
  - `dbghelp`/`AddVectoredExceptionHandler` symbols resolve to the POSIX
    equivalents; `print_guest_context()` uses `dladdr` for symbol names.
  - `_putenv_s`/`SetEnvironmentVariableA` → `setenv`; `GetModuleFileNameA` →
    `readlink("/proc/self/exe")`; `GetFileAttributesA` → `access`;
    `MessageBoxA` → stderr; `MAX_PATH`/`SetThreadStackGuarantee` handled by
    the platform layer.
- **`orta/src/recomp_manual.c`** — the `_WIN32`-only gamepad path gained a
  POSIX `SDL_GameController` branch (buttons, triggers, sticks with the
  Xbox Y inversion, rumble via `SDL_GameControllerRumble`), an SDL keyboard
  map, and an SDL focus check (`SDL_GetKeyboardFocus`) replacing
  `GetForegroundWindow`.
- **`orta/src/launcher.c`** — the Win32 settings dialog is `if(WIN32)`.
  `launcher_maybe_run()` is a no-op on Linux (settings come from
  `settings.ini`, the F1 overlay is the long-term surface). The shared
  `orta_res_*` helpers are portable; `orta_screen_size` uses SDL on Linux.
- **`orta/orta_loop.sh`** — bash port of `orta_loop.ps1` (collect missing
  functions → recompile if needed → `cmake --build` → run → summary). The
  recompiler itself is unchanged (pure Python).
- **`orta/orta_package.sh`** — bash port of `orta_package.ps1`.

## Phase 2 — F1 overlay on Linux (implemented)

- **Engine plumbing** (`xboxrecomp-main/src/d3d/d3d8_gl.c`): the GL backend
  now owns the overlay/input hooks the Windows path keeps in `nv2a_gpu.c` /
  `fb_present.c` (`g_present_overlay_hook`, `g_present_pause_hook`,
  `g_sdlevent_hook`, `g_present_vsync`, `g_present_crop`). `dev_Present()`
  calls the overlay before `SDL_GL_SwapWindow`, and while the menu is open it
  loops on the game thread (holding the game at its flip), with a snapshot of
  the last frame drawn behind the menu.
- **`orta/src/overlay_gl.cpp`** — the Linux overlay: same menu/pages/help as
  `overlay.cpp`, but single-threaded (no window-thread event queue needed),
  SDL events translated to ImGui input, `SDL_GameController` for the pad,
  ImGui OpenGL3 rendering, and `settings.ini` written on close.
- **`third_party/imgui/imgui_impl_opengl3.{h,cpp}` + loader** — vendored from
  Dear ImGui 1.91.9 (matching the existing DX11 backend).

## Phase 3 / Phase 4

- Phase 3 (FMV/WMA): not implemented — conditional on the Phase 0 runtime
  question, which needs the disc image.
- Phase 4: `Setup.exe` is still Windows-only; `orta_package.sh` covers the
  "assemble a runnable folder" half. A Linux disc-image detection + toolchain
  bootstrap installer was not written.

## Building and running on Linux

```sh
# Dependencies (SDL2, epoxy, OpenSSL, ffmpeg headers, toolchain):
bash xboxrecomp-main/tools/linux/install_deps.sh

# Put your own disc's files in xboxrecomp-main/game_files/ (default.xbe etc.),
# and the seeds from orta/seeds/ are copied there automatically by the loop.

cd orta
bash orta_loop.sh          # translate (first run), build, run
```

Or by hand:

```sh
cmake -S orta -B orta/build64 -DCMAKE_BUILD_TYPE=Release
cmake --build orta/build64 -j
orta/build64/orta_recomp
```

On a non-Windows configure the runtime selects `d3d8_gl.c` (OpenGL 3.3 + SDL2)
instead of the D3D11 backend, and `xinput_device.c` selects SDL2. The GL
window is created when the game first creates its D3D8 device. The game
renders into an offscreen target at its own resolution and `dev_Present`
scales it to the window (16:9 or 4:3, centred with black bars), so
`WindowHeight`/`Fullscreen`/`Widescreen` take effect on Linux too.

## Remaining gaps (honest list)

1. **On-screen rendering on Linux — works, with fidelity gaps.** The OpenGL
   GPU back end (`nv2a_gpu_gl.c`) renders the game (title screen confirmed). What
   remains is correctness/perf polish: cyan texture/combiner artifacts on the
   title, GPU vertex programs, mipmaps/cube/bump stages, polygon offset,
   occlusion queries, and frame pacing.
2. **FMV and WMA are not engine gaps for Orta.** The disc's `.sfd` (Sofdec) and
   `.wma` are decoded by the game's own statically-linked code — the XBE has
   `WMADEC` and `PSGSFD*` sections and imports no media kernel APIs. The
   engine's `video_player`/`wma_decoder` HLE is a fallback used only for `.wmv`
   behind `RECOMP_FMV_HOST`. So Phase 3 is, for this title, no engine work.
3. **Palettized (P8) surface locking** returns the BGRA staging buffer, not raw
   palette indices — the GL backend has no D3D8 format decoder.
4. **Crop border and frame-rate cap** are honoured by the Windows present path
   only.
5. **The port has been run only as far as CPU rendering.** It boots and runs
   without crashing, but the renderer gap above has not been closed, so
   correctness of the GL backend under real draw traffic is still unverified.

## Longer term

- **Move the GPU backend from OpenGL to Vulkan.** `nv2a_gpu_gl.c` is a first-cut
  OpenGL 3.3 implementation; Vulkan is the more future-proof target for a GPU
  backend (and what modern emulators use). It is isolated behind the `nv2a_gpu.h`
  interface, so a Vulkan implementation could replace the GL one on POSIX without
  changing the executor; the real work is SPIR-V shaders, render targets/depth,
  texture upload and present.
