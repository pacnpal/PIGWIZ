/*
 * pgwiz_version.h - Single source of truth for the version string baked
 * into both PGINST.EXE and PGSETUP.EXE.
 *
 * The build system (Makefile / build.sh / GitHub Actions) is expected
 * to set PGWIZ_VERSION on the compiler command line, e.g.
 *
 *     owcc -DPGWIZ_VERSION="\"v1.2.3\"" ...
 *
 * If nothing was set, we fall back to "dev" so local hacking still
 * compiles. Keep this string short - the title bar has finite width.
 */
#ifndef PGWIZ_VERSION_H
#define PGWIZ_VERSION_H

#ifndef PGWIZ_VERSION
#define PGWIZ_VERSION "dev"
#endif

#endif /* PGWIZ_VERSION_H */
