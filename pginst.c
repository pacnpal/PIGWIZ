/*
 * pginst.c - PicoGUS first-run installer.
 *
 * A guided 11-screen wizard. It detects the runtime environment, asks the
 * user for a card mode and hardware addresses, then writes:
 *   C:\PICOGUS\*         - batch helpers and the install destination
 *   C:\ULTRASND\         - GUS patch root (GUS mode only)
 *   C:\AUTOEXEC.BAT      - SET BLASTER/ULTRASND, PGUSINIT, CTMOUSE
 *   C:\CONFIG.SYS        - HIMEM/EMM386/FILES/BUFFERS
 *   C:\AUTOEXEC.BAK      - one-shot backup written before patching
 *   C:\CONFIG.BAK        - one-shot backup written before patching
 *
 * Final step shells out to PGUSINIT.EXE with the appropriate flags so the
 * card is configured immediately, without requiring a reboot.
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

/* ------------------------------------------------------------------ */
/* Card mode codes - kept stable so pgsetup.c can share them          */
/* ------------------------------------------------------------------ */
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

/* String passed to PGUSINIT /mode. Tandy and CMS both use "psg". */
static const char *MODE_PGUSINIT[MODE_COUNT] = {
    "gus", "sb", "sb", "adlib", "mpu", "psg", "psg", "usb"
};

/* sbtype passed to PGUSINIT for SB modes. */
static const int SB_TYPE_FOR_MODE[MODE_COUNT] = {
    0, 6 /* SB16 */, 4 /* SB Pro 2 */, 0, 0, 0, 0, 0
};

/* ------------------------------------------------------------------ */
/* Configuration struct                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    int mode;            /* MODE_*                                  */
    int gus_port;        /* hex word, e.g. 0x240                    */
    int sb_port;
    int opl_port;
    int mpu_port;        /* 0 = disabled                            */
    int tandy_port;
    int cms_port;
    int irq;             /* decimal                                 */
    int dma;             /* decimal                                 */
    int main_vol;        /* 0..100 percent                          */
    int wt_vol;
    int audio_buffer;    /* GUS buffer samples                      */
    int dma_interval;    /* GUS DMA interval us, 0=default          */
    int sb_type;         /* 1,2,3,4,6                               */
    /* Feature toggles - 0/1 booleans. */
    int usb_joy;
    int mouse;
    int mouse_com;       /* 1..4                                    */
    int mpu_sysex;
    int mpu_fake;
    int opl_wait;
    int sb_fix_tc;
    int cd_rom;
    int save_flash;
    /* Environment knowledge filled in during detect. */
    int win9x;           /* C:\WINDOWS\ exists                      */
    int picogus_dir;     /* C:\PICOGUS\ exists                      */
    int has_himem;
    int has_emm386;
    int has_pgusinit;
    int has_autoexec;
    int has_config;
    int has_ctmouse;
} Config;

/* ------------------------------------------------------------------ */
/* Step actions returned by each screen function                       */
/* ------------------------------------------------------------------ */
typedef enum { ACT_NEXT = 0, ACT_BACK = -1, ACT_QUIT = -2 } StepAction;

/* ------------------------------------------------------------------ */
/* Defaults                                                            */
/* ------------------------------------------------------------------ */
static void cfg_defaults(Config *c) {
    memset(c, 0, sizeof(*c));
    c->mode         = MODE_GUS;
    c->gus_port     = 0x240;
    c->sb_port      = 0x220;
    c->opl_port     = 0x388;
    c->mpu_port     = 0x330;
    c->tandy_port   = 0x2C0;
    c->cms_port     = 0x220;
    c->irq          = 5;
    c->dma          = 1;
    c->main_vol     = 90;
    c->wt_vol       = 75;
    c->audio_buffer = 4;
    c->dma_interval = 0;
    c->sb_type      = 6; /* SB16 */
    c->mouse_com    = 1;
    c->save_flash   = 1; /* recommended */
}

/* ------------------------------------------------------------------ */
/* Small filesystem helpers                                            */
/* ------------------------------------------------------------------ */

/* Cross between DOS and Watcom: _dos_getfileattr() returns 0 on success,
 * and the attribute byte tells us file vs directory. */
static int file_exists(const char *path) {
    unsigned attr;
    if (_dos_getfileattr(path, &attr) != 0) return 0;
    return (attr & _A_SUBDIR) ? 0 : 1;
}

static int dir_exists(const char *path) {
    unsigned attr;
    if (_dos_getfileattr(path, &attr) != 0) return 0;
    return (attr & _A_SUBDIR) ? 1 : 0;
}

static int make_dir(const char *path) {
    if (dir_exists(path)) return 0;
    return mkdir(path);
}

/* Case-insensitive substring search. Used to spot existing lines in
 * AUTOEXEC.BAT and CONFIG.SYS we should not duplicate. */
static int strcontains_ci(const char *haystack, const char *needle) {
    int nlen = (int)strlen(needle);
    if (nlen == 0) return 1;
    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        int k = 0;
        while (k < nlen && toupper((unsigned char)*h) == toupper((unsigned char)*n)) {
            h++; n++; k++;
        }
        if (k == nlen) return 1;
        haystack++;
    }
    return 0;
}

/* Copy src -> dst overwriting dst. Returns 0 on success. */
static int copy_file(const char *src, const char *dst) {
    FILE *in, *out;
    char buf[512];
    size_t n;
    in = fopen(src, "rb");
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            fclose(in); fclose(out);
            return -1;
        }
    }
    fclose(in);
    fclose(out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Embedded batch helpers (written to C:\PICOGUS during install)       */
/* DOS prefers CRLF; literals use \r\n.                                */
/* ------------------------------------------------------------------ */

typedef struct { const char *name; const char *body; } BatFile;

static const BatFile BAT_FILES[] = {
    { "GUS.BAT",
      "@echo off\r\n"
      "echo Switching PicoGUS to GUS mode...\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /mode gus\r\n" },
    { "SB.BAT",
      "@echo off\r\n"
      "echo Switching PicoGUS to Sound Blaster 16 mode...\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /mode sb /sbtype 6\r\n" },
    { "SB_PRO.BAT",
      "@echo off\r\n"
      "echo Switching PicoGUS to Sound Blaster Pro 2 mode...\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /mode sb /sbtype 4\r\n" },
    { "MPU.BAT",
      "@echo off\r\n"
      "echo Switching PicoGUS to MPU-401 mode...\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /mode mpu\r\n" },
    { "ADLIB.BAT",
      "@echo off\r\n"
      "echo Switching PicoGUS to AdLib mode...\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /mode adlib\r\n" },
    { "TANDY.BAT",
      "@echo off\r\n"
      "echo Switching PicoGUS to Tandy 3-voice mode...\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /mode psg\r\n" },
    { "CMS.BAT",
      "@echo off\r\n"
      "echo Switching PicoGUS to CMS/Game Blaster mode...\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /mode psg\r\n" },
    { "JOY_ON.BAT",
      "@echo off\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /joy 1\r\n" },
    { "JOY_OFF.BAT",
      "@echo off\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /joy 0\r\n" },
    { "MPU_ON.BAT",
      "@echo off\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /mpuport 330\r\n" },
    { "MPU_OFF.BAT",
      "@echo off\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /mpuport 0\r\n" },
    { "SYSEX_ON.BAT",
      "@echo off\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /mpudelay 1\r\n" },
    { "SYSEX_OFF.BAT",
      "@echo off\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /mpudelay 0\r\n" },
    { "OPLWAIT_ON.BAT",
      "@echo off\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /oplwait\r\n" },
    { "FAKEANOFF_ON.BAT",
      "@echo off\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /mpufake 1\r\n" },
    { "FAKEANOFF_OFF.BAT",
      "@echo off\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /mpufake 0\r\n" },
    { "VOL.BAT",
      "@echo off\r\n"
      "if \"%1\"==\"\" goto :usage\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /mainvol %1\r\n"
      "goto :eof\r\n"
      ":usage\r\n"
      "echo Usage: VOL n   (n = 0..100, percent)\r\n" },
    { "WTVOL.BAT",
      "@echo off\r\n"
      "if \"%1\"==\"\" goto :usage\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /wtvol %1\r\n"
      "goto :eof\r\n"
      ":usage\r\n"
      "echo Usage: WTVOL n   (n = 0..100, percent, PicoGUS 2.0 only)\r\n" },
    { "STATUS.BAT",
      "@echo off\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE\r\n" },
    { "DEFAULTS.BAT",
      "@echo off\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /defaults\r\n" },
    { "FLASH.BAT",
      "@echo off\r\n"
      "if \"%1\"==\"\" goto :usage\r\n"
      "C:\\PICOGUS\\PGUSINIT.EXE /flash %1\r\n"
      "goto :eof\r\n"
      ":usage\r\n"
      "echo Usage: FLASH file.uf2\r\n" }
};
#define BAT_COUNT (sizeof(BAT_FILES)/sizeof(BAT_FILES[0]))

/* ------------------------------------------------------------------ */
/* Spinner used during the detect screen so the user knows the program */
/* is alive while we touch the disk.                                   */
/* ------------------------------------------------------------------ */
static void spin_tick(int x, int y) {
    static const char glyphs[] = { '|', '/', '-', '\\' };
    static int i = 0;
    char s[2];
    s[0] = glyphs[i++ & 3];
    s[1] = '\0';
    tui_print_at(x, y, TUI_YELLOW, TUI_BLUE, s);
    /* tiny delay so the user actually sees the spinner. */
    {
        volatile unsigned long k;
        for (k = 0; k < 30000UL; k++) ;
    }
}

/* ------------------------------------------------------------------ */
/* Hex/dec helpers                                                     */
/* ------------------------------------------------------------------ */
static int parse_hex(const char *s) {
    int v = 0;
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
    while (*s >= '0' && *s <= '9') v = v*10 + (*s++ - '0');
    return v;
}

/* ------------------------------------------------------------------ */
/* Screen 1: welcome                                                   */
/* ------------------------------------------------------------------ */
static StepAction screen_welcome(Config *c) {
    int k;
    (void)c;
    tui_clear();
    tui_title_bar("PicoGUS Installer " PGWIZ_VERSION);
    tui_draw_box(15, 4, 50, 17, "Welcome", 1);

    /* Tiny ASCII art - small enough to read on a CGA monitor. */
    tui_centre(6,  TUI_LIGHTCYAN, TUI_BLUE, "  ____  _           ____ _   _ ____  ");
    tui_centre(7,  TUI_LIGHTCYAN, TUI_BLUE, " |  _ \\(_) ___ ___ / ___| | | / ___| ");
    tui_centre(8,  TUI_LIGHTCYAN, TUI_BLUE, " | |_) | |/ __/ _ \\ |  _| | | \\___ \\ ");
    tui_centre(9,  TUI_LIGHTCYAN, TUI_BLUE, " |  __/| | (_| (_) | |_| | |_| |___) |");
    tui_centre(10, TUI_LIGHTCYAN, TUI_BLUE, " |_|   |_|\\___\\___/ \\____|\\___/|____/ ");

    tui_centre(13, TUI_WHITE,  TUI_BLUE, "Welcome to the PicoGUS Setup Wizard");
    tui_centre(15, TUI_LIGHTGRAY, TUI_BLUE,
        "This will configure your PicoGUS ISA sound card emulator.");
    tui_centre(17, TUI_LIGHTGRAY, TUI_BLUE,
        "Press ENTER to continue, ESC to exit.");

    tui_status_bar(" ENTER continue   ESC exit ");

    for (;;) {
        k = tui_getkey();
        if (k == KEY_ENTER) return ACT_NEXT;
        if (k == KEY_ESC || k == KEY_F10) return ACT_QUIT;
    }
}

/* ------------------------------------------------------------------ */
/* Screen 2: environment detect                                         */
/* ------------------------------------------------------------------ */

static void detect_row(int row, const char *label, int *result_ptr,
                       int (*probe)(void)) {
    int rc;
    tui_print_at(20, row, TUI_WHITE, TUI_BLUE, label);
    spin_tick(56, row); spin_tick(56, row); spin_tick(56, row);
    rc = probe();
    if (result_ptr) *result_ptr = rc;
    if (rc)
        tui_print_at(56, row, TUI_LIGHTGREEN, TUI_BLUE, "[ found ]");
    else
        tui_print_at(56, row, TUI_LIGHTRED,   TUI_BLUE, "[  no   ]");
}

static int probe_picogus_dir(void)  { return dir_exists ("C:\\PICOGUS");           }
static int probe_pgusinit(void)     { return file_exists("PGUSINIT.EXE")
                                          || file_exists("C:\\PICOGUS\\PGUSINIT.EXE"); }
static int probe_autoexec(void)     { return file_exists("C:\\AUTOEXEC.BAT");      }
static int probe_config(void)       { return file_exists("C:\\CONFIG.SYS");        }
static int probe_himem(void)        { return file_exists("C:\\WINDOWS\\HIMEM.SYS")
                                          || file_exists("C:\\DOS\\HIMEM.SYS");    }
static int probe_emm386(void)       { return file_exists("C:\\WINDOWS\\EMM386.EXE")
                                          || file_exists("C:\\DOS\\EMM386.EXE");   }
static int probe_windows(void)      { return dir_exists ("C:\\WINDOWS");           }
static int probe_ctmouse(void)      { return file_exists("CTMOUSE.EXE")
                                          || file_exists("C:\\PICOGUS\\CTMOUSE.EXE"); }

static StepAction screen_detect(Config *c) {
    int k;
    tui_clear();
    tui_title_bar("PicoGUS Installer " PGWIZ_VERSION " - Detect Environment");
    tui_draw_box(8, 3, 64, 18, "System Check", 1);
    tui_print_at(10, 5, TUI_LIGHTCYAN, TUI_BLUE,
        "Scanning for required files and DOS components...");

    detect_row( 7, "C:\\PICOGUS\\ directory.................", &c->picogus_dir, probe_picogus_dir);
    detect_row( 8, "PGUSINIT.EXE (current dir or C:\\PICOGUS).", &c->has_pgusinit, probe_pgusinit);
    detect_row( 9, "C:\\AUTOEXEC.BAT.......................", &c->has_autoexec, probe_autoexec);
    detect_row(10, "C:\\CONFIG.SYS.........................", &c->has_config, probe_config);
    detect_row(11, "HIMEM.SYS (WINDOWS or DOS dir)........", &c->has_himem, probe_himem);
    detect_row(12, "EMM386.EXE (WINDOWS or DOS dir).......", &c->has_emm386, probe_emm386);
    detect_row(13, "C:\\WINDOWS\\ (Win9x detection).........", &c->win9x, probe_windows);
    detect_row(14, "CTMOUSE.EXE (current or C:\\PICOGUS)...", &c->has_ctmouse, probe_ctmouse);

    tui_print_at(10, 17, TUI_WHITE, TUI_BLUE,
        c->has_pgusinit
          ? "PGUSINIT.EXE found - good. We can configure the card now."
          : "PGUSINIT.EXE NOT found - install will copy it from current dir.");

    if (c->win9x) {
        tui_print_at(10, 18, TUI_YELLOW, TUI_BLUE,
            "Windows 9x detected. CONFIG.SYS edits apply to DOS-mode boot.");
    }

    tui_status_bar(" ENTER continue   ESC back ");
    for (;;) {
        k = tui_getkey();
        if (k == KEY_ENTER) return ACT_NEXT;
        if (k == KEY_ESC)   return ACT_BACK;
        if (k == KEY_F10)   return ACT_QUIT;
    }
}

/* ------------------------------------------------------------------ */
/* Screen 3: card mode                                                 */
/* ------------------------------------------------------------------ */
static StepAction screen_mode(Config *c) {
    static const char *items[] = {
        "GUS    -- Gravis UltraSound  (best for demos and GUS games)",
        "SB16   -- Sound Blaster 16   (OPL3, widest game compat)",
        "SB Pro -- Sound Blaster Pro 2 (OPL3 stereo FM)",
        "AdLib  -- AdLib / OPL2 only  (serial mouse available)",
        "MPU    -- MPU-401 MIDI only  (intelligent mode, IRQ support)",
        "Tandy  -- Tandy 3-Voice      (IBM PCjr / Tandy games)",
        "CMS    -- CMS/Game Blaster   (earliest Creative card)",
        "USB    -- USB only           (joystick + mouse + CD-ROM)"
    };
    int sel;
    tui_clear();
    tui_title_bar("PicoGUS Installer " PGWIZ_VERSION " - Select Emulation Mode");
    tui_draw_box(4, 3, 72, 16, "Card Mode", 1);
    tui_print_at(6, 5, TUI_WHITE, TUI_BLUE,
        "Choose the sound card mode for your PicoGUS. This is saved to the");
    tui_print_at(6, 6, TUI_WHITE, TUI_BLUE,
        "card and applied on every boot.");
    tui_print_at(6, 17, TUI_LIGHTCYAN, TUI_BLUE,
        "Note: MPU-401 is available simultaneously in every mode.");
    tui_status_bar(" UP/DOWN select   ENTER pick   ESC back ");

    sel = tui_menu(6, 8, 68, items, MODE_COUNT, c->mode);
    if (sel == -1) return ACT_BACK;
    if (sel == -2) return ACT_QUIT;
    c->mode = sel;
    return ACT_NEXT;
}

/* ------------------------------------------------------------------ */
/* Screen 4: hardware settings                                         */
/* ------------------------------------------------------------------ */

static void edit_hex(int x, int y, const char *label, int *val,
                     int min_v, int max_v) {
    char buf[8];
    int k;
    sprintf(buf, "%X", *val);
    tui_input(x, y, 6, label, buf, 5);
    k = parse_hex(buf);
    if (k < min_v) k = min_v;
    if (k > max_v) k = max_v;
    *val = k;
}

static void edit_dec(int x, int y, const char *label, int *val,
                     int min_v, int max_v) {
    char buf[8];
    int k;
    sprintf(buf, "%d", *val);
    tui_input(x, y, 6, label, buf, 5);
    k = parse_dec(buf);
    if (k < min_v) k = min_v;
    if (k > max_v) k = max_v;
    *val = k;
}

static StepAction screen_hardware(Config *c) {
    int row = 0;
    int k;
    tui_clear();
    tui_title_bar("PicoGUS Installer " PGWIZ_VERSION " - Hardware Configuration");
    tui_draw_box(2, 3, 76, 18, "Hardware", 1);

    /* Left column - the always-shown port + IRQ + DMA + MPU port. */
    tui_print_at(5, 5, TUI_YELLOW, TUI_BLUE, "Addresses & Resources");
    tui_print_at(5, 7,  TUI_WHITE, TUI_BLUE, "Port (hex):");
    tui_print_at(5, 9,  TUI_WHITE, TUI_BLUE, "IRQ (2-12):");
    tui_print_at(5, 11, TUI_WHITE, TUI_BLUE, "DMA (1 or 3):");
    tui_print_at(5, 13, TUI_WHITE, TUI_BLUE, "MPU Port (0=off):");

    /* Right column - mode specifics. */
    tui_print_at(42, 5, TUI_YELLOW, TUI_BLUE, "Mode-Specific");

    switch (c->mode) {
    case MODE_GUS:
        tui_print_at(42, 7,  TUI_WHITE, TUI_BLUE, "Audio Buffer:");
        tui_print_at(42, 9,  TUI_WHITE, TUI_BLUE, "DMA Interval:");
        break;
    case MODE_SB16:
    case MODE_SBPRO:
        tui_print_at(42, 7,  TUI_WHITE, TUI_BLUE, "OPL Port:");
        break;
    case MODE_ADLIB:
        tui_print_at(42, 7,  TUI_WHITE, TUI_BLUE, "OPL Port:");
        break;
    case MODE_TANDY:
        tui_print_at(42, 7,  TUI_WHITE, TUI_BLUE, "Tandy Port:");
        break;
    case MODE_CMS:
        tui_print_at(42, 7,  TUI_WHITE, TUI_BLUE, "CMS Port:");
        break;
    default:
        break;
    }
    tui_print_at(42, 11, TUI_WHITE, TUI_BLUE, "Main Volume:");
    tui_print_at(42, 13, TUI_WHITE, TUI_BLUE, "WT Header Vol:");

    /* Info banner. */
    tui_fill(5, 15, 70, 4, TUI_WHITE, TUI_BLUE, ' ');
    tui_draw_box(5, 15, 70, 4, " Note ", 0);
    tui_print_at(7, 16, TUI_YELLOW, TUI_BLUE,
        "IRQ and DMA must match the PHYSICAL JUMPERS on your PicoGUS card.");
    tui_print_at(7, 17, TUI_LIGHTGRAY, TUI_BLUE,
        "Available IRQs: 2 3 4 5 7 10 11 12.  DMAs: 1 3.");

    tui_status_bar(" ENTER edits / next field   TAB skip   ESC back   F10 quit ");

    /* Edit each field in turn. ESC at any step returns to mode screen. */
    for (row = 0; row < 7; row++) {
        switch (row) {
        case 0:
            edit_hex(22, 7, "", c->mode == MODE_GUS ? &c->gus_port :
                                c->mode == MODE_SB16 || c->mode == MODE_SBPRO ? &c->sb_port :
                                c->mode == MODE_TANDY ? &c->tandy_port :
                                c->mode == MODE_CMS   ? &c->cms_port :
                                                        &c->mpu_port, 0x200, 0x3FF);
            break;
        case 1:
            edit_dec(22, 9,  "", &c->irq, 2, 15);
            break;
        case 2:
            edit_dec(22, 11, "", &c->dma, 0, 7);
            break;
        case 3:
            edit_hex(22, 13, "", &c->mpu_port, 0, 0x3FF);
            break;
        case 4:
            if (c->mode == MODE_GUS) {
                edit_dec(59, 7, "", &c->audio_buffer, 1, 256);
                edit_dec(59, 9, "", &c->dma_interval, 0, 1000);
            } else if (c->mode == MODE_SB16 || c->mode == MODE_SBPRO || c->mode == MODE_ADLIB) {
                edit_hex(59, 7, "", &c->opl_port, 0x200, 0x3FF);
            }
            break;
        case 5:
            edit_dec(59, 11, "", &c->main_vol, 0, 100);
            break;
        case 6:
            edit_dec(59, 13, "", &c->wt_vol, 0, 100);
            break;
        }
    }

    tui_status_bar(" ENTER continue   ESC back ");
    for (;;) {
        k = tui_getkey();
        if (k == KEY_ENTER) return ACT_NEXT;
        if (k == KEY_ESC)   return ACT_BACK;
        if (k == KEY_F10)   return ACT_QUIT;
    }
}

/* ------------------------------------------------------------------ */
/* Screen 5: feature toggles                                           */
/* ------------------------------------------------------------------ */

typedef struct { const char *label; int *flag; const char *help; } Toggle;

static StepAction screen_features(Config *c) {
    Toggle toggles[8];
    int n = 0, sel = 0, k;
    int i;

    toggles[n].label = "USB Joystick Support";
    toggles[n].flag  = &c->usb_joy;
    toggles[n].help  = "Enable gameport joystick via USB controller.";
    n++;

    toggles[n].label = "Serial Mouse Emulation";
    toggles[n].flag  = &c->mouse;
    toggles[n].help  = "Emulate a COM port mouse from a USB mouse.";
    n++;

    toggles[n].label = "MPU-401 Sysex Delay";
    toggles[n].flag  = &c->mpu_sysex;
    toggles[n].help  = "For Roland MT-32 rev.0 - prevents sysex buffer overflow.";
    n++;

    toggles[n].label = "MPU-401 Fake All-Notes-Off";
    toggles[n].flag  = &c->mpu_fake;
    toggles[n].help  = "Compatibility hack for the Roland RA-50 synth.";
    n++;

    toggles[n].label = "OPL Wait (slows OPL writes)";
    toggles[n].flag  = &c->opl_wait;
    toggles[n].help  = "Fixes speed-sensitive AdLib games (688 Attack Sub etc).";
    n++;

    toggles[n].label = "SB Fix Time Constant";
    toggles[n].flag  = &c->sb_fix_tc;
    toggles[n].help  = "Fix sampling-rate rounding in some SB games.";
    n++;

    toggles[n].label = "Enable CD-ROM Emulation";
    toggles[n].flag  = &c->cd_rom;
    toggles[n].help  = "Mount CD images from USB; UIDE+SHSUCDX will be added.";
    n++;

    toggles[n].label = "Save settings to Flash";
    toggles[n].flag  = &c->save_flash;
    toggles[n].help  = "Persist all settings to the card. Recommended.";
    n++;

    tui_clear();
    tui_title_bar("PicoGUS Installer " PGWIZ_VERSION " - Optional Features");
    tui_draw_box(8, 3, 64, 17, "Feature Toggles", 1);
    tui_print_at(10, 5, TUI_WHITE, TUI_BLUE,
        "SPACE toggles, UP/DOWN moves, ENTER continues, ESC goes back.");

    for (;;) {
        for (i = 0; i < n; i++) {
            int fg = (i == sel) ? TUI_BLACK : TUI_WHITE;
            int bg = (i == sel) ? TUI_CYAN  : TUI_BLUE;
            char line[80];
            sprintf(line, " [%c] %-44s ",
                    *(toggles[i].flag) ? 'X' : ' ', toggles[i].label);
            tui_print_at(10, 7 + i, fg, bg, line);
        }
        /* Sub-prompt: mouse COM port shows when mouse enabled and selected */
        if (c->mouse) {
            char line[40];
            sprintf(line, "     COM port (1-4): %d", c->mouse_com);
            tui_print_at(10, 7 + n + 1, TUI_LIGHTCYAN, TUI_BLUE, line);
        } else {
            tui_fill(10, 7 + n + 1, 50, 1, TUI_WHITE, TUI_BLUE, ' ');
        }
        tui_status_bar(toggles[sel].help);

        k = tui_getkey();
        switch (k) {
        case KEY_UP:    sel = (sel == 0)     ? n - 1 : sel - 1; break;
        case KEY_DOWN:  sel = (sel == n - 1) ? 0     : sel + 1; break;
        case KEY_SPACE: *(toggles[sel].flag) = !*(toggles[sel].flag); break;
        case '+':
            if (toggles[sel].flag == &c->mouse && c->mouse) {
                if (c->mouse_com < 4) c->mouse_com++;
            }
            break;
        case '-':
            if (toggles[sel].flag == &c->mouse && c->mouse) {
                if (c->mouse_com > 1) c->mouse_com--;
            }
            break;
        case KEY_ENTER: return ACT_NEXT;
        case KEY_ESC:   return ACT_BACK;
        case KEY_F10:   return ACT_QUIT;
        }
    }
}

/* ------------------------------------------------------------------ */
/* File preview helper                                                 */
/* ------------------------------------------------------------------ */
static void preview_file(int x, int y, int w, int h, const char *path) {
    FILE *f;
    char line[120];
    int row = 0;
    tui_draw_box(x, y, w, h, path, 0);
    f = fopen(path, "r");
    if (!f) {
        tui_print_at(x + 2, y + 1, TUI_LIGHTRED, TUI_BLUE, "(file not present)");
        return;
    }
    while (row < h - 2 && fgets(line, sizeof(line), f)) {
        int len = (int)strlen(line);
        while (len && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len > w - 4) len = w - 4;
        tui_printn_at(x + 2, y + 1 + row, TUI_LIGHTGRAY, TUI_BLUE, line, len);
        row++;
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* Plan additions: figure out the lines we want to add to AUTOEXEC.BAT */
/* and CONFIG.SYS based on Config.                                     */
/* ------------------------------------------------------------------ */

#define MAX_PATCH_LINES 12

typedef struct {
    char lines[MAX_PATCH_LINES][120];
    int  count;
} PatchPlan;

/* The BLASTER environment variable uses the conventional H<dma> for high
 * DMA. We don't configure high DMA here so we omit it; many games tolerate
 * its absence. We do include P<mpu> if the MPU port is enabled. */
static void build_autoexec_plan(const Config *c, PatchPlan *p) {
    p->count = 0;
    strcpy(p->lines[p->count++], "SET PATH=%PATH%;C:\\PICOGUS");

    if (c->mode == MODE_GUS) {
        sprintf(p->lines[p->count++], "SET ULTRASND=%X,%d,%d,%d,%d",
                c->gus_port, c->dma, c->dma, c->irq, c->irq);
        strcpy(p->lines[p->count++], "SET ULTRADIR=C:\\ULTRASND");
    }
    if (c->mode == MODE_SB16 || c->mode == MODE_SBPRO) {
        if (c->mpu_port)
            sprintf(p->lines[p->count++], "SET BLASTER=A%X I%d D%d H%d P%X T%d",
                    c->sb_port, c->irq, c->dma, c->dma,
                    c->mpu_port, SB_TYPE_FOR_MODE[c->mode]);
        else
            sprintf(p->lines[p->count++], "SET BLASTER=A%X I%d D%d H%d T%d",
                    c->sb_port, c->irq, c->dma, c->dma,
                    SB_TYPE_FOR_MODE[c->mode]);
    }
    strcpy(p->lines[p->count++], "C:\\PICOGUS\\PGUSINIT.EXE");
    if (c->mouse)
        sprintf(p->lines[p->count++], "LH C:\\PICOGUS\\CTMOUSE.EXE /R%d",
                c->mouse_com);
    if (c->cd_rom)
        strcpy(p->lines[p->count++],
            "LH C:\\PICOGUS\\SHSUCDX.COM /D:PGUCD001 /L:D");
}

static void build_config_plan(const Config *c, PatchPlan *p) {
    p->count = 0;
    if (!c->has_himem) {
        const char *src = file_exists("C:\\DOS\\HIMEM.SYS")
            ? "DEVICE=C:\\DOS\\HIMEM.SYS"
            : "DEVICE=C:\\WINDOWS\\HIMEM.SYS";
        strcpy(p->lines[p->count++], src);
    }
    if (!c->has_emm386) {
        const char *src = file_exists("C:\\DOS\\EMM386.EXE")
            ? "DEVICE=C:\\DOS\\EMM386.EXE NOEMS"
            : "DEVICE=C:\\WINDOWS\\EMM386.EXE NOEMS";
        strcpy(p->lines[p->count++], src);
    }
    strcpy(p->lines[p->count++], "DOS=HIGH,UMB");
    strcpy(p->lines[p->count++], "FILES=40");
    strcpy(p->lines[p->count++], "BUFFERS=20");
    if (c->cd_rom)
        strcpy(p->lines[p->count++],
            "DEVICE=C:\\PICOGUS\\UIDE.SYS /D:PGUCD001");
}

static void preview_plan(int x, int y, int w, int h,
                         const PatchPlan *p) {
    int i;
    tui_draw_box(x, y, w, h, " Lines to add ", 0);
    for (i = 0; i < p->count && i < h - 2; i++) {
        int len = (int)strlen(p->lines[i]);
        if (len > w - 4) len = w - 4;
        tui_printn_at(x + 2, y + 1 + i, TUI_LIGHTGREEN, TUI_BLUE,
                      p->lines[i], len);
    }
}

/* ------------------------------------------------------------------ */
/* Screen 6: AUTOEXEC.BAT preview                                      */
/* ------------------------------------------------------------------ */
static StepAction screen_autoexec(Config *c) {
    int k;
    PatchPlan plan;
    build_autoexec_plan(c, &plan);

    tui_clear();
    tui_title_bar("PicoGUS Installer " PGWIZ_VERSION " - AUTOEXEC.BAT Setup");
    preview_file(2, 3, 76, 11, "C:\\AUTOEXEC.BAT");
    preview_plan(2, 14, 76, 8, &plan);
    tui_print_at(2, 23, TUI_LIGHTCYAN, TUI_BLUE,
        "Lines already present in AUTOEXEC.BAT will not be duplicated.");
    tui_status_bar(" ENTER continue   ESC back   F10 quit ");

    for (;;) {
        k = tui_getkey();
        if (k == KEY_ENTER) return ACT_NEXT;
        if (k == KEY_ESC)   return ACT_BACK;
        if (k == KEY_F10)   return ACT_QUIT;
    }
}

/* ------------------------------------------------------------------ */
/* Screen 7: CONFIG.SYS preview                                        */
/* ------------------------------------------------------------------ */
static StepAction screen_config(Config *c) {
    int k;
    PatchPlan plan;
    build_config_plan(c, &plan);

    tui_clear();
    tui_title_bar("PicoGUS Installer " PGWIZ_VERSION " - CONFIG.SYS Setup");
    preview_file(2, 3, 76, 11, "C:\\CONFIG.SYS");
    preview_plan(2, 14, 76, 8, &plan);

    if (c->win9x)
        tui_print_at(2, 22, TUI_YELLOW, TUI_BLUE,
            "Win9x: CONFIG.SYS edits apply only when booting DOS-mode.");
    tui_print_at(2, 23, TUI_LIGHTCYAN, TUI_BLUE,
        "Existing values for FILES/BUFFERS won't be lowered.");
    tui_status_bar(" ENTER continue   ESC back   F10 quit ");

    for (;;) {
        k = tui_getkey();
        if (k == KEY_ENTER) return ACT_NEXT;
        if (k == KEY_ESC)   return ACT_BACK;
        if (k == KEY_F10)   return ACT_QUIT;
    }
}

/* ------------------------------------------------------------------ */
/* Screen 8: GUS driver files                                          */
/* ------------------------------------------------------------------ */
static StepAction screen_gus(Config *c) {
    int k;
    if (c->mode != MODE_GUS) return ACT_NEXT; /* skip silently */

    tui_clear();
    tui_title_bar("PicoGUS Installer " PGWIZ_VERSION " - Gravis UltraSound Drivers");
    tui_draw_box(4, 3, 72, 18, "GUS Patch Files", 1);

    {
        int have_zip   = file_exists("ULTRASND.ZIP");
        int have_unzip = file_exists("UNZIP.EXE");
        int bundled    = have_zip && have_unzip;

        tui_print_at(6, 5, TUI_WHITE, TUI_BLUE,
            "GUS mode needs the UltraSound v4.11 patch files in C:\\ULTRASND\\.");
        tui_print_at(6, 7, TUI_WHITE, TUI_BLUE, "C:\\ULTRASND\\..........");
        tui_print_at(6, 8, TUI_WHITE, TUI_BLUE, "C:\\ULTRASND\\MIDI\\.....");
        tui_print_at(6, 9, TUI_WHITE, TUI_BLUE, "C:\\ULTRASND\\PATCHES\\..");

        tui_print_at(28, 7, dir_exists("C:\\ULTRASND")          ? TUI_LIGHTGREEN : TUI_LIGHTRED,
                     TUI_BLUE,
                     dir_exists("C:\\ULTRASND")          ? "[ ok ]" : "[ missing ]");
        tui_print_at(28, 8, dir_exists("C:\\ULTRASND\\MIDI")    ? TUI_LIGHTGREEN : TUI_LIGHTRED,
                     TUI_BLUE,
                     dir_exists("C:\\ULTRASND\\MIDI")    ? "[ ok ]" : "[ missing ]");
        tui_print_at(28, 9, dir_exists("C:\\ULTRASND\\PATCHES") ? TUI_LIGHTGREEN : TUI_LIGHTRED,
                     TUI_BLUE,
                     dir_exists("C:\\ULTRASND\\PATCHES") ? "[ ok ]" : "[ missing ]");

        if (bundled) {
            tui_print_at(6, 11, TUI_LIGHTGREEN, TUI_BLUE,
                "GUS v4.11 + Pro Patches Lite 1.61 (anti-loop fix) bundled.");
            tui_print_at(6, 12, TUI_LIGHTGREEN, TUI_BLUE,
                "Installer will extract them to C:\\ULTRASND\\ in the next step.");
            tui_print_at(6, 13, TUI_WHITE,      TUI_BLUE,
                "(Source: ULTRASND.ZIP + UNZIP.EXE next to PGINST.EXE.)");
        } else {
            tui_print_at(6, 11, TUI_LIGHTCYAN, TUI_BLUE,
                "Download the GUS v4.11 package and extract to C:\\ULTRASND\\.");
            tui_print_at(6, 12, TUI_LIGHTCYAN, TUI_BLUE,
                "The setup program in that package is NOT compatible with PicoGUS;");
            tui_print_at(6, 13, TUI_LIGHTCYAN, TUI_BLUE,
                "do not run it.");
        }
    }

    {
        char buf[64];
        sprintf(buf, "SET ULTRASND=%X,%d,%d,%d,%d",
                c->gus_port, c->dma, c->dma, c->irq, c->irq);
        tui_print_at(6, 16, TUI_YELLOW, TUI_BLUE, buf);
        tui_print_at(6, 17, TUI_YELLOW, TUI_BLUE, "SET ULTRADIR=C:\\ULTRASND");
    }

    tui_print_at(6, 19, TUI_WHITE, TUI_BLUE,
        "Installer will create C:\\ULTRASND\\ and subdirs for you.");
    tui_status_bar(" ENTER continue   ESC back   F10 quit ");

    for (;;) {
        k = tui_getkey();
        if (k == KEY_ENTER) return ACT_NEXT;
        if (k == KEY_ESC)   return ACT_BACK;
        if (k == KEY_F10)   return ACT_QUIT;
    }
}

/* ------------------------------------------------------------------ */
/* Screen 9: review & confirm                                          */
/* ------------------------------------------------------------------ */
static StepAction screen_review(Config *c) {
    char line[80];
    int k;
    PatchPlan ae, cs;
    build_autoexec_plan(c, &ae);
    build_config_plan  (c, &cs);

    tui_clear();
    tui_title_bar("PicoGUS Installer " PGWIZ_VERSION " - Review Installation");
    tui_draw_box(2, 3, 76, 19, "Summary", 1);

    sprintf(line, "Mode:           %s", MODE_NAME[c->mode]);
    tui_print_at(4, 5, TUI_WHITE, TUI_BLUE, line);

    sprintf(line, "Port:           %Xh", c->mode == MODE_GUS ? c->gus_port :
                                          c->mode == MODE_SB16 || c->mode == MODE_SBPRO ? c->sb_port :
                                          c->mode == MODE_TANDY ? c->tandy_port :
                                          c->mode == MODE_CMS   ? c->cms_port :
                                                                  c->mpu_port);
    tui_print_at(4, 6, TUI_WHITE, TUI_BLUE, line);
    sprintf(line, "IRQ / DMA:      %d / %d", c->irq, c->dma);
    tui_print_at(4, 7, TUI_WHITE, TUI_BLUE, line);
    sprintf(line, "MPU Port:       %Xh", c->mpu_port);
    tui_print_at(4, 8, TUI_WHITE, TUI_BLUE, line);
    sprintf(line, "Main Volume:    %d%%", c->main_vol);
    tui_print_at(4, 9, TUI_WHITE, TUI_BLUE, line);

    sprintf(line, "USB Joystick:   %s", c->usb_joy   ? "Enabled" : "Disabled");
    tui_print_at(42, 5, TUI_WHITE, TUI_BLUE, line);
    sprintf(line, "Serial Mouse:   %s", c->mouse     ? "Enabled" : "Disabled");
    tui_print_at(42, 6, TUI_WHITE, TUI_BLUE, line);
    sprintf(line, "CD-ROM Emul.:   %s", c->cd_rom    ? "Enabled" : "Disabled");
    tui_print_at(42, 7, TUI_WHITE, TUI_BLUE, line);
    sprintf(line, "Sysex Delay:    %s", c->mpu_sysex ? "On"      : "Off");
    tui_print_at(42, 8, TUI_WHITE, TUI_BLUE, line);
    sprintf(line, "Save to Flash:  %s", c->save_flash? "Yes"     : "No");
    tui_print_at(42, 9, TUI_WHITE, TUI_BLUE, line);

    tui_print_at(4, 11, TUI_YELLOW, TUI_BLUE, "Files to create:");
    tui_print_at(8, 12, TUI_LIGHTGRAY, TUI_BLUE, "C:\\PICOGUS\\  (directory + helper batches)");
    if (c->mode == MODE_GUS)
        tui_print_at(8, 13, TUI_LIGHTGRAY, TUI_BLUE, "C:\\ULTRASND\\ (directory + MIDI / PATCHES)");
    sprintf(line, "AUTOEXEC.BAT:   %d line(s) to add  (backup -> C:\\AUTOEXEC.BAK)",
            ae.count);
    tui_print_at(4, 15, TUI_LIGHTGRAY, TUI_BLUE, line);
    sprintf(line, "CONFIG.SYS:     %d line(s) to add  (backup -> C:\\CONFIG.BAK)",
            cs.count);
    tui_print_at(4, 16, TUI_LIGHTGRAY, TUI_BLUE, line);

    tui_print_at(4, 18, TUI_LIGHTCYAN, TUI_BLUE, "pgusinit will be called immediately to configure the card.");

    tui_print_at(20, 21, TUI_BLACK, TUI_GREEN, "  [ Install ]  ");
    tui_print_at(48, 21, TUI_BLACK, TUI_CYAN,  "  [   Back  ]  ");
    tui_status_bar(" ENTER install   ESC back ");

    for (;;) {
        k = tui_getkey();
        if (k == KEY_ENTER) return ACT_NEXT;
        if (k == KEY_ESC)   return ACT_BACK;
        if (k == KEY_F10)   return ACT_QUIT;
    }
}

/* ------------------------------------------------------------------ */
/* Real work: patch a DOS text file, removing our own lines first then  */
/* appending the new ones. The "marker" array is a list of keywords; if  */
/* any of them appears in a line, we treat the line as ours and drop it. */
/* ------------------------------------------------------------------ */
static int patch_text_file(const char *path, const char *backup_path,
                           const char *const *markers, int marker_count,
                           const char *const *new_lines, int new_count) {
    FILE *in, *out;
    char buf[256];
    char tmp[64];
    int i;
    sprintf(tmp, "%s.TMP", path);

    /* Step 1: snapshot the original to backup (only if backup does not
     * already exist - we want to preserve the very first backup). */
    if (!file_exists(backup_path) && file_exists(path)) {
        if (copy_file(path, backup_path) != 0) return -1;
    }

    /* Step 2: walk source, drop lines matching any marker. */
    in = fopen(path, "r");
    out = fopen(tmp, "w");
    if (!out) { if (in) fclose(in); return -1; }
    if (in) {
        while (fgets(buf, sizeof(buf), in)) {
            int drop = 0;
            for (i = 0; i < marker_count; i++) {
                if (strcontains_ci(buf, markers[i])) { drop = 1; break; }
            }
            if (!drop) fputs(buf, out);
        }
        fclose(in);
    }

    /* Step 3: append new lines. fopen("w") writes \n, but DOS text mode
     * translates to \r\n on write, so this is fine. */
    for (i = 0; i < new_count; i++) {
        fputs(new_lines[i], out);
        fputs("\n", out);
    }
    fclose(out);

    /* Step 4: replace original. */
    remove(path);
    if (rename(tmp, path) != 0) return -1;
    return 0;
}

/* Write a small text file (e.g., a batch helper) verbatim. */
static int write_text_file(const char *path, const char *body) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fwrite(body, 1, strlen(body), f);
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Compose the PGUSINIT command line that matches our final settings.   */
/* ------------------------------------------------------------------ */
static void build_pgusinit_cmd(const Config *c, char *out, size_t outlen) {
    char piece[64];
    out[0] = '\0';

    strcat(out, "C:\\PICOGUS\\PGUSINIT.EXE /mode ");
    strcat(out, MODE_PGUSINIT[c->mode]);

    if (c->mode == MODE_GUS) {
        sprintf(piece, " /gusport %X", c->gus_port); strcat(out, piece);
        sprintf(piece, " /gusbuf %d",  c->audio_buffer); strcat(out, piece);
        if (c->dma_interval) {
            sprintf(piece, " /gusdma %d", c->dma_interval); strcat(out, piece);
        }
    }
    if (c->mode == MODE_SB16 || c->mode == MODE_SBPRO) {
        sprintf(piece, " /sbport %X", c->sb_port); strcat(out, piece);
        sprintf(piece, " /sbirq %d",  c->irq);     strcat(out, piece);
        sprintf(piece, " /sbdma %d",  c->dma);     strcat(out, piece);
        sprintf(piece, " /sbtype %d", SB_TYPE_FOR_MODE[c->mode]); strcat(out, piece);
        sprintf(piece, " /oplport %X", c->opl_port); strcat(out, piece);
        if (c->sb_fix_tc)
            strcat(out, " /sbfixtc 1");
    }
    if (c->mode == MODE_ADLIB) {
        sprintf(piece, " /oplport %X", c->opl_port); strcat(out, piece);
    }
    if (c->mode == MODE_TANDY) {
        sprintf(piece, " /tandyport %X", c->tandy_port); strcat(out, piece);
    }
    if (c->mode == MODE_CMS) {
        sprintf(piece, " /cmsport %X", c->cms_port); strcat(out, piece);
    }

    /* Cross-mode options. */
    if (c->mpu_port) {
        sprintf(piece, " /mpuport %X", c->mpu_port); strcat(out, piece);
    } else {
        strcat(out, " /mpuport 0");
    }
    if (c->mpu_sysex) strcat(out, " /mpudelay 1");
    if (c->mpu_fake)  strcat(out, " /mpufake 1");
    if (c->opl_wait)  strcat(out, " /oplwait");
    if (c->usb_joy)   strcat(out, " /joy 1");
    if (c->mouse) {
        sprintf(piece, " /mousecom %d", c->mouse_com); strcat(out, piece);
    }
    sprintf(piece, " /mainvol %d", c->main_vol); strcat(out, piece);
    sprintf(piece, " /wtvol %d",   c->wt_vol);   strcat(out, piece);

    if (c->save_flash) strcat(out, " /save");

    (void)outlen; /* caller passes a generous buffer */
}

/* ------------------------------------------------------------------ */
/* Screen 10: do the installation                                      */
/* ------------------------------------------------------------------ */

static void step_line(int row, const char *what, int ok) {
    tui_print_at(8, row, ok ? TUI_LIGHTGREEN : TUI_LIGHTRED, TUI_BLUE,
                 ok ? "[ ok ]" : "[fail]");
    tui_print_at(15, row, TUI_WHITE, TUI_BLUE, what);
}

static StepAction screen_install(Config *c) {
    char cmd[256];
    int row = 5;
    int rc;
    int i;
    const char *autoexec_markers[] = {
        "PICOGUS", "ULTRASND", "ULTRADIR", "BLASTER",
        "PGUSINIT", "CTMOUSE", "MSCDEX", "SHSUCDX"
    };
    const char *config_markers[] = {
        "HIMEM.SYS", "EMM386.EXE", "OAKCDROM", "UIDE.SYS", "UDVD2.SYS",
        "DOS=HIGH", "FILES=", "BUFFERS="
    };
    PatchPlan ae, cs;
    const char *new_ae[MAX_PATCH_LINES];
    const char *new_cs[MAX_PATCH_LINES];

    build_autoexec_plan(c, &ae);
    build_config_plan  (c, &cs);
    for (i = 0; i < ae.count; i++) new_ae[i] = ae.lines[i];
    for (i = 0; i < cs.count; i++) new_cs[i] = cs.lines[i];

    tui_clear();
    tui_title_bar("PicoGUS Installer " PGWIZ_VERSION " - Installing...");
    tui_draw_box(2, 3, 76, 20, "Progress", 1);

    rc = make_dir("C:\\PICOGUS");
    step_line(row++, "Create C:\\PICOGUS\\",  rc == 0 || dir_exists("C:\\PICOGUS"));

    if (c->mode == MODE_GUS) {
        rc = make_dir("C:\\ULTRASND");
        step_line(row++, "Create C:\\ULTRASND\\", rc == 0 || dir_exists("C:\\ULTRASND"));
        rc = make_dir("C:\\ULTRASND\\MIDI");
        step_line(row++, "Create C:\\ULTRASND\\MIDI\\", rc == 0 || dir_exists("C:\\ULTRASND\\MIDI"));
        rc = make_dir("C:\\ULTRASND\\PATCHES");
        step_line(row++, "Create C:\\ULTRASND\\PATCHES\\", rc == 0 || dir_exists("C:\\ULTRASND\\PATCHES"));

        /* If the bundled GUS driver/patch zip is sitting next to us
         * (ULTRASNDPPL161FIX repackaged as ULTRASND.ZIP), unzip it into
         * C:\ULTRASND\. UNZIP.EXE is Info-ZIP for DOS; -d preserves the
         * MIDI\ / PATCHES\ subdirs from inside the archive. */
        if (file_exists("ULTRASND.ZIP") && file_exists("UNZIP.EXE")) {
            rc = system("UNZIP -o -q ULTRASND.ZIP -d C:\\ULTRASND");
            step_line(row++,
                rc == 0 ? "Extract GUS v4.11 + PPL 1.61 -> C:\\ULTRASND\\"
                        : "UNZIP returned an error - patches not installed",
                rc == 0);
        }
    }

    /* Copy PGUSINIT if it is next to us and not already installed. */
    if (file_exists("PGUSINIT.EXE")
        && !file_exists("C:\\PICOGUS\\PGUSINIT.EXE")) {
        rc = copy_file("PGUSINIT.EXE", "C:\\PICOGUS\\PGUSINIT.EXE");
        step_line(row++, "Copy PGUSINIT.EXE -> C:\\PICOGUS\\", rc == 0);
    }

    /* Write the helper batches. */
    for (i = 0; i < (int)BAT_COUNT; i++) {
        char dst[64];
        sprintf(dst, "C:\\PICOGUS\\%s", BAT_FILES[i].name);
        rc = write_text_file(dst, BAT_FILES[i].body);
        if ((i & 3) == 0) {
            char what[40];
            sprintf(what, "Helper batches (%d / %d)", i+1, (int)BAT_COUNT);
            step_line(row, what, rc == 0);
        }
    }
    step_line(row++, "Helper batches written", 1);

    rc = patch_text_file("C:\\AUTOEXEC.BAT", "C:\\AUTOEXEC.BAK",
                          autoexec_markers,
                          sizeof(autoexec_markers)/sizeof(autoexec_markers[0]),
                          new_ae, ae.count);
    step_line(row++, "Patch AUTOEXEC.BAT", rc == 0);

    rc = patch_text_file("C:\\CONFIG.SYS", "C:\\CONFIG.BAK",
                          config_markers,
                          sizeof(config_markers)/sizeof(config_markers[0]),
                          new_cs, cs.count);
    step_line(row++, "Patch CONFIG.SYS", rc == 0);

    /* Run pgusinit. */
    build_pgusinit_cmd(c, cmd, sizeof(cmd));
    tui_print_at(8, row, TUI_YELLOW, TUI_BLUE, "Running pgusinit:");
    row++;
    {
        int len = (int)strlen(cmd);
        int colw = 70;
        const char *p = cmd;
        while (len > 0) {
            int n = len > colw ? colw : len;
            tui_printn_at(8, row++, TUI_LIGHTGRAY, TUI_BLUE, p, n);
            p += n; len -= n;
        }
    }
    rc = system(cmd);
    step_line(row++, rc == 0 ? "pgusinit succeeded" : "pgusinit returned an error", rc == 0);

    tui_status_bar(" ENTER continue ");
    tui_getkey();
    return ACT_NEXT;
}

/* ------------------------------------------------------------------ */
/* Screen 11: complete                                                 */
/* ------------------------------------------------------------------ */
static StepAction screen_complete(Config *c) {
    char line[80];
    int k;
    tui_clear();
    tui_title_bar("PicoGUS Installer " PGWIZ_VERSION " - Complete");
    tui_fill(8, 3, 64, 3, TUI_BLACK, TUI_GREEN, ' ');
    tui_draw_box(8, 3, 64, 3, 0, 1);
    tui_centre(4, TUI_BLACK, TUI_GREEN, "Installation complete!");

    tui_print_at(6, 7, TUI_WHITE, TUI_BLUE,
        "PicoGUS is now configured. Reboot to apply CONFIG.SYS changes.");

    tui_print_at(6, 9,  TUI_YELLOW, TUI_BLUE, "Quick reference:");
    sprintf(line, "  Card mode:   %s   (pgusinit /mode %s)",
            MODE_NAME[c->mode], MODE_PGUSINIT[c->mode]);
    tui_print_at(6, 10, TUI_LIGHTGRAY, TUI_BLUE, line);
    sprintf(line, "  Port:        %Xh", c->mode == MODE_GUS ? c->gus_port :
                                          c->mode == MODE_SB16 || c->mode == MODE_SBPRO ? c->sb_port :
                                          c->mode == MODE_TANDY ? c->tandy_port :
                                          c->mode == MODE_CMS   ? c->cms_port :
                                                                  c->mpu_port);
    tui_print_at(6, 11, TUI_LIGHTGRAY, TUI_BLUE, line);
    sprintf(line, "  IRQ / DMA:   %d / %d", c->irq, c->dma);
    tui_print_at(6, 12, TUI_LIGHTGRAY, TUI_BLUE, line);
    if (c->mode == MODE_GUS) {
        sprintf(line, "  ULTRASND:    %X,%d,%d,%d,%d",
                c->gus_port, c->dma, c->dma, c->irq, c->irq);
        tui_print_at(6, 13, TUI_LIGHTGRAY, TUI_BLUE, line);
        tui_print_at(6, 14, TUI_LIGHTGRAY, TUI_BLUE, "  ULTRADIR:    C:\\ULTRASND");
    }
    tui_print_at(6, 16, TUI_WHITE, TUI_BLUE,
        "For settings management, run: C:\\PICOGUS\\PGSETUP.EXE");
    tui_print_at(6, 17, TUI_WHITE, TUI_BLUE,
        "Mode-switch batch files are in C:\\PICOGUS\\*.BAT");

    tui_print_at(20, 21, TUI_BLACK, TUI_GREEN, "  [ Reboot now ]  ");
    tui_print_at(48, 21, TUI_BLACK, TUI_CYAN,  "  [ Exit to DOS ]  ");
    tui_status_bar(" R reboot   ESC / ENTER exit ");

    for (;;) {
        k = tui_getkey();
        if (k == 'r' || k == 'R') {
            /* INT 19h: BIOS bootstrap entry. Equivalent to a warm boot. */
            union REGS regs;
            tui_shutdown();
            int86(0x19, &regs, &regs);
            /* fall through if it returned */
            return ACT_QUIT;
        }
        if (k == KEY_ENTER || k == KEY_ESC || k == KEY_F10) return ACT_QUIT;
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */
int main(void) {
    Config cfg;
    int step = 0;
    StepAction a = ACT_NEXT;

    cfg_defaults(&cfg);
    tui_init();

    for (;;) {
        switch (step) {
        case 0:  a = screen_welcome  (&cfg); break;
        case 1:  a = screen_detect   (&cfg); break;
        case 2:  a = screen_mode     (&cfg); break;
        case 3:  a = screen_hardware (&cfg); break;
        case 4:  a = screen_features (&cfg); break;
        case 5:  a = screen_autoexec (&cfg); break;
        case 6:  a = screen_config   (&cfg); break;
        case 7:  a = screen_gus      (&cfg); break;
        case 8:  a = screen_review   (&cfg); break;
        case 9:  a = screen_install  (&cfg); break;
        case 10: a = screen_complete (&cfg); break;
        default: a = ACT_QUIT;               break;
        }
        if (a == ACT_QUIT) break;
        step += (a == ACT_NEXT) ? 1 : -1;
        if (step < 0)  step = 0;
        if (step > 10) break;
    }

    tui_shutdown();
    return 0;
}
