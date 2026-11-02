#!/bin/sh
# KPM install hook; runs inside the unpacked package. Like KOReader, the app
# itself lives in a user-visible folder, /mnt/us/xournal-scribe, next to the
# notebooks (which an install or upgrade never touches).
set -e

APP=/mnt/us/xournal-scribe
mkdir -p "${APP}"
cp -rf ./app/* "${APP}/"
rm -rf ./app

for s in ./scriptlets/*.sh; do
    cp -f "${s}" /mnt/us/documents/
done
echo "Installed to ${APP}"
