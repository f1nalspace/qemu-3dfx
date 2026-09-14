# gammafix — rescuing the host's gamma ramp

## What for

In the guest, qemu-3dfx hooks `GDI32.SetDeviceGammaRamp`
(`wrappers/mesa/src/wrapgl32.c`, `HookPatchGamma`) and redirects the call across the
device boundary to the host. There it lands in
`hw/mesa/mglcntx_linux.c`, `wglSetDeviceGammaRamp3DFX`, and becomes:

    XF86VidModeSetGammaRamp(dpy, DefaultScreen(dpy), rampsz, r, g, b);

That is **the whole X screen**, not the QEMU window. When a guest game sets its
brightness, it sets the brightness of the entire host desktop with it, on every screen
attached to it.

The ramp is only reset in `MGLWndRelease()`. If the guest program crashes or QEMU is
killed, the host stays in the game's ramp. If that one is dark, the desktop is unreadable
— it keeps **working** (switching windows, typing, scrolling), you just cannot see it any
more.

That is no reason for a reboot. It is one line.

## Building

    make -C tools/gammafix

Needs only `libX11` and `libXxf86vm`, both of which come with Xorg anyway.

## Using it

Show what is currently set:

    tools/gammafix/build/gammafix

Reset:

    tools/gammafix/build/gammafix --reset

The ramp it writes is bit for bit the one `MesaInitGammaRamp()` writes in qemu-3dfx — so
exactly what a clean `MGLWndRelease()` would have done.

## In an emergency

If the screen is dark already, it works blind over a second console:

    Ctrl+Alt+F3
    cd ~/_projects/qemu-3dfx-build
    DISPLAY=:0 tools/gammafix/build/gammafix --reset
    Ctrl+Alt+F1

`DISPLAY=:0` is needed because the console knows no X display.
