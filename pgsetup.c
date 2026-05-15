/*
 * pgsetup.c - PicoGUS settings manager.
 *
 * Persistent UI for adjusting card settings between sessions. Reads the
 * current state by invoking PGUSINIT with no args and parsing its stdout,
 * then provides a menu-driven editor that re-invokes PGUSINIT with the
 * appropriate option flags to apply each change.
 *
 * Unlike PGINST this program does NOT touch AUTOEXEC.BAT or CONFIG.SYS.
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

/* Mode codes shared with pginst.c. */
#define MODE_GUS    0
#define MODE_SB16   1
#define MODE_SBPRO  2
#define MODE_ADLIB  3
#define MODE_MPU    4
#define MODE_TANDY  5
#define MODE_CMS    6
#define MODE_USB    7
#define MODE_COUNT  8

static const char *MODE_NAME[MODE_COUNT] = {
    "GUS",   "SB16",  "SBPro", "AdLib",
    "MPU",   "Tandy", "CMS",   "USB"
};
static const char *MODE_PGUSINIT[MODE_COUNT] = {
    "gus", "sb", "sb", "adlib", "mpu", "psg", "psg", "usb"
};

/* ------------------------------------------------------------------ */
/* Card state                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int  mode;
    int  port;             /* primary port for current mode           */
    int  irq;
    int  dma;
    int  mpu_port;
    int  opl_port;
    int  main_vol;
    int  wt_vol;
    int  sb_vol;
    int  opl_vol;
    int  psg_vol;
    int  audio_buffer;
    int  dma_interval;
    int  sb_type;          /* 1..6                                    */
    int  usb_joy;
    int  mouse_com;        /* 0=disabled, 1..4                        */
    int  mpu_sysex;
    int  mpu_fake;
    int  opl_wait;
    int  sb_fix_tc;
    int  cd_auto;
    int  sb_lockmix;       /* 0..2                                    */
    char fw_version[40];
    char hw_version[40];
    int  detected;         /* 1 if we read these from a running card  */
} CardState;

static void state_defaults(CardState *s) {
    memset(s, 0, sizeof(*s));
    s->mode = MODE_GUS;
    s->port = 0x240;
    s->irq = 5;
    s->dma = 1;
    s->mpu_port = 0x330;
    s->opl_port = 0x388;
    s->main_vol = 90;
    s->wt_vol = 75;
    s->sb_vol = 90;
    s->opl_vol = 90;
    s->psg_vol = 90;
    s->audio_buffer = 4;
    s->dma_interval = 0;
    s->sb_type = 6;
    strcpy(s->fw_version, "(unknown)");
    strcpy(s->hw_version, "(unknown)");
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int parse_hex(const char *s) {
    int v = 0;
    while (*s == ' ') s++;
    while (*s) {
        char c = *s++;
        if (c >= '0' && c <= '9')       v = v*16 + (c - '0');
        else if (c >= 'a' && c <= 'f')  v = v*16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')  v = v*16 + (c - 'A' + 10);
        else break;
    }
    return v;
}

static int parse_dec(const char *s) {
    int v = 0;
    int neg = 0;
    while (*s == ' ') s++;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v*10 + (*s++ - '0');
    return neg ? -v : v;
}

static void strncpy_safe(char *dst, const char *src, int n) {
    int i;
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* find a substring (case-insensitive) and return pointer to first char
 * after the match, or NULL if no match. */
static const char *find_ci(const char *hay, const char *needle) {
    int nlen = (int)strlen(needle);
    while (*hay) {
        int i;
        for (i = 0; i < nlen; i++) {
            if (toupper((unsigned char)hay[i]) != toupper((unsigned char)needle[i])) break;
        }
        if (i == nlen) return hay + nlen;
        hay++;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Run a shell command, optionally collecting stdout to a buffer.       */
/* We use a small temp file because real-mode DOS has no pipes.         */
/* ------------------------------------------------------------------ */

#define TMP_PATH "C:\\PGUS_TMP.TXT"

static int run_silent(const char *cmd) {
    char full[256];
    int rc;
    /* > redirects stdout; 2>&1 isn't portable across DOS shells, so we
     * accept that stderr will still show up on screen. */
    sprintf(full, "%s > %s", cmd, TMP_PATH);
    rc = system(full);
    return rc;
}

static int read_tmp(char *buf, int max) {
    FILE *f = fopen(TMP_PATH, "r");
    int n;
    if (!f) return 0;
    n = (int)fread(buf, 1, max - 1, f);
    fclose(f);
    buf[n] = '\0';
    remove(TMP_PATH);
    return n;
}

/* ------------------------------------------------------------------ */
/* Parse PGUSINIT's status output.                                     */
/* ------------------------------------------------------------------ */
static void parse_status(CardState *s, const char *txt) {
    const char *p;

    /* Firmware version line. Example: "PicoGUS detected: Firmware version: picogus-gus v3.9.0" */
    p = find_ci(txt, "Firmware version:");
    if (p) {
        while (*p == ' ') p++;
        {
            const char *end = p;
            while (*end && *end != '\r' && *end != '\n') end++;
            strncpy_safe(s->fw_version, p, (int)(end - p + 1));
            if ((int)(end - p) >= (int)sizeof(s->fw_version))
                s->fw_version[sizeof(s->fw_version)-1] = '\0';
        }
        s->detected = 1;
    }

    /* Hardware line. Example: "Hardware: PicoGUS v2.0" */
    p = find_ci(txt, "Hardware:");
    if (p) {
        while (*p == ' ') p++;
        {
            const char *end = p;
            while (*end && *end != '\r' && *end != '\n') end++;
            strncpy_safe(s->hw_version, p, (int)(end - p + 1));
            s->detected = 1;
        }
    }

    /* Operating mode. */
    p = find_ci(txt, "Running in GUS mode on port");
    if (p) {
        s->mode = MODE_GUS;
        s->port = parse_hex(p);
        s->detected = 1;
        return;
    }
    p = find_ci(txt, "Running in Sound Blaster");
    if (p) {
        const char *q = find_ci(p, "on port");
        s->mode = MODE_SB16; /* exact subtype hard to derive; user can refine */
        if (q) s->port = parse_hex(q);
        q = find_ci(p, "IRQ");
        if (q) s->irq = parse_dec(q);
        q = find_ci(p, "DMA");
        if (q) s->dma = parse_dec(q);
        s->detected = 1;
        return;
    }
    p = find_ci(txt, "Running in AdLib/OPL2 mode on port");
    if (p) {
        s->mode = MODE_ADLIB;
        s->opl_port = parse_hex(p);
        s->detected = 1;
        return;
    }
    p = find_ci(txt, "Running in MPU-401 only mode");
    if (p) {
        s->mode = MODE_MPU;
        s->detected = 1;
        return;
    }
    p = find_ci(txt, "Running in PSG mode");
    if (p) {
        /* Could be Tandy or CMS - we pick Tandy and let the user switch. */
        s->mode = MODE_TANDY;
        s->detected = 1;
        return;
    }
    p = find_ci(txt, "Running in USB mode");
    if (p) {
        s->mode = MODE_USB;
        s->detected = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Detect: run pgusinit, parse, fill state                             */
/* ------------------------------------------------------------------ */
static int detect_card(CardState *s) {
    static char buf[2048];
    int rc;
    /* Try ./PGUSINIT.EXE then C:\PICOGUS\PGUSINIT.EXE.                  */
    rc = run_silent("PGUSINIT.EXE");
    if (rc != 0 || read_tmp(buf, sizeof(buf)) == 0) {
        rc = run_silent("C:\\PICOGUS\\PGUSINIT.EXE");
        read_tmp(buf, sizeof(buf));
    }
    parse_status(s, buf);
    return s->detected ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Header strip drawn on the main screen and after every action.        */
/* ------------------------------------------------------------------ */
static void draw_header(const CardState *s) {
    char line[80];
    tui_title_bar("PicoGUS Setup  " PGWIZ_VERSION "       [F1=Help] [F10=Exit]");
    tui_fill(1, 2, 80, 2, TUI_WHITE, TUI_BLUE, ' ');
    sprintf(line, "Mode: %-5s  Port: %03Xh  IRQ: %-2d  DMA: %-2d  MPU: %03Xh  Vol: %3d%%",
            MODE_NAME[s->mode], s->port, s->irq, s->dma, s->mpu_port, s->main_vol);
    tui_print_at(2, 2, TUI_LIGHTCYAN, TUI_BLUE, line);
    sprintf(line, "Firmware: %-22s  Hardware: %s",
            s->fw_version, s->hw_version);
    tui_print_at(2, 3, TUI_LIGHTGRAY, TUI_BLUE, line);
}

/* ------------------------------------------------------------------ */
/* Edit primitives                                                     */
/* ------------------------------------------------------------------ */

static void edit_hex_field(int x, int y, int *v, int min_v, int max_v) {
    char buf[8];
    int t;
    sprintf(buf, "%X", *v);
    tui_input(x, y, 6, "", buf, 5);
    t = parse_hex(buf);
    if (t < min_v) t = min_v;
    if (t > max_v) t = max_v;
    *v = t;
}

static void edit_dec_field(int x, int y, int *v, int min_v, int max_v) {
    char buf[8];
    int t;
    sprintf(buf, "%d", *v);
    tui_input(x, y, 6, "", buf, 5);
    t = parse_dec(buf);
    if (t < min_v) t = min_v;
    if (t > max_v) t = max_v;
    *v = t;
}

/* ------------------------------------------------------------------ */
/* Apply: run pgusinit with the given arg string and refresh detect.    */
/* ------------------------------------------------------------------ */
static void apply_cmd(CardState *s, const char *args) {
    char cmd[256];
    sprintf(cmd, "PGUSINIT.EXE %s > %s", args, TMP_PATH);
    if (system(cmd) != 0) {
        sprintf(cmd, "C:\\PICOGUS\\PGUSINIT.EXE %s > %s", args, TMP_PATH);
        system(cmd);
    }
    remove(TMP_PATH);
    detect_card(s); /* refresh - not all commands echo new state */
}

/* ------------------------------------------------------------------ */
/* Sub-screen: card mode                                                */
/* ------------------------------------------------------------------ */
static void sub_mode(CardState *s) {
    static const char *items[] = {
        "GUS    - Gravis UltraSound",
        "SB16   - Sound Blaster 16",
        "SBPro  - Sound Blaster Pro 2",
        "AdLib  - AdLib / OPL2",
        "MPU    - MPU-401 MIDI",
        "Tandy  - Tandy 3-Voice",
        "CMS    - CMS/Game Blaster",
        "USB    - USB only (mouse/joystick/CD-ROM)"
    };
    int sel;
    tui_fill(30, 5, 50, 18, TUI_WHITE, TUI_BLUE, ' ');
    tui_draw_box(30, 5, 50, 18, "Card Mode", 1);
    tui_print_at(32, 7,  TUI_WHITE,     TUI_BLUE, "Current mode marked with *");
    sel = tui_menu(32, 9, 46, items, MODE_COUNT, s->mode);
    if (sel < 0) return;

    if (sel != s->mode) {
        char buf[80];
        sprintf(buf, "Change card mode to %s?\nSettings will apply immediately.",
                MODE_NAME[sel]);
        if (!tui_confirm(buf)) return;
        sprintf(buf, "/mode %s", MODE_PGUSINIT[sel]);
        if (sel == MODE_SB16)  strcat(buf, " /sbtype 6");
        if (sel == MODE_SBPRO) strcat(buf, " /sbtype 4");
        apply_cmd(s, buf);
        s->mode = sel;
    }
}

/* ------------------------------------------------------------------ */
/* Sub-screen: hardware (port / irq / dma)                              */
/* ------------------------------------------------------------------ */
static void sub_hardware(CardState *s) {
    char args[160];
    char piece[32];
    int row = 7;

    tui_fill(30, 5, 50, 18, TUI_WHITE, TUI_BLUE, ' ');
    tui_draw_box(30, 5, 50, 18, "Hardware Addresses", 1);

    /* Show fields appropriate to current mode. */
    switch (s->mode) {
    case MODE_GUS:
        tui_print_at(32, row,   TUI_WHITE, TUI_BLUE, "GUS Port:");
        tui_print_at(32, row+2, TUI_WHITE, TUI_BLUE, "IRQ:");
        tui_print_at(32, row+4, TUI_WHITE, TUI_BLUE, "DMA:");
        tui_print_at(32, row+6, TUI_WHITE, TUI_BLUE, "MPU Port:");
        break;
    case MODE_SB16:
    case MODE_SBPRO:
        tui_print_at(32, row,   TUI_WHITE, TUI_BLUE, "SB Port:");
        tui_print_at(32, row+2, TUI_WHITE, TUI_BLUE, "SB IRQ:");
        tui_print_at(32, row+4, TUI_WHITE, TUI_BLUE, "SB DMA:");
        tui_print_at(32, row+6, TUI_WHITE, TUI_BLUE, "OPL Port:");
        tui_print_at(32, row+8, TUI_WHITE, TUI_BLUE, "MPU Port:");
        break;
    case MODE_ADLIB:
        tui_print_at(32, row,   TUI_WHITE, TUI_BLUE, "OPL Port:");
        tui_print_at(32, row+2, TUI_WHITE, TUI_BLUE, "MPU Port:");
        break;
    case MODE_MPU:
        tui_print_at(32, row,   TUI_WHITE, TUI_BLUE, "MPU Port:");
        break;
    case MODE_TANDY:
        tui_print_at(32, row,   TUI_WHITE, TUI_BLUE, "Tandy Port:");
        tui_print_at(32, row+2, TUI_WHITE, TUI_BLUE, "MPU Port:");
        break;
    case MODE_CMS:
        tui_print_at(32, row,   TUI_WHITE, TUI_BLUE, "CMS Port:");
        tui_print_at(32, row+2, TUI_WHITE, TUI_BLUE, "MPU Port:");
        break;
    default:
        tui_print_at(32, row, TUI_WHITE, TUI_BLUE, "MPU Port:");
        break;
    }

    /* Red warning block. */
    tui_fill(32, row + 11, 46, 3, TUI_WHITE, TUI_RED, ' ');
    tui_draw_box(32, row + 11, 46, 3, " WARNING ", 0);
    tui_print_at(34, row + 12, TUI_WHITE, TUI_RED,
                 "IRQ/DMA must match physical jumper settings.");

    /* Walk fields appropriate to mode. */
    args[0] = '\0';
    switch (s->mode) {
    case MODE_GUS:
        edit_hex_field(48, row,   &s->port,    0x200, 0x3FF);
        edit_dec_field(48, row+2, &s->irq,     2, 15);
        edit_dec_field(48, row+4, &s->dma,     1, 7);
        edit_hex_field(48, row+6, &s->mpu_port, 0, 0x3FF);
        sprintf(piece, "/gusport %X ",  s->port);    strcat(args, piece);
        sprintf(piece, "/mpuport %X ",  s->mpu_port); strcat(args, piece);
        break;
    case MODE_SB16:
    case MODE_SBPRO:
        edit_hex_field(48, row,   &s->port,     0x200, 0x3FF);
        edit_dec_field(48, row+2, &s->irq,      2, 15);
        edit_dec_field(48, row+4, &s->dma,      0, 7);
        edit_hex_field(48, row+6, &s->opl_port, 0x200, 0x3FF);
        edit_hex_field(48, row+8, &s->mpu_port, 0, 0x3FF);
        sprintf(piece, "/sbport %X ",   s->port);     strcat(args, piece);
        sprintf(piece, "/sbirq %d ",    s->irq);      strcat(args, piece);
        sprintf(piece, "/sbdma %d ",    s->dma);      strcat(args, piece);
        sprintf(piece, "/oplport %X ",  s->opl_port); strcat(args, piece);
        sprintf(piece, "/mpuport %X ",  s->mpu_port); strcat(args, piece);
        break;
    case MODE_ADLIB:
        edit_hex_field(48, row,   &s->opl_port, 0x200, 0x3FF);
        edit_hex_field(48, row+2, &s->mpu_port, 0, 0x3FF);
        sprintf(piece, "/oplport %X ",  s->opl_port); strcat(args, piece);
        sprintf(piece, "/mpuport %X ",  s->mpu_port); strcat(args, piece);
        break;
    case MODE_MPU:
        edit_hex_field(48, row, &s->mpu_port, 0, 0x3FF);
        sprintf(piece, "/mpuport %X ",  s->mpu_port); strcat(args, piece);
        break;
    case MODE_TANDY:
        edit_hex_field(48, row,   &s->port,     0x200, 0x3FF);
        edit_hex_field(48, row+2, &s->mpu_port, 0, 0x3FF);
        sprintf(piece, "/tandyport %X ", s->port);    strcat(args, piece);
        sprintf(piece, "/mpuport %X ",   s->mpu_port); strcat(args, piece);
        break;
    case MODE_CMS:
        edit_hex_field(48, row,   &s->port,     0x200, 0x3FF);
        edit_hex_field(48, row+2, &s->mpu_port, 0, 0x3FF);
        sprintf(piece, "/cmsport %X ",   s->port);    strcat(args, piece);
        sprintf(piece, "/mpuport %X ",   s->mpu_port); strcat(args, piece);
        break;
    default:
        break;
    }

    if (tui_confirm("Apply hardware changes?")) {
        apply_cmd(s, args);
    }
}

/* ------------------------------------------------------------------ */
/* Sub-screen: volumes                                                  */
/* ------------------------------------------------------------------ */

static void draw_slider(int x, int y, const char *label, int v) {
    char buf[64];
    int fill = (v * 20) / 100;
    int i;
    tui_print_at(x, y, TUI_WHITE, TUI_BLUE, label);
    tui_print_at(x + 18, y, TUI_CYAN, TUI_BLUE, "[");
    for (i = 0; i < 20; i++) {
        char ch[2];
        ch[0] = (i < fill) ? (char)0xDB : (char)0xB0;
        ch[1] = '\0';
        tui_print_at(x + 19 + i, y, TUI_LIGHTGREEN, TUI_BLUE, ch);
    }
    tui_print_at(x + 39, y, TUI_CYAN, TUI_BLUE, "]");
    sprintf(buf, " %3d%%", v);
    tui_print_at(x + 40, y, TUI_WHITE, TUI_BLUE, buf);
}

static void sub_volume(CardState *s) {
    int *vols[5];
    const char *labels[5];
    int n = 0;
    int sel = 0;
    int k;
    int row;

    vols[n] = &s->main_vol; labels[n++] = "Main Volume:      ";
    vols[n] = &s->wt_vol;   labels[n++] = "WT Header Volume: ";
    if (s->mode == MODE_SB16 || s->mode == MODE_SBPRO) {
        vols[n] = &s->sb_vol;  labels[n++] = "SB Volume:        ";
    }
    if (s->mode == MODE_SB16 || s->mode == MODE_SBPRO || s->mode == MODE_ADLIB) {
        vols[n] = &s->opl_vol; labels[n++] = "OPL Volume:       ";
    }
    if (s->mode == MODE_TANDY || s->mode == MODE_CMS) {
        vols[n] = &s->psg_vol; labels[n++] = "PSG Volume:       ";
    }

    tui_fill(20, 5, 60, 18, TUI_WHITE, TUI_BLUE, ' ');
    tui_draw_box(20, 5, 60, 18, "Volume Controls", 1);
    tui_print_at(22, 20, TUI_LIGHTCYAN, TUI_BLUE,
                 "UP/DOWN select  LEFT/RIGHT +/-5  +/- +/-1");
    tui_print_at(22, 21, TUI_LIGHTCYAN, TUI_BLUE,
                 "ENTER apply     S apply+save     ESC back");

    for (;;) {
        int i;
        for (i = 0; i < n; i++) {
            int marker_x = 22;
            int y = 8 + i * 2;
            tui_print_at(marker_x - 1, y, TUI_YELLOW, TUI_BLUE,
                         i == sel ? ">" : " ");
            draw_slider(marker_x, y, labels[i], *vols[i]);
        }

        k = tui_getkey();
        switch (k) {
        case KEY_UP:    sel = (sel == 0)     ? n - 1 : sel - 1; break;
        case KEY_DOWN:  sel = (sel == n - 1) ? 0     : sel + 1; break;
        case KEY_LEFT:  *vols[sel] -= 5; if (*vols[sel] < 0)   *vols[sel] = 0;   break;
        case KEY_RIGHT: *vols[sel] += 5; if (*vols[sel] > 100) *vols[sel] = 100; break;
        case '-':       *vols[sel] -= 1; if (*vols[sel] < 0)   *vols[sel] = 0;   break;
        case '+': case '=':
                        *vols[sel] += 1; if (*vols[sel] > 100) *vols[sel] = 100; break;
        case KEY_ENTER: {
            char args[128];
            char piece[32];
            args[0] = '\0';
            sprintf(piece, "/mainvol %d ", s->main_vol); strcat(args, piece);
            sprintf(piece, "/wtvol %d ",   s->wt_vol);   strcat(args, piece);
            if (s->mode == MODE_SB16 || s->mode == MODE_SBPRO) {
                sprintf(piece, "/sbvol %d ",  s->sb_vol);  strcat(args, piece);
                sprintf(piece, "/oplvol %d ", s->opl_vol); strcat(args, piece);
            }
            if (s->mode == MODE_ADLIB) {
                sprintf(piece, "/oplvol %d ", s->opl_vol); strcat(args, piece);
            }
            if (s->mode == MODE_TANDY || s->mode == MODE_CMS) {
                sprintf(piece, "/psgvol %d ", s->psg_vol); strcat(args, piece);
            }
            apply_cmd(s, args);
            tui_message("Volume", "Volumes applied.", TUI_OK);
            return;
        }
        case 's': case 'S': {
            apply_cmd(s, "/save");
            tui_message("Volume", "Saved to flash.", TUI_OK);
            return;
        }
        case KEY_ESC:
        case KEY_F10:
            return;
        }
        for (row = 0; row < n; row++) {
            /* re-render in next iteration */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Sub-screen: MPU-401 settings                                         */
/* ------------------------------------------------------------------ */
static void sub_mpu(CardState *s) {
    char buf[128];
    char piece[40];
    tui_fill(25, 5, 55, 18, TUI_WHITE, TUI_BLUE, ' ');
    tui_draw_box(25, 5, 55, 18, "MPU-401 Settings", 1);

    tui_print_at(27, 7,  TUI_WHITE, TUI_BLUE, "MPU Port:");
    tui_print_at(27, 9,  TUI_WHITE, TUI_BLUE, "Sysex Delay:");
    tui_print_at(27, 11, TUI_WHITE, TUI_BLUE, "Fake All-Notes-Off:");

    edit_hex_field(48, 7, &s->mpu_port, 0, 0x3FF);

    if (tui_confirm("Toggle Sysex Delay?")) s->mpu_sysex = !s->mpu_sysex;
    if (tui_confirm("Toggle Fake All-Notes-Off?")) s->mpu_fake = !s->mpu_fake;

    sprintf(piece, "%-3s", s->mpu_sysex ? "ON" : "OFF");
    tui_print_at(48, 9,  TUI_LIGHTGREEN, TUI_BLUE, piece);
    sprintf(piece, "%-3s", s->mpu_fake  ? "ON" : "OFF");
    tui_print_at(48, 11, TUI_LIGHTGREEN, TUI_BLUE, piece);

    tui_print_at(27, 14, TUI_LIGHTCYAN, TUI_BLUE,
                 "MPU-401 works in every card mode simultaneously.");
    tui_print_at(27, 15, TUI_LIGHTCYAN, TUI_BLUE,
                 "Sysex Delay -> Roland MT-32 rev.0 compatibility.");
    tui_print_at(27, 16, TUI_LIGHTCYAN, TUI_BLUE,
                 "Fake all-notes-off -> Roland RA-50 compatibility.");

    if (tui_confirm("Apply MPU-401 changes?")) {
        sprintf(buf, "/mpuport %X /mpudelay %d /mpufake %d",
                s->mpu_port, s->mpu_sysex, s->mpu_fake);
        apply_cmd(s, buf);
    }
}

/* ------------------------------------------------------------------ */
/* Sub-screen: GUS settings                                             */
/* ------------------------------------------------------------------ */
static void sub_gus(CardState *s) {
    char args[128];
    if (s->mode != MODE_GUS) {
        tui_message("GUS Settings",
                    "(not applicable in current mode)\nSwitch to GUS first.",
                    TUI_INFO);
        return;
    }
    tui_fill(20, 5, 60, 18, TUI_WHITE, TUI_BLUE, ' ');
    tui_draw_box(20, 5, 60, 18, "GUS Settings", 1);
    tui_print_at(22, 7,  TUI_WHITE, TUI_BLUE, "GUS Port:");
    tui_print_at(22, 9,  TUI_WHITE, TUI_BLUE, "Audio Buffer:");
    tui_print_at(22, 11, TUI_WHITE, TUI_BLUE, "DMA Interval:");

    edit_hex_field(48, 7,  &s->port,         0x200, 0x3FF);
    edit_dec_field(48, 9,  &s->audio_buffer, 1, 256);
    edit_dec_field(48, 11, &s->dma_interval, 0, 1000);

    tui_print_at(22, 14, TUI_LIGHTCYAN, TUI_BLUE,
                 "Buffer 1-3: lower latency, may glitch some programs");
    tui_print_at(22, 15, TUI_LIGHTCYAN, TUI_BLUE,
                 "Buffer 4:   default, works for most software");
    tui_print_at(22, 16, TUI_LIGHTCYAN, TUI_BLUE,
                 "Buffer 8+:  helps programs that hang or drop notes");
    tui_print_at(22, 18, TUI_YELLOW, TUI_BLUE,
                 "Recommended: leave Buffer at 4 unless you have a problem.");

    if (tui_confirm("Apply GUS settings?")) {
        sprintf(args, "/gusport %X /gusbuf %d /gusdma %d",
                s->port, s->audio_buffer, s->dma_interval);
        apply_cmd(s, args);
    }
}

/* ------------------------------------------------------------------ */
/* Sub-screen: SB settings                                              */
/* ------------------------------------------------------------------ */
static void sub_sb(CardState *s) {
    static const char *sbtypes[] = {
        "1 - SB 1.x",
        "2 - SB Pro 1 (dual OPL2)",
        "3 - SB 2.0",
        "4 - SB Pro 2 (OPL3)",
        "6 - SB 16"
    };
    static const int  sbtype_codes[] = { 1, 2, 3, 4, 6 };
    int chosen, i, current = 4;
    char args[160], piece[40];

    if (s->mode != MODE_SB16 && s->mode != MODE_SBPRO) {
        tui_message("SB Settings",
                    "(not applicable in current mode)\nSwitch to SB first.",
                    TUI_INFO);
        return;
    }

    tui_fill(20, 5, 60, 18, TUI_WHITE, TUI_BLUE, ' ');
    tui_draw_box(20, 5, 60, 18, "Sound Blaster Settings", 1);
    tui_print_at(22, 7,  TUI_WHITE, TUI_BLUE, "SB Port:");
    tui_print_at(22, 9,  TUI_WHITE, TUI_BLUE, "SB IRQ:");
    tui_print_at(22, 11, TUI_WHITE, TUI_BLUE, "SB DMA:");
    tui_print_at(22, 13, TUI_WHITE, TUI_BLUE, "OPL Port:");
    tui_print_at(22, 15, TUI_WHITE, TUI_BLUE, "SB Volume:");
    edit_hex_field(48, 7,  &s->port,    0x200, 0x3FF);
    edit_dec_field(48, 9,  &s->irq,     2, 15);
    edit_dec_field(48, 11, &s->dma,     0, 7);
    edit_hex_field(48, 13, &s->opl_port, 0x200, 0x3FF);
    edit_dec_field(48, 15, &s->sb_vol,  0, 100);

    /* SB type chooser. */
    for (i = 0; i < 5; i++) if (sbtype_codes[i] == s->sb_type) current = i;
    tui_print_at(22, 17, TUI_WHITE, TUI_BLUE, "SB Type:");
    chosen = tui_menu(32, 17, 30, sbtypes, 5, current);
    if (chosen >= 0) s->sb_type = sbtype_codes[chosen];

    if (tui_confirm("Apply SB settings?")) {
        args[0] = '\0';
        sprintf(piece, "/sbport %X ",   s->port);      strcat(args, piece);
        sprintf(piece, "/sbirq %d ",    s->irq);       strcat(args, piece);
        sprintf(piece, "/sbdma %d ",    s->dma);       strcat(args, piece);
        sprintf(piece, "/sbtype %d ",   s->sb_type);   strcat(args, piece);
        sprintf(piece, "/oplport %X ",  s->opl_port);  strcat(args, piece);
        sprintf(piece, "/sbvol %d ",    s->sb_vol);    strcat(args, piece);
        if (s->sb_fix_tc) strcat(args, "/sbfixtc 1 ");
        apply_cmd(s, args);
    }
}

/* ------------------------------------------------------------------ */
/* Sub-screen: AdLib / OPL settings                                     */
/* ------------------------------------------------------------------ */
static void sub_adlib(CardState *s) {
    char args[128], piece[40];
    tui_fill(20, 5, 60, 18, TUI_WHITE, TUI_BLUE, ' ');
    tui_draw_box(20, 5, 60, 18, "AdLib / OPL Settings", 1);
    tui_print_at(22, 7,  TUI_WHITE, TUI_BLUE, "OPL Port:");
    tui_print_at(22, 9,  TUI_WHITE, TUI_BLUE, "OPL Volume:");
    edit_hex_field(48, 7, &s->opl_port, 0x200, 0x3FF);
    edit_dec_field(48, 9, &s->opl_vol,  0, 100);

    if (tui_confirm("Enable OPL Wait (slow OPL writes)?")) s->opl_wait = 1;
    else s->opl_wait = 0;
    tui_print_at(22, 11, TUI_WHITE, TUI_BLUE, "OPL Wait:");
    tui_print_at(48, 11, TUI_LIGHTGREEN, TUI_BLUE, s->opl_wait ? "ON " : "OFF");

    tui_print_at(22, 14, TUI_LIGHTCYAN, TUI_BLUE,
                 "OPL Wait fixes 688 Attack Sub and similar speed-sensitive AdLib games.");

    if (tui_confirm("Apply OPL settings?")) {
        args[0] = '\0';
        sprintf(piece, "/oplport %X /oplvol %d ", s->opl_port, s->opl_vol);
        strcat(args, piece);
        if (s->opl_wait) strcat(args, "/oplwait ");
        apply_cmd(s, args);
    }
}

/* ------------------------------------------------------------------ */
/* Sub-screen: feature toggles                                          */
/* ------------------------------------------------------------------ */
static void sub_features(CardState *s) {
    static const char *labels[] = {
        "USB Joystick   ",
        "Serial Mouse   ",
        "Mouse COM Port ",
        "CD-ROM Auto-Adv"
    };
    int sel = 0;
    int k;
    char args[128], piece[40];

    tui_fill(20, 5, 60, 16, TUI_WHITE, TUI_BLUE, ' ');
    tui_draw_box(20, 5, 60, 16, "Feature Toggles", 1);

    for (;;) {
        char line[60];
        int i;
        const char *vals[4];
        char com_buf[6];
        char joy_buf[6];
        char mouse_buf[6];
        char cd_buf[6];
        strcpy(joy_buf,   s->usb_joy ? "ON " : "OFF");
        strcpy(mouse_buf, s->mouse_com ? "ON " : "OFF");
        sprintf(com_buf,  "%d", s->mouse_com ? s->mouse_com : 1);
        strcpy(cd_buf,    s->cd_auto ? "ON " : "OFF");
        vals[0] = joy_buf;
        vals[1] = mouse_buf;
        vals[2] = com_buf;
        vals[3] = cd_buf;

        for (i = 0; i < 4; i++) {
            int fg = (i == sel) ? TUI_BLACK : TUI_WHITE;
            int bg = (i == sel) ? TUI_CYAN  : TUI_BLUE;
            sprintf(line, " %-18s [%-3s]", labels[i], vals[i]);
            tui_print_at(22, 7 + i, fg, bg, line);
        }

        tui_status_bar(sel == 0 ? " SPACE toggle USB joystick support " :
                       sel == 1 ? " SPACE toggle serial mouse (0=off, 1-4=COM port) " :
                       sel == 2 ? " +/- adjusts COM port (1-4) " :
                                  " SPACE toggle CD-ROM auto-advance ");

        k = tui_getkey();
        switch (k) {
        case KEY_UP:    sel = (sel == 0) ? 3 : sel - 1; break;
        case KEY_DOWN:  sel = (sel == 3) ? 0 : sel + 1; break;
        case KEY_SPACE:
            if (sel == 0) s->usb_joy = !s->usb_joy;
            if (sel == 1) s->mouse_com = s->mouse_com ? 0 : 1;
            if (sel == 3) s->cd_auto = !s->cd_auto;
            break;
        case '+': case '=':
            if (sel == 2 && s->mouse_com > 0 && s->mouse_com < 4) s->mouse_com++;
            break;
        case '-':
            if (sel == 2 && s->mouse_com > 1) s->mouse_com--;
            break;
        case KEY_ENTER:
            if (tui_confirm("Apply feature changes?")) {
                args[0] = '\0';
                sprintf(piece, "/joy %d ",       s->usb_joy);   strcat(args, piece);
                sprintf(piece, "/mousecom %d ",  s->mouse_com); strcat(args, piece);
                sprintf(piece, "/cdauto %d ",    s->cd_auto);   strcat(args, piece);
                apply_cmd(s, args);
            }
            return;
        case KEY_ESC:
        case KEY_F10:
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Sub-screen: save to flash                                            */
/* ------------------------------------------------------------------ */
static void sub_save(CardState *s) {
    static const char *items[] = {
        "Save current settings to flash",
        "Restore all defaults",
        "Cancel"
    };
    int sel;
    tui_fill(20, 6, 60, 14, TUI_WHITE, TUI_BLUE, ' ');
    tui_draw_box(20, 6, 60, 14, "Save Settings", 1);
    tui_print_at(22,  8, TUI_LIGHTCYAN, TUI_BLUE,
                 "Settings persist across reboots and power cycles.");
    tui_print_at(22,  9, TUI_LIGHTCYAN, TUI_BLUE,
                 "PGUSINIT will not need to be run from AUTOEXEC.BAT");
    tui_print_at(22, 10, TUI_LIGHTCYAN, TUI_BLUE,
                 "if settings are saved here.");

    sel = tui_menu(22, 13, 56, items, 3, 0);
    if (sel == 0) {
        if (tui_confirm("Save current card settings to flash?")) {
            apply_cmd(s, "/save");
            tui_message("Saved", "Card settings written to flash.", TUI_OK);
        }
    } else if (sel == 1) {
        if (tui_confirm("Restore all card settings to defaults?\nThis cannot be undone.")) {
            apply_cmd(s, "/defaults");
            tui_message("Defaults", "Card restored to factory defaults.", TUI_OK);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Sub-screen: firmware update                                          */
/* ------------------------------------------------------------------ */
static void sub_firmware(CardState *s) {
    char fname[64];
    char cmd[160];
    strcpy(fname, "picogus.uf2");
    tui_fill(15, 5, 65, 17, TUI_WHITE, TUI_BLUE, ' ');
    tui_draw_box(15, 5, 65, 17, "Firmware Update", 1);
    tui_print_at(17, 7, TUI_WHITE, TUI_BLUE,
                 "Place the .uf2 file in the current directory or enter a path.");

    tui_print_at(17, 9, TUI_WHITE, TUI_BLUE, "Firmware file:");
    tui_input(32, 9, 40, "", fname, 60);

    tui_print_at(17, 11, TUI_LIGHTCYAN, TUI_BLUE, "Common names: picogus.uf2, pg-ne2k.uf2");
    tui_fill(17, 13, 60, 4, TUI_WHITE, TUI_RED, ' ');
    tui_draw_box(17, 13, 60, 4, " WARNING ", 0);
    tui_print_at(19, 14, TUI_WHITE, TUI_RED,
                 "Do NOT power off during firmware update.");
    tui_print_at(19, 15, TUI_WHITE, TUI_RED,
                 "The card will be unavailable for up to 60 seconds.");

    if (!tui_confirm("Begin firmware flash?")) return;

    tui_clear();
    tui_title_bar("PicoGUS Setup - Flashing Firmware");
    tui_print_at(10, 5, TUI_YELLOW, TUI_BLUE, "Flashing firmware. Please wait...");
    tui_progress(10, 7, 60, 50);

    sprintf(cmd, "/flash %s", fname);
    apply_cmd(s, cmd);
    tui_progress(10, 7, 60, 100);
    tui_message("Flash complete",
                "Firmware uploaded. Please reboot the host PC.",
                TUI_OK);
}

/* ------------------------------------------------------------------ */
/* Sub-screen: about                                                    */
/* ------------------------------------------------------------------ */
static void sub_about(CardState *s) {
    (void)s;
    tui_fill(15, 4, 65, 19, TUI_WHITE, TUI_BLUE, ' ');
    tui_draw_box(15, 4, 65, 19, "About", 1);
    tui_print_at(17, 6,  TUI_YELLOW, TUI_BLUE, "PicoGUS Setup Utility " PGWIZ_VERSION);
    tui_print_at(17, 7,  TUI_LIGHTGRAY, TUI_BLUE, "For PicoGUS firmware v3.x and pgusinit v3.x");
    tui_print_at(17, 9,  TUI_WHITE,  TUI_BLUE, "PicoGUS emulates:");
    tui_print_at(19, 10, TUI_LIGHTCYAN, TUI_BLUE, "Gravis UltraSound (GUS)");
    tui_print_at(19, 11, TUI_LIGHTCYAN, TUI_BLUE, "Sound Blaster 16 / Pro 2 / 2.0 / 1.x");
    tui_print_at(19, 12, TUI_LIGHTCYAN, TUI_BLUE, "AdLib / OPL2 / OPL3");
    tui_print_at(19, 13, TUI_LIGHTCYAN, TUI_BLUE, "MPU-401 (intelligent mode)");
    tui_print_at(19, 14, TUI_LIGHTCYAN, TUI_BLUE, "Tandy 3-Voice / CMS / Game Blaster");
    tui_print_at(19, 15, TUI_LIGHTCYAN, TUI_BLUE, "Panasonic/MKE CD-ROM emulation");
    tui_print_at(19, 16, TUI_LIGHTCYAN, TUI_BLUE, "Serial mouse (COM1-COM4)");
    tui_print_at(19, 17, TUI_LIGHTCYAN, TUI_BLUE, "USB gameport joystick");
    tui_print_at(17, 19, TUI_WHITE,  TUI_BLUE, "Project: https://picog.us/");
    tui_print_at(17, 20, TUI_WHITE,  TUI_BLUE, "GitHub:  https://github.com/polpo/picogus");
    tui_print_at(17, 21, TUI_WHITE,  TUI_BLUE, "Batches: see C:\\PICOGUS\\*.BAT");
    tui_pause(" Press any key to return ");
}

/* ------------------------------------------------------------------ */
/* Main menu                                                            */
/* ------------------------------------------------------------------ */
static const char *MAIN_ITEMS[] = {
    "Card Mode",
    "Port / IRQ / DMA",
    "Volume Controls",
    "MPU-401 Settings",
    "GUS Settings",
    "Sound Blaster Settings",
    "AdLib / OPL Settings",
    "Feature Toggles",
    "Save to Flash",
    "Restore Defaults",
    "Firmware Update",
    "About"
};
#define MAIN_COUNT (sizeof(MAIN_ITEMS)/sizeof(MAIN_ITEMS[0]))

static const char *MAIN_HINTS[MAIN_COUNT] = {
    " Change the emulated card mode (GUS, SB, AdLib, MPU, etc.). ",
    " Set base port, IRQ, and DMA for the current mode. ",
    " Adjust main and per-output volumes. ",
    " Configure MPU-401 MIDI behaviour. ",
    " Buffer size and DMA interval for GUS mode. ",
    " SB type, port, IRQ, DMA and SB-specific quirks. ",
    " OPL port, volume, and OPL wait option. ",
    " Joystick, mouse, CD-ROM auto-advance. ",
    " Persist current settings to the card's flash. ",
    " Reset every card setting to factory defaults. ",
    " Upload a new .uf2 firmware file to the card. ",
    " Version info and credits. "
};

static void redraw_main(const CardState *s, int sel) {
    int i;
    tui_clear();
    draw_header(s);

    tui_draw_box(1, 4, 30, 20, "Menu", 1);
    for (i = 0; i < (int)MAIN_COUNT; i++) {
        int fg = (i == sel) ? TUI_BLACK : TUI_WHITE;
        int bg = (i == sel) ? TUI_CYAN  : TUI_BLUE;
        char line[30];
        sprintf(line, " %-26s ", MAIN_ITEMS[i]);
        tui_print_at(2, 6 + i, fg, bg, line);
    }

    tui_draw_box(32, 4, 47, 20, "Details", 1);
    tui_print_at(34, 6, TUI_YELLOW, TUI_BLUE, MAIN_ITEMS[sel]);
    tui_print_at(34, 8, TUI_LIGHTGRAY, TUI_BLUE, MAIN_HINTS[sel]);
    if (!s->detected) {
        tui_print_at(34, 11, TUI_LIGHTRED, TUI_BLUE,
                     "WARNING: card not detected.");
        tui_print_at(34, 12, TUI_LIGHTRED, TUI_BLUE,
                     "Run PGUSINIT.EXE directly to diagnose.");
    }
    tui_status_bar(MAIN_HINTS[sel]);
}

/* ------------------------------------------------------------------ */
/* Entry                                                                */
/* ------------------------------------------------------------------ */
int main(void) {
    CardState st;
    int sel = 0;
    int k;

    state_defaults(&st);
    tui_init();
    tui_title_bar("PicoGUS Setup  " PGWIZ_VERSION "       Detecting card, please wait...");
    detect_card(&st);

    for (;;) {
        redraw_main(&st, sel);
        k = tui_getkey();
        switch (k) {
        case KEY_UP:    sel = (sel == 0) ? MAIN_COUNT - 1 : sel - 1; break;
        case KEY_DOWN:  sel = (sel == MAIN_COUNT - 1) ? 0 : sel + 1; break;
        case KEY_HOME:  sel = 0; break;
        case KEY_END:   sel = MAIN_COUNT - 1; break;
        case KEY_F1:
            tui_message("Help",
                "PicoGUS Setup\n\n"
                "Use UP/DOWN to highlight a menu item, ENTER to open it.\n"
                "ESC backs out of a sub-screen.\n"
                "F10 or ESC at the main menu exits.\n",
                TUI_INFO);
            break;
        case KEY_ENTER:
            switch (sel) {
            case 0: sub_mode    (&st); break;
            case 1: sub_hardware(&st); break;
            case 2: sub_volume  (&st); break;
            case 3: sub_mpu     (&st); break;
            case 4: sub_gus     (&st); break;
            case 5: sub_sb      (&st); break;
            case 6: sub_adlib   (&st); break;
            case 7: sub_features(&st); break;
            case 8: sub_save    (&st); break;
            case 9:
                if (tui_confirm("Restore card defaults? This cannot be undone.")) {
                    apply_cmd(&st, "/defaults");
                    tui_message("Defaults", "Restored.", TUI_OK);
                }
                break;
            case 10: sub_firmware(&st); break;
            case 11: sub_about   (&st); break;
            }
            break;
        case KEY_ESC:
        case KEY_F10:
            tui_shutdown();
            return 0;
        }
    }
}
