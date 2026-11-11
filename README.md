# Scrible

A handwriting notes app for jailbroken Kindle Scribes that reads and writes
[Xournal++](https://xournalpp.github.io/) `.xopp` files, so notebooks move freely between
the Kindle and desktop Xournal++.

It isn't a port of the Xournal++ codebase. The Kindle firmware has GTK2 rather than
GTK3, the original Scribe has 1 GB of RAM, and handwriting on e-ink only feels right when
the app controls the panel's fast refresh modes directly. So this is a small C++ app
that draws straight to the e-ink framebuffer (via [FBInk](https://github.com/NiLuJe/FBInk))
and reads the pen from evdev, sharing Xournal++'s file format.

## Features (1.1, in development)

- A notebook list: name notebooks with an on-screen keyboard; rename, duplicate,
  delete (into a Trash folder); sort by date or by name
- Selection: move, resize, duplicate and delete groups of strokes and texts, or read the numbers
  written in them and get their sum, average, minimum, maximum or count
- Paper for each page: blank, ruled (three spacings, with or without a margin line) or
  grid (three sizes), stored the way Xournal++ stores it
- Shape recognition, as in Xournal: lines, triangles, quadrilaterals, rectangles,
  circles and ellipses, plus polygons drawn as separate sides and arrows (a line, then
  a "V" at its end)
- Pen with pressure, highlighter (also the pen's side button), and an eraser (also the
  pen's eraser end) that either cuts away just the ink it passes over, like Xournal++'s
  standard eraser, or removes whole strokes
- Solid, dotted or dashed lines for the pen (shapes too), saved the way Xournal++ saves
  them, so they look the same on the desktop; dotted and dashed lines from Xournal++
  show as they are
- Typed text: tap the page with TEXT and type on an on-screen keyboard, or tap a text
  (from Scrible or desktop Xournal++) to edit it. Drawn in DejaVu Sans, built in, which
  is Xournal++'s usual "Sans" on Linux
- Drawing tools tucked into a panel at the edge of the page, out of the way until tapped
- Zoom up to 400%: pinch with two fingers, or use the panel's buttons and PAN tool
- Three sizes per tool, undo/redo, multiple pages (`>` on the last page adds one;
  PAGE deletes one)
- Opens `.xopp` and `.xoj` files from Xournal++ and the original Xournal. Anything it
  can't edit yet (images, LaTeX, PDF backgrounds, unknown attributes) is kept and
  written back unchanged. Text is drawn in Sans whatever its font; images show as boxes.
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
the wheel or trackpad moves the page and zooms with Cmd/Ctrl,
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
   onto it. That gives `scrible/` (the app; your notebooks go here too) and a
   scriptlet in `documents/`. On a Mac, use an MTP app such as OpenMTP.
2. Eject, and open **Scrible**, which is now in the library. It starts on the list of
   notebooks, which are the `.xopp` and `.xoj` files in `scrible/notebooks/`. Copy files
   from desktop Xournal++ there to open them on the Kindle. The log is
   `scrible/scrible.log`.

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
- ABOUT shows the version and exactly which build this is (the git commit it was built
  from, or the release tag), the device, and the license.

In a notebook the toolbar along the top has UNDO / REDO, the notebook's name
(shortened to fit; tap it to see it whole across the bar, and tap the bar to go back),
PAGE, `<` page `>`, SAVE, BACK and EXIT. The drawing tools are tucked away behind the arrow tab at the top-left of the
page. Tap it to open a column of PEN, SHAPE, SELECT, MARK (highlighter), ERASE, TEXT and
PAN, then zoom buttons. PEN, MARK, ERASE and TEXT show their sizes beside the column, and
each keeps its own; PEN also has SOLID, DOTS and DASHES below its sizes. Picking one
puts the tools away again, and so does a finger tap on the page or writing on it.
Fingers only operate the toolbar, the tools and the selection buttons, so your palm can
rest on the page. Buttons invert for a moment when pressed, to show they were hit.

- SELECT: draw a loop around strokes and texts to select those mostly inside it. Drag
  inside the dashed frame to move them, or drag the corner handle to resize (line widths
  and text sizes scale too). The selection's menu has EDIT and MATH. Tapping one puts it at the start of
  the row, with an up arrow, followed by its options; tap it again to go back. A tap
  elsewhere lets go of the selection. Each change is one UNDO. It
  works on the top layer's strokes and texts; images from desktop Xournal++ can't be
  selected yet.
- EDIT: DELETE and DUPLICATE.
- MATH: SUM, AVERAGE, MIN, MAX and COUNT. Select a list of numbers you've written (one
  per line, or several on a line with a wide gap between; on any paper, blank too), and
  Scrible reads them and puts the answer on the page below them, as text. Typed texts
  in the selection count too, read exactly as typed rather than recognized: each line
  that starts with a number is one ("12 apples" is 12), and other lines, like headings,
  are left out. Earlier answers are typed text too, so subtotals can be added up. Drag it
  anywhere with the pen (any tool but the eraser, which erases it). The little i beside it shows the numbers it was
  worked out from, as they were read; tap a character that was misread to correct it,
  and the answer follows. DELETE there removes the answer. It knows digits, decimal
  points, minus signs and thousands commas. Corrected digits become examples of your
  handwriting (kept in `notebooks/.scrible-digits`), so it reads you better as you go.
  Reading happens on the Kindle, from the strokes, by comparison with handwriting
  samples from 44 people (UCI's "Pen-Based Recognition of Handwritten Digits", CC BY
  4.0). In desktop Xournal++ the answer is ordinary text.

- SHAPE switches shape recognition on or off; its box is filled while it's on. With it
  on, strokes that look like a line, triangle, rectangle, circle and so on are replaced
  by a clean version. Nearly horizontal or vertical lines are straightened, and
  right-angled four-sided shapes become exact rectangles. Separate lines that close up
  become one polygon, and a line followed by a "V" at one end becomes an arrow. Anything
  else (handwriting, mostly) stays as drawn. If a shape was guessed wrong, one UNDO
  brings back what you drew.
- ERASE also offers PARTIAL, which cuts away just what you erase, and WHOLE, which
  removes whole strokes. The pen's eraser end follows the same setting. Text goes
  whole as soon as the eraser touches it.
- TEXT: tap the page with the pen where the text should go, and a keyboard comes up
  along the bottom of the screen (or the top, if the text is low on the page). Type
  with your fingers or the pen; SHIFT gives one capital, ?123 the symbols, and ENTER a
  new line. Lines that reach the page's right edge break by themselves. Tap in the text
  to move the caret, tap a text to edit it, or tap elsewhere to start another. Drag a
  text with the pen to move it, with any tool but the eraser (with TEXT, a tap types in
  it and a drag moves it, even while you're typing it); each move is one UNDO. DONE (or
  any other tool) finishes; a text left empty goes. Each text you type or edit is one
  UNDO. Its sizes are 10, 14 and 20 pt; picking one while typing resizes that text.
  Texts from desktop Xournal++ keep their font and color in the file.
- Holding the pen's side button highlights, whichever tool is selected, as in the
  Kindle's own notebooks. It uses MARK's size.
- These choices and each tool's size are remembered.
- Zooming: pinch with two fingers to zoom (up to 400%) and move the page, with the pen
  lifted away from the screen, since a hand holding the pen near the page is taken for
  a resting palm. Fingers have to move a few millimetres before anything happens, so a
  hand coming down on the page doesn't zoom; three or more contacts are a palm too. Or use the panel: `-` and `+` zoom in steps, FIT goes back to the
  whole page, and with PAN selected, dragging with the pen or a finger moves the page.
  While the page moves you see a quick black-and-white preview; when you let go it's
  redrawn properly, with a flash. Turning the page goes back to the whole page.
- PAGE sets this page's paper: BLANK, RULED or GRID; how far apart the lines or squares
  are (NARROW / MEDIUM / WIDE, or SMALL / MEDIUM / LARGE); and for ruled paper, whether
  it has a margin line, and whether the top and bottom are left blank (untick BLANK TOP
  & BOTTOM for lines all the way down; desktop Xournal++ still shows them blank). The
  page changes as you choose. Tap PAGE again, or anywhere
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
