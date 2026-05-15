/*
 * tui.c - Implementation of the shared TUI for PicoGUS setup tools.
 *
 * Implementation notes:
 *  - All drawing goes straight to the colour-text video buffer at
 *    B800:0000.  Open Watcom's conio.h does NOT carry the Borland-style
 *    textcolor/gotoxy/clrscr helpers, so we build our own that work on
 *    every 80x25 EGA/VGA capable machine without linking graph.lib.
 *  - getch() and kbhit() do exist in Open Watcom's conio.h and we use
 *    them as-is for keyboard input.
 *  - Cursor positioning uses BIOS INT 10h ah=02h.  Cursor shape uses
 *    ah=01h.  Video mode reset uses ah=00h al=03h (80x25 colour text).
 *  - Extended keys (arrows / F-keys) arrive from BIOS as a leading 0
 *    (or 0xE0 on enhanced keyboards) followed by the scan code.
 */

#include "tui.h"
#include <dos.h>
#include <i86.h>      /* MK_FP */
#include <conio.h>    /* getch, kbhit */
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Direct video buffer at B800:0000. Two bytes per cell: char, attr.   */
/* attr = (bg << 4) | fg. We never use the blink bit so bg may use the */
/* full 0..15 range on a VGA card that has set the bit appropriately,  */
/* but we restrict ourselves to 0..7 for portability.                  */
/* ------------------------------------------------------------------ */
#define VIDEO_SEG  0xB800
#define COLS       80
#define ROWS       25

static unsigned char far *vid_ptr(int col, int row) {
    /* Each cell takes 2 bytes - (row * 80 + col) * 2 from the buffer base. */
    unsigned offset = (unsigned)((row * COLS + col) * 2);
    return (unsigned char far *)MK_FP(VIDEO_SEG, offset);
}

static unsigned char make_attr(int fg, int bg) {
    return (unsigned char)(((bg & 0x07) << 4) | (fg & 0x0F));
}

/* ------------------------------------------------------------------ */
/* Box-drawing glyphs in CP437.                                        */
/* ------------------------------------------------------------------ */
static const unsigned char BOX_DOUBLE[6] = {
    0xC9, 0xBB, 0xC8, 0xBC, 0xCD, 0xBA
};
static const unsigned char BOX_SINGLE[6] = {
    0xDA, 0xBF, 0xC0, 0xD9, 0xC4, 0xB3
};

/* ------------------------------------------------------------------ */
/* BIOS helpers                                                        */
/* ------------------------------------------------------------------ */

static void bios_set_cursor_pos(int col, int row) {
    union REGS r;
    r.h.ah = 0x02;
    r.h.bh = 0;          /* video page 0                              */
    r.h.dh = (unsigned char)row;
    r.h.dl = (unsigned char)col;
    int86(0x10, &r, &r);
}

static void bios_set_cursor_shape(unsigned char start, unsigned char end) {
    union REGS r;
    r.h.ah = 0x01;
    r.h.ch = start;
    r.h.cl = end;
    int86(0x10, &r, &r);
}

static void bios_set_text_mode(void) {
    union REGS r;
    r.h.ah = 0x00;
    r.h.al = 0x03;       /* 80x25 16-colour text                       */
    int86(0x10, &r, &r);
}

void tui_show_cursor(void) { bios_set_cursor_shape(0x06, 0x07); }
void tui_hide_cursor(void) { bios_set_cursor_shape(0x20, 0x00); }

void tui_init(void) {
    bios_set_text_mode();
    tui_hide_cursor();
    tui_clear();
}

void tui_shutdown(void) {
    tui_fill(1, 1, COLS, ROWS, TUI_LIGHTGRAY, TUI_BLACK, ' ');
    tui_show_cursor();
    bios_set_cursor_pos(0, 0);
}

int tui_strlen(const char *s) {
    int n = 0;
    while (*s++) n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* Painting primitives. All coordinates are 1-based (1..80 cols,      */
/* 1..25 rows) to match conio's traditional indexing.                  */
/* ------------------------------------------------------------------ */

static void put_cell(int col, int row, unsigned char ch, unsigned char attr) {
    unsigned char far *p;
    if (col < 1 || col > COLS || row < 1 || row > ROWS) return;
    p = vid_ptr(col - 1, row - 1);
    p[0] = ch;
    p[1] = attr;
}

void tui_fill(int x, int y, int w, int h, int fg, int bg, char ch) {
    unsigned char attr = make_attr(fg, bg);
    int i, j;
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            put_cell(x + i, y + j, (unsigned char)ch, attr);
        }
    }
}

void tui_clear(void) {
    tui_fill(1, 1, COLS, ROWS, TUI_LIGHTGRAY, TUI_BLUE, ' ');
}

void tui_print_at(int x, int y, int fg, int bg, const char *text) {
    unsigned char attr = make_attr(fg, bg);
    int i = 0;
    while (*text) {
        put_cell(x + i, y, (unsigned char)*text, attr);
        text++;
        i++;
    }
}

void tui_printn_at(int x, int y, int fg, int bg, const char *text, int n) {
    unsigned char attr = make_attr(fg, bg);
    int i;
    for (i = 0; i < n && text[i]; i++) {
        put_cell(x + i, y, (unsigned char)text[i], attr);
    }
}

void tui_centre(int row, int fg, int bg, const char *text) {
    int len = tui_strlen(text);
    int x = (COLS - len) / 2 + 1;
    if (x < 1) x = 1;
    tui_print_at(x, row, fg, bg, text);
}

void tui_draw_box(int x, int y, int w, int h,
                  const char *title, int double_line) {
    const unsigned char *g = double_line ? BOX_DOUBLE : BOX_SINGLE;
    unsigned char border = make_attr(TUI_CYAN, TUI_BLUE);
    unsigned char inner  = make_attr(TUI_WHITE, TUI_BLUE);
    int i;

    /* Top edge. */
    put_cell(x,         y, g[0], border);
    for (i = 1; i < w - 1; i++) put_cell(x + i, y, g[4], border);
    put_cell(x + w - 1, y, g[1], border);

    /* Sides + interior. */
    for (i = 1; i < h - 1; i++) {
        int k;
        put_cell(x,         y + i, g[5], border);
        for (k = 1; k < w - 1; k++) put_cell(x + k, y + i, ' ', inner);
        put_cell(x + w - 1, y + i, g[5], border);
    }

    /* Bottom edge. */
    put_cell(x,         y + h - 1, g[2], border);
    for (i = 1; i < w - 1; i++) put_cell(x + i, y + h - 1, g[4], border);
    put_cell(x + w - 1, y + h - 1, g[3], border);

    /* Centred title in the top edge. */
    if (title && *title) {
        int len = tui_strlen(title);
        int tx;
        if (len > w - 4) len = w - 4;
        tx = x + (w - len - 2) / 2;
        put_cell(tx,             y, ' ', make_attr(TUI_YELLOW, TUI_BLUE));
        tui_printn_at(tx + 1, y, TUI_YELLOW, TUI_BLUE, title, len);
        put_cell(tx + 1 + len,   y, ' ', make_attr(TUI_YELLOW, TUI_BLUE));
    }
}

void tui_title_bar(const char *text) {
    tui_fill(1, 1, COLS, 1, TUI_WHITE, TUI_BLUE, ' ');
    tui_centre(1, TUI_YELLOW, TUI_BLUE, text);
}

void tui_status_bar(const char *text) {
    int len = tui_strlen(text);
    tui_fill(1, ROWS, COLS, 1, TUI_BLACK, TUI_LIGHTGRAY, ' ');
    if (len > COLS - 2) len = COLS - 2;
    tui_printn_at(2, ROWS, TUI_BLACK, TUI_LIGHTGRAY, text, len);
}

void tui_progress(int x, int y, int w, int pct) {
    int fill, i;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    fill = ((w - 2) * pct) / 100;

    put_cell(x, y, '[', make_attr(TUI_CYAN, TUI_BLUE));
    for (i = 0; i < w - 2; i++) {
        put_cell(x + 1 + i, y,
                 (unsigned char)(i < fill ? 0xDB : 0xB0),
                 make_attr(TUI_LIGHTGREEN, TUI_BLUE));
    }
    put_cell(x + w - 1, y, ']', make_attr(TUI_CYAN, TUI_BLUE));

    {
        char num[8];
        sprintf(num, " %3d%%", pct);
        tui_print_at(x + w, y, TUI_WHITE, TUI_BLUE, num);
    }
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */

int tui_getkey(void) {
    int c = getch();
    if (c == 0 || c == 0xE0) {
        c = getch();
        return 0x100 | (c & 0xFF);
    }
    return c;
}

void tui_pause(const char *prompt) {
    tui_status_bar(prompt);
    tui_getkey();
}

/* ------------------------------------------------------------------ */
/* Menu widget                                                         */
/* ------------------------------------------------------------------ */

static void menu_render(int x, int y, int w,
                        const char *const *items, int count, int selected) {
    int i;
    for (i = 0; i < count; i++) {
        int fg = (i == selected) ? TUI_BLACK  : TUI_WHITE;
        int bg = (i == selected) ? TUI_CYAN   : TUI_BLUE;
        unsigned char attr = make_attr(fg, bg);
        int len = tui_strlen(items[i]);
        int k;
        if (len > w - 2) len = w - 2;
        put_cell(x, y + i, ' ', attr);
        for (k = 0; k < len; k++) {
            put_cell(x + 1 + k, y + i, (unsigned char)items[i][k], attr);
        }
        for (k = len; k < w - 2; k++) {
            put_cell(x + 1 + k, y + i, ' ', attr);
        }
        put_cell(x + w - 1, y + i, ' ', attr);
    }
}

int tui_menu(int x, int y, int w,
             const char *const *items, int count, int selected) {
    int k;
    if (selected < 0 || selected >= count) selected = 0;
    menu_render(x, y, w, items, count, selected);
    for (;;) {
        k = tui_getkey();
        switch (k) {
        case KEY_UP:
            selected = (selected > 0) ? selected - 1 : count - 1;
            break;
        case KEY_DOWN:
            selected = (selected < count - 1) ? selected + 1 : 0;
            break;
        case KEY_HOME:
        case KEY_PGUP:
            selected = 0;
            break;
        case KEY_END:
        case KEY_PGDN:
            selected = count - 1;
            break;
        case KEY_ENTER:
            return selected;
        case KEY_ESC:
            return -1;
        case KEY_F10:
            return -2;
        default:
            if (k >= '1' && k <= '9') {
                int idx = k - '1';
                if (idx < count) {
                    selected = idx;
                    menu_render(x, y, w, items, count, selected);
                    return selected;
                }
            }
            break;
        }
        menu_render(x, y, w, items, count, selected);
    }
}

/* ------------------------------------------------------------------ */
/* Editable input field                                                */
/* ------------------------------------------------------------------ */

static void input_render(int x, int y, int w, const char *buf) {
    int len = tui_strlen(buf);
    unsigned char attr = make_attr(TUI_BLACK, TUI_WHITE);
    int i;
    for (i = 0; i < w; i++) {
        unsigned char c = (i < len) ? (unsigned char)buf[i] : ' ';
        put_cell(x + i, y, c, attr);
    }
}

int tui_input(int x, int y, int w,
              const char *prompt, char *buf, int maxlen) {
    int len = tui_strlen(buf);
    int k;
    int px = x;

    if (prompt && *prompt) {
        tui_print_at(x, y, TUI_WHITE, TUI_BLUE, prompt);
        px = x + tui_strlen(prompt) + 1;
    }
    tui_show_cursor();
    for (;;) {
        input_render(px, y, w, buf);
        bios_set_cursor_pos(px - 1 + (len < w ? len : w - 1), y - 1);
        k = tui_getkey();
        if (k == KEY_ENTER) {
            tui_hide_cursor();
            return 0;
        }
        if (k == KEY_ESC) {
            tui_hide_cursor();
            return -1;
        }
        if (k == KEY_BACKSPACE) {
            if (len > 0) buf[--len] = '\0';
            continue;
        }
        if (k >= 0x20 && k < 0x7F && len < maxlen && len < w) {
            buf[len++] = (char)k;
            buf[len] = '\0';
        }
    }
}

/* ------------------------------------------------------------------ */
/* Modal dialogs                                                       */
/* ------------------------------------------------------------------ */

static void measure_text(const char *text, int *out_w, int *out_h) {
    int w = 0, line_w = 0, h = 1;
    const char *p = text;
    while (*p) {
        if (*p == '\n') {
            if (line_w > w) w = line_w;
            line_w = 0;
            h++;
        } else {
            line_w++;
        }
        p++;
    }
    if (line_w > w) w = line_w;
    *out_w = w;
    *out_h = h;
}

static void draw_text_block(int x, int y, int fg, int bg, const char *text) {
    const char *line = text;
    int row = y;
    const char *p = text;
    for (;;) {
        if (*p == '\n' || *p == '\0') {
            tui_printn_at(x, row, fg, bg, line, (int)(p - line));
            if (*p == '\0') break;
            row++;
            line = p + 1;
        }
        p++;
    }
}

void tui_message(const char *title, const char *text, int type) {
    int tw, th, w, h, x, y, fg, bg;
    measure_text(text, &tw, &th);
    w = tw + 4;
    h = th + 4;
    if (w < 30) w = 30;
    if (w > 76) w = 76;
    if (h > 20) h = 20;
    x = (COLS - w) / 2 + 1;
    y = (ROWS - h) / 2;

    switch (type) {
    case TUI_ERROR: fg = TUI_WHITE; bg = TUI_RED;       break;
    case TUI_WARN:  fg = TUI_BLACK; bg = TUI_BROWN;     break;
    case TUI_OK:    fg = TUI_BLACK; bg = TUI_GREEN;     break;
    case TUI_INFO:
    default:        fg = TUI_WHITE; bg = TUI_BLUE;      break;
    }

    /* Drop-shadow. */
    tui_fill(x + 1, y + h, w, 1, TUI_BLACK, TUI_BLACK, ' ');
    tui_fill(x + w, y + 1, 1, h, TUI_BLACK, TUI_BLACK, ' ');

    tui_fill(x, y, w, h, fg, bg, ' ');
    tui_draw_box(x, y, w, h, title, 1);
    draw_text_block(x + 2, y + 2, fg, bg, text);
    tui_centre(y + h - 1, TUI_YELLOW, TUI_BLUE, " Press any key ");
    tui_getkey();
}

int tui_confirm(const char *text) {
    int tw, th, w, h, x, y;
    int sel = 0; /* 0 = Yes, 1 = No */
    measure_text(text, &tw, &th);
    w = tw + 4;
    h = th + 5;
    if (w < 34) w = 34;
    if (w > 76) w = 76;
    x = (COLS - w) / 2 + 1;
    y = (ROWS - h) / 2;

    tui_fill(x + 1, y + h, w, 1, TUI_BLACK, TUI_BLACK, ' ');
    tui_fill(x + w, y + 1, 1, h, TUI_BLACK, TUI_BLACK, ' ');
    tui_fill(x, y, w, h, TUI_WHITE, TUI_BLUE, ' ');
    tui_draw_box(x, y, w, h, "Confirm", 1);
    draw_text_block(x + 2, y + 2, TUI_WHITE, TUI_BLUE, text);

    for (;;) {
        int yfg = sel == 0 ? TUI_BLACK : TUI_WHITE;
        int ybg = sel == 0 ? TUI_CYAN  : TUI_BLUE;
        int nfg = sel == 1 ? TUI_BLACK : TUI_WHITE;
        int nbg = sel == 1 ? TUI_CYAN  : TUI_BLUE;
        int by = y + h - 2;
        tui_print_at(x + w/2 - 10, by, yfg, ybg, "  [ Yes ]  ");
        tui_print_at(x + w/2 + 1,  by, nfg, nbg, "  [ No  ]  ");

        switch (tui_getkey()) {
        case KEY_LEFT:
        case KEY_UP:
            sel = 0; break;
        case KEY_RIGHT:
        case KEY_DOWN:
        case KEY_TAB:
            sel = 1; break;
        case 'y': case 'Y':
            return 1;
        case 'n': case 'N':
        case KEY_ESC:
            return 0;
        case KEY_ENTER:
            return sel == 0 ? 1 : 0;
        }
    }
}
