#!/bin/sh
# Name: Xournal-Scribe
# Author: Xournal-Scribe
# DontUseFBInk

# Library entry (a "scriptlet", run by sh_integration). The build inserts an
# "# Icon:" line after "# Author:"; sh_integration only reads the first 6 lines.
# Calls run.sh directly rather than `kpm launch`, so a plain USB copy works too.
if [ -f /mnt/us/xournal-scribe/run.sh ]; then
    sh /mnt/us/xournal-scribe/run.sh
else
    eips 2 10 "Xournal-Scribe is not installed in /mnt/us/xournal-scribe"
fi
