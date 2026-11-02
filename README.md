# Xournal-Scribe

A handwriting notes app for jailbroken Kindle Scribes that reads and writes
[Xournal++](https://xournalpp.github.io/) `.xopp` files, so notebooks move freely between
the Kindle and desktop Xournal++.

It isn't a port of the Xournal++ codebase. The Kindle firmware has GTK2 rather than
GTK3, the original Scribe has 1 GB of RAM, and handwriting on e-ink only feels right when
the app controls the panel's fast refresh modes directly. So this is a small C++ app
that draws straight to the e-ink framebuffer (via [FBInk](https://github.com/NiLuJe/FBInk))
and reads the pen from evdev, sharing Xournal++'s file format.

## Status: v0.1

- Pen with pressure, highlighter, whole-stroke eraser (including the pen's eraser end)
- Three sizes per tool, undo/redo, multiple pages (`>` on the last page adds one)
- Opens `.xopp` and `.xoj` files from Xournal++ and the original Xournal. Anything it
  can't edit yet (text, images, LaTeX, PDF backgrounds, unknown attributes) is kept and
  written back unchanged. Text is drawn roughly with a bitmap font; images show as boxes.
- Autosaves 30 s after a change once the pen rests, on sleep and on exit. Saves are atomic.
- Live ink uses the Scribe's DU "pen mode"; once the pen rests the region is redrawn in
  full grayscale.
- Desktop build (SDL3) of the same app for development, with an e-ink look

**Not yet run on a Kindle.** The Kindle build works and needs only `libc` and `libm`
from the firmware (glibc ≤ 2.18). Under QEMU, the ARM test suite passes and ARM renders
of real Xournal++ files are byte-identical to desktop ones. What still needs a real
Scribe: input orientation, pen mode, and the launcher. See
[docs/kindle-notes.md](docs/kindle-notes.md) for what is verified.

## Desktop (development)

```sh
brew install cmake sdl3            # or your Linux distro's packages
cmake -S . -B build
cmake --build build -j
./build/xscribe-tests              # unit tests
./build/xscribe-desktop notes.xopp
```

Left mouse button draws, right button erases. A drawing tablet gives real pressure.
Keys: Cmd/Ctrl+Z undo, Shift+Cmd/Ctrl+Z redo, Cmd/Ctrl+S save, P/H/E switch tools,
1/2/3 set the size, ←/→ change pages, Esc quits. `--no-eink-sim` shows grays while drawing.

`./build/xopp-render file.xopp page.png --page 1` renders a page with the Kindle's
renderer, which is handy for checking files from desktop Xournal++.

## Kindle

### Build

On macOS, with Docker Desktop, OrbStack or colima running:

```sh
scripts/build-kindle-docker.sh
```

On x86_64 Linux with koxtoolchain's `kindlehf` toolchain in `~/x-tools`, run
`scripts/build-kindle.sh` directly. Either way you get:

- `dist/kindle-root/`, the files to copy onto the Kindle (also zipped), and
- `dist/xournal-scribe_<version>_kindlehf.kpkg`, a [KPM](https://github.com/KindleModding/KPM)
  package for publishing in a KPM repository.

This needs a Kindle on firmware 5.16.3 or newer with a current jailbreak, i.e. the
KindleModding stack with scriptlet support. KUAL isn't used: it no longer works.

### Install and first run

1. Connect the Kindle to your computer and copy the **contents** of `dist/kindle-root/`
   onto it. That gives `xournal-scribe/` (the app; your notebooks go here too) and two
   scriptlets in `documents/`. On a Mac, use an MTP app such as OpenMTP.
2. Eject. Two new entries appear in the library. Open **Xournal-Scribe Input Test**,
   write near each corner and tap each corner with a finger. The marks should land under
   the pen tip and your finger. It ends by itself after 30 s. If the marks are off, see
   Troubleshooting below.
3. Open **Xournal-Scribe**. The notebook is `xournal-scribe/notebook.xopp` and the log
   is `xournal-scribe/xscribe.log`.

KPM can't install a local `.kpkg` file, only packages from a repository. Until this
project publishes one, use the copy method above. The package installs to the same
place, so both methods are interchangeable, and upgrading or uninstalling never
touches your notebooks.

The toolbar is along the top: PEN / MARK / ERASE, sizes S / M / L, UNDO / REDO,
`<` page `>`, SAVE, EXIT. Fingers only operate the toolbar, so your palm can rest on the
page. EXIT saves and returns to the Kindle home screen.

### Troubleshooting

- **Marks land in the wrong place:** over SSH, run
  `sh /mnt/us/xournal-scribe/run.sh --probe 30 --pen-transform swap`
  (combine `swap`, `flipx`, `flipy`) until they line up, then add the same flag to the
  `xscribe` line in `run.sh`. Use `--touch-transform` for fingers.
- **Ink lags or the screen flickers:** try `--no-pen-mode`.
- **Stuck:** hold the power button until the Kindle restarts (up to ~40 s). Your notes
  are saved at most 30 s behind.
- `xournal-scribe/bin/xscribe-tests` runs the unit tests on the device.

## Layout

```
src/core/      platform-independent: document model, .xopp I/O, rasterizer, editor, toolbar
src/kindle/    FBInk display, evdev pen/touch, power events, main
src/desktop/   SDL3 front end
tools/         xopp-render
tests/         unit tests (no framework; also build for the Kindle)
kindle/        launcher (run.sh), library scriptlets, KPM hooks, scriptlet cover
scripts/       Kindle cross-build (native Linux or Docker)
docs/          platform research notes
```

## Licensing

The source in this repository is public domain (see [LICENSE](LICENSE)). The Kindle binary
statically links FBInk, which is GPL-3.0-or-later, so **the distributed Kindle build is
GPL-3.0-or-later**. The build script puts a NOTICE and FBInk's license in the package.
Nothing here is copied from Xournal++ (GPL-2.0-or-later); only its file format is
implemented.
