/* The terminal engine. See terminal.h. */
#define _GNU_SOURCE
#include "terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SB_MAX 2000
#define MAXP 24

enum { S_GROUND, S_ESC, S_ESC_ONE, S_CSI, S_OSC, S_STR, S_STR_ESC };

typedef struct {
    TCell *c;
    int w;
} Line;

struct Term {
    int cols, rows;
    TCell *grid[2];
    int alt;
    Line *sb;
    int sb_n, sb_head;
    int view;

    int cx, cy, wrap_pending;
    int scx, scy, s_wrap, s_dec;
    uint32_t s_fg, s_bg;
    uint8_t s_attr;
    uint32_t fg, bg;
    uint8_t attr;
    int top, bot;
    unsigned char *tabs;
    int g0_dec;
    int app_cursor, autowrap, cur_visible, bracketed, insert;
    uint32_t last_ch;
    int bell;

    int state, nparams, priv, inter, sub[MAXP];
    int params[MAXP];
    int have_digit;
    uint32_t cp;
    int need;
    char osc[256];
    int osc_n;
    char title[256];

    int fd, exited, code;
    pid_t pid;
    struct timespec started;
};

static int cp_width(uint32_t c)
{
    if (c < 0x300) return 1;
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) || (c >= 0xFE00 && c <= 0xFE0F) || c == 0x200D) return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
        (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

static TCell blank_cell(const Term *t)
{
    TCell c = { ' ', TC_DEFAULT, t->bg, 0 };
    return c;
}

static TCell *row_at(Term *t, int y) { return t->grid[t->alt] + (size_t)y * (size_t)t->cols; }

static void fill_cells(TCell *c, int n, TCell v)
{
    for (int i = 0; i < n; i++) c[i] = v;
}

static void tabs_reset(Term *t)
{
    for (int i = 0; i < t->cols; i++) t->tabs[i] = (i % 8) == 0;
}

static void push_scrollback(Term *t, const TCell *row)
{
    if (t->alt) return;
    int idx = (t->sb_head + t->sb_n) % SB_MAX;
    if (t->sb_n == SB_MAX) {
        idx = t->sb_head;
        free(t->sb[idx].c);
        t->sb_head = (t->sb_head + 1) % SB_MAX;
    } else {
        t->sb_n++;
    }
    t->sb[idx].c = malloc((size_t)t->cols * sizeof(TCell));
    t->sb[idx].w = t->cols;
    if (t->sb[idx].c) memcpy(t->sb[idx].c, row, (size_t)t->cols * sizeof(TCell));
    else t->sb[idx].w = 0;
    if (t->view > 0 && t->view < t->sb_n) t->view++;
}

static void scroll_up(Term *t, int top, int bot, int n)
{
    if (n <= 0) return;
    int h = bot - top + 1;
    if (n > h) n = h;
    if (top == 0 && !t->alt)
        for (int i = 0; i < n; i++) push_scrollback(t, row_at(t, i));
    memmove(row_at(t, top), row_at(t, top + n), (size_t)(h - n) * (size_t)t->cols * sizeof(TCell));
    TCell b = blank_cell(t);
    for (int y = bot - n + 1; y <= bot; y++) fill_cells(row_at(t, y), t->cols, b);
}

static void scroll_down(Term *t, int top, int bot, int n)
{
    if (n <= 0) return;
    int h = bot - top + 1;
    if (n > h) n = h;
    memmove(row_at(t, top + n), row_at(t, top), (size_t)(h - n) * (size_t)t->cols * sizeof(TCell));
    TCell b = blank_cell(t);
    for (int y = top; y < top + n; y++) fill_cells(row_at(t, y), t->cols, b);
}

static void index_down(Term *t)
{
    if (t->cy == t->bot) scroll_up(t, t->top, t->bot, 1);
    else if (t->cy < t->rows - 1) t->cy++;
}

static void reverse_index(Term *t)
{
    if (t->cy == t->top) scroll_down(t, t->top, t->bot, 1);
    else if (t->cy > 0) t->cy--;
}

static void reset_state(Term *t)
{
    t->fg = t->bg = TC_DEFAULT;
    t->attr = 0;
    t->top = 0;
    t->bot = t->rows - 1;
    t->cx = t->cy = 0;
    t->wrap_pending = 0;
    t->g0_dec = 0;
    t->app_cursor = 0;
    t->autowrap = 1;
    t->cur_visible = 1;
    t->bracketed = 0;
    t->insert = 0;
    t->scx = t->scy = 0;
    t->s_fg = t->s_bg = TC_DEFAULT;
    t->s_attr = 0;
    t->s_wrap = t->s_dec = 0;
    tabs_reset(t);
}

Term *term_new(int cols, int rows)
{
    if (cols < 2) cols = 2;
    if (rows < 1) rows = 1;
    Term *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->cols = cols;
    t->rows = rows;
    t->fd = -1;
    t->tabs = malloc((size_t)cols);
    t->sb = calloc(SB_MAX, sizeof(Line));
    for (int i = 0; i < 2; i++) t->grid[i] = calloc((size_t)cols * (size_t)rows, sizeof(TCell));
    if (!t->tabs || !t->sb || !t->grid[0] || !t->grid[1]) {
        term_free(t);
        return NULL;
    }
    reset_state(t);
    TCell b = blank_cell(t);
    for (int i = 0; i < 2; i++) fill_cells(t->grid[i], cols * rows, b);
    return t;
}

void term_free(Term *t)
{
    if (!t) return;
    if (t->fd >= 0) close(t->fd);
    if (t->pid > 0 && !t->exited) {
        kill(t->pid, SIGHUP);
        waitpid(t->pid, NULL, WNOHANG);
    }
    if (t->sb) {
        for (int i = 0; i < SB_MAX; i++) free(t->sb[i].c);
        free(t->sb);
    }
    free(t->grid[0]);
    free(t->grid[1]);
    free(t->tabs);
    free(t);
}

int term_spawn(Term *t, char *const argv[], const char *cwd)
{
    struct winsize ws = { (unsigned short)t->rows, (unsigned short)t->cols, 0, 0 };
    int fd;
    pid_t pid = forkpty(&fd, NULL, NULL, &ws);
    if (pid < 0) return -1;
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        const char *lc = getenv("LC_ALL");
        if (!lc || !*lc) lc = getenv("LC_CTYPE");
        if (!lc || !*lc) lc = getenv("LANG");
        if (!lc || (!strstr(lc, "UTF-8") && !strstr(lc, "utf8") && !strstr(lc, "UTF8") && !strstr(lc, "utf-8"))) {
            setenv("LC_CTYPE", "C.UTF-8", 1);
            unsetenv("LC_ALL");
        }
        unsetenv("COLUMNS");
        unsetenv("LINES");
        if (cwd && *cwd && chdir(cwd) != 0) {}
        if (argv && argv[0]) {
            execvp(argv[0], argv);
        } else {
            const char *sh = getenv("SHELL");
            if (!sh || !*sh) sh = "/bin/bash";
            char *a[2] = { (char *)sh, NULL }; /* not a login shell: ~/.profile would start the desktop again */
            execvp(sh, a);
            char *b[2] = { "/bin/sh", NULL };
            execv("/bin/sh", b);
        }
        fprintf(stderr, "couldn't run %s: %s\n", argv && argv[0] ? argv[0] : "the shell", strerror(errno));
        _exit(127);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    t->fd = fd;
    t->pid = pid;
    t->exited = 0;
    t->code = 0;
    clock_gettime(CLOCK_MONOTONIC, &t->started);
    return 0;
}

double term_age(const Term *t)
{
    struct timespec n;
    clock_gettime(CLOCK_MONOTONIC, &n);
    return (double)(n.tv_sec - t->started.tv_sec) + (double)(n.tv_nsec - t->started.tv_nsec) / 1e9;
}

int term_child_reaped(Term *t, pid_t pid, int status)
{
    if (!t || t->pid <= 0 || pid != t->pid) return 0;
    t->exited = 1;
    t->code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return 1;
}

int term_ended(const Term *t) { return t->exited; }
int term_exit_code(const Term *t) { return t->code; }
void term_hangup(Term *t)
{
    if (t->pid > 0 && !t->exited) kill(t->pid, SIGHUP);
}

void term_write(Term *t, const void *buf, size_t n)
{
    if (t->fd < 0 || t->exited) return;
    const char *p = buf;
    int tries = 0;
    while (n > 0 && tries < 20) {
        ssize_t w = write(t->fd, p, n);
        if (w > 0) {
            p += w;
            n -= (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            struct pollfd pf = { t->fd, POLLOUT, 0 };
            poll(&pf, 1, 5);
            tries++;
            continue;
        }
        break;
    }
    t->view = 0;
}

void term_key(Term *t, int key)
{
    const char *s = NULL;
    char b[8];
    int app = t->app_cursor;
    switch (key) {
    case TK_UP: s = app ? "\033OA" : "\033[A"; break;
    case TK_DOWN: s = app ? "\033OB" : "\033[B"; break;
    case TK_RIGHT: s = app ? "\033OC" : "\033[C"; break;
    case TK_LEFT: s = app ? "\033OD" : "\033[D"; break;
    case TK_HOME: s = app ? "\033OH" : "\033[H"; break;
    case TK_END: s = app ? "\033OF" : "\033[F"; break;
    case TK_PGUP: s = "\033[5~"; break;
    case TK_PGDN: s = "\033[6~"; break;
    case TK_DEL: s = "\033[3~"; break;
    case TK_INS: s = "\033[2~"; break;
    case TK_TAB_BACK: s = "\033[Z"; break;
    default: return;
    }
    snprintf(b, sizeof b, "%s", s);
    term_write(t, b, strlen(b));
}

void term_paste_begin(Term *t) { (void)t; }
int term_take_bell(Term *t)
{
    int b = t->bell;
    t->bell = 0;
    return b;
}

/* ---------- the screen -------------------------------------------------- */

static void clear_wide_at(Term *t, int x, int y)
{
    TCell *r = row_at(t, y);
    if (r[x].attr & TA_CONT) {
        if (x > 0) r[x - 1].ch = ' ', r[x - 1].attr &= (uint8_t)~TA_WIDE;
        r[x].attr &= (uint8_t)~TA_CONT;
    }
    if ((r[x].attr & TA_WIDE) && x + 1 < t->cols) {
        r[x + 1].ch = ' ';
        r[x + 1].attr &= (uint8_t)~TA_CONT;
    }
}

static const uint32_t dec_map[32] = {
    0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0xB0, 0xB1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, 0x23BA,
    0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265, 0x3C0, 0x2260, 0xA3, 0xB7, 0x20
}; /* for 0x60 .. 0x7F */

static void put_char(Term *t, uint32_t cp)
{
    if (t->g0_dec && cp >= 0x60 && cp <= 0x7F) cp = dec_map[cp - 0x60];
    int w = cp_width(cp);
    if (w == 0) return;
    t->last_ch = cp;
    if (t->wrap_pending && t->autowrap) {
        t->cx = 0;
        t->wrap_pending = 0;
        index_down(t);
    }
    if (w == 2 && t->cx >= t->cols - 1) {
        if (t->autowrap) {
            row_at(t, t->cy)[t->cx].ch = ' ';
            t->cx = 0;
            index_down(t);
        } else {
            w = 1;
        }
    }
    TCell *r = row_at(t, t->cy);
    if (t->insert) {
        int n = w;
        if (t->cx + n < t->cols) memmove(&r[t->cx + n], &r[t->cx], (size_t)(t->cols - t->cx - n) * sizeof(TCell));
    }
    clear_wide_at(t, t->cx, t->cy);
    if (w == 2) clear_wide_at(t, t->cx + 1, t->cy);
    TCell c = { cp, t->fg, t->bg, t->attr };
    if (w == 2) c.attr |= TA_WIDE;
    r[t->cx] = c;
    if (w == 2 && t->cx + 1 < t->cols) {
        TCell h = { 0, t->fg, t->bg, (uint8_t)(t->attr | TA_CONT) };
        r[t->cx + 1] = h;
    }
    t->cx += w;
    if (t->cx >= t->cols) {
        t->cx = t->cols - 1;
        t->wrap_pending = 1;
    }
}

static void erase_range(Term *t, int y, int x0, int x1) /* x1 exclusive */
{
    if (x0 < 0) x0 = 0;
    if (x1 > t->cols) x1 = t->cols;
    if (x0 >= x1) return;
    TCell *r = row_at(t, y);
    if (x0 > 0) clear_wide_at(t, x0, y);
    if (x1 < t->cols) clear_wide_at(t, x1 - 1, y);
    fill_cells(r + x0, x1 - x0, blank_cell(t));
}

static int pnum(const Term *t, int i, int def)
{
    if (i >= t->nparams) return def;
    return t->params[i] > 0 ? t->params[i] : def;
}

static void sgr(Term *t)
{
    if (t->nparams == 0) {
        t->fg = t->bg = TC_DEFAULT;
        t->attr = 0;
        return;
    }
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        if (p == 0) t->fg = t->bg = TC_DEFAULT, t->attr = 0;
        else if (p == 1) t->attr |= TA_BOLD;
        else if (p == 2) t->attr |= TA_DIM;
        else if (p == 3) t->attr |= TA_ITALIC;
        else if (p == 4) t->attr |= TA_UNDER;
        else if (p == 7) t->attr |= TA_REVERSE;
        else if (p == 9) t->attr |= TA_STRIKE;
        else if (p == 22) t->attr &= (uint8_t)~(TA_BOLD | TA_DIM);
        else if (p == 23) t->attr &= (uint8_t)~TA_ITALIC;
        else if (p == 24) t->attr &= (uint8_t)~TA_UNDER;
        else if (p == 27) t->attr &= (uint8_t)~TA_REVERSE;
        else if (p == 29) t->attr &= (uint8_t)~TA_STRIKE;
        else if (p >= 30 && p <= 37) t->fg = TC_PAL(p - 30);
        else if (p == 39) t->fg = TC_DEFAULT;
        else if (p >= 40 && p <= 47) t->bg = TC_PAL(p - 40);
        else if (p == 49) t->bg = TC_DEFAULT;
        else if (p >= 90 && p <= 97) t->fg = TC_PAL(p - 90 + 8);
        else if (p >= 100 && p <= 107) t->bg = TC_PAL(p - 100 + 8);
        else if (p == 38 || p == 48) {
            uint32_t col = TC_DEFAULT;
            int ok = 0;
            if (i + 1 < t->nparams && t->params[i + 1] == 5 && i + 2 < t->nparams) {
                col = TC_PAL(t->params[i + 2] & 255);
                i += 2;
                ok = 1;
            } else if (i + 1 < t->nparams && t->params[i + 1] == 2) {
                int o = 2;
                if (i + 2 < t->nparams && t->sub[i + 2] && i + 5 < t->nparams) o = 3; /* 38:2::r:g:b has a color space id */
                if (i + o + 2 < t->nparams) {
                    col = TC_RGB(t->params[i + o] & 255, t->params[i + o + 1] & 255, t->params[i + o + 2] & 255);
                    i += o + 2;
                    ok = 1;
                }
            }
            if (ok) {
                if (p == 38) t->fg = col;
                else t->bg = col;
            }
        }
    }
}

static void reply(Term *t, const char *s) { term_write(t, s, strlen(s)); }

static void set_alt(Term *t, int on, int clear)
{
    if (on == t->alt) return;
    t->alt = on;
    t->view = 0;
    if (on && clear) fill_cells(t->grid[1], t->cols * t->rows, blank_cell(t));
}

static void save_cursor(Term *t)
{
    t->scx = t->cx, t->scy = t->cy;
    t->s_fg = t->fg, t->s_bg = t->bg, t->s_attr = t->attr;
    t->s_wrap = t->wrap_pending, t->s_dec = t->g0_dec;
}

static void restore_cursor(Term *t)
{
    t->cx = t->scx < t->cols ? t->scx : t->cols - 1;
    t->cy = t->scy < t->rows ? t->scy : t->rows - 1;
    t->fg = t->s_fg, t->bg = t->s_bg, t->attr = t->s_attr;
    t->wrap_pending = t->s_wrap, t->g0_dec = t->s_dec;
}

static void set_mode(Term *t, int on)
{
    for (int i = 0; i < (t->nparams ? t->nparams : 1); i++) {
        int m = t->params[i];
        if (t->priv == '?') {
            switch (m) {
            case 1: t->app_cursor = on; break;
            case 7: t->autowrap = on; break;
            case 25: t->cur_visible = on; break;
            case 47:
            case 1047: set_alt(t, on, on); break;
            case 1048: if (on) save_cursor(t); else restore_cursor(t); break;
            case 1049:
                if (on) {
                    save_cursor(t);
                    set_alt(t, 1, 1);
                } else {
                    set_alt(t, 0, 0);
                    restore_cursor(t);
                }
                break;
            case 2004: t->bracketed = on; break;
            default: break;
            }
        } else if (m == 4) {
            t->insert = on;
        }
    }
}

static void csi(Term *t, int final)
{
    int n = pnum(t, 0, 1);
    switch (final) {
    case 'A': {
        int lim = t->cy >= t->top ? t->top : 0;
        t->cy -= n;
        if (t->cy < lim) t->cy = lim;
        t->wrap_pending = 0;
        break;
    }
    case 'B':
    case 'e': {
        int lim = t->cy <= t->bot ? t->bot : t->rows - 1;
        t->cy += n;
        if (t->cy > lim) t->cy = lim;
        t->wrap_pending = 0;
        break;
    }
    case 'C':
    case 'a': t->cx += n; if (t->cx >= t->cols) t->cx = t->cols - 1; t->wrap_pending = 0; break;
    case 'D': t->cx -= n; if (t->cx < 0) t->cx = 0; t->wrap_pending = 0; break;
    case 'E': t->cy += n; if (t->cy >= t->rows) t->cy = t->rows - 1; t->cx = 0; t->wrap_pending = 0; break;
    case 'F': t->cy -= n; if (t->cy < 0) t->cy = 0; t->cx = 0; t->wrap_pending = 0; break;
    case 'G':
    case '`': t->cx = n - 1; if (t->cx >= t->cols) t->cx = t->cols - 1; t->wrap_pending = 0; break;
    case 'd': t->cy = n - 1; if (t->cy >= t->rows) t->cy = t->rows - 1; t->wrap_pending = 0; break;
    case 'H':
    case 'f':
        t->cy = pnum(t, 0, 1) - 1;
        t->cx = pnum(t, 1, 1) - 1;
        if (t->cy >= t->rows) t->cy = t->rows - 1;
        if (t->cx >= t->cols) t->cx = t->cols - 1;
        t->wrap_pending = 0;
        break;
    case 'J': {
        int m = t->nparams ? t->params[0] : 0;
        if (m == 0) {
            erase_range(t, t->cy, t->cx, t->cols);
            for (int y = t->cy + 1; y < t->rows; y++) erase_range(t, y, 0, t->cols);
        } else if (m == 1) {
            for (int y = 0; y < t->cy; y++) erase_range(t, y, 0, t->cols);
            erase_range(t, t->cy, 0, t->cx + 1);
        } else if (m == 2 || m == 3) {
            for (int y = 0; y < t->rows; y++) erase_range(t, y, 0, t->cols);
            if (m == 3 && !t->alt) {
                for (int i = 0; i < SB_MAX; i++) free(t->sb[i].c), t->sb[i].c = NULL, t->sb[i].w = 0;
                t->sb_n = t->sb_head = t->view = 0;
            }
        }
        break;
    }
    case 'K': {
        int m = t->nparams ? t->params[0] : 0;
        if (m == 0) erase_range(t, t->cy, t->cx, t->cols);
        else if (m == 1) erase_range(t, t->cy, 0, t->cx + 1);
        else if (m == 2) erase_range(t, t->cy, 0, t->cols);
        break;
    }
    case 'L':
        if (t->cy >= t->top && t->cy <= t->bot) scroll_down(t, t->cy, t->bot, n);
        t->cx = 0;
        t->wrap_pending = 0;
        break;
    case 'M':
        if (t->cy >= t->top && t->cy <= t->bot) scroll_up(t, t->cy, t->bot, n);
        t->cx = 0;
        t->wrap_pending = 0;
        break;
    case 'P': {
        TCell *r = row_at(t, t->cy);
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        clear_wide_at(t, t->cx, t->cy);
        memmove(&r[t->cx], &r[t->cx + n], (size_t)(t->cols - t->cx - n) * sizeof(TCell));
        fill_cells(&r[t->cols - n], n, blank_cell(t));
        t->wrap_pending = 0;
        break;
    }
    case '@': {
        TCell *r = row_at(t, t->cy);
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        clear_wide_at(t, t->cx, t->cy);
        memmove(&r[t->cx + n], &r[t->cx], (size_t)(t->cols - t->cx - n) * sizeof(TCell));
        fill_cells(&r[t->cx], n, blank_cell(t));
        t->wrap_pending = 0;
        break;
    }
    case 'X': erase_range(t, t->cy, t->cx, t->cx + n); break;
    case 'S': scroll_up(t, t->top, t->bot, n); break;
    case 'T': scroll_down(t, t->top, t->bot, n); break;
    case 'r': {
        int top = pnum(t, 0, 1) - 1, bot = pnum(t, 1, t->rows) - 1;
        if (bot >= t->rows) bot = t->rows - 1;
        if (top < 0) top = 0;
        if (top < bot) t->top = top, t->bot = bot;
        else t->top = 0, t->bot = t->rows - 1;
        t->cx = t->cy = 0;
        t->wrap_pending = 0;
        break;
    }
    case 's': save_cursor(t); break;
    case 'u': restore_cursor(t); break;
    case 'm': sgr(t); break;
    case 'h': set_mode(t, 1); break;
    case 'l': set_mode(t, 0); break;
    case 'n': {
        int m = t->nparams ? t->params[0] : 0;
        if (m == 5) reply(t, "\033[0n");
        else if (m == 6) {
            char b[32];
            snprintf(b, sizeof b, "\033[%d;%dR", t->cy + 1, t->cx + 1);
            reply(t, b);
        }
        break;
    }
    case 'c':
        if (t->priv == '>') reply(t, "\033[>0;0;0c");
        else if (!t->priv) reply(t, "\033[?62;c");
        break;
    case 'g': {
        int m = t->nparams ? t->params[0] : 0;
        if (m == 0) t->tabs[t->cx] = 0;
        else if (m == 3) memset(t->tabs, 0, (size_t)t->cols);
        break;
    }
    case 'I':
        while (n-- > 0) {
            do t->cx++; while (t->cx < t->cols - 1 && !t->tabs[t->cx]);
            if (t->cx > t->cols - 1) t->cx = t->cols - 1;
        }
        t->wrap_pending = 0;
        break;
    case 'Z':
        while (n-- > 0) {
            do t->cx--; while (t->cx > 0 && !t->tabs[t->cx]);
            if (t->cx < 0) t->cx = 0;
        }
        t->wrap_pending = 0;
        break;
    case 'b':
        if (t->last_ch)
            for (int i = 0; i < n && i < 2000; i++) put_char(t, t->last_ch);
        break;
    default: break; /* window operations, cursor styles, mouse and the like: nothing to do */
    }
}

static void control(Term *t, unsigned char c)
{
    switch (c) {
    case 0x07: t->bell = 1; break;
    case 0x08: if (t->cx > 0) t->cx--; t->wrap_pending = 0; break;
    case 0x09:
        do t->cx++; while (t->cx < t->cols - 1 && !t->tabs[t->cx]);
        if (t->cx > t->cols - 1) t->cx = t->cols - 1;
        t->wrap_pending = 0;
        break;
    case 0x0A: case 0x0B: case 0x0C: index_down(t); t->wrap_pending = 0; break;
    case 0x0D: t->cx = 0; t->wrap_pending = 0; break;
    default: break;
    }
}

static void csi_begin(Term *t)
{
    t->state = S_CSI;
    t->nparams = 0;
    t->priv = 0;
    t->inter = 0;
    t->have_digit = 0;
    memset(t->params, 0, sizeof t->params);
    memset(t->sub, 0, sizeof t->sub);
}

static void feed_byte(Term *t, unsigned char c)
{
    switch (t->state) {
    case S_GROUND:
        if (t->need > 0) {
            if ((c & 0xC0) == 0x80) {
                t->cp = (t->cp << 6) | (c & 0x3F);
                if (--t->need == 0) put_char(t, t->cp);
                return;
            }
            t->need = 0; /* broken sequence: treat this byte afresh */
        }
        if (c == 0x1B) t->state = S_ESC;
        else if (c < 0x20) control(t, c);
        else if (c < 0x7F) put_char(t, c);
        else if (c >= 0xC2 && c < 0xE0) t->cp = c & 0x1F, t->need = 1;
        else if (c >= 0xE0 && c < 0xF0) t->cp = c & 0x0F, t->need = 2;
        else if (c >= 0xF0 && c < 0xF5) t->cp = c & 0x07, t->need = 3;
        break;
    case S_ESC:
        t->state = S_GROUND;
        switch (c) {
        case '[': csi_begin(t); break;
        case ']': t->state = S_OSC; t->osc_n = 0; break;
        case 'P': case '_': case '^': case 'X': t->state = S_STR; break;
        case '(': case ')': case '*': case '+': case '#': case '%': case ' ':
            t->state = S_ESC_ONE;
            t->inter = c;
            break;
        case '7': save_cursor(t); break;
        case '8': restore_cursor(t); break;
        case 'M': reverse_index(t); t->wrap_pending = 0; break;
        case 'D': index_down(t); t->wrap_pending = 0; break;
        case 'E': t->cx = 0; index_down(t); t->wrap_pending = 0; break;
        case 'H': t->tabs[t->cx] = 1; break;
        case 'c': {
            t->alt = 0;
            reset_state(t);
            TCell b = blank_cell(t);
            fill_cells(t->grid[0], t->cols * t->rows, b);
            fill_cells(t->grid[1], t->cols * t->rows, b);
            break;
        }
        case 0x1B: t->state = S_ESC; break;
        default: break;
        }
        break;
    case S_ESC_ONE:
        t->state = S_GROUND;
        if (t->inter == '(') t->g0_dec = c == '0';
        else if (t->inter == '#' && c == '8') {
            TCell e = { 'E', TC_DEFAULT, TC_DEFAULT, 0 };
            for (int y = 0; y < t->rows; y++) fill_cells(row_at(t, y), t->cols, e);
        }
        break;
    case S_CSI:
        if (c == 0x1B) {
            t->state = S_ESC;
        } else if (c == 0x18 || c == 0x1A) {
            t->state = S_GROUND;
        } else if (c < 0x20) {
            control(t, c);
        } else if (c >= '0' && c <= '9') {
            if (t->nparams == 0) t->nparams = 1;
            if (t->nparams <= MAXP) {
                int *p = &t->params[t->nparams - 1];
                if (*p < 100000) *p = *p * 10 + (c - '0');
            }
            t->have_digit = 1;
        } else if (c == ';' || c == ':') {
            if (t->nparams == 0) t->nparams = 1;
            if (t->nparams < MAXP) {
                t->sub[t->nparams] = c == ':';
                t->nparams++;
            }
        } else if (c >= '<' && c <= '?') {
            t->priv = c;
        } else if (c >= 0x20 && c <= 0x2F) {
            t->inter = c;
        } else if (c >= 0x40 && c <= 0x7E) {
            t->state = S_GROUND;
            if (t->nparams > MAXP) t->nparams = MAXP;
            if (!t->inter) csi(t, c);
        }
        break;
    case S_OSC:
        if (c == 0x07) {
            t->osc[t->osc_n] = 0;
            t->state = S_GROUND;
            if ((t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';') snprintf(t->title, sizeof t->title, "%s", t->osc + 2);
        } else if (c == 0x1B) {
            t->osc[t->osc_n] = 0;
            if ((t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';') snprintf(t->title, sizeof t->title, "%s", t->osc + 2);
            t->state = S_STR_ESC;
        } else if (t->osc_n < (int)sizeof t->osc - 1) {
            t->osc[t->osc_n++] = (char)c;
        }
        break;
    case S_STR:
        if (c == 0x1B) t->state = S_STR_ESC;
        else if (c == 0x07) t->state = S_GROUND;
        break;
    case S_STR_ESC:
        t->state = c == '\\' ? S_GROUND : (c == 0x1B ? S_STR_ESC : S_STR);
        break;
    }
}

void term_feed(Term *t, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) feed_byte(t, (unsigned char)s[i]);
}

int term_pump(Term *t)
{
    int changed = 0;
    if (t->fd >= 0) {
        unsigned char buf[8192];
        int total = 0;
        for (;;) {
            ssize_t n = read(t->fd, buf, sizeof buf);
            if (n > 0) {
                for (ssize_t i = 0; i < n; i++) feed_byte(t, buf[i]);
                changed = 1;
                total += (int)n;
                if (total > 65536) break; /* keep the frame short; the rest comes next time */
                continue;
            }
            if (n < 0 && (errno == EINTR)) continue;
            break;
        }
    }
    if (!t->exited && t->pid > 0) {
        int st = 0;
        pid_t r = waitpid(t->pid, &st, WNOHANG);
        if (r == t->pid) term_child_reaped(t, r, st);
        else if (r < 0 && errno == ECHILD) t->exited = 1;
    }
    return (changed ? 1 : 0) | (t->exited ? 2 : 0);
}

void term_resize(Term *t, int cols, int rows)
{
    if (cols < 2) cols = 2;
    if (rows < 1) rows = 1;
    if (cols == t->cols && rows == t->rows) return;
    for (int g = 0; g < 2; g++) {
        TCell *n = calloc((size_t)cols * (size_t)rows, sizeof(TCell));
        if (!n) return;
        TCell b = blank_cell(t);
        fill_cells(n, cols * rows, b);
        /* keep the bottom of the screen when it gets shorter, so the cursor stays in view */
        int shift = (g == t->alt && t->cy >= rows) ? t->cy - rows + 1 : 0;
        if (g == 0 && shift > 0 && !t->alt)
            for (int y = 0; y < shift && y < t->rows; y++) push_scrollback(t, t->grid[0] + (size_t)y * (size_t)t->cols);
        for (int y = 0; y < rows; y++) {
            int sy = y + shift;
            if (sy >= t->rows) break;
            int w = cols < t->cols ? cols : t->cols;
            memcpy(n + (size_t)y * (size_t)cols, t->grid[g] + (size_t)sy * (size_t)t->cols, (size_t)w * sizeof(TCell));
        }
        free(t->grid[g]);
        t->grid[g] = n;
    }
    int shift = t->cy >= rows ? t->cy - rows + 1 : 0;
    t->cy -= shift;
    if (t->cx >= cols) t->cx = cols - 1;
    if (t->cy < 0) t->cy = 0;
    unsigned char *tb = realloc(t->tabs, (size_t)cols);
    if (tb) t->tabs = tb;
    t->cols = cols;
    t->rows = rows;
    t->top = 0;
    t->bot = rows - 1;
    t->wrap_pending = 0;
    t->view = 0;
    tabs_reset(t);
    if (t->fd >= 0) {
        struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
        ioctl(t->fd, TIOCSWINSZ, &ws);
    }
}

int term_cols(const Term *t) { return t->cols; }
int term_rows(const Term *t) { return t->rows; }
const char *term_title(const Term *t) { return t->title; }
int term_app_cursor(const Term *t) { return t->app_cursor; }
int term_alt_screen(const Term *t) { return t->alt; }
int term_scrolled(const Term *t) { return t->view; }

void term_scroll_view(Term *t, int lines)
{
    if (t->alt) return;
    t->view += lines;
    if (t->view < 0) t->view = 0;
    if (t->view > t->sb_n) t->view = t->sb_n;
}

void term_scroll_reset(Term *t) { t->view = 0; }

const TCell *term_cell(const Term *t, int x, int y)
{
    static const TCell empty = { ' ', TC_DEFAULT, TC_DEFAULT, 0 };
    if (x < 0 || y < 0 || x >= t->cols || y >= t->rows) return &empty;
    int line = t->sb_n - t->view + y; /* 0 .. sb_n - 1 are the history, then the screen */
    if (t->view > 0 && !t->alt && line < t->sb_n) {
        const Line *l = &t->sb[(t->sb_head + line) % SB_MAX];
        if (!l->c || x >= l->w) return &empty;
        return &l->c[x];
    }
    int sy = (t->view > 0 && !t->alt) ? line - t->sb_n : y;
    if (sy < 0 || sy >= t->rows) return &empty;
    return &t->grid[t->alt][(size_t)sy * (size_t)t->cols + (size_t)x];
}

int term_cursor(const Term *t, int *x, int *y)
{
    *x = t->cx;
    *y = t->cy;
    return t->cur_visible && t->view == 0;
}
