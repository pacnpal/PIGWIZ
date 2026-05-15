#!/usr/bin/env bash
#
# Launch PGINST.EXE or PGSETUP.EXE in DOSBox-staging.
#
# Usage:
#   ./run-in-dosbox.sh pginst       (or PGINST, or pginst.exe, or i)
#   ./run-in-dosbox.sh pgsetup      (or PGSETUP, etc.)
#   ./run-in-dosbox.sh both         shows a menu prompt inside DOSBox
#
# What it does:
#   * Stages a temp C:\ drive at $TMPDIR/pgwiz-dos with both EXEs and a
#     dummy PGUSINIT.EXE shim (so the installer can run end-to-end
#     without a real PicoGUS card).
#   * Writes a one-shot dosbox.conf and starts dosbox-staging.
#   * After exit, copies any C:\AUTOEXEC.BAT / C:\CONFIG.SYS the
#     installer wrote out to that drive into ./dosbox-artifacts/ so you
#     can inspect them on the host.
#
set -euo pipefail

REPO="$(cd "$(dirname "$0")" && pwd)"

# ---------------------------------------------------------------------
# Pick a DOSBox.
# ---------------------------------------------------------------------
DOSBOX="${DOSBOX:-}"
if [[ -z "${DOSBOX}" ]]; then
    for candidate in dosbox-staging dosbox-x dosbox; do
        if command -v "${candidate}" >/dev/null 2>&1; then
            DOSBOX="${candidate}"
            break
        fi
    done
fi
if [[ -z "${DOSBOX}" ]]; then
    echo "!! No DOSBox found. Install with: brew install dosbox-staging" >&2
    exit 1
fi

# ---------------------------------------------------------------------
# Pick which EXE the user wants.
# ---------------------------------------------------------------------
WHICH="${1:-pginst}"
WHICH="${WHICH%.exe}"
WHICH="${WHICH%.EXE}"
case "${WHICH,,}" in
    i|install|pginst)   TARGET="PGINST.EXE";   LABEL="installer" ;;
    s|setup|pgsetup)    TARGET="PGSETUP.EXE";  LABEL="settings"  ;;
    b|bundle|pgbundle)  TARGET="PGBUNDLE.EXE"; LABEL="bundle"    ;;
    both)               TARGET="";             LABEL="both"      ;;
    *) echo "Usage: $0 [pginst | pgsetup | pgbundle | both]"; exit 2 ;;
esac

# ---------------------------------------------------------------------
# Make sure the EXEs exist - build them if missing.
# ---------------------------------------------------------------------
if [[ ! -x "${REPO}/pginst.exe" || ! -x "${REPO}/pgsetup.exe" ]]; then
    echo ">> EXEs missing, running build.sh first..."
    "${REPO}/build.sh"
fi

# ---------------------------------------------------------------------
# Stage a clean DOS C:\ drive.
# ---------------------------------------------------------------------
DRIVE="${TMPDIR:-/tmp}/pgwiz-dos"
rm -rf "${DRIVE}"
mkdir -p "${DRIVE}"

cp "${REPO}/pginst.exe"  "${DRIVE}/PGINST.EXE"
cp "${REPO}/pgsetup.exe" "${DRIVE}/PGSETUP.EXE"
# PGBUNDLE.EXE only exists once bundle.sh has run (it produces the SFX
# under outputs/). Copy whichever versioned SFX is there.
sfx=$(ls "${REPO}/outputs/"PGBUNDLE-*-pg-*.EXE 2>/dev/null | head -1)
if [[ -n "${sfx}" ]]; then
    cp "${sfx}" "${DRIVE}/PGBUNDLE.EXE"
fi

# Drop in a stub PGUSINIT.EXE that just echoes plausible status text so
# the installer/settings UI can call it without a real PicoGUS card.
# DOS .BAT can be used in place of an EXE if we put it on the search path
# - DOSBox will resolve `PGUSINIT` to PGUSINIT.BAT before .EXE if .BAT
# is found first, so we name it PGUSINIT.COM (no, simpler: write a tiny
# .COM that prints text via INT 21h... overkill). Use a .BAT and have
# autoexec PATH it.
cat > "${DRIVE}/PGUSINIT.BAT" <<'BAT'
@echo off
echo PicoGUS detected: Firmware version: picogus-gus v3.9.0 (stub)
echo Hardware: PicoGUS v2.0
echo Running in GUS mode on port 240
echo PicoGUS initialized!
BAT

# Seed an existing AUTOEXEC.BAT / CONFIG.SYS so the installer's preview
# screens have something to render. The installer will rewrite these.
cat > "${DRIVE}/AUTOEXEC.BAT" <<'BAT'
@echo off
SET PATH=C:\DOS;C:\
PROMPT $P$G
BAT

cat > "${DRIVE}/CONFIG.SYS" <<'CFG'
FILES=20
BUFFERS=10
CFG

# ---------------------------------------------------------------------
# Build the launch script for inside DOSBox.
# ---------------------------------------------------------------------
LAUNCH="${DRIVE}/RUN.BAT"
case "${LABEL}" in
    installer) cat > "${LAUNCH}" <<EOF
@echo off
C:\\PGINST.EXE
EOF
    ;;
    settings)  cat > "${LAUNCH}" <<EOF
@echo off
C:\\PGSETUP.EXE
EOF
    ;;
    bundle)    cat > "${LAUNCH}" <<EOF
@echo off
C:\\PGBUNDLE.EXE
EOF
    ;;
    both)      cat > "${LAUNCH}" <<EOF
@echo off
echo Choose: 1) installer  2) settings  3) bundle extractor
choice /c:123
if errorlevel 3 goto SFX
if errorlevel 2 goto SET
:INST
C:\\PGINST.EXE
goto END
:SET
C:\\PGSETUP.EXE
goto END
:SFX
C:\\PGBUNDLE.EXE
:END
EOF
    ;;
esac

# ---------------------------------------------------------------------
# Dosbox config.
# ---------------------------------------------------------------------
CONF="${DRIVE}/dosbox.conf"
cat > "${CONF}" <<EOF
[sdl]
fullscreen=false
output=texture

[dos]
xms=true
ems=true
umb=true

[cpu]
core=auto
cputype=386
cycles=auto

[autoexec]
@echo off
mount c "${DRIVE}"
c:
PATH=C:\\
RUN.BAT
EOF

# ---------------------------------------------------------------------
# Launch.
# ---------------------------------------------------------------------
echo ">> ${LABEL} -> ${DOSBOX}"
echo ">> drive C: at ${DRIVE}"
"${DOSBOX}" -conf "${CONF}" "$@" || true

# ---------------------------------------------------------------------
# Pull anything the program wrote back to the host so we can inspect it.
# ---------------------------------------------------------------------
mkdir -p "${REPO}/dosbox-artifacts"
for f in AUTOEXEC.BAT AUTOEXEC.BAK CONFIG.SYS CONFIG.BAK; do
    if [[ -f "${DRIVE}/${f}" ]]; then
        cp "${DRIVE}/${f}" "${REPO}/dosbox-artifacts/${f}"
    fi
done
if [[ -d "${DRIVE}/PICOGUS" ]]; then
    rm -rf "${REPO}/dosbox-artifacts/PICOGUS"
    cp -r "${DRIVE}/PICOGUS" "${REPO}/dosbox-artifacts/PICOGUS"
fi
if [[ -d "${DRIVE}/ULTRASND" ]]; then
    rm -rf "${REPO}/dosbox-artifacts/ULTRASND"
    cp -r "${DRIVE}/ULTRASND" "${REPO}/dosbox-artifacts/ULTRASND"
fi

echo
echo ">> Done. Artifacts (if any) are under ${REPO}/dosbox-artifacts/"
ls -la "${REPO}/dosbox-artifacts/" 2>/dev/null || true
