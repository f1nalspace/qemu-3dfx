# glidecube — ein Prüffall für den Glide-Pfad

Das Gegenstück zu `wglgears` für OpenGL: ein rotierender, Gouraud-schattierter Würfel
mit Tiefenpuffer, dazu die Bildrate und die Angaben aus `grSstQueryHardware`.

Der Grund für dieses Programm: Quake 1 und Quake 2 erreichen Glide nur über die
MiniGL-Zwischenschicht `3dfxgl.dll`, und deren `wglCreateContext` scheitert
(`docs/LOG.md` [102]). Ein fertiges Glide-Prüfprogramm gibt es nirgends — auch
`testqmfx.exe` von der SoftGPU-ISO ist trotz des Namens ein WGL-Test [105]. Also ein
eigenes, das `glide2x.dll` **direkt** anspricht.

## Aufbau

    src/glidecube.c   das Programm
    src/glidemin.h    die kleinste Teilmenge der Glide-2.x-Schnittstelle
    Makefile          drei Bauwege
    build/            Bauergebnisse, nicht im Git

`glidemin.h` ist bewusst selbstgeschrieben statt aus dem 3Dfx-SDK oder aus OpenGLide
übernommen: Die SDK-Fassung in OpenGLide zieht ein von `configure` erzeugtes
`sdk2_unix.h` nach, das im Quellbaum gar nicht liegt, und definiert für MinGW
`FX_ENTRY` als `extern "C"` — das ist C++ und bricht in reinem C. Alle Werte in
`glidemin.h` sind gegen `sdk2_glide.h` und `sdk2_sst1vid.h` aus OpenGLide geprüft.

## Bauen

### Host, gegen OpenGLide

```sh
make host
make run
```

Damit lässt sich das Programm prüfen, **bevor** eine VM überhaupt läuft — OpenGLide
setzt Glide auf dem Host in OpenGL um und öffnet ein eigenes X11-Fenster.

### Gast, Cross-Bau mit MinGW

```sh
make win32
```

MinGW braucht eine Importbibliothek. Der Makefile erzeugt sie aus der gebauten
`glide2x.dll` des Wrappers: `gendef` liest die Exporte, `dlltool` macht daraus
`libglide2x.a`. Zwei Schalter sind Pflicht:

- `-march=pentium2` — der Gast läuft mit `-cpu pentium3`.
- `-mcrtdll=msvcrt-os` — ohne ihn bindet die Werkzeugkette gegen die UCRT, die es unter
  Windows 9x nicht gibt. Der Makefile gibt zur Kontrolle die DLL-Abhängigkeiten aus; es
  darf kein `api-ms-win-crt-*` darunter sein.

### Gast, Visual C++ 6.0

Für den Bau im Gast selbst. Benötigt `glide2x.lib` — entweder aus dem 3Dfx-SDK oder
mit `IMPLIB` aus der `glide2x.dll` erzeugt.

```
cl /W3 /O2 /I. glidecube.c glide2x.lib /Fe GLIDECUB.EXE
```

Die Quelle ist deshalb reines C89: Deklarationen am Blockanfang, keine `//`-Kommentare,
keine C99-Konstrukte.

## Aufruf

```
glidecube [Sekunden] [Auflösung] [-info] [-vsync]
```

- **Sekunden** — Laufzeit, Voreinstellung 15. `0` heißt endlos.
- **Auflösung** — 320, 512, 640 oder 800. Voreinstellung 640.
- **-info** — nur die Hardwareangaben ausgeben, nichts zeichnen.
- **-vsync** — auf den Strahlrücklauf warten. **Ohne** diesen Schalter misst die
  Bildrate den Durchsatz; mit ihm misst sie die Bildwiederholrate des Bildschirms.

## Was das Programm zeigt

- `grSstQueryHardware` — ob die Gegenstelle überhaupt eine Karte meldet, und mit welchen
  Angaben.
- Der Würfel selbst — dass Dreiecke gerastert, Farben über die Fläche interpoliert und
  verdeckte Flächen über den W-Puffer aussortiert werden.
- Die Bildrate — als Maß dafür, ob wirklich beschleunigt wird.

## Zwei Fallstricke, die beim Bau auftraten

**Durchsichtiges Fenster.** In Glide ist die Schreibmaske für den Alphakanal
voreingestellt **aus**. OpenGLide wählt auf dem Host ein GLX-Visual mit acht Alphabits;
bleibt der Kanal auf null, hält der Compositor das Fenster für durchsichtig. Abhilfe:
`grColorMask(FXTRUE, FXTRUE)`.

**Rasende Drehung.** Der Drehwinkel darf nicht an der Zahl der Bilder hängen. Bei über
15.000 Bildern je Sekunde auf dem Host wird daraus ein unbrauchbares Flimmern, und die
Anzeige sähe auf jedem Rechner anders aus. Der Winkel folgt deshalb der verstrichenen
Zeit.

## OpenGLide einstellen

Beim ersten Start legt OpenGLide im Arbeitsverzeichnis `OpenGLid.ini` an. Zwei Werte
darin erklären, was `-info` meldet:

    FrameBufferMemorySize=8      -> "Bildspeicher 8 MB"
    TextureMemorySize=16         -> "TMU0: 16 MB"

`Resolution=0.0` bedeutet Originalgröße; ein Wert über 16 setzt eine feste Breite und
skaliert die Ausgabe hoch. Die Datei gehört nicht ins Git, sie entsteht bei jedem Lauf
neu.
