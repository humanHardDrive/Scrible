# Third-party components

Scrible is licensed under the GNU General Public License, version 3 or (at your option)
any later version; see [LICENSE](LICENSE). It is built with the components below, whose
licenses all allow them to be combined into a GPL-3.0-or-later program. The Kindle
package carries a copy of each license text that asks for one, in `licenses/`.

| Component | License | Used in |
|---|---|---|
| [FBInk](https://github.com/NiLuJe/FBInk) © 2018-2024 NiLuJe | GPL-3.0-or-later | Kindle app (statically linked) |
| [pugixml](https://pugixml.org) © 2006-2025 Arseny Kapoulkine | MIT | all builds (statically linked) |
| [zlib](https://zlib.net) © 1995-2022 Jean-loup Gailly and Mark Adler | zlib | all builds (statically linked on the Kindle) |
| [font8x8](https://github.com/dhepper/font8x8) by Daniel Hepper | public domain | all builds (`src/core/Font8x8.h`) |
| [DejaVu Sans](https://dejavu-fonts.github.io) 2.37, © 2003 Bitstream, Inc. (Bitstream Vera), with DejaVu changes in the public domain and glyphs from Arev © 2006 Tavmjong Bah | Bitstream Vera and Arev font licenses (free, GPL-compatible); `licenses/DejaVu-fonts.txt` in the Kindle package | all builds: built into the app, for typed text on the page |
| [stb_truetype](https://github.com/nothings/stb) 1.26 by Sean Barrett | public domain (or MIT, at your choice; used as public domain) | all builds (statically linked) |
| [SDL3](https://libsdl.org) © 1997-2026 Sam Lantinga | zlib | desktop build only (not distributed) |
| [Pen-Based Recognition of Handwritten Digits](https://doi.org/10.24432/C5MG6K), E. Alpaydin & F. Alimoglu, UCI Machine Learning Repository | CC BY 4.0 | all builds: the digit samples in `src/core/DigitSamples.inc`, converted by `tools/gen_digit_samples.py` |
| GCC runtime (libstdc++, libgcc) | GPL-3.0 with the GCC Runtime Library Exception | Kindle app (statically linked) |

FBInk itself contains code from other projects (listed in its
[CREDITS](https://github.com/NiLuJe/FBInk/blob/master/CREDITS)). The parts compiled into
Scrible's minimal FBInk build include font8x8 and stb (public domain), cutef8 (MIT and
ISC), and the qimagescale image scaler (LGPL-3.0, from Qt, with BSD-licensed parts from
Daniel M. Duley and Imlib2). FBInk distributes all of these as part of a
GPL-3.0-or-later library.

The glibc on the Kindle is linked dynamically and isn't shipped with Scrible.

## File format

Scrible reads and writes the file format of [Xournal++](https://xournalpp.github.io)
(`.xopp`, and the older `.xoj`). It contains no Xournal++ code.
