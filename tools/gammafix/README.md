# gammafix — die Gamma-Rampe des Hosts retten

## Wozu

qemu-3dfx hakt sich im Gast in `GDI32.SetDeviceGammaRamp` ein
(`wrappers/mesa/src/wrapgl32.c`, `HookPatchGamma`) und leitet den Aufruf über die
Gerätegrenze auf den Host um. Dort landet er in
`hw/mesa/mglcntx_linux.c`, `wglSetDeviceGammaRamp3DFX`, und wird zu:

    XF86VidModeSetGammaRamp(dpy, DefaultScreen(dpy), rampsz, r, g, b);

Das ist **der ganze X-Bildschirm**, nicht das QEMU-Fenster. Setzt ein Gastspiel
seine Helligkeit, setzt es damit die Helligkeit des kompletten Host-Desktops, auf
allen daran hängenden Bildschirmen.

Zurückgesetzt wird die Rampe nur in `MGLWndRelease()`. Stürzt das Gastprogramm ab
oder wird QEMU hart beendet, bleibt der Host in der Rampe des Spiels stehen. Ist
die dunkel, ist der Desktop unlesbar — er **funktioniert** weiter (Fenster
wechseln, tippen, scrollen), man sieht ihn nur nicht mehr.

Das ist kein Grund für einen Neustart. Es ist eine Zeile.

## Bauen

    make -C tools/gammafix

Braucht nur `libX11` und `libXxf86vm`, beide sind mit Xorg ohnehin da.

## Benutzen

Anzeigen, was gerade gesetzt ist:

    tools/gammafix/build/gammafix

Zurücksetzen:

    tools/gammafix/build/gammafix --reset

Die geschriebene Rampe ist Bit für Bit dieselbe, die `MesaInitGammaRamp()` in
qemu-3dfx schreibt — also genau das, was ein sauberes `MGLWndRelease()` getan hätte.

## Im Notfall

Ist der Bildschirm schon dunkel, geht es blind über eine zweite Konsole:

    Strg+Alt+F3
    cd ~/_projects/qemu-3dfx-build
    DISPLAY=:0 tools/gammafix/build/gammafix --reset
    Strg+Alt+F1

`DISPLAY=:0` ist nötig, weil die Konsole kein X-Display kennt.
