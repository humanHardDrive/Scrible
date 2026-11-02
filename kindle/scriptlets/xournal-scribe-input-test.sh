#!/bin/sh
# Name: Xournal-Scribe Input Test
# Author: Xournal-Scribe
# DontUseFBInk

# 30-second pen/touch test: draws marks where the app thinks the pen and finger
# are, and logs the coordinates to /mnt/us/xournal-scribe/xscribe.log.
if [ -f /mnt/us/xournal-scribe/run.sh ]; then
    sh /mnt/us/xournal-scribe/run.sh --probe 30
else
    eips 2 10 "Xournal-Scribe is not installed in /mnt/us/xournal-scribe"
fi
