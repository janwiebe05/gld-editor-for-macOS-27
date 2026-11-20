# GLD Editor on macOS 27

Unofficial patch that makes **Allen & Heath GLD Editor 1.61** run on **macOS 27**
again, plus a few quality-of-life improvements. It does not contain any
Allen & Heath software: the script patches a copy of *your own* GLD Editor
installation and leaves the original untouched.

> Not affiliated with or endorsed by Allen & Heath. Use at your own risk.

[Deutsch weiter unten](#deutsch)

## What it fixes

| Problem on macOS 27 | Fix |
|---|---|
| GLD Editor crashes at launch | Apple disabled the old ATS font API that Qt 4 uses to load embedded fonts. A small replacement library implements it with CoreText. |
| Random crash when Wi-Fi changes | Qt's CoreWLAN bearer plug-in is removed; Qt falls back to its generic one. |
| Old system libraries (`libcrypto.0.9.8`, `libxml2`, `libbz2`) may disappear | Bundled: built from source, `libcrypto` replaced by a CommonCrypto shim. |
| `QTKit` / `AGL` are deprecated | Made optional (they are linked but not used). |

And some improvements:

- **Full screen and resizable windows** for the console surface and the touch-screen window
  (green window button, Control-Command-F, or drag the corner).
- **Sharp rendering on Retina displays**: buttons, labels, meters and lists are drawn at
  double resolution. Only graphics shipped as image files (e.g. fader caps) stay at their
  original resolution.
- **No more cut-off labels** ("DCA", "Custom 1", "Processing", "Compressor"): text that does
  not fit is scaled down slightly.

## Requirements

- macOS 27 (Apple Silicon or Intel); also works on macOS 26
- **Rosetta 2** on Apple Silicon – the script offers to install it. After upgrading to
  macOS 27 it is often missing.
- GLD Editor **1.61** (Rev. 38134) in `/Applications`
- Command Line Tools: `xcode-select --install`

macOS 28 will remove Rosetta for regular apps. This patch cannot fix that; consider a
macOS 27 virtual machine (UTM, Parallels) if you need GLD Editor beyond macOS 27.

## Usage

```sh
git clone https://github.com/janwiebe05/gld-editor-for-macOS-27.git
cd gld-editor-for-macOS-27
./gld-patch.sh --test
```

This creates `/Applications/GLD Editor 1.61 (macOS 27).app` (or `~/Applications/…` if
`/Applications` is not writable). Options:

```
--source <app>   original GLD Editor 1.61.app (default: /Applications/GLD Editor 1.61.app)
--dest <app>     where to put the patched copy
--force          patch an unknown build anyway
--test           start the patched app once and check that it keeps running
```

If it does not start, run `tools/gld-check.sh "<path to patched app>"`. It writes a
report (`GLD-Report.txt`) to your Desktop.

### Good to know

- Only one GLD Editor can run at a time; a second one quits silently.
- After a crash or force quit, the next launch only cleans up and quits. Just start it again.
- Runtime switches (environment variables) for troubleshooting:
  `GLD_NO_FULLSCREEN=1`, `GLD_PIXMAP_SCALE=1` (no Retina rendering), `GLD_NO_TEXTFIT=1`,
  `GLD_RASTER=1` (Qt raster instead of native CoreGraphics rendering).

## How it works

| File | Loaded by | Purpose |
|---|---|---|
| `src/libATSCompat.c` | QtGui (replaces ApplicationServices, re-exports it) | ATS font functions via CoreText; high-quality image scaling |
| `src/libGLDWindow.m` | libATSCompat | full screen, scaling the fixed-size Qt windows, green button |
| `src/libGLDHiDPI.c` | main executable (replaces QtGui, re-exports it) | double-resolution pixmaps, text fitting |
| `src/libcrypto-shim.c` | bundled libarchive | the 12 hash functions it needs (SHA-2 via CommonCrypto, RIPEMD-160) |
| `tools/weaklink.py` | – | turns `LC_LOAD_DYLIB` into `LC_LOAD_WEAK_DYLIB` |

The patched app is not code-signed, like the original; x86_64 code runs unsigned under Rosetta.

---

## Deutsch

Inoffizieller Patch, damit der **Allen & Heath GLD Editor 1.61** unter **macOS 27** wieder
läuft – mit Vollbild, scharfer Retina-Darstellung und ohne abgeschnittene Beschriftungen.
Das Repository enthält keine Software von Allen & Heath; das Skript patcht eine Kopie deiner
eigenen Installation, das Original bleibt unverändert.

**Schnellstart**

1. Command Line Tools installieren: `xcode-select --install`
2. GLD Editor 1.61 von Allen & Heath nach `/Applications` installieren
3. Im Terminal:
   ```sh
   git clone https://github.com/janwiebe05/gld-editor-for-macOS-27.git
   cd gld-editor-for-macOS-27
   ./gld-patch.sh --test
   ```
4. Die gepatchte App heißt `GLD Editor 1.61 (macOS 27).app`.
   Vollbild: grüner Fensterknopf oder Control-Command-F.

Fehlt Rosetta (häufig nach dem Update auf macOS 27), bietet das Skript die Installation an.
Startet die App nicht, erstellt `tools/gld-check.sh "<Pfad zur App>"` einen Bericht auf dem
Schreibtisch.

Ab macOS 28 entfällt Rosetta für normale Apps – dann hilft nur noch eine virtuelle Maschine
mit macOS 27.

## License

MIT for the code in this repository, see [LICENSE](LICENSE).
Third-party components: see [THIRD_PARTY.md](THIRD_PARTY.md).
