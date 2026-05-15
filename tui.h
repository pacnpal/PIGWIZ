/*
 * tui.h - Shared TUI primitives for PicoGUS setup tools.
 *
 * 80x25 colour text mode. Built on Open Watcom conio.h (gotoxy / cputs /
 * textcolor / textbackground / getch / kbhit). Uses CP437 box-drawing.
 *
 * Target: 16-bit real-mode DOS, large memory model. No allocations.
 */
#ifndef TUI_H
#define TUI_H

#include <conio.h>

/* Conio palette mnemonics, restated here so callers do not have to include
 * conio.h. Values match the BIOS/VGA attribute byte. */
#define TUI_BLACK         0
#define TUI_BLUE          1
#define TUI_GREEN         2
#define TUI_CYAN          3
#define TUI_RED           4
#define TUI_MAGENTA       5
#define TUI_BROWN         6
#define TUI_LIGHTGRAY     7
#define TUI_DARKGRAY      8
#define TUI_LIGHTBLUE     9
#define TUI_LIGHTGREEN    10
#define TUI_LIGHTCYAN     11
#define TUI_LIGHTRED      12
#define TUI_LIGHTMAGENTA  13
#define TUI_YELLOW        14
#define TUI_WHITE         15

/* Modal message types (control colour scheme). */
#define TUI_INFO    0
#define TUI_WARN    1
#define TUI_ERROR   2
#define TUI_OK      3

/* Key constants. Extended (function/arrow) keys come back from BIOS as a
 * leading 0x00 (or 0xE0) followed by a scan code. We OR 0x100 to keep them
 * out of the 7-bit ASCII range. */
#define KEY_ESC        0x1B
#define KEY_ENTER      0x0D
#define KEY_TAB        0x09
#define KEY_BACKSPACE  0x08
#define KEY_SPACE      0x20
#define KEY_F1         (0x100 | 0x3B)
#define KEY_F10        (0x100 | 0x44)
#define KEY_UP         (0x100 | 0x48)
#define KEY_DOWN       (0x100 | 0x50)
#define KEY_LEFT       (0x100 | 0x4B)
#define KEY_RIGHT      (0x100 | 0x4D)
#define KEY_PGUP       (0x100 | 0x49)
#define KEY_PGDN       (0x100 | 0x51)
#define KEY_HOME       (0x100 | 0x47)
#define KEY_END        (0x100 | 0x4F)

/* Screen lifecycle. */
void tui_init(void);            /* set 80x25 colour mode, hide cursor */
void tui_shutdown(void);        /* restore cursor + reset attrs       */
void tui_show_cursor(void);
void tui_hide_cursor(void);

/* Painting. */
void tui_clear(void);           /* fill 80x25 with blue background    */
void tui_fill(int x, int y, int w, int h, int fg, int bg, char ch);
void tui_print_at(int x, int y, int fg, int bg, const char *text);
void tui_printn_at(int x, int y, int fg, int bg, const char *text, int n);
void tui_draw_box(int x, int y, int w, int h,
                  const char *title, int double_line);
void tui_title_bar(const char *text);
void tui_status_bar(const char *text);
void tui_progress(int x, int y, int w, int pct);

/* Input. Returns a key code as defined above. */
int  tui_getkey(void);

/* High-level widgets. items[] is an array of NUL-terminated strings.
 *   selected: index to highlight on entry.
 *   returns the chosen index (>=0), -1 on ESC, -2 on F10.            */
int  tui_menu(int x, int y, int w,
              const char *const *items, int count, int selected);

/* Editable string input. Returns 0 on Enter, -1 on ESC. Buf must hold
 * at least maxlen+1 bytes. */
int  tui_input(int x, int y, int w,
               const char *prompt, char *buf, int maxlen);

/* Centred modal. type controls colour: INFO=cyan, WARN=brown,
 * ERROR=red, OK=green. Waits for any key. */
void tui_message(const char *title, const char *text, int type);

/* Yes/No modal. Returns 1 for yes, 0 for no/ESC. */
int  tui_confirm(const char *text);

/* Helpers used by both apps. */
void tui_centre(int row, int fg, int bg, const char *text);
int  tui_strlen(const char *s);
void tui_pause(const char *prompt); /* show prompt on status bar, wait key */

#endif /* TUI_H */
