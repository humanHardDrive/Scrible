#!/bin/sh
# `kpm launch xournal-scribe [args]`. The library scriptlets call run.sh
# directly instead, so that installing by USB copy works too.
exec sh /mnt/us/xournal-scribe/run.sh "$@"
