#!/usr/bin/env bash
# Demo PGINST.EXE (the installer wizard) in DOSBox.
exec "$(cd "$(dirname "$0")" && pwd)/run-in-dosbox.sh" pginst "$@"
