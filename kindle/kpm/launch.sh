#!/bin/sh
# `kpm launch scrible [args]`. The library scriptlets call run.sh
# directly instead, so that installing by USB copy works too.
exec sh /mnt/us/scrible/run.sh "$@"
