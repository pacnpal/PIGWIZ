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

# ---------------------------------------------------------------------
# Gravis UltraSound driver patches.
#
# Payload: ULTRASNDPPL161FIX.zip - the GUS v4.11 driver tree with Pro
# Patches Lite 1.61 and the anti-loop fix pre-applied. Required for GUS
# mode on PicoGUS. Not FreeDOS-licensed, but mirrored by the Internet
# Archive's "Gravis UltraSound DOS Driver Package" item, which is where
# we fetch from by default. Resolution order:
#
#   1. $GUS_PATCHES_LOCAL on disk (defaults to assets/ULTRASNDPPL161FIX.zip)
#   2. curl $GUS_PATCHES_URL
#
# If both fail, bundle.sh still succeeds - the installer just keeps its
# "Download the GUS v4.11 package..." prompt at runtime.
#
# UNZIP_URL points at the FreeDOS 1.1 repository package for Info-ZIP
# UnZip; we unwrap it on the host to pull BIN/UNZIP.EXE out and ship
# it alongside the patches.
# ---------------------------------------------------------------------
GUS_PATCHES_URL="${GUS_PATCHES_URL:-https://archive.org/download/GravisUltrasoundDOSDriverPackage/ultrasound.zip/ULTRASNDPPL161FIX.zip}"
GUS_PATCHES_LOCAL="${GUS_PATCHES_LOCAL:-${REPO}/assets/ULTRASNDPPL161FIX.zip}"
UNZIP_URL="${UNZIP_URL:-https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.1/archivers/unzip.zip}"

# DJ Delorie's CWSDPMI for the floppy edition - UNZIP.EXE on DOS uses
# the DJGPP go32 stub which needs a DPMI host present. CWSDPMI is the
# canonical free one (~21KB), redistributable under its own permissive
# licence (see csdpmi.doc).
CWSDPMI_URL="${CWSDPMI_URL:-https://www.delorie.com/pub/djgpp/current/v2misc/csdpmi7b.zip}"

require() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "!! Missing required tool: $1" >&2
        exit 1
    }
}
require curl
require wget
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
# GUS v4.11 patches + a DOS UNZIP.EXE to extract them at install time.
# By default we pull from archive.org; either source may be unreachable
# (offline build, transient 502, user blanked GUS_PATCHES_URL=) - in
# that case the installer silently falls back to its "download
# yourself" prompt for GUS mode and the bundle still ships.
# ---------------------------------------------------------------------
HAVE_GUS_PATCHES=0
GUS_ZIP_SRC=""

if [[ -f "${GUS_PATCHES_LOCAL}" ]]; then
    echo ">> Using local GUS v4.11 patches: ${GUS_PATCHES_LOCAL}"
    GUS_ZIP_SRC="${GUS_PATCHES_LOCAL}"
elif [[ -n "${GUS_PATCHES_URL}" ]]; then
    echo ">> Fetching GUS v4.11 patches: ${GUS_PATCHES_URL}"
    # wget (not curl) because archive.org's /download/<item>/<container>/<file>
    # URLs 302 to a dynamic ia######.us.archive.org/view_archive.php endpoint
    # whose host varies by load balancer. wget follows redirects by default
    # and handles the resulting query-string URL cleanly.
    #
    # -4 forces IPv4: some archive.org mirror hostnames return a bogus
    # ::/0.0.0.0 AAAA record that breaks dual-stack resolvers (seen on
    # macOS homebrew wget). Sticking to IPv4 avoids it.
    #
    # The retry loop re-invokes wget (not --tries) so each attempt goes
    # back through the archive.org load balancer and gets a fresh mirror
    # - retrying the same dead ia###### host is pointless.
    gus_fetched=0
    for attempt in 1 2 3 4 5; do
        if wget -4 -nv --timeout=30 \
                -O "${STAGE}/dl/ULTRASND.zip" "${GUS_PATCHES_URL}"; then
            gus_fetched=1
            break
        fi
        rm -f "${STAGE}/dl/ULTRASND.zip"
        echo "   attempt ${attempt}/5 failed - retrying through a fresh mirror in 5s..."
        sleep 5
    done
    if [[ "${gus_fetched}" -eq 1 ]]; then
        GUS_ZIP_SRC="${STAGE}/dl/ULTRASND.zip"
    else
        echo "!! All 5 fetch attempts failed - continuing without GUS patches" >&2
    fi
else
    echo ">> No GUS_PATCHES_URL / ${GUS_PATCHES_LOCAL#${REPO}/} - bundle will"
    echo "   ship without GUS patches; installer prompts the user to fetch them."
fi

if [[ -n "${GUS_ZIP_SRC}" ]]; then
    # Quick sanity check: must be a real zip.
    if unzip -l "${GUS_ZIP_SRC}" >/dev/null 2>&1; then
        # Normalise the zip layout. The canonical ULTRASNDPPL161FIX.zip
        # has a single "ULTRASND/" wrap directory at the top - if we
        # ship that as-is, PGINST's `UNZIP -d C:\ULTRASND` would nest
        # everything at C:\ULTRASND\ULTRASND\. Detect the wrap and
        # repack flat; tolerate already-flat zips too.
        echo ">> Normalising GUS zip layout..."
        gus_extract="${STAGE}/extract/gus-patches"
        rm -rf "${gus_extract}"
        mkdir -p "${gus_extract}"
        unzip -qo "${GUS_ZIP_SRC}" -d "${gus_extract}"
        shopt -s nullglob
        gus_top=( "${gus_extract}"/* )
        shopt -u nullglob
        if [[ ${#gus_top[@]} -eq 1 && -d "${gus_top[0]}" ]]; then
            echo "   stripping wrap dir: $(basename "${gus_top[0]}")"
            gus_pack_dir="${gus_top[0]}"
        else
            echo "   zip already flat"
            gus_pack_dir="${gus_extract}"
        fi
        rm -f "${STAGE}/dl/ULTRASND-flat.zip"
        ( cd "${gus_pack_dir}" && zip -qr "${STAGE}/dl/ULTRASND-flat.zip" . )
        GUS_ZIP_SRC="${STAGE}/dl/ULTRASND-flat.zip"

        # Fetch the FreeDOS Info-ZIP package and pull BIN/UNZIP.EXE out
        # of it. The package is a plain zip; `unzip` extracts the whole
        # tree, then we hunt for UNZIP.EXE inside (tolerates layout
        # changes between FreeDOS repository versions).
        echo ">> Fetching Info-ZIP UNZIP.EXE: ${UNZIP_URL}"
        if curl -fsSL -o "${STAGE}/dl/freedos-unzip.zip" "${UNZIP_URL}"; then
            mkdir -p "${STAGE}/extract/unzip"
            if unzip -qo "${STAGE}/dl/freedos-unzip.zip" \
                    -d "${STAGE}/extract/unzip" 2>/dev/null; then
                UNZIP_BIN=$(find "${STAGE}/extract/unzip" -iname "UNZIP.EXE" -type f -print -quit)
                if [[ -n "${UNZIP_BIN}" ]]; then
                    cp "${GUS_ZIP_SRC}" "${STAGE}/PICOGUS/ULTRASND.ZIP"
                    cp "${UNZIP_BIN}"   "${STAGE}/PICOGUS/UNZIP.EXE"
                    HAVE_GUS_PATCHES=1
                    echo ">> Bundled ULTRASND.ZIP ($(stat -f%z "${STAGE}/PICOGUS/ULTRASND.ZIP" 2>/dev/null || stat -c%s "${STAGE}/PICOGUS/ULTRASND.ZIP") bytes) + UNZIP.EXE"
                else
                    echo "!! UNZIP.EXE not found inside ${UNZIP_URL##*/}; skipping GUS bundle" >&2
                fi
            else
                echo "!! Could not unwrap ${UNZIP_URL##*/}; skipping GUS bundle" >&2
            fi
        else
            echo "!! Failed to fetch UNZIP_URL; skipping GUS bundle" >&2
        fi
    else
        echo "!! ${GUS_ZIP_SRC} is not a valid zip; skipping GUS bundle" >&2
    fi
fi

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
{
    cat <<EOF
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
EOF
    if [[ "${HAVE_GUS_PATCHES}" -eq 1 ]]; then
        cat <<EOF
ULTRASND.ZIP   Gravis UltraSound v4.11 driver with Pro Patches Lite 1.61
               + anti-loop fix pre-applied. PGINST.EXE extracts it into
               C:\\ULTRASND\\ when GUS mode is selected.
UNZIP.EXE      Info-ZIP UnZip for DOS (used by PGINST to extract the
               patches above)
EOF
    fi
    cat <<EOF

Licences
--------
PGINST.EXE / PGSETUP.EXE  See repo LICENSE
PGUSINIT + firmware       GPLv2 (polpo/picogus)
CTMOUSE.EXE               GPLv2 (CuteMouse)
SHSUCDX.COM               GPLv2 (Jason Hood / Eric Auer)
UIDE.SYS                  Public domain (Jack Ellis)
UDVD2.SYS                 Public domain (Jack Ellis)
EOF
    if [[ "${HAVE_GUS_PATCHES}" -eq 1 ]]; then
        cat <<EOF
UNZIP.EXE                 Info-ZIP licence (BSD-like, freely distributable)
ULTRASND.ZIP              Gravis UltraSound v4.11 driver (Advanced Gravis,
                          retained under the long-standing community
                          practice of mirroring their abandoned DOS
                          driver kit) plus Pro Patches Lite 1.61
                          (community-maintained patch replacement set,
                          GameSrv / Tom Klok et al) with the anti-loop
                          fix already applied. Not relicenced.
EOF
    fi
    cat <<EOF

Recommended install
-------------------
1. Copy PICOGUS/ to C:\\PICOGUS\\.
2. Run C:\\PICOGUS\\PGINST.EXE for first-time setup.
3. For ongoing tweaks: C:\\PICOGUS\\PGSETUP.EXE.

For firmware upgrades:
  PGUSINIT /flash PICOGUS.UF2
  PGUSINIT /flash PG-NE2K.UF2  (NE2000 build only)
EOF
} > "${STAGE}/PICOGUS/PIGWIZ.TXT"

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
SFX_FILES=(
    "${STAGE}/PICOGUS/PGINST.EXE"
    "${STAGE}/PICOGUS/PGSETUP.EXE"
    "${STAGE}/PICOGUS/PGUSINIT.EXE"
    "${STAGE}/PICOGUS/PICOGUS.UF2"
    "${STAGE}/PICOGUS/PG-NE2K.UF2"
    "${STAGE}/PICOGUS/CTMOUSE.EXE"
    "${STAGE}/PICOGUS/SHSUCDX.COM"
    "${STAGE}/PICOGUS/UIDE.SYS"
    "${STAGE}/PICOGUS/UDVD2.SYS"
    "${STAGE}/PICOGUS/PIGWIZ.TXT"
)
if [[ "${HAVE_GUS_PATCHES}" -eq 1 ]]; then
    SFX_FILES+=(
        "${STAGE}/PICOGUS/ULTRASND.ZIP"
        "${STAGE}/PICOGUS/UNZIP.EXE"
    )
fi
python3 "${REPO}/pack-bundle.py" \
    --stub "${STUB}" \
    --out  "${OUT_DIR}/${SFX_NAME}" \
    "${SFX_FILES[@]}"

cp "${OUT_DIR}/${SFX_NAME}" "${OUT_DIR}/PGBUNDLE-latest.EXE"

echo
echo ">> SFX bundle written:"
ls -lh "${OUT_DIR}/${SFX_NAME}" "${OUT_DIR}/PGBUNDLE-latest.EXE"

# ---------------------------------------------------------------------
# Floppy edition: 1.44MB FAT12 disk image (PIGWIZ-floppy-*.IMG).
#
# Drops the 11MB of GUS .PAT audio samples - they can't compress lossless
# below ~9MB so a floppy with them is mathematically impossible. Keeps
# everything else: setup wizard, settings manager, PGUSINIT, both
# firmware UF2s, the four FreeDOS helper drivers, plus Info-ZIP UnZip
# and CWSDPMI on the DOS side. Firmware + drivers go into PGFIRM.ZIP
# (DEFLATE -9, ~786 KB) and an A:\INSTALL.BAT extracts them to
# C:\PICOGUS\ then launches PGINST.EXE.
#
# Requires mtools (mformat + mcopy) on the host. Skipped non-fatally
# if mtools is absent.
# ---------------------------------------------------------------------
if ! command -v mformat >/dev/null 2>&1 || ! command -v mcopy >/dev/null 2>&1; then
    echo
    echo ">> Skipping floppy edition (mtools not installed)."
else
    echo
    echo ">> Building floppy edition..."

    # Make sure UNZIP.EXE is staged - the GUS-patches path may have
    # already done it, but a fork build without patches still needs it
    # on the floppy.
    if [[ ! -f "${STAGE}/PICOGUS/UNZIP.EXE" ]]; then
        if [[ ! -d "${STAGE}/extract/unzip" ]]; then
            echo ">> Fetching Info-ZIP UNZIP.EXE for floppy: ${UNZIP_URL}"
            curl -fsSL -o "${STAGE}/dl/freedos-unzip.zip" "${UNZIP_URL}"
            mkdir -p "${STAGE}/extract/unzip"
            unzip -qo "${STAGE}/dl/freedos-unzip.zip" -d "${STAGE}/extract/unzip"
        fi
        UNZIP_BIN=$(find_in "${STAGE}/extract/unzip" "UNZIP.EXE")
        [[ -n "${UNZIP_BIN}" ]] || { echo "!! UNZIP.EXE missing - cannot build floppy" >&2; exit 1; }
        cp "${UNZIP_BIN}" "${STAGE}/PICOGUS/UNZIP.EXE"
    fi

    # Fetch CWSDPMI.
    echo ">> Fetching CWSDPMI: ${CWSDPMI_URL}"
    curl -fsSL -o "${STAGE}/dl/csdpmi.zip" "${CWSDPMI_URL}"
    mkdir -p "${STAGE}/extract/csdpmi"
    unzip -qo "${STAGE}/dl/csdpmi.zip" -d "${STAGE}/extract/csdpmi"
    CWSDPMI_BIN=$(find_in "${STAGE}/extract/csdpmi" "CWSDPMI.EXE")
    [[ -n "${CWSDPMI_BIN}" ]] || { echo "!! CWSDPMI.EXE missing - cannot build floppy" >&2; exit 1; }

    # Pack firmware + small drivers into PGFIRM.ZIP (DEFLATE -9, junked paths).
    FLOPPY_STAGE="${STAGE}/floppy"
    rm -rf "${FLOPPY_STAGE}"
    mkdir -p "${FLOPPY_STAGE}"
    ( cd "${STAGE}/PICOGUS" && zip -qj9 "${FLOPPY_STAGE}/PGFIRM.ZIP" \
        PICOGUS.UF2 PG-NE2K.UF2 CTMOUSE.EXE SHSUCDX.COM UIDE.SYS UDVD2.SYS )

    # INSTALL.BAT - one-shot deploy + launch.
    cat > "${FLOPPY_STAGE}/INSTALL.BAT" <<'BAT'
@echo off
echo.
echo PIGWIZ floppy edition - deploying to C:\PICOGUS\...
echo.
A:
SET PATH=A:\;%PATH%
if not exist C:\PICOGUS\NUL md C:\PICOGUS
UNZIP.EXE -o PGFIRM.ZIP -d C:\PICOGUS\
copy PGSETUP.EXE C:\PICOGUS\
copy PGUSINIT.EXE C:\PICOGUS\
copy PGINST.EXE C:\PICOGUS\
echo.
echo Done. Launching PGINST.EXE...
echo.
PGINST.EXE
BAT

    # User-facing readme for the floppy.
    cat > "${FLOPPY_STAGE}/FLOPPY.TXT" <<EOF
PIGWIZ ${PIGWIZ_VERSION:-dev} - floppy edition
==============================================

Insert disk, then at the A> prompt run:

    INSTALL

That copies the firmware and helper EXEs to C:\\PICOGUS\\ and starts
the setup wizard. The Gravis UltraSound v4.11 patches are NOT on this
disk (they need ~9 MB even at max compression). For GUS mode, grab
the full bundle from the release page:

    https://github.com/pacnpal/PIGWIZ/releases

Files on this disk:
  PGINST.EXE     setup wizard
  PGSETUP.EXE    live settings manager
  PGUSINIT.EXE   PicoGUS init/control
  UNZIP.EXE      Info-ZIP UnZip for DOS (FreeDOS package)
  CWSDPMI.EXE    DPMI host required by UNZIP (DJ Delorie, free)
  PGFIRM.ZIP     firmware + helper drivers, DEFLATE-compressed
  INSTALL.BAT    one-shot deploy + launch
EOF

    # Floppy filename mirrors the SFX naming.
    FLOPPY_NAME="PIGWIZ-floppy-pg-${PG_TAG}.IMG"
    if [[ -n "${PIGWIZ_VERSION:-}" && "${PIGWIZ_VERSION}" != "dev" ]]; then
        FLOPPY_NAME="PIGWIZ-floppy-${PIGWIZ_VERSION}-pg-${PG_TAG}.IMG"
    fi
    FLOPPY_IMG="${OUT_DIR}/${FLOPPY_NAME}"

    # Make a 1.44MB zero-filled image (2880 * 512 = 1474560 bytes).
    dd if=/dev/zero of="${FLOPPY_IMG}" bs=512 count=2880 2>/dev/null
    mformat -i "${FLOPPY_IMG}" -f 1440 -v PIGWIZ ::
    mcopy -i "${FLOPPY_IMG}" \
        "${STAGE}/PICOGUS/PGINST.EXE" \
        "${STAGE}/PICOGUS/PGSETUP.EXE" \
        "${STAGE}/PICOGUS/PGUSINIT.EXE" \
        "${STAGE}/PICOGUS/UNZIP.EXE" \
        "${CWSDPMI_BIN}" \
        "${FLOPPY_STAGE}/PGFIRM.ZIP" \
        "${FLOPPY_STAGE}/INSTALL.BAT" \
        "${FLOPPY_STAGE}/FLOPPY.TXT" \
        ::

    cp "${FLOPPY_IMG}" "${OUT_DIR}/PIGWIZ-floppy-latest.IMG"

    echo
    echo ">> Floppy image written:"
    ls -lh "${FLOPPY_IMG}" "${OUT_DIR}/PIGWIZ-floppy-latest.IMG"
    echo
    mdir -i "${FLOPPY_IMG}" ::
fi
