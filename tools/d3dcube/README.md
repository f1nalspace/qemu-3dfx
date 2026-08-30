# d3dcube — ein Prüffall für den Direct3D-Pfad

Das Gegenstück zu `glidecube`, nur eine Schnittstelle höher: derselbe rotierende,
Gouraud-schattierte Würfel mit Tiefenpuffer, diesmal über **Direct3D 8**. Dazu die
Bildrate und das, was `GetAdapterIdentifier` meldet.

Belegt werden soll die Kette

    d3dcube → D3D8 → WineD3D (wine9x) → OPENGL32.DLL (qemu-3dfx) → MESAPT → Host-GPU

## Warum nicht `dxdiag`

`dxdiag` setzt ein installiertes DirectX-Redistributable voraus. Der kürzere Weg
ergibt sich aus den Importen der wine9x-DLLs: `wined8.dll` hängt an nichts außer
`wined3d.dll` und `msvcrt`, und es exportiert `Direct3DCreate8`. Deshalb holt
`d3dcube` diesen Einsprung per `LoadLibrary` aus einer **wählbaren** DLL, statt ihn
zu binden:

    -dll d3d8.dll      der gewöhnliche Weg. Auf dem Host unter Wine ist das Wines
                       eigenes d3d8; im Gast das von Microsoft oder der Umschalter.
    -dll wined8.dll    unmittelbar die Wine-Umsetzung aus wine9x. Damit braucht der
                       Gast weder DirectX noch den Umschalter.

## Aufbau

    src/d3dcube.c   das Programm
    Makefile        ein Bauweg, zwei Laufumgebungen
    build/          Bauergebnis, nicht im Git

Reines C89 ohne `d3dx` und ohne C++. Die Matrizen sind von Hand aufgebaut, damit die
Quelle auch unter Visual C++ 6.0 im Gast übersetzt, ohne dass dort das DirectX-SDK
liegen muss.

## Bauen

```sh
make
```

Erzeugt `build/D3DCUBE.EXE`. **Dieselbe EXE** läuft auf dem Host unter Wine und im
Gast unter Windows 98 — damit sind die beiden Bildraten unmittelbar vergleichbar,
ohne dass zwei Übersetzungen dazwischenstehen.

Zwei Schalter sind entscheidend, dieselben wie bei `glidecube` und beim
qemu-3dfx-Wrapper:

- `-march=pentium2` wegen der Gast-CPU (`-cpu pentium3`)
- `-mcrtdll=msvcrt-os` wegen Windows 98. Ohne ihn bindet die Werkzeugkette auf Arch
  gegen die UCRT, die es unter Windows 9x nicht gibt.

Der Bau prüft das gleich selbst nach: in der Abhängigkeitsliste darf kein
`api-ms-win-crt` auftauchen.

## Auf dem Host laufen lassen

```sh
make hostinfo     # nur die Adapterangaben
make hostrun      # zeichnen
```

Achtung bei der Bildrate: im **Fenstermodus** schreibt Direct3D 8
`D3DPRESENT_INTERVAL_DEFAULT` vor, das Programm kann den Strahlrücklauf gar nicht
abwählen. Ohne Zutun misst man deshalb die Bildwiederholrate des Monitors und nicht
den Durchsatz. Auf dem Host hilft der Treiber:

```sh
__GL_SYNC_TO_VBLANK=0 vblank_mode=0 wine build/D3DCUBE.EXE 15
```

## Im Gast laufen lassen

Ein Verzeichnis, alles wieder wegzuwerfen:

    C:\D3DTEST\
      D3DCUBE.EXE
      opengl32.dll     ← qemu-3dfx-Wrapper
      wined3d.dll  winedd.dll  wined8.dll  wined9.dll
      wrapgl32.ext     ← eine Zeile: ContextVsyncOff,1

```
D3DCUBE.EXE -dll wined8.dll -info      Adapterangaben
D3DCUBE.EXE 15 -dll wined8.dll         zeichnen, 15 Sekunden
```

Der Strahlrücklauf wird hier nicht über den Treiber abgewählt, sondern über den
Wrapper: `ContextVsyncOff,1` in `wrapgl32.ext` **neben der EXE**. Der Wrapper sucht
die Datei über `GetModuleFileName(NULL, …)`, es ist dieselbe, über die auch
`ExtensionsYear` gesetzt wird.

Ohne diese Zeile bleibt die Bildrate bei der Bildwiederholrate des **Host**-Monitors
hängen — was für sich schon zeigt, dass die Darstellung dort stattfindet.

## Aufruf

    d3dcube [Sekunden] [-dll NAME] [-info] [-fs] [-vsync]

    Sekunden   Laufzeit, Voreinstellung 15. 0 heisst endlos.
    -dll NAME  DLL, die Direct3DCreate8 liefert. Voreinstellung d3d8.dll.
    -info      nur die Angaben zum Adapter ausgeben, nichts zeichnen.
    -fs        Vollbild 640x480 statt Fenster.
    -vsync     auf den Strahlruecklauf warten (nur im Vollbild wirksam).

## Ergebnisse

Siehe `docs/LOG.md` [124]–[131].

| | Bildrate |
|---|---|
| Host, Wines heutiges WineD3D, unmittelbar auf die GPU | 11.031,4 FPS |
| Gast, wine9x-WineD3D über Wrapper → QEMU → Host-GL | 7.843,5 FPS |

Die 71 % sind ein **Systemvergleich**, keine isolierte Messung des Passthroughs: auf
dem Host lief Wines heutiges WineD3D, im Gast das von 1.7.55 aus wine9x. Der Versuch,
dieselben DLLs auf beiden Seiten zu fahren, ist gescheitert — sie sind gegen `nocrt`
und `pthread9x` gebaut und stürzen unter heutigem Wine ab [131].
