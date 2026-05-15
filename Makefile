# wmake-compatible Makefile for PicoGUS setup tools.
#
# Builds two 16-bit real-mode DOS EXEs (pginst.exe and pgsetup.exe) using
# Open Watcom v2 owcc with the large memory model. tui.c is compiled once
# and linked into both programs.
#
# Run via build.sh (which sets up Open Watcom and the env vars) or directly
# with `wmake` when WATCOM, INCLUDE and LIB are already set.

CC       = owcc

# Version baked into the EXEs is set by defining PGWIZ_VERSION on the
# compiler command line. build.sh and the GitHub Actions workflow set
# PGWIZ_EXTRA_CFLAGS in the environment to something like:
#     -DPGWIZ_VERSION="\"v1.2.3\""
# wmake's $(%VAR) reads that env var (returns empty if unset).
EXTRA_CFLAGS = $(%PGWIZ_EXTRA_CFLAGS)

# -bdos            : target DOS
# -mcmodel=l       : large memory model (16-bit far pointers everywhere)
# -Os              : optimise for size, helps the EXEs stay small
# -fno-stack-check : no runtime stack probes; bloat + no benefit on DOS
# -Wc,-zq          : silence the Watcom compiler banner
CFLAGS  = -bdos -mcmodel=l -Os -fno-stack-check -Wc,-zq $(EXTRA_CFLAGS)

OBJS_TUI = tui.o

all : pginst.exe pgsetup.exe pgbundle.exe

tui.o : tui.c tui.h pgwiz_version.h
	$(CC) $(CFLAGS) -c tui.c -o tui.o

pginst.exe : pginst.c tui.o tui.h pgwiz_version.h
	$(CC) $(CFLAGS) -o pginst.exe pginst.c tui.o

pgsetup.exe : pgsetup.c tui.o tui.h pgwiz_version.h
	$(CC) $(CFLAGS) -o pgsetup.exe pgsetup.c tui.o

# pgbundle.exe is built as a *stub* here. bundle.sh appends the file
# directory and file data afterwards via pack-bundle.py.
pgbundle.exe : pgbundle.c tui.o tui.h pgwiz_version.h
	$(CC) $(CFLAGS) -o pgbundle.exe pgbundle.c tui.o

clean : .SYMBOLIC
	-rm -f tui.o pginst.o pgsetup.o pgbundle.o
	-rm -f pginst.exe pgsetup.exe pgbundle.exe
	-rm -f *.err
