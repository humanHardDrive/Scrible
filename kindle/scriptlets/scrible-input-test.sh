#!/bin/sh
# Name: Scrible Input Test
# Author: Scrible
# DontUseFBInk

# 30-second pen/touch test: draws marks where the app thinks the pen and finger
# are, and logs the coordinates to /mnt/us/scrible/scrible.log.
if [ -f /mnt/us/scrible/run.sh ]; then
    sh /mnt/us/scrible/run.sh --probe 30
else
    eips 2 10 "Scrible is not installed in /mnt/us/scrible"
fi
