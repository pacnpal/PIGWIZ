#!/usr/bin/env bash
# Demo PGBUNDLE.EXE (the self-extracting bundle) in DOSBox.
#
# Note: PGBUNDLE.EXE only exists after bundle.sh has run and produced
# outputs/PGBUNDLE-*-pg-*.EXE. run-in-dosbox.sh copies it into C:\ if
# present; otherwise you'll see "Bad command or file name".
exec "$(cd "$(dirname "$0")" && pwd)/run-in-dosbox.sh" pgbundle "$@"
