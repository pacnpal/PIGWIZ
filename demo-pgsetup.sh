#!/usr/bin/env bash
# Demo PGSETUP.EXE (the live settings manager) in DOSBox.
exec "$(cd "$(dirname "$0")" && pwd)/run-in-dosbox.sh" pgsetup "$@"
