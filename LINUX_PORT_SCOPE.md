# Orta PC Recomp — Native Linux Port Scope

Source: `Orta_PC_Recomp-0.5.1_Beta.zip` (built on the `xboxrecomp` toolkit by
sp00nznet). Scoped 2026-10-27.

## TL;DR

The underlying `xboxrecomp-main` engine already has a real Linux track:
POSIX `win32_compat.c`, an OpenGL 3.3 + SDL2 backend for the D3D8/GPU layer
(`d3d8_gl.c`) as an alternative to D3D11, SDL2 backends for audio/input, and
`tools/linux/install_deps.sh`. That part is mostly done upstream.

The Orta-specific wrapper (`orta/`) is **Windows-only**: a `WIN32`-subsystem
CMake target, a D3D11 ImGui overlay, direct `windows.h`/`d3d11.h`/`xinput.h`/
`dbghelp.h`/`commctrl.h` usage, a PowerShell build pipeline, and a Windows
`Setup.exe` installer. Porting is real but bounded engineering, not a toggle.

No prior art was found for this specific work (checked publicly — nobody has
documented wiring xboxrecomp's overlay/input to the Linux GL path yet).

You still need your own legally-owned Xbox disc image, same as on Windows —
this scope doesn't change that requirement.

## Phase 0 — De-risk before committing (1-3 days)

- Build `xboxrecomp-main`'s Linux path (SDL2+epoxy) standalone; run its
  existing test/conformance suite on Linux to confirm the engine is healthy
  there independent of Orta.
- Determine whether Orta's game code actually plays FMV cutscenes and/or WMA
  audio at runtime. **Neither has a Linux implementation upstream** —
  `video_player.c`'s non-`_WIN32` branch always returns failure/finished,
  and `wma_decoder.c`'s non-`_WIN32` branch is a two-line stub returning -1.
  Both are Media-Foundation-only on Windows. If Orta doesn't hit these paths,
  this entire gap drops out of scope.
- Check how much Orta's rendering depends on `IDirect3DSurface8` locking
  (screenshots, palette-texture write-back, render-to-texture). The GL
  backend's surface implementation is explicitly flagged in-code as
  "minimal stub, first cut" (`d3d8_gl.c` ~L346, ~L494).

## Phase 1 — Minimal playable build: gamepad, no menus (1-2 weeks)

This is the core of the port.

- **`orta/CMakeLists.txt`**: Windows libs (`d3d11 dxgi dxguid xinput comctl32
  winmm dbghelp`) and `orta.rc` are linked/compiled **unconditionally** —
  need `if(WIN32)` guards. The `orta_overlay` static lib unconditionally
  compiles `imgui_impl_dx11.cpp` and links `d3d11`/`xinput` — won't even
  compile on Linux without a guard (defer the overlay itself to Phase 2).
  `add_subdirectory(xboxrecomp-main)` already auto-selects its POSIX/SDL2/GL
  branches on a non-Windows configure — no upstream CMake changes needed
  there.
- **`main.c`**: replace `dbghelp.h`/`AddVectoredExceptionHandler` crash
  diagnostics with a POSIX `sigaction` SIGSEGV handler (+ `backtrace_symbols`
  or DWARF lookup — or just stub this for v1, it's not blocking). Replace
  `GetModuleFileNameA`, `GetFileAttributesA`, `ExitProcess`, `MessageBoxA`,
  `_putenv_s`/`SetEnvironmentVariableA`, `MAX_PATH`, `SetThreadStackGuarantee`
  with POSIX equivalents. All mechanical, low risk.
- **`recomp_manual.c`**: `orta_read_pad()`'s entire body is inside
  `#ifdef _WIN32` — it returns an all-zero pad state unconditionally on
  Linux today. Add an SDL_GameController `#else` branch
  (`SDL_GameControllerGetState`, `SDL_GameControllerRumble`) plus an
  SDL-based window-focus check to replace `orta_has_focus()`'s
  `GetForegroundWindow`-based detection. This file's Windows-specific code
  is narrowly confined (~L112-306 of ~1850 lines); the rest (stub-hit
  logging, icall diagnostics, the background-loader-thread reimplementation)
  is already portable C with no OS dependency.
- **`launcher.c`** (the ~500-line hand-rolled Win32 pre-game settings
  dialog — `CreateWindowExW`, comctl32 widgets, `GetMessageW` pump, INI I/O):
  **skip entirely** for this phase. Force `launcher_maybe_run()` to no-op
  and drive settings from `settings.ini` only. Rebuilding this dialog isn't
  worth it — the F1 overlay should be the long-term settings surface on
  both platforms.
- **Build pipeline**: write a bash equivalent of `orta_loop.ps1`. The
  recompiler toolchain itself (disasm → func_id → abi_analysis → recomp) is
  already pure Python and documented upstream as working on Linux via
  `python3` (`tools/linux/install_deps.sh` exists for this). Only the
  PowerShell orchestration (the `Step`/`Fail`/`RunTool` helpers, the
  `cmake --build`/run-the-exe glue, the "Developer PowerShell for VS 2022"
  dependency for `cl.exe`) needs porting — low risk.

**End state: the game runs and is playable on Linux with a gamepad, no
in-game settings menu.**

## Phase 2 — F1 overlay on Linux (3-7 days, touches shared engine code)

The overlay (`overlay.cpp`, ~950 lines) is well-architected for this — it's
invoked through a generic `void*` function-pointer hook and doesn't read
back D3D11 state beyond one init call — but the plumbing to reach it on
Linux doesn't exist yet:

- `xboxrecomp-main/src/video/fb_present.c`'s overlay/input hooks
  (`g_present_overlay_hook`, `g_wndproc_hook`) are wired only into the
  `_WIN32` branch (which spans nearly the whole 588-line file). The GL
  backend (`d3d8_gl.c`) owns its own independent SDL window/GL-context/swap
  loop and never calls either hook — grep confirms zero references. This is
  **new engine-level plumbing**, not Orta-only, and could be upstreamed.
- Vendor `imgui_impl_opengl3` (only the DX11 backend is currently in
  `orta/third_party/imgui/`) and write an SDL-event → the overlay's existing
  `Ev` queue translator, replacing `ovl_wndproc`'s Win32-message translation
  (`ovl_wndproc` already funnels everything through that one queue, so this
  is the same shape of work, not a redesign).
- Swap the overlay's direct `XInputGetState` gamepad polling
  (`pad_poll`/`pad_combo`/`pad_to_imgui`) for `SDL_GameController` — same
  mapping table, different source API.

## Phase 3 — FMV / WMA (conditional — size unknown until Phase 0 answers this)

If Orta uses either at runtime: new FFmpeg/libavcodec-based decode path
feeding into the GL present pipeline and the APU mixer. If unused: zero work.

## Phase 4 — Distribution polish (S-M, not blocking)

`Setup.exe` is Windows-only by construction (it's a from-scratch Win32 GUI
installer). On Linux, replace it with a shell script doing disc-image
detection + Python/toolchain bootstrap + build — most of the logic in
`installer/setup_main.py` is already Python and reusable.

## Overall estimate

Phases 0-2 (de-risk → playable with gamepad → F1 overlay) is roughly
**3-4 weeks** of solo C/C++ work, assuming no significant GL-backend
correctness bugs surface during first bring-up (real unknown: the OpenGL
path has no documented caveats anywhere, and at least one spot in it is
explicitly marked "first cut"). FMV/WMA and the native settings-dialog
rebuild are open-ended tails worth deferring until Phase 0 confirms whether
Orta even needs them.

## Suggested order

1. Phase 0 (cheap, de-risks everything downstream)
2. Phase 1 (playable milestone)
3. Phase 2 (feature parity on the settings UI)
4. Phase 3/4 only if Phase 0 shows they're needed
