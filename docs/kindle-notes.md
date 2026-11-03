# Kindle Scribe platform notes

What the code relies on, and where each fact came from. Items marked
**unverified** come from research only and need checking on a real device.

## Models

| Model | FBInk platform | Pen mode (`fbink_mtk_toggle_pen_mode`) |
|---|---|---|
| Scribe (2022), Scribe 2 (2024) | Bellatrix3 (MediaTek MT8113, 1 GB RAM) | supported |
| Scribe 3 (2025), Scribe Colorsoft | Bellatrix4 | may fail; the app falls back to plain DU |

Source: `fbink_device_id.c` and `fbink.h` in [FBInk](https://github.com/NiLuJe/FBInk);
TechInsights teardown for the RAM.

## Input devices

Measured on a Scribe (FBInk: "Barolo on Bellatrix3") on 2026-11-02:

| Node | Name | Axes |
|---|---|---|
| event0 | `bd71828-pwrkey` | power button |
| event1, event2 | `bma4xy_acc`, `bma4xy_feature` | accelerometer |
| event3 | `WacomDigitizer` | pen, raw: x 0..15624, y 0..20832, pressure 0..4095 |
| event4 | `pt_mt` | touch, multitouch protocol B: x 0..1859, y 0..2479 |
| event5 | `stylus-custom` | pen, screen space: x 0..1860, y 0..2480, pressure axis 0..4095 |

The numbering differs from [koreader#11156](https://github.com/koreader/koreader/issues/11156)
(older firmware), so devices are found by name and capabilities, not by number.

- **Verified:** `stylus-custom` and `pt_mt` coordinates land under the pen tip and finger
  with no transform, with the framebuffer at rotation 3.
- **Verified:** `stylus-custom` reports pressure 0 on every sample (1,711 samples) despite its
  pressure axis. The app therefore also reads ABS_PRESSURE from `WacomDigitizer` (read-only,
  never grabbed, since the framework reads it to produce `stylus-custom`). If no device
  reports pressure, it falls back to 0.5, the preset width. The log line
  `pressure from …` at exit says which source was used.
- Pen protocol is Wacom-style: BTN_TOOL_PEN (hover), BTN_TOOL_RUBBER (eraser end),
  BTN_TOUCH (contact), BTN_STYLUS/BTN_STYLUS2 (side buttons). The side button
  highlights, as the native notebook's default does; the eraser end erases.
- Pinch zoom relies on hover (BTN_TOOL_PEN) for palm rejection: touches on the page are
  ignored while the pen is in range. **Unverified:** that `stylus-custom` reports hover;
  the log's `pen hover …` line at exit says. Each zoom or pan logs its preview frame
  count, the slowest preview and the final redraw time (`scrible: zoom …`).
  Confirmed on the Scribe: hover is reported; a preview takes ~50 ms to compute and the
  final redraw 10–50 ms, so the panel, not the CPU, sets the pace.
- Zoom/pan previews are near-full-screen DU updates, which take the panel longer than
  the gesture takes to produce the next one. Sent every 120 ms with pen mode on, they
  queued up (sluggish) and left black marks on rule lines after the final redraw. Now
  each preview waits for the previous one to complete (`fbink_wait_for_complete`), any
  other update except ink waits for the last preview, and previews run with pen mode
  off. That got rid of the marks (confirmed on the Scribe). The log reports the waits (`scrible: waited for N previews …`): about 165 ms on
  top of each preview's ~50 ms, i.e. ~4 updates a second with DU.
- Previews are drawn from the page (first versions scaled a snapshot of the screen,
  which left anything panned into view blank). The first preview of a gesture uses DU
  to get the screen to pure black and white; the rest use A2 (~120 ms per FBInk, B&W to
  B&W only). Confirmed on the Scribe: waits fell from ~165 ms to ~40–100 ms per
  preview, no errors, and it felt better. `--no-a2` goes back to DU.
- Palm rejection for pinching: a hand landing on the page made two contacts before the
  pen came near, which zoomed to ~101–103% and flashed. Now fingers must move 50 px
  (spread or midpoint) before anything shows, the pen coming near undoes a gesture in
  progress, three or more contacts are a palm, and a rejected hand is ignored until it
  lifts. Each rejection is logged (`scrible: touch taken for a palm …`). On the Scribe
  this removed the accidental 101–103% zooms; in one session it caught a hand three
  times, once only after a preview had shown (undone with a flash). Good enough for
  now; `kFingerSlop` in Editor.cpp, or a short delay before the first preview, would
  tighten it.

Re-check orientation over SSH with `sh /mnt/us/scrible/run.sh --probe 30`, which marks
where the app sees the pen and fingers; fix it with `--pen-transform` /
`--touch-transform` if needed.

## Display

- 1860×2480 at 300 dpi (1st/2nd gen), 8 bpp grayscale (Y8), line length 1872 bytes,
  rotation 3. The driver is MediaTek `hwtcon_v2`, driven through FBInk's mxcfb-style API.
- Pen ink is drawn without antialiasing on the device. A thin antialiased line is mostly
  gray pixels, which DU turns white while you write, and then the grayscale cleanup
  "fills it in". Users noticed exactly that change.
- Waveforms used: **DU** for ink, the eraser preview, the toolbar and the tools panel
  (fast, black/white only); **GC16, partial** for undo/redo and the cleanup after the
  pen rests; **GC16 + flash** for page changes.
- **DU can't draw gray.** A pixel whose new value is gray keeps showing its old value, and
  the controller records it as updated anyway. A later partial update (which only drives
  pixels whose value changed) won't fix it; only a flashing update does. Erasing with DU
  left black marks wherever erased ink had crossed a rule line (seen on the device).
  Rules that follow from this:
  - Only send DU strictly black/white content. The toolbar never uses gray (disabled
    buttons are checkered).
  - Within one batch, GC16 goes out before DU. The editor sends one rectangle per
    waveform, and the DU one (toolbar plus tools panel) can span page pixels that the
    GC16 one repaints, e.g. where a closing panel uncovers a rule line. Sent first, DU
    would mark those gray pixels done without drawing them; sent after, it finds them
    unchanged and leaves them alone.
  - Erasing shows a black/white version of the erased area right away (rule lines and
    highlights there turn white), then repaints the real content with GC16 once the pen
    rests. Those pixels really change from white to gray then, so the controller redraws
    them. This keeps the eraser as responsive as ink.
  - Undo/redo goes straight to GC16. GC16 rather than GL16, because GL16 assumes pixels
    start out white.

  `EinkPanel` models these rules for the tests and the desktop build.
- "Fast mode" (`fbink_mtk_toggle_auto_reagl(fd, false)`) stops the driver from silently
  upgrading large partial updates to REAGL; KOReader enables it on the Scribe.
- "Pen mode" (`fbink_mtk_toggle_pen_mode`) is the low-latency inking mode. FBInk's docs:
  pen-mode updates *must* be DU, or the driver drops out of pen mode. The app turns it off
  before any non-DU refresh and back on at the next stroke. **Unverified:** latency and
  whether toggling is visible.

## Launching: scriptlets and KPM

KUAL is obsolete on current jailbreaks; the [Kindle Modding Wiki](https://kindlemodding.org/jailbreaking/whats-next/installing-homebrew.html)
says it no longer works. Apps now appear as **scriptlets**: `.sh` files in
`/mnt/us/documents` that the library lists like books.
[sh_integration](https://github.com/KindleModding/sh_integration) indexes and runs them:

- Only the **first 6 lines** are scanned for headers: `# Name:`, `# Author:`, `# Icon:`
  (an absolute path or a `data:image/png;base64,…` URI), `# DontUseFBInk` and `# UseHooks`.
  Library covers are cover-sized; KOReader's is 938×1432.
- Tapping a scriptlet runs `su -c "sh -l <script>"` as root, from a framework app that
  sh_integration registers. Without `# DontUseFBInk`, stdout and stderr are piped onto the
  screen through FBInk, which would draw over us. When the script exits, sh_integration
  stops its app and runs `xrefresh`. If appmgrd unloads it, it `kill -9`s the script.

[KPM](https://github.com/KindleModding/KPM) packages (`.kpkg`) are gzipped tarballs with
a `manifest.json` (v2) and optional `install.sh`, `launch.sh` and `uninstall.sh` hooks,
run with `sh` from the unpacked folder `/mnt/us/kmc/kpm/packages/<id>`. Packages install
their own scriptlets. `kpm launch <id>` runs `launch.sh`. The CLI installs only from
repositories (`;kpm add-repo URL`, then `;kpm install <id>`), not from local files.

We copy KOReader's KPM package layout. `install.sh` puts the app in a user-visible folder
(`/mnt/us/scrible`, like `/mnt/us/koreader`) and copies the scriptlets. Our
scriptlets call `run.sh` there directly instead of `kpm launch`, so copying the files over
USB works the same as a KPM install.

## Running alongside the Kindle UI

[KOReader's launcher](https://github.com/koreader/koreader/blob/master/platform/kindle/koreader.sh)
does this on firmware 5.x, still when started from its KPM scriptlet
(`kpm launch koreader --asap`). `run.sh` follows the same steps:

1. `lipc-set-prop com.lab126.pillow disableEnablePillow disable`
2. `killall -STOP awesome` (otherwise the clock keeps redrawing)
3. `stop statusbar` if `/etc/upstart/statusbar.conf` exists (newer firmware)
4. On exit, reverse all of it, restore the saved framebuffer and start the home booklet.

The app also grabs the pen and touch devices (EVIOCGRAB) so the framework doesn't react to
them. Because of that, the launcher sets `preventScreenSaver` so the device doesn't go idle
while you write. Sleep and wake come from `lipc-wait-event -m -s 0 com.lab126.powerd
goingToScreenSaver,outOfScreenSaver`: the app saves on sleep and redraws on wake.
**Unverified:** the `lipc-wait-event` flags, and how the stock screensaver behaves while
awesome is stopped.

## Toolchain

[koxtoolchain](https://github.com/koreader/koxtoolchain) `kindlehf` (release 2026.08) covers
any Kindle on firmware 5.16.3 or newer. It ships `arm-kindlehf-linux-gnueabihf` GCC 14.4.0
with glibc 2.20 and Linux 4.1.52 headers. libstdc++ is linked statically because the
firmware's copy is older. Prebuilt toolchains are x86_64 Linux only, hence the Docker build
on macOS.

Everything is compiled with `_FILE_OFFSET_BITS=64`. On 32-bit ARM, `stat()`/`readdir()`
otherwise fail with EOVERFLOW on filesystems with large inode numbers, and the notebook list
silently came up empty under QEMU.

The ARM tests can run under QEMU with Debian's `libc6-armhf-cross` (`qemu-arm -L
/usr/arm-linux-gnueabihf …`). The toolchain sysroot's own `ld.so` can't execute.

## Xournal++ file format (as implemented)

From Xournal++'s `SaveHandler.cpp` / `XmlParser.cpp`:

- `.xopp` is gzip-compressed XML. Xournal++ 1.3.x still writes this. It can also *read* a
  2019-era zip-packaged variant, which this app rejects with a clear error.
- `<stroke width="W w0 w1 …">`: W is the nominal width, then one absolute width per segment
  (n−1 values for n points). Strokes with fewer than 2 points are dropped on load, so dots
  are stored as two equal points.
- Colors are `#rrggbbaa`. Stroke alpha is implied by the tool: `ff` for pen, `7f` for
  highlighter.
- `<background type="solid" color="…" style="…" config="…"/>`, per page. `style` is
  `plain`, `ruled`, `lined` (ruled with a margin line), `graph`, `dotted`, …. `config`
  holds comma-separated `key=value` settings; we use `r1` (line or square spacing, pt)
  and `m1` (the margin line's x; negative counts from the right edge). Defaults: 24pt
  ruling, 14.17pt (5 mm) squares, margin at 72pt. Ruled lines start 80pt down and stop
  60pt above the bottom. Xournal++ only writes `config` when it isn't empty, and so do
  we. Scrible adds `scrible-full=1` for ruled lines without the blank header and footer
  (lines from one spacing below the top to half a spacing above the bottom); Xournal++
  ignores the key and draws its usual header and footer. From `view/background/*BackgroundView.cpp` and `model/BackgroundConfig.h`.
- `fileversion` 4 is the classic layout. Version 5 adds `matrix=` transforms on text and
  images. The app never lowers the version of a file it loaded.
- A missing `fileversion` means the file came from the original Xournal; Xournal++ then
  forces "Save As". We always write one.
