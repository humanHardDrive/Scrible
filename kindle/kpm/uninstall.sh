#!/bin/sh
# KPM uninstall hook (also run with "upgrade" before an upgrade). Removes the
# scriptlets and app files, never the notebooks in /mnt/us/scrible.

# Only delete scriptlets that are still ours.
for s in ./scriptlets/*.sh; do
    t="/mnt/us/documents/$(basename "${s}")"
    if [ -f "${t}" ] && [ "$(md5sum <"${t}")" = "$(md5sum <"${s}")" ]; then
        rm -f "${t}"
    fi
done

if [ "$1" != "upgrade" ]; then
    APP=/mnt/us/scrible
    rm -rf "${APP}/bin" "${APP}/licenses"
    rm -f "${APP}/run.sh" "${APP}/LICENSE" "${APP}/NOTICE" "${APP}/THIRD-PARTY-NOTICES.md"
fi
exit 0
