# d3dcube — a test case for the Direct3D path

The counterpart to `glidecube`, one interface higher up: the same rotating,
Gouraud-shaded cube with a depth buffer, this time over **Direct3D 8**. Plus the frame
rate and what `GetAdapterIdentifier` reports.

What it is meant to prove is the chain

    d3dcube → D3D8 → WineD3D (wine9x) → OPENGL32.DLL (qemu-3dfx) → MESAPT → host GPU

## Why not `dxdiag`

`dxdiag` presupposes an installed DirectX redistributable. The shorter route follows from
the imports of the wine9x DLLs: `wined8.dll` depends on nothing but `wined3d.dll` and
`msvcrt`, and it exports `Direct3DCreate8`. So `d3dcube` fetches that entry point through
`LoadLibrary` from a **selectable** DLL instead of linking against it:

    -dll d3d8.dll      the ordinary route. On the host under Wine that is Wine's own
                       d3d8; in the guest the one from Microsoft, or the switcher.
    -dll wined8.dll    the Wine implementation from wine9x directly. That way the guest
                       needs neither DirectX nor the switcher.

## Layout

    src/d3dcube.c   the program
    Makefile        one build route, two runtime environments
    build/          build output, not in git

Plain C89 without `d3dx` and without C++. The matrices are built by hand so that the
source also compiles under Visual C++ 6.0 in the guest, without the DirectX SDK having to
be there.

## Building

```sh
make
```

Produces `build/D3DCUBE.EXE`. **The same EXE** runs on the host under Wine and in the
guest under Windows 98 — which makes the two frame rates directly comparable, with no two
separate compilations in between.

Two switches are decisive, the same as for `glidecube` and for the qemu-3dfx wrapper:

- `-march=prescott -mtune=core2` because of the guest CPU (`-cpu coreduo`, SSE3 is the
  ceiling)
- `-mcrtdll=msvcrt-os` because of Windows 98. Without it the toolchain on Arch links
  against the UCRT, which does not exist under Windows 9x.

The build checks that itself right away: no `api-ms-win-crt` may show up in the dependency
list.

## Running it on the host

```sh
make hostinfo     # the adapter data only
make hostrun      # draw
```

Mind the frame rate: in **windowed** mode Direct3D 8 prescribes
`D3DPRESENT_INTERVAL_DEFAULT`, and the program cannot deselect the vertical retrace at
all. Without help you therefore measure the monitor's refresh rate and not the throughput.
On the host the driver helps:

```sh
__GL_SYNC_TO_VBLANK=0 vblank_mode=0 wine build/D3DCUBE.EXE 15
```

## Running it in the guest

One directory, all of it throwaway:

    C:\D3DTEST\
      D3DCUBE.EXE
      opengl32.dll     ← qemu-3dfx wrapper
      wined3d.dll  winedd.dll  wined8.dll  wined9.dll
      wrapgl32.ext     ← one line: ContextVsyncOff,1

```
D3DCUBE.EXE -dll wined8.dll -info      adapter data
D3DCUBE.EXE 15 -dll wined8.dll         draw, 15 seconds
```

Here the vertical retrace is not deselected through the driver but through the wrapper:
`ContextVsyncOff,1` in `wrapgl32.ext` **next to the EXE**. The wrapper looks for the file
through `GetModuleFileName(NULL, …)`; it is the same one `ExtensionsYear` is set in.

Without that line the frame rate stays pinned to the refresh rate of the **host** monitor
— which by itself shows that the drawing happens there.

## Usage

    d3dcube [seconds] [-dll NAME] [-info] [-fs] [-vsync]

    seconds    run time, default 15. 0 means endless.
    -dll NAME  DLL providing Direct3DCreate8. Default d3d8.dll.
    -info      print the adapter data only, draw nothing.
    -fs        fullscreen 640x480 instead of a window.
    -vsync     wait for the vertical retrace (only effective in fullscreen).

## Results

See `docs/LOG.md` [124]–[131].

| | Frame rate |
|---|---|
| Host, Wine's current WineD3D, straight onto the GPU | 11,031.4 FPS |
| Guest, wine9x WineD3D over wrapper → QEMU → host GL | 7,843.5 FPS |

The 71 % are a **system comparison**, not an isolated measurement of the pass-through: on
the host Wine's current WineD3D was running, in the guest the one from 1.7.55 out of
wine9x. The attempt to run the same DLLs on both sides failed — they are built against
`nocrt` and `pthread9x` and crash under today's Wine [131].
