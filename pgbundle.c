/*
 * pgbundle.c - Self-extracting bundle stub.
 *
 * This program is the *stub* portion of the PIGWIZ self-extracting
 * installer.  Built on its own it is useless - the bundling step
 * (bundle.sh / pack-bundle.py) appends a file directory and the file
 * contents after the EXE on disk, then patches up the trailer.
 *
 * On disk the final file looks like:
 *
 *   [ MZ executable image (this stub)         ]
 *   [ file 1 header (20 bytes) + file 1 data  ]
 *   [ file 2 header (20 bytes) + file 2 data  ]
 *   ...
 *   [ 12-byte trailer at end of file:         ]
 *     +0  magic[4]    = "PGZ1"
 *     +4  dir_offset  = u32 (from start of file)
 *     +8  file_count  = u32
 *
 * Each file header:
 *     +0   name[16]   uppercase DOS 8.3, NUL padded
 *     +16  size       u32 little-endian
 *     +20  data follows
 *
 * DOS only loads the MZ portion described by the EXE header; the
 * appended directory + file data sit on disk and we seek into them
 * with fopen/fseek at run time.  Memory footprint = stub only.
 *
 * Target: 16-bit real-mode DOS, large memory model.
 */

#include "tui.h"
#include "pgwiz_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <conio.h>
#include <dos.h>
#include <direct.h>
#include <io.h>

#define TRAILER_SIZE       12
#define ENTRY_HEADER_SIZE  20
#define COPY_CHUNK         2048

typedef unsigned long u32;

/* ------------------------------------------------------------------ */
/* Filesystem helpers                                                  */
/* ------------------------------------------------------------------ */

static int dir_exists(const char *path) {
    unsigned attr;
    if (_dos_getfileattr(path, &attr) != 0) return 0;
    return (attr & _A_SUBDIR) ? 1 : 0;
}

static int make_dir(const char *path) {
    if (dir_exists(path)) return 0;
    return mkdir(path);
}

/* Little-endian u32 from a 4-byte buffer. */
static u32 read_u32(const unsigned char *p) {
    return  ((u32)p[0])        |
            ((u32)p[1] << 8)   |
            ((u32)p[2] << 16)  |
            ((u32)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* Locate the running EXE.                                             */
/*                                                                     */
/* DOS 3+ gives argv[0] as the full pathname used to load the program  */
/* (the loader stores it at the tail of the environment block, just    */
/* past two consecutive NULs followed by a word 0x0001). Modern        */
/* Watcom's startup code reads this and hands it to main.              */
/*                                                                     */
/* If argv[0] looks bare (no backslash, no colon), we fall back to     */
/* parsing the environment block ourselves via the PSP.                */
/* ------------------------------------------------------------------ */

static unsigned bios_get_psp(void) {
    union REGS r;
    r.h.ah = 0x62;       /* DOS 3.0+ Get PSP */
    int86(0x21, &r, &r);
    return r.x.bx;
}

static int has_path_separator(const char *s) {
    while (*s) {
        if (*s == '\\' || *s == ':' || *s == '/') return 1;
        s++;
    }
    return 0;
}

static int resolve_self_path(char *argv0, char *out, int outlen) {
    /* Best case: argv[0] already carries the full path. */
    if (argv0 && has_path_separator(argv0)) {
        int n = (int)strlen(argv0);
        if (n >= outlen) n = outlen - 1;
        memcpy(out, argv0, n);
        out[n] = '\0';
        return 0;
    }

    /* Fallback: dig through the env block for the program path. */
    {
        unsigned psp = bios_get_psp();
        unsigned env_seg;
        unsigned char far *env;
        unsigned i = 0;

        /* PSP[0x2C] is the env segment (word). */
        env_seg = *((unsigned far *)MK_FP(psp, 0x2C));
        env = (unsigned char far *)MK_FP(env_seg, 0);

        /* Skip env vars: each is a NUL-terminated string; block ends
         * at a double NUL. */
        while (env[i] != 0) {
            while (env[i] != 0) i++;
            i++;
        }
        i++; /* past the second NUL of the double-NUL terminator */

        /* DOS 3+ then has a 16-bit count of strings (usually 1) followed
         * by the program path. The count is at env[i], i+1 - skip it. */
        i += 2;

        /* Copy the NUL-terminated path. */
        {
            int n = 0;
            while (env[i] != 0 && n < outlen - 1) {
                out[n++] = (char)env[i++];
            }
            out[n] = '\0';
            return n > 0 ? 0 : -1;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Extraction                                                          */
/* ------------------------------------------------------------------ */

static int extract_one(FILE *self_f, const char *dest_dir,
                       int row_y, int *out_ok) {
    unsigned char hdr[ENTRY_HEADER_SIZE];
    char name[20];
    char path[140];
    u32 size, remaining;
    int k;
    FILE *out;

    if (fread(hdr, 1, ENTRY_HEADER_SIZE, self_f) != ENTRY_HEADER_SIZE) {
        return -1;
    }
    memcpy(name, hdr, 16);
    name[16] = '\0';
    /* trim trailing NULs / spaces from the fixed-width name slot */
    k = 16;
    while (k > 0 && (name[k-1] == '\0' || name[k-1] == ' ')) k--;
    name[k] = '\0';
    size = read_u32(hdr + 16);

    {
        char line[60];
        sprintf(line, "%-14s %7lu bytes ", name, (unsigned long)size);
        tui_print_at(6, row_y, TUI_WHITE, TUI_BLUE, line);
    }
    tui_progress(36, row_y, 30, 0);

    sprintf(path, "%s\\%s", dest_dir, name);
    out = fopen(path, "wb");
    if (!out) {
        tui_print_at(70, row_y, TUI_LIGHTRED, TUI_BLUE, "FAIL");
        /* Still seek past the data so the next entry is readable. */
        if (size) fseek(self_f, (long)size, SEEK_CUR);
        return -1;
    }

    remaining = size;
    while (remaining > 0) {
        unsigned char buf[COPY_CHUNK];
        size_t want = remaining > COPY_CHUNK ? COPY_CHUNK : (size_t)remaining;
        size_t got  = fread(buf, 1, want, self_f);
        int pct;
        if (got == 0) {
            fclose(out);
            tui_print_at(70, row_y, TUI_LIGHTRED, TUI_BLUE, "EOF ");
            return -1;
        }
        if (fwrite(buf, 1, got, out) != got) {
            fclose(out);
            tui_print_at(70, row_y, TUI_LIGHTRED, TUI_BLUE, "WERR");
            return -1;
        }
        remaining -= (u32)got;
        pct = size > 0 ? (int)(((size - remaining) * 100UL) / size) : 100;
        tui_progress(36, row_y, 30, pct);
    }
    fclose(out);

    tui_print_at(70, row_y, TUI_LIGHTGREEN, TUI_BLUE, " OK ");
    if (out_ok) (*out_ok)++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv) {
    char self_path[128];
    char dest[80];
    FILE *self_f;
    unsigned char trailer[TRAILER_SIZE];
    u32 dir_offset, file_count, i;
    int ok = 0;
    int rc = 0;

    tui_init();
    tui_title_bar("PicoGUS Bundle Extractor " PGWIZ_VERSION);

    /* Welcome panel. */
    tui_draw_box(8, 4, 64, 12, "Welcome", 1);
    tui_centre(6, TUI_LIGHTCYAN, TUI_BLUE, "PicoGUS self-extracting installer bundle");
    tui_centre(8, TUI_WHITE,     TUI_BLUE, "This will extract the installer + utilities + firmware");
    tui_centre(9, TUI_WHITE,     TUI_BLUE, "into a directory of your choice.");
    tui_centre(11, TUI_LIGHTGRAY, TUI_BLUE,
        "Press ENTER to accept the default path, or edit and press ENTER.");
    tui_centre(12, TUI_LIGHTGRAY, TUI_BLUE,
        "Press ESC to cancel.");

    /* Locate our own EXE before asking the user anything, so we can
     * fail fast if we can't read it back. */
    if (resolve_self_path(argv ? argv[0] : 0, self_path, sizeof(self_path)) != 0) {
        tui_message("Error", "Could not determine path to this EXE.", TUI_ERROR);
        tui_shutdown();
        return 1;
    }

    strcpy(dest, "C:\\PICOGUS");
    tui_status_bar(" ENTER accept   ESC cancel ");
    if (tui_input(15, 14, 50, "Destination:", dest, 79) < 0) {
        tui_shutdown();
        return 0;
    }

    self_f = fopen(self_path, "rb");
    if (!self_f) {
        char msg[160];
        sprintf(msg, "Could not open self for reading:\n%s", self_path);
        tui_message("Error", msg, TUI_ERROR);
        tui_shutdown();
        return 1;
    }

    /* Read the trailer at the end of the file. */
    if (fseek(self_f, -(long)TRAILER_SIZE, SEEK_END) != 0
        || fread(trailer, 1, TRAILER_SIZE, self_f) != TRAILER_SIZE) {
        tui_message("Error", "Could not read SFX trailer.", TUI_ERROR);
        fclose(self_f);
        tui_shutdown();
        return 1;
    }
    if (trailer[0] != 'P' || trailer[1] != 'G' ||
        trailer[2] != 'Z' || trailer[3] != '1') {
        tui_message("Error",
            "Bundle data missing or corrupt.\n"
            "This stub was not packed properly.",
            TUI_ERROR);
        fclose(self_f);
        tui_shutdown();
        return 1;
    }
    dir_offset = read_u32(trailer + 4);
    file_count = read_u32(trailer + 8);

    /* Create destination directory if it isn't there. */
    if (!dir_exists(dest)) {
        if (make_dir(dest) != 0) {
            char msg[160];
            sprintf(msg, "Could not create directory:\n%s", dest);
            tui_message("Error", msg, TUI_ERROR);
            fclose(self_f);
            tui_shutdown();
            return 1;
        }
    }

    /* Repaint for the extraction phase. */
    tui_clear();
    tui_title_bar("PicoGUS Bundle Extractor " PGWIZ_VERSION " - Extracting");
    tui_draw_box(2, 3, 76, 20, "Files", 1);
    {
        char line[80];
        sprintf(line, "Destination: %s    Files: %lu",
                dest, (unsigned long)file_count);
        tui_print_at(4, 4, TUI_LIGHTCYAN, TUI_BLUE, line);
    }

    if (fseek(self_f, (long)dir_offset, SEEK_SET) != 0) {
        tui_message("Error", "Seek to file directory failed.", TUI_ERROR);
        fclose(self_f);
        tui_shutdown();
        return 1;
    }

    /* Extract each file. */
    for (i = 0; i < file_count; i++) {
        int row = 6 + (int)i;
        if (row > 21) {
            /* If we ever pack more than ~15 files, keep printing on the
             * bottom row and let earlier ones scroll out of the visible
             * area. Good enough; the user can still see progress. */
            row = 21;
        }
        if (extract_one(self_f, dest, row, &ok) != 0) {
            rc = 1;
            break;
        }
    }
    fclose(self_f);

    tui_status_bar(" ENTER continue ");
    tui_getkey();

    if (rc == 0 && ok == (int)file_count) {
        char buf[200];
        sprintf(buf,
            "Bundle extracted to:\n  %s\n\n"
            "%d files written.\n\n"
            "Next steps:\n"
            "  %s\\PGINST.EXE   - first-run setup wizard\n"
            "  %s\\PGSETUP.EXE  - settings manager",
            dest, ok, dest, dest);
        tui_message("Done", buf, TUI_OK);
    } else {
        char buf[200];
        sprintf(buf,
            "Extracted %d of %lu files.\n"
            "Some files failed - check destination free space and try again.",
            ok, (unsigned long)file_count);
        tui_message("Incomplete", buf, TUI_WARN);
    }

    tui_shutdown();
    return rc;
}
