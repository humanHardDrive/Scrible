#!/bin/sh
# Starts Scrible. Normally run by the library scriptlet in /mnt/us/documents
# (via sh_integration, as root) or by `kpm launch scrible`.
# Gets the Kindle UI out of the way (the same steps KOReader takes on firmware 5.x),
# runs the app, then puts everything back. Extra arguments go to the app,
# e.g. `run.sh --probe 30` (over SSH) to check pen and touch input.

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
NOTEBOOKS="${APP_DIR}/notebooks"
LOG="${APP_DIR}/scrible.log"
FB_DUMP="/var/tmp/scrible-fb.dump"

if [ -f "${LOG}" ]; then
    tail -c 200000 "${LOG}" >"${LOG}.tmp" && mv -f "${LOG}.tmp" "${LOG}"
fi
exec >>"${LOG}" 2>&1
echo "=== $(date) starting $*"

# Before the rename the app was Xournal-Scribe, in /mnt/us/xournal-scribe. Bring its
# notebooks over (and version 0.1's single notebook.xopp) and remove the old app;
# anything whose name is already taken here stays behind, so nothing is overwritten.
OLD_DIR=/mnt/us/xournal-scribe
move_if_free() {
    [ -e "$1" ] && [ ! -e "$2" ] && mv "$1" "$2"
}
if [ -d "${OLD_DIR}" ] && [ "${OLD_DIR}" != "${APP_DIR}" ]; then
    echo "moving notebooks over from ${OLD_DIR}"
    mkdir -p "${NOTEBOOKS}/Trash"
    move_if_free "${OLD_DIR}/notebooks/.xournal-scribe" "${NOTEBOOKS}/.scrible"
    rm -f "${OLD_DIR}/notebooks/.xournal-scribe"
    for f in "${OLD_DIR}/notebooks/Trash/"* "${OLD_DIR}/notebooks/Trash/".[!.]*; do
        move_if_free "${f}" "${NOTEBOOKS}/Trash/$(basename "${f}")"
    done
    rmdir "${OLD_DIR}/notebooks/Trash" 2>/dev/null
    for f in "${OLD_DIR}/notebooks/"* "${OLD_DIR}/notebooks/".[!.]* "${OLD_DIR}/notebook.xopp"; do
        move_if_free "${f}" "${NOTEBOOKS}/$(basename "${f}")"
    done
    rmdir "${NOTEBOOKS}/Trash" 2>/dev/null
    for s in /mnt/us/documents/xournal-scribe.sh /mnt/us/documents/xournal-scribe-input-test.sh; do
        grep -q "${OLD_DIR}/run.sh" "${s}" 2>/dev/null && rm -f "${s}"
    done
    rm -rf "${OLD_DIR}/bin"
    rm -f "${OLD_DIR}/run.sh" "${OLD_DIR}/LICENSE" "${OLD_DIR}/LICENSE.FBInk" "${OLD_DIR}/NOTICE" \
        "${OLD_DIR}/xscribe.log" "${OLD_DIR}/xscribe.log.tmp"
    rmdir "${OLD_DIR}/notebooks" "${OLD_DIR}" 2>/dev/null
fi

# Versions before 1.0 also had an "Input Test" library entry; remove it if it's still ours.
INPUT_TEST=/mnt/us/documents/scrible-input-test.sh
grep -q "${APP_DIR}/run.sh --probe" "${INPUT_TEST}" 2>/dev/null && rm -f "${INPUT_TEST}"

STATUSBAR_STOPPED=no
RESTORED=no
restore() {
    [ "${RESTORED}" = "yes" ] && return
    RESTORED=yes
    [ "${STATUSBAR_STOPPED}" = "yes" ] && start statusbar
    killall -CONT awesome
    lipc-set-prop com.lab126.powerd preventScreenSaver 0  # set by versions before 1.1
    # Leave something sensible on screen until the home screen redraws.
    if [ -f "${FB_DUMP}" ]; then
        cat "${FB_DUMP}" >/dev/fb0
        rm -f "${FB_DUMP}"
    fi
    lipc-set-prop com.lab126.pillow disableEnablePillow enable
    lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home
}
trap 'restore; exit 1' HUP INT TERM

cat /dev/fb0 >"${FB_DUMP}"
lipc-set-prop com.lab126.pillow disableEnablePillow disable
# Disabling pillow isn't enough to stop the clock from redrawing; freeze the window manager.
# (The app lets it run while the Kindle shows its sleep screen, which awesome draws.)
killall -STOP awesome
export SCRIBLE_AWESOME_STOPPED=yes
# On newer firmware the status bar is a separate job that would draw over us.
if [ -f /etc/upstart/statusbar.conf ]; then
    stop statusbar && STATUSBAR_STOPPED=yes
fi
# We grab the touch and pen devices, so the framework can't see activity: the app
# resets powerd's idle timer while you write (as KOReader does). preventScreenSaver
# would also keep the power button from putting the Kindle to sleep.

# Copying over USB/MTP can drop the executable bit.
chmod +x "${APP_DIR}/bin/scrible" 2>/dev/null
"${APP_DIR}/bin/scrible" "${NOTEBOOKS}" "$@"
echo "=== exited with status $?"

restore
