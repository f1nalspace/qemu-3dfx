# QEMU MESA GL / 3Dfx Glide Pass-Through

Hardware-accelerated OpenGL, Glide, Direct3D and DirectDraw for Windows 9x/ME and Windows 2000/XP guests under QEMU, rendered by the host GPU.

This is an independent fork of qemu-3dfx by KJ Liew (Copyright (C) 2018-2025 KJ Liew \<liewkj@yahoo.com\>). It carries its own fixes and additions and is developed separately.

A step-by-step guide will be published separately.

## Tested with

- QEMU 9.2.2 with `00-qemu92x-mesa-glide.patch`
- Linux host (Arch-based), NVIDIA GeForce RTX 3090
- Windows 98 SE and Windows XP guests

The 8.2.x and 7.2.x patches are carried along unchanged and have not been tested with the changes below.

## What this fork changes

### Host side (QEMU device models)

- **The guest's gamma ramp no longer reaches the host screen.** A crashing game used to leave the whole host desktop dark. `QEMU_3DFX_HOST_GAMMA=1` restores the old behaviour.
- **GL takes over the window only on the first `SwapBuffers`**, not on `SetPixelFormat`. Renderers that draw into an FBO and read back into guest video memory keep their picture.
- **Fullscreen and window scaling for GL and Glide:** the guest window size is reported to the host, the image is scaled and centred with letterboxing, the viewport is restored when fullscreen is toggled, and the Glide window can be resized at runtime.
- **Write-only buffer maps without a round trip:** `glMapBufferRange` with write access returns guest memory, and flushed ranges reach the host as `glBufferSubData` in the command queue.
- **Fixes:** a crash when a context was recreated after the level-0 context was purged, a missing bound on the vertex array destination, window handover restoring the display and the pointer grab.
- **Diagnostics:** a frame counter and a flight recorder, see below.

### Guest wrappers

- Build with the i686 MinGW cross toolchain on Linux, from a clean tree and under `make -j`.
- Target SSE3 (`-march=prescott`) with LTO.
- An OpenGL ICD flavour, `qmfxgl32.dll`, next to `opengl32.dll`, including fixes for Windows NT (layer plane 255, placeholder context).

### Tools (`tools/`)

- `glidecube`, `glidepal` — Glide test programs (spinning cube, palettised textures and chroma key), same source for host and guest
- `d3dcube`, `ddcube` — Direct3D 8 and DirectDraw/Direct3D 7 test programs with a transparency check
- `gammafix` — shows and resets the host gamma ramp (X11)
- `sinetest` — plays six known tones through waveOut and DirectSound, to check sound device models
- `timecheck` — measures the guest clocks against `QueryPerformanceCounter`

## What it takes to be usable

**On the host**

- KVM, and QEMU built from this tree with the 9.2 patch.
- **QEMU and the guest wrappers built from the same commit.** Both carry the short commit id and refuse to work together otherwise; Windows then only reports that the DLL cannot start. After changing the commit, rebuild the wrappers from a clean build directory.
- For Glide: OpenGLide as the host library (`libglide2x.so`, `libglide3x.so`) on the library path.

**In a Windows 9x/ME guest**

- `FXMEMMAP.VXD` and the Glide DLLs in `C:\WINDOWS\SYSTEM`.
- OpenGL either through `opengl32.dll` in the game folder, or through the ICD `qmfxgl32.dll`, which needs a display driver that registers it (`icd-enable.reg`).

**In a Windows 2000/XP guest**

- `FXPTL.SYS` installed with `INSTDRV.EXE`, and the Glide DLLs in `system32`.
- For the ICD: a display driver that answers the OpenGL ICD escape, and the registry key `HKLM\Software\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\QEMUFX` with `DLL`, `Flags`, `Version` and `DriverVersion`.

**For Direct3D and DirectDraw**

- A WineD3D build for Windows 9x/XP on top of the OpenGL path. Direct3D and DirectDraw have no pass-through of their own here.

## What it takes to be fast

- **A guest CPU model with SSE3**, for example `-cpu coreduo`. The wrappers are built for it.
- **ICD `Flags` set to 3 on Windows 2000/XP.** Bit 0 makes Windows ask the ICD for pixel formats; without it applications land in software rendering. Bit 1 stops a `glFinish` before every `SwapBuffers`, which otherwise costs about a third of the frame rate.
- **WineD3D with dynamic buffers kept in system memory** (`DynamicBufferObjects` = `disabled`). Otherwise every Direct3D 7 `DrawPrimitive` maps a buffer object, and each map is a synchronous round trip into the host. Measured on Drakan under Windows XP: 20.9 fps by default, 488.8 fps with the setting.
- **For older OpenGL games,** a `wrapgl32.ext` next to the executable with `ExtensionsYear,2000`. Games such as Quake 2 and Oni overflow on the full extension string of a modern driver.

## Options

**QEMU environment variables**

| Variable | Effect |
|---|---|
| `QEMU_3DFX_HOST_GAMMA=1` | let the guest's gamma ramp reach the host screen again |
| `QEMU_3DFX_FPS=1` | count finished frames (SwapBuffers and ReadPixels) and print them once per second |
| `QEMU_3DFX_UI_DIAG=1` | log size and scaling decisions of the GL window |
| `QEMU_3DFX_FLIGHT=<file>` | flight recorder: every access to the pass-through registers with a timestamp, kept in memory and written when the GL program ends and when QEMU exits |
| `QEMU_3DFX_FLIGHT_LEVEL=1\|2` | level 2 also records every queued call |
| `QEMU_3DFX_FLIGHT_MB=<n>` | size of the recorder's ring buffer (default 1024) |

**`wrapgl32.ext` next to the game executable** (one option per line)

| Option | Effect |
|---|---|
| `ExtensionsYear,<year>` | hide GL extensions newer than the given year |
| `MapBufferInGuestOff,1` | map buffers through the host again instead of in guest memory |
| `ContextVsyncOff,1` | ignore vertical sync requests |
| `SwapInterval,<n>` | force a swap interval |

## Content

    qemu-0/hw/3dfx       - Overlay for the QEMU source tree: 3Dfx Glide pass-through device model
    qemu-1/hw/mesa       - Overlay for the QEMU source tree: MESA GL pass-through device model
    scripts/sign_commit  - Stamps the commit id into the QEMU source tree
    scripts/conf_wrapper - Configures a wrapper build directory
    wrappers/3dfx        - Glide wrappers (DOS/Windows/DJGPP/Linux)
    wrappers/mesa        - OpenGL wrapper and ICD for Windows
    tools/               - Test programs and helpers, see above
    icd-enable.reg       - Enables the OpenGL ICD for a Windows 9x display driver that supports it

## Patches

    00-qemu92x-mesa-glide.patch - QEMU 9.2.x (MESA & Glide), with this fork's changes
    01-qemu82x-mesa-glide.patch - QEMU 8.2.x (MESA & Glide), as inherited
    02-qemu72x-mesa-glide.patch - QEMU 7.2.x (MESA & Glide), as inherited
