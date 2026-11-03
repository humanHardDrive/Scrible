# Scrible

A handwriting notes app for jailbroken Kindle Scribes that reads and writes
[Xournal++](https://xournalpp.github.io/) `.xopp` files, so notebooks move freely between
the Kindle and desktop Xournal++.

It isn't a port of the Xournal++ codebase. The Kindle firmware has GTK2 rather than
GTK3, the original Scribe has 1 GB of RAM, and handwriting on e-ink only feels right when
the app controls the panel's fast refresh modes directly. So this is a small C++ app
that draws straight to the e-ink framebuffer (via [FBInk](https://github.com/NiLuJe/FBInk))
and reads the pen from evdev, sharing Xournal++'s file format.

## Status: v0.2 in progress

- A notebook list: name notebooks with an on-screen keyboard; rename, duplicate,
  delete (into a Trash folder); sort by date or by name
- Lasso selection: move, resize, duplicate and delete groups of strokes
- Paper for each page: blank, ruled (three spacings, with or without a margin line) or
  grid (three sizes), stored the way Xournal++ stores it
- Shape recognition, as in Xournal: lines, triangles, quadrilaterals, rectangles,
  circles and ellipses, plus polygons drawn as separate sides and arrows (a line, then
  a "V" at its end)
- Pen with pressure, highlighter, and an eraser (also the pen's eraser end) that either
  cuts away just the ink it passes over, like Xournal++'s standard eraser, or removes
  whole strokes
- Three sizes per tool, undo/redo, multiple pages (`>` on the last page adds one;
  PAGE deletes one)
- Opens `.xopp` and `.xoj` files from Xournal++ and the original Xournal. Anything it
  can't edit yet (text, images, LaTeX, PDF backgrounds, unknown attributes) is kept and
  written back unchanged. Text is drawn roughly with a bitmap font; images show as boxes.
- Autosaves 30 s after a change once the pen rests, on sleep and on exit. Saves are atomic.
- Live ink uses the Scribe's DU "pen mode"; once the pen rests the region is redrawn in
  full grayscale.
- Desktop build (SDL3) of the same app for development, with an e-ink look

Runs on a Kindle Scribe (2022): pen and touch line up and writing works. The Kindle
build needs only `libc` and `libm` from the firmware (glibc ≤ 2.18), and the test suite
also passes on ARM under QEMU. See [docs/kindle-notes.md](docs/kindle-notes.md) for what
has been verified on the device.

## Desktop (development)

```sh
brew install cmake sdl3            # or your Linux distro's packages
cmake -S . -B build
cmake --build build -j
./build/scrible-tests              # unit tests
./build/scrible-desktop            # notebook list in ~/Documents/Scrible
```

Pass a folder to list the notebooks in it, or a `.xopp` file to open just that one.
Left mouse button draws, right button erases. A drawing tablet gives real pressure.
Keys: Cmd/Ctrl+Z undo, Shift+Cmd/Ctrl+Z redo, Cmd/Ctrl+S save, P/H/E/L switch tools,
R switches shape recognition, 1/2/3 set the tool's size, Delete removes the selection,
←/→ change pages, N new notebook (in the list), Enter confirms, Esc goes back or quits.
`--no-eink-sim` shows antialiased ink and no e-ink refresh artifacts.

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
- `dist/scrible_<version>_kindlehf.kpkg`, a [KPM](https://github.com/KindleModding/KPM)
  package for publishing in a KPM repository.

This needs a Kindle on firmware 5.16.3 or newer with a current jailbreak, i.e. the
KindleModding stack with scriptlet support. KUAL isn't used: it no longer works.

### Install and first run

1. Connect the Kindle to your computer and copy the **contents** of `dist/kindle-root/`
   onto it. That gives `scrible/` (the app; your notebooks go here too) and two
   scriptlets in `documents/`. On a Mac, use an MTP app such as OpenMTP.
2. Eject. Two new entries appear in the library. Open **Scrible Input Test**,
   write near each corner and tap each corner with a finger. The marks should land under
   the pen tip and your finger. It ends by itself after 30 s. If the marks are off, see
   Troubleshooting below.
3. Open **Scrible**. It starts on the list of notebooks, which are the `.xopp` and
   `.xoj` files in `scrible/notebooks/`. Copy files from desktop Xournal++ there to
   open them on the Kindle. The log is `scrible/scrible.log`.

Upgrading from Xournal-Scribe, this app's old name: on the first start, Scrible moves the
notebooks from `xournal-scribe/notebooks/` (or a version 0.1 `notebook.xopp`) into
`scrible/notebooks/`, removes the old app and its two library entries, and deletes the
`xournal-scribe` folder once it's empty. A notebook whose name is already taken in
`scrible/notebooks/` is left where it was.

KPM can't install a local `.kpkg` file, only packages from a repository. Until this
project publishes one, use the copy method above. The package installs to the same
place, so both methods are interchangeable, and upgrading or uninstalling never
touches your notebooks.

In the list:

- Tap a notebook to open it.
- NEW asks for a name on an on-screen keyboard; leave it empty to use the date and time.
- NEWEST / A-Z switches the order, and the choice is remembered.
- Each notebook's "..." button offers RENAME, DUPLICATE and DELETE. Deleted notebooks
  go into `notebooks/Trash/`, where you can recover them over USB.
- Files Xournal++ keeps next to a notebook are renamed, copied and deleted along with
  it (e.g. `Name.xopp.bg.pdf` for an annotated PDF).
- `<` `>` page through a long list, and EXIT returns to the Kindle home screen.

In a notebook the toolbar along the top has UNDO / REDO, PAGE, `<` page `>`, SAVE,
BACK and EXIT. The drawing tools are tucked away behind the arrow tab at the top-left of the
page. Tap it to open a column of PEN, SHAPE, LASSO, MARK (highlighter) and ERASE. PEN,
MARK and ERASE show their sizes beside the column, and each keeps its own. Picking one
puts the tools away again, and so does a finger tap on the page or writing on it.
Fingers only operate the toolbar, the tools and the selection buttons, so your palm can
rest on the page.

- LASSO: draw a loop around strokes to select those mostly inside it. Drag inside the
  dashed frame to move them, or drag the corner handle to resize (line widths scale
  too). DELETE and DUPLICATE act on the selection, and DONE or a tap elsewhere lets go.
  Each change is one UNDO. It works on the top layer's strokes; text and images from
  desktop Xournal++ can't be selected yet.

- SHAPE switches shape recognition on or off; its box is filled while it's on. With it
  on, strokes that look like a line, triangle, rectangle, circle and so on are replaced
  by a clean version. Nearly horizontal or vertical lines are straightened, and
  right-angled four-sided shapes become exact rectangles. Separate lines that close up
  become one polygon, and a line followed by a "V" at one end becomes an arrow. Anything
  else (handwriting, mostly) stays as drawn. If a shape was guessed wrong, one UNDO
  brings back what you drew.
- ERASE also offers PARTIAL, which cuts away just what you erase, and WHOLE, which
  removes whole strokes. The pen's eraser end follows the same setting.
- Holding the pen's side button highlights, whichever tool is selected, as in the
  Kindle's own notebooks. It uses MARK's size.
- These choices and each tool's size are remembered.
- PAGE sets this page's paper: BLANK, RULED or GRID; how far apart the lines or squares
  are (NARROW / MEDIUM / WIDE, or SMALL / MEDIUM / LARGE); and for ruled paper, whether
  it has a margin line. The page changes as you choose. Tap PAGE again, or anywhere
  else, to close the menu; one UNDO undoes everything you changed in it. A new page
  starts with the paper of the page before it. Pages with a PDF or image background
  keep it.
- DELETE PAGE, at the bottom of that menu, asks for a second tap before it deletes the
  page; UNDO brings it back. A notebook's only page can't be deleted.
- BACK saves and returns to the list; EXIT saves and leaves the app.

### Troubleshooting

- **Marks land in the wrong place:** over SSH, run
  `sh /mnt/us/scrible/run.sh --probe 30 --pen-transform swap`
  (combine `swap`, `flipx`, `flipy`) until they line up, then add the same flag to the
  `scrible` line in `run.sh`. Use `--touch-transform` for fingers.
- **Ink lags or the screen flickers:** try `--no-pen-mode`.
- **Stuck:** hold the power button until the Kindle restarts (up to ~40 s). Your notes
  are saved at most 30 s behind.
- `scrible/bin/scrible-tests` runs the unit tests on the device.

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

Copyright (C) 2026 Sam Stratter. Scrible is free software, licensed under the GNU General
Public License version 3 or (at your option) any later version; see [LICENSE](LICENSE).
GPL-3.0-or-later is what FBInk, which the Kindle app links statically, requires; the other
components (pugixml, zlib, font8x8, SDL3) are all GPL-compatible. They're listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

`scripts/build-kindle.sh` puts the license, a NOTICE and the third-party license texts in
the package, and writes `dist/scrible-<version>-source.tar.gz` next to it. If you share a
build, share that source archive with it (or point to a public copy of this repository by
setting `SCRIBLE_SOURCE_URL` or a git remote, and the NOTICE will name it).

Scrible contains no Xournal++ code; it implements Xournal++'s file format.
