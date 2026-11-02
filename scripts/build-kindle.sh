#!/usr/bin/env bash
# Cross-compiles Xournal-Scribe for the Kindle and packages it in dist/, both as
# files to copy onto the Kindle and as a KPM package. Needs Linux x86_64 with
# koxtoolchain's kindlehf toolchain; on macOS, run scripts/build-kindle-docker.sh.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TRIPLE=arm-kindlehf-linux-gnueabihf
TC="${XS_KINDLE_TC:-$HOME/x-tools/$TRIPLE}"
export PATH="$TC/bin:$PATH"
if ! command -v "$TRIPLE-gcc" >/dev/null; then
    echo "error: $TRIPLE-gcc not found. Install koxtoolchain's kindlehf toolchain or set XS_KINDLE_TC." >&2
    exit 1
fi

FBINK_REV=886f25f13368859ad8a899b88d04c26e19cda32e
ZLIB_VER=1.3.1
ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23

WORK="$ROOT/build-kindle"
DEPS="$WORK/deps"
STAGE="$WORK/stage"
JOBS="$(nproc 2>/dev/null || echo 4)"
mkdir -p "$DEPS" "$STAGE"

# zlib, static (the firmware has one, but the toolchain's sysroot doesn't)
if [ ! -f "$STAGE/lib/libz.a" ]; then
    cd "$DEPS"
    curl -fsSLO "https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz"
    echo "$ZLIB_SHA256  zlib-$ZLIB_VER.tar.gz" | sha256sum -c -
    rm -rf "zlib-$ZLIB_VER" && tar xzf "zlib-$ZLIB_VER.tar.gz"
    cd "zlib-$ZLIB_VER"
    CHOST="$TRIPLE" CC="$TRIPLE-gcc" CFLAGS="-O2" ./configure --static --prefix="$STAGE"
    make -j"$JOBS" && make install
fi

# FBInk, static. MINIMAL + IMAGE is what we use (raw pixel blits); BITMAP matches
# upstream's "small" preset.
if [ ! -f "$DEPS/FBInk/Release/libfbink.a" ]; then
    cd "$DEPS"
    [ -d FBInk ] || git clone https://github.com/NiLuJe/FBInk.git
    cd FBInk
    git checkout -q "$FBINK_REV"
    # Only what this build uses: font8x8 (BITMAP), stb (IMAGE), and i2c-tools,
    # which FBInk's Makefile builds for every target.
    git submodule update --init font8x8 stb i2c-tools
    make -j"$JOBS" staticlib KINDLE=true MINIMAL=true BITMAP=true IMAGE=true CROSS_TC="$TRIPLE"
fi

cmake -S "$ROOT" -B "$WORK/build" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/kindlehf.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DXS_BUILD_KINDLE=ON -DXS_BUILD_DESKTOP=OFF -DXS_BUILD_TESTS=ON \
    -DFBINK_DIR="$DEPS/FBInk" \
    -DZLIB_INCLUDE_DIR="$STAGE/include" -DZLIB_LIBRARY="$STAGE/lib/libz.a"
cmake --build "$WORK/build" -j"$JOBS"

# ---- Packaging ----
VERSION="$(sed -n 's/^project(XournalScribe VERSION \([0-9.]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
IFS=. read -r V_MAJOR V_MINOR V_PATCH <<<"$VERSION"
ICON="data:image/png;base64,$(base64 <"$ROOT/kindle/cover.png" | tr -d '\n')"

# dist/kindle-root/ mirrors the Kindle's storage (/mnt/us): copy its contents over USB.
DIST="$ROOT/dist"
APP="$DIST/kindle-root/xournal-scribe"
DOCS="$DIST/kindle-root/documents"
rm -rf "$DIST"
mkdir -p "$APP/bin" "$DOCS"
cp "$ROOT/kindle/run.sh" "$APP/"
for bin in xscribe xscribe-tests; do
    "$TRIPLE-strip" -o "$APP/bin/$bin" "$WORK/build/$bin"
done
cp "$ROOT/LICENSE" "$APP/LICENSE"
cp "$DEPS/FBInk/LICENSE" "$APP/LICENSE.FBInk"
cat >"$APP/NOTICE" <<'EOF'
Xournal-Scribe's own source code is dedicated to the public domain (see LICENSE).
The xscribe binary statically links FBInk (https://github.com/NiLuJe/FBInk),
which is licensed under the GNU GPL v3 or later (see LICENSE.FBInk), so the
binary as a whole is distributed under the GPL v3 or later.
EOF
# Library entries; sh_integration reads headers from the first 6 lines only.
for s in "$ROOT"/kindle/scriptlets/*.sh; do
    awk -v icon="# Icon: $ICON" '{ print } /^# Author:/ { print icon }' "$s" >"$DOCS/$(basename "$s")"
done
(cd "$DIST/kindle-root" && zip -qr "$DIST/xournal-scribe-$VERSION-kindle-root.zip" .)

# KPM package: a gzipped tarball of the package folder, as kpm-helper.py packs it.
PKG="$WORK/kpkg"
rm -rf "$PKG"
mkdir -p "$PKG"
cp "$ROOT"/kindle/kpm/*.sh "$PKG/"
cp -R "$APP" "$PKG/app"
cp -R "$DOCS" "$PKG/scriptlets"
cat >"$PKG/manifest.json" <<EOF
{
  "manifest_version": 2,
  "id": "xournal-scribe",
  "name": "Xournal-Scribe",
  "author": "Xournal-Scribe",
  "description": "Handwritten notes, compatible with Xournal++ (.xopp)",
  "version": [$V_MAJOR, $V_MINOR, $V_PATCH],
  "dependencies": [],
  "supported_platforms": ["kindlehf"]
}
EOF
(cd "$PKG" && tar -czf "$DIST/xournal-scribe_${VERSION}_kindlehf.kpkg" -- *)

file "$APP/bin/xscribe"
echo "Built:"
echo "  $DIST/kindle-root/  (copy onto the Kindle; also zipped as xournal-scribe-$VERSION-kindle-root.zip)"
echo "  $DIST/xournal-scribe_${VERSION}_kindlehf.kpkg  (KPM package, for a KPM repository)"
