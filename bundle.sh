#!/usr/bin/env bash
#
# bundle.sh - Assemble a PIGWIZ release zip.
#
# Pulls the latest PicoGUS release (PGUSINIT.EXE + firmware) plus the
# FreeDOS-licensed mouse and CD-ROM helpers from ibiblio, lays them
# alongside our freshly built PGINST.EXE / PGSETUP.EXE, and writes a
# single PIGWIZ-pg-vX.Y.Z.zip suitable for sticking on a release.
#
# Requires: pginst.exe + pgsetup.exe already built (run build.sh first).
# Tools used: curl, unzip, zip, python3 (for parsing the GitHub release
# JSON without needing jq).
#
# Layout inside the resulting zip:
#   PICOGUS/PGINST.EXE       - our installer wizard
#   PICOGUS/PGSETUP.EXE      - our settings manager
#   PICOGUS/PGUSINIT.EXE     - upstream pgusinit
#   PICOGUS/picogus.uf2      - main firmware
#   PICOGUS/pg-ne2k.uf2      - NE2000/WiFi firmware
#   PICOGUS/CTMOUSE.EXE      - CuteMouse 2.1b4 (GPL)
#   PICOGUS/SHSUCDX.COM      - MSCDEX replacement (GPL)
#   PICOGUS/UIDE.SYS         - ATAPI/IDE CD-ROM driver (PD)
#   PICOGUS/UDVD2.SYS        - ATAPI DVD-capable alternative
#   PICOGUS/PIGWIZ.TXT       - what each file is and its licence
#
set -euo pipefail

REPO="$(cd "$(dirname "$0")" && pwd)"
STAGE="${REPO}/.bundle-stage"
OUT_DIR="${REPO}/outputs"

PICOGUS_API="https://api.github.com/repos/polpo/picogus/releases/latest"
CTMOUSE_URL="https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/dos/mouse/2.1beta4/ctm21b4.zip"
SHSUCDX_URL="https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/dos/shsucdx/shcdx308.zip"
UIDE_URL="https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/dos/cdrom/uide/uide/2020-07-04b/UIDE.zip"
UDVD2_URL="https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/dos/cdrom/uide/udvd2/2015-03-05d/UDVD2.zip"

require() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "!! Missing required tool: $1" >&2
        exit 1
    }
}
require curl
require unzip
require zip
require python3

# Accept either lowercase (fresh local build) or UPPERCASE staged copies
# under outputs/ (which is what CI downloads from the build artifact).
PGINST_SRC=""; PGSETUP_SRC=""
for cand in "${REPO}/pginst.exe" "${REPO}/outputs/PGINST.EXE"; do
    [[ -f "${cand}" ]] && PGINST_SRC="${cand}" && break
done
for cand in "${REPO}/pgsetup.exe" "${REPO}/outputs/PGSETUP.EXE"; do
    [[ -f "${cand}" ]] && PGSETUP_SRC="${cand}" && break
done
[[ -n "${PGINST_SRC}"  ]] || { echo "!! pginst.exe missing - run ./build.sh first."  >&2; exit 1; }
[[ -n "${PGSETUP_SRC}" ]] || { echo "!! pgsetup.exe missing - run ./build.sh first." >&2; exit 1; }

rm -rf "${STAGE}"
mkdir -p "${STAGE}/dl" "${STAGE}/extract" "${STAGE}/PICOGUS"

# ---------------------------------------------------------------------
# Discover the latest PicoGUS release and pull its zip.
# ---------------------------------------------------------------------
echo ">> Querying polpo/picogus latest release..."
read -r PG_TAG PG_URL < <(curl -sL "${PICOGUS_API}" | python3 -c '
import json, sys
d = json.load(sys.stdin)
tag = d["tag_name"]
zip_url = next(
    (a["browser_download_url"] for a in d["assets"]
     if a["name"].lower().endswith(".zip") and a["name"].lower().startswith("picogus-")),
    None,
)
if not zip_url:
    sys.exit("No picogus-*.zip asset on latest release")
print(tag, zip_url)
')
echo "   PicoGUS release: ${PG_TAG}"
echo "   Asset URL:       ${PG_URL}"

curl -fsSL -o "${STAGE}/dl/picogus.zip" "${PG_URL}"
unzip -q "${STAGE}/dl/picogus.zip" -d "${STAGE}/extract/picogus"

# ---------------------------------------------------------------------
# FreeDOS-licensed extras.
# ---------------------------------------------------------------------
fetch_unzip() {
    local name="$1" url="$2"
    echo ">> Fetching ${name}..."
    curl -fsSL -o "${STAGE}/dl/${name}.zip" "${url}"
    mkdir -p "${STAGE}/extract/${name}"
    unzip -q "${STAGE}/dl/${name}.zip" -d "${STAGE}/extract/${name}"
}
fetch_unzip ctmouse "${CTMOUSE_URL}"
fetch_unzip shsucdx "${SHSUCDX_URL}"
fetch_unzip uide    "${UIDE_URL}"
fetch_unzip udvd2   "${UDVD2_URL}"

# ---------------------------------------------------------------------
# Assemble PICOGUS/ directory. Use a tolerant copy-by-name so we cope
# with archives that use different case for filenames.
# ---------------------------------------------------------------------
find_in() {
    # find_in <dir> <basename>   -> prints first match, exit 1 if none
    local dir="$1" name="$2"
    find "${dir}" -iname "${name}" -type f -print -quit
}

copy_required() {
    local src dst dir
    dir="$1"; src_name="$2"; dst="$3"
    src=$(find_in "${dir}" "${src_name}")
    [[ -n "${src}" ]] || { echo "!! ${src_name} not found in ${dir}" >&2; exit 1; }
    cp "${src}" "${STAGE}/PICOGUS/${dst}"
}

echo ">> Staging files..."
cp "${PGINST_SRC}"  "${STAGE}/PICOGUS/PGINST.EXE"
cp "${PGSETUP_SRC}" "${STAGE}/PICOGUS/PGSETUP.EXE"

copy_required "${STAGE}/extract/picogus" "pgusinit.exe" "PGUSINIT.EXE"
copy_required "${STAGE}/extract/picogus" "picogus.uf2"  "PICOGUS.UF2"
copy_required "${STAGE}/extract/picogus" "pg-ne2k.uf2"  "PG-NE2K.UF2"

copy_required "${STAGE}/extract/ctmouse" "CTMOUSE.EXE"  "CTMOUSE.EXE"
copy_required "${STAGE}/extract/shsucdx" "shsucdx.com"  "SHSUCDX.COM"
copy_required "${STAGE}/extract/uide"    "UIDE.SYS"     "UIDE.SYS"
copy_required "${STAGE}/extract/udvd2"   "UDVD2.SYS"    "UDVD2.SYS"

# Carry the upstream PicoGUS README in case the user wants the original
# notes; rename to avoid clobbering our own README.
if [[ -f "${STAGE}/extract/picogus/README.md" ]]; then
    cp "${STAGE}/extract/picogus/README.md" "${STAGE}/PICOGUS/PICOGUS.MD"
fi

# ---------------------------------------------------------------------
# Bundle-level README explaining provenance and licences. Name stays
# inside DOS 8.3 so the SFX (16-char name slot) can carry it too.
# ---------------------------------------------------------------------
cat > "${STAGE}/PICOGUS/PIGWIZ.TXT" <<EOF
PIGWIZ - PicoGUS DOS setup tools bundle
========================================
Built against PicoGUS upstream release: ${PG_TAG}

Contents
--------
PGINST.EXE     PIGWIZ first-run installer wizard
PGSETUP.EXE    PIGWIZ settings manager
PGUSINIT.EXE   PicoGUS init/control utility (from polpo/picogus)
PICOGUS.UF2    PicoGUS main firmware (sound card emulations)
PG-NE2K.UF2    PicoGUS NE2000/WiFi firmware (Femto + PicoGUS 1.x w/Pico W)
CTMOUSE.EXE    CuteMouse 2.1b4 serial / PS/2 mouse driver
SHSUCDX.COM    MSCDEX-compatible CD-ROM filesystem extension (FreeDOS)
UIDE.SYS       ATAPI/IDE CD-ROM driver (FreeDOS)
UDVD2.SYS      ATAPI CD/DVD driver alternative (FreeDOS)
PICOGUS.MD     Upstream PicoGUS README

Licences
--------
PGINST.EXE / PGSETUP.EXE  See repo LICENSE
PGUSINIT + firmware       GPLv2 (polpo/picogus)
CTMOUSE.EXE               GPLv2 (CuteMouse)
SHSUCDX.COM               GPLv2 (Jason Hood / Eric Auer)
UIDE.SYS                  Public domain (Jack Ellis)
UDVD2.SYS                 Public domain (Jack Ellis)

Recommended install
-------------------
1. Copy PICOGUS/ to C:\\PICOGUS\\.
2. Run C:\\PICOGUS\\PGINST.EXE for first-time setup.
3. For ongoing tweaks: C:\\PICOGUS\\PGSETUP.EXE.

For firmware upgrades:
  PGUSINIT /flash PICOGUS.UF2
  PGUSINIT /flash PG-NE2K.UF2  (NE2000 build only)
EOF

# ---------------------------------------------------------------------
# Pick a bundle filename. CI passes BUNDLE_VERSION (the PIGWIZ release
# tag), or we default to the picogus upstream tag.
# ---------------------------------------------------------------------
BUNDLE_NAME="PIGWIZ-pg-${PG_TAG}.zip"
if [[ -n "${PIGWIZ_VERSION:-}" && "${PIGWIZ_VERSION}" != "dev" ]]; then
    BUNDLE_NAME="PIGWIZ-${PIGWIZ_VERSION}-pg-${PG_TAG}.zip"
fi

mkdir -p "${OUT_DIR}"
rm -f "${OUT_DIR}/${BUNDLE_NAME}"
( cd "${STAGE}" && zip -qr "${OUT_DIR}/${BUNDLE_NAME}" PICOGUS )

# Always keep a stable name pointing at the latest build, too, so CI
# downstream steps don't need to guess.
cp "${OUT_DIR}/${BUNDLE_NAME}" "${OUT_DIR}/PIGWIZ-latest.zip"

echo
echo ">> Zip bundle written:"
ls -lh "${OUT_DIR}/${BUNDLE_NAME}" "${OUT_DIR}/PIGWIZ-latest.zip"
echo
unzip -l "${OUT_DIR}/${BUNDLE_NAME}"

# ---------------------------------------------------------------------
# Self-extracting EXE bundle.
#
# Stub: pgbundle.exe (compiled by Makefile). We append all the same
# files the zip carries and stamp a 12-byte trailer onto the end.
# Resulting PGBUNDLE.EXE is a runnable DOS MZ that knows how to extract
# its own payload.
# ---------------------------------------------------------------------
STUB=""
for cand in "${REPO}/pgbundle.exe" "${REPO}/outputs/PGBUNDLE-STUB.EXE"; do
    [[ -f "${cand}" ]] && STUB="${cand}" && break
done
if [[ -z "${STUB}" ]]; then
    echo "!! pgbundle.exe stub missing - run ./build.sh first." >&2
    exit 1
fi

SFX_NAME="PGBUNDLE-pg-${PG_TAG}.EXE"
if [[ -n "${PIGWIZ_VERSION:-}" && "${PIGWIZ_VERSION}" != "dev" ]]; then
    SFX_NAME="PGBUNDLE-${PIGWIZ_VERSION}-pg-${PG_TAG}.EXE"
fi

echo ">> Packing self-extracting EXE..."
python3 "${REPO}/pack-bundle.py" \
    --stub "${STUB}" \
    --out  "${OUT_DIR}/${SFX_NAME}" \
    "${STAGE}/PICOGUS/PGINST.EXE" \
    "${STAGE}/PICOGUS/PGSETUP.EXE" \
    "${STAGE}/PICOGUS/PGUSINIT.EXE" \
    "${STAGE}/PICOGUS/PICOGUS.UF2" \
    "${STAGE}/PICOGUS/PG-NE2K.UF2" \
    "${STAGE}/PICOGUS/CTMOUSE.EXE" \
    "${STAGE}/PICOGUS/SHSUCDX.COM" \
    "${STAGE}/PICOGUS/UIDE.SYS" \
    "${STAGE}/PICOGUS/UDVD2.SYS" \
    "${STAGE}/PICOGUS/PIGWIZ.TXT"

cp "${OUT_DIR}/${SFX_NAME}" "${OUT_DIR}/PGBUNDLE-latest.EXE"

echo
echo ">> SFX bundle written:"
ls -lh "${OUT_DIR}/${SFX_NAME}" "${OUT_DIR}/PGBUNDLE-latest.EXE"
