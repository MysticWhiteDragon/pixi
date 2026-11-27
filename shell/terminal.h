/* The terminal engine: a pty, a shell on it, and an xterm-style screen.
 * It draws nothing; the shell reads the cells and paints them. */
#ifndef PIXI_TERMINAL_H
#define PIXI_TERMINAL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* A color is a kind in the top byte and a value below: default, a palette number or 24-bit RGB. */
#define TC_DEFAULT 0u
#define TC_PAL(n) (0x01000000u | (unsigned)(n))
#define TC_RGB(r, g, b) (0x02000000u | ((unsigned)(r) << 16) | ((unsigned)(g) << 8) | (unsigned)(b))
#define TC_KIND(c) ((c) >> 24)

enum { TA_BOLD = 1, TA_DIM = 2, TA_ITALIC = 4, TA_UNDER = 8, TA_REVERSE = 16, TA_WIDE = 32, TA_CONT = 64, TA_STRIKE = 128 };

typedef struct {
    uint32_t ch; /* code point; 0 for the right half of a wide character */
    uint32_t fg, bg;
    uint8_t attr;
} TCell;

enum { TK_UP, TK_DOWN, TK_RIGHT, TK_LEFT, TK_HOME, TK_END, TK_PGUP, TK_PGDN, TK_DEL, TK_INS, TK_TAB_BACK };

typedef struct Term Term;

Term *term_new(int cols, int rows);
void term_free(Term *t);

/* Starts argv (or the user's shell when it's NULL) in the home folder. 0 on success. */
int term_spawn(Term *t, char *const argv[], const char *cwd);

/* Reads what the program printed. Bit 0 of the result: the screen changed. Bit 1: the program has ended. */
int term_pump(Term *t);
int term_ended(const Term *t);
int term_exit_code(const Term *t); /* meaningful once ended */
double term_age(const Term *t);    /* seconds since it started */
int term_child_reaped(Term *t, pid_t pid, int status); /* for a SIGCHLD handler that reaps everything: 1 if it was ours */
void term_hangup(Term *t);

void term_resize(Term *t, int cols, int rows);
void term_write(Term *t, const void *buf, size_t n); /* typed text, to the program */
void term_feed(Term *t, const char *s, size_t n);     /* text for the screen itself, as if the program printed it */
void term_key(Term *t, int key);                     /* one of TK_*, in the right form for the mode */
void term_paste_begin(Term *t);
int term_take_bell(Term *t);

int term_cols(const Term *t);
int term_rows(const Term *t);
const TCell *term_cell(const Term *t, int x, int y); /* as seen now, scrolled back or not */
int term_cursor(const Term *t, int *x, int *y);      /* 1 when it should be drawn */
const char *term_title(const Term *t);
void term_scroll_view(Term *t, int lines); /* positive goes back in time */
void term_scroll_reset(Term *t);
int term_scrolled(const Term *t);
int term_app_cursor(const Term *t);
int term_alt_screen(const Term *t);

#endif
