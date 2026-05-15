#!/usr/bin/env bash
#
# Self-contained build for the PicoGUS setup tools.
#
# Strategy:
#   * On Linux x86_64 with WATCOM already set in the environment, build
#     natively with the local Open Watcom v2.
#   * Otherwise (macOS / no local Watcom), build inside a linux/amd64
#     Docker container based on pangbox/openwatcom-action which ships
#     Open Watcom v2 prebuilt at /opt/watcom.  Rosetta on Apple Silicon
#     runs the amd64 image at near-native speed.
#
# We deliberately do NOT use the official open-watcom-2_0-c-linux-x86
# installer here.  That installer is a curses program that crashes on
# Rosetta and provides no documented unattended path.  The pangbox
# image is a thin wrapper around the same Open Watcom drop, just with
# the install already done at image-build time.
#
# Outputs land in:
#   pginst.exe  pgsetup.exe   (repo root)
#   outputs/PGINST.EXE  outputs/PGSETUP.EXE  (release names)
#
set -euo pipefail

REPO="$(cd "$(dirname "$0")" && pwd)"
IMAGE="pangbox/openwatcom-action:latest"

# Version string baked into the EXE title bars.
# Override on the command line:  PGWIZ_VERSION=v1.2.3 ./build.sh
# When unset, the source-level fallback in pgwiz_version.h ("dev") wins.
PGWIZ_VERSION="${PGWIZ_VERSION:-}"
if [[ -n "${PGWIZ_VERSION}" ]]; then
    # The macro must expand to a quoted C string literal, so we wrap the
    # value in escaped double-quotes before passing -D.
    PGWIZ_EXTRA_CFLAGS="-DPGWIZ_VERSION=\"\\\"${PGWIZ_VERSION}\\\"\""
else
    PGWIZ_EXTRA_CFLAGS=""
fi
export PGWIZ_EXTRA_CFLAGS

build_native_linux() {
    : "${WATCOM:?WATCOM env var must point at an Open Watcom install}"
    export PATH="${WATCOM}/binl64:${WATCOM}/binl:${PATH}"
    export INCLUDE="${WATCOM}/h"
    export LIB="${WATCOM}/lib286/dos"
    cd "${REPO}"
    "${WATCOM}/binl64/wmake" -f Makefile || "${WATCOM}/binl/wmake" -f Makefile
    finalise
}

build_docker() {
    if ! docker info >/dev/null 2>&1; then
        echo "!! Docker daemon not reachable. Start Colima or Docker Desktop." >&2
        exit 1
    fi

    echo ">> Pulling Open Watcom v2 image..."
    docker pull --platform linux/amd64 "${IMAGE}" >/dev/null

    echo ">> Building inside linux/amd64 container..."
    [[ -n "${PGWIZ_VERSION}" ]] && echo ">> Version: ${PGWIZ_VERSION}"

    # We use create + start so we can tar-pipe sources in even when the
    # host path is symlinked through a volume Colima cannot resolve.
    local cid
    cid=$(docker create --platform linux/amd64 \
        --entrypoint /bin/sh \
        -e "PGWIZ_EXTRA_CFLAGS=${PGWIZ_EXTRA_CFLAGS}" \
        "${IMAGE}" -c '
        set -e
        mkdir -p /work && cd /work
        tar -xzf /src.tgz
        export WATCOM=/opt/watcom
        export PATH="$WATCOM/binl64:$WATCOM/binl:$PATH"
        export INCLUDE="$WATCOM/h"
        "$WATCOM/binl64/wmake" -f Makefile
        ls -l *.exe
    ')

    tar -C "${REPO}" -czf "/tmp/pigwiz-src.$$.tgz" \
        Makefile tui.h tui.c pginst.c pgsetup.c pgbundle.c pgwiz_version.h

    docker cp "/tmp/pigwiz-src.$$.tgz" "${cid}":/src.tgz
    rm -f "/tmp/pigwiz-src.$$.tgz"

    docker start -a "${cid}"
    local rc=$?

    if [ $rc -eq 0 ]; then
        docker cp "${cid}":/work/pginst.exe   "${REPO}/pginst.exe"
        docker cp "${cid}":/work/pgsetup.exe  "${REPO}/pgsetup.exe"
        docker cp "${cid}":/work/pgbundle.exe "${REPO}/pgbundle.exe"
    fi

    docker rm "${cid}" >/dev/null

    [ $rc -eq 0 ] || exit $rc
    finalise
}

finalise() {
    mkdir -p "${REPO}/outputs"
    cp -f "${REPO}/pginst.exe"   "${REPO}/outputs/PGINST.EXE"
    cp -f "${REPO}/pgsetup.exe"  "${REPO}/outputs/PGSETUP.EXE"
    # Stub only at this stage; bundle.sh will pack it into PGBUNDLE.EXE.
    cp -f "${REPO}/pgbundle.exe" "${REPO}/outputs/PGBUNDLE-STUB.EXE"
    echo
    echo ">> Build complete."
    ls -lh "${REPO}/pginst.exe" "${REPO}/pgsetup.exe" "${REPO}/pgbundle.exe"
    echo
    echo "Release outputs:"
    ls -lh "${REPO}/outputs/"
}

uname_s="$(uname -s)"
case "${uname_s}" in
    Linux)
        if [[ -n "${WATCOM:-}" && -x "${WATCOM}/binl/wmake" ]]; then
            build_native_linux
        else
            build_docker
        fi
        ;;
    *)
        build_docker
        ;;
esac
