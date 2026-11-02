#!/bin/sh
# Starts Xournal-Scribe. Normally run by the library scriptlets in /mnt/us/documents
# (via sh_integration, as root) or by `kpm launch xournal-scribe`.
# Gets the Kindle UI out of the way (the same steps KOReader takes on firmware 5.x),
# runs the app, then puts everything back. Extra arguments go to the app,
# e.g. `run.sh --probe 30`.

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
NOTEBOOK="${APP_DIR}/notebook.xopp"
LOG="${APP_DIR}/xscribe.log"
FB_DUMP="/var/tmp/xscribe-fb.dump"

if [ -f "${LOG}" ]; then
    tail -c 200000 "${LOG}" >"${LOG}.tmp" && mv -f "${LOG}.tmp" "${LOG}"
fi
exec >>"${LOG}" 2>&1
echo "=== $(date) starting $*"

STATUSBAR_STOPPED=no
RESTORED=no
restore() {
    [ "${RESTORED}" = "yes" ] && return
    RESTORED=yes
    [ "${STATUSBAR_STOPPED}" = "yes" ] && start statusbar
    killall -CONT awesome
    lipc-set-prop com.lab126.powerd preventScreenSaver 0
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
killall -STOP awesome
# On newer firmware the status bar is a separate job that would draw over us.
if [ -f /etc/upstart/statusbar.conf ]; then
    stop statusbar && STATUSBAR_STOPPED=yes
fi
# We grab the touch and pen devices, so the framework can't see activity; don't
# let it decide we're idle and sleep mid-sentence. The power button still works.
lipc-set-prop com.lab126.powerd preventScreenSaver 1

# Copying over USB/MTP can drop the executable bit.
chmod +x "${APP_DIR}/bin/xscribe" 2>/dev/null
"${APP_DIR}/bin/xscribe" "${NOTEBOOK}" "$@"
echo "=== exited with status $?"

restore
