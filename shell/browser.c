/* A web browser engine for the shell: Chromium, run without a window and driven over its DevTools protocol.
 *
 * A worker thread starts Chromium, talks to it over a WebSocket, keeps the tab list, and keeps the newest
 * screencast picture. The shell asks for things through the br_* functions and draws what comes back. */
#define _GNU_SOURCE
#include "browser.h"

#include <SDL.h>
#include <SDL_image.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { C_NAV = 1, C_BACK, C_FWD, C_RELOAD, C_STOP, C_NEWTAB, C_CLOSE, C_SWITCH, C_MOVE, C_CLICK, C_WHEEL, C_KEY, C_VIEWPORT, C_CLEAR, C_PROBE, C_SETFIELD };
enum { P_NONE, P_ATTACH, P_HIST, P_PROBE, P_TITLE };

typedef struct {
    int type, a, b, c, d;
    char s[1100];
} Cmd;

#define QN 128
#define PN 64

static struct {
    pthread_mutex_t lock;
    pthread_t thread;
    int running, started;
    pid_t pid;
    int port, fd, W, H, css_w, mobile, next_id;
    char path[512], profile[512], ua[400], ua_mobile[400], wsurl[300];
    BrState st;
    struct {
        char tid[80], sess[80];
        int attached, shown, hist_n, hist_i;
    } tab[BR_MAX_TABS];
    int want_active; /* the next new tab should come to the front */
    unsigned char *rgba;
    int fw, fh, fcap;
    unsigned seq;
    Cmd q[QN];
    int qh, qt;
    struct {
        int id, kind, tab;
    } pend[PN];
    int field_ready, field_pw;
    char field_text[1024];
    unsigned char *rb; /* what's been read from the socket and not yet used */
    size_t rn, rcap;
    char *msg;
    size_t mn, mcap;
} B = { .lock = PTHREAD_MUTEX_INITIALIZER, .fd = -1 };

/* ---------- small helpers ------------------------------------------------- */

static const char *jfind(const char *s, const char *key)
{
    char pat[96];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(s, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ') p++;
    return p;
}

static int jstr_at(const char *p, char *out, size_t cap)
{
    if (!p || *p != '"') return 0;
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') { if (o + 1 < cap) out[o++] = ' '; }
            else if (*p == 't') { if (o + 1 < cap) out[o++] = ' '; }
            else if (*p == 'u' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4])) {
                unsigned cp = (unsigned)strtoul((char[]){ p[1], p[2], p[3], p[4], 0 }, NULL, 16);
                p += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && p[1] == '\\' && p[2] == 'u') { /* a pair */
                    unsigned lo = (unsigned)strtoul((char[]){ p[3], p[4], p[5], p[6], 0 }, NULL, 16);
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
                char u[5];
                int n = 0;
                if (cp < 0x80) u[n++] = (char)cp;
                else if (cp < 0x800) u[n++] = (char)(0xC0 | (cp >> 6)), u[n++] = (char)(0x80 | (cp & 63));
                else if (cp < 0x10000) u[n++] = (char)(0xE0 | (cp >> 12)), u[n++] = (char)(0x80 | ((cp >> 6) & 63)), u[n++] = (char)(0x80 | (cp & 63));
                else u[n++] = (char)(0xF0 | (cp >> 18)), u[n++] = (char)(0x80 | ((cp >> 12) & 63)), u[n++] = (char)(0x80 | ((cp >> 6) & 63)), u[n++] = (char)(0x80 | (cp & 63));
                if (o + (size_t)n < cap) {
                    memcpy(out + o, u, (size_t)n);
                    o += (size_t)n;
                }
            } else if (o + 1 < cap) out[o++] = *p;
            p++;
            continue;
        }
        if (o + 1 < cap) out[o++] = *p;
        p++;
    }
    out[o] = 0;
    return 1;
}

static int jstr(const char *s, const char *key, char *out, size_t cap)
{
    out[0] = 0;
    return jstr_at(jfind(s, key), out, cap);
}

/* the last "sessionId":"..." in a message, which is the one that says which tab it came from */
static int jsession(const char *s, char *out, size_t cap)
{
    const char *last = NULL, *p = s;
    while ((p = strstr(p, "\"sessionId\":\""))) {
        last = p + 12;
        p += 12;
    }
    out[0] = 0;
    return jstr_at(last, out, cap);
}

/* JSON-escape into a buffer; returns the length */
static size_t jesc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 8 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') out[o++] = '\\', out[o++] = (char)c;
        else if (c == '\n') out[o++] = '\\', out[o++] = 'n';
        else if (c == '\r') out[o++] = '\\', out[o++] = 'r';
        else if (c == '\t') out[o++] = '\\', out[o++] = 't';
        else if (c < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
    return o;
}

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_decode(const char *in, size_t n, unsigned char *out)
{
    static signed char rev[256];
    if (!rev['B']) {
        memset(rev, -1, sizeof rev);
        for (int i = 0; i < 64; i++) rev[(unsigned char)b64chars[i]] = (signed char)i;
        rev['B'] = 1;
    }
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int v = rev[(unsigned char)in[i]];
        if (v < 0) continue;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)(acc >> bits);
            acc &= (1u << bits) - 1;
        }
    }
    return o;
}

static void b64_encode(const unsigned char *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | (i + 1 < n ? (unsigned)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = b64chars[v >> 18];
        out[o++] = b64chars[(v >> 12) & 63];
        out[o++] = i + 1 < n ? b64chars[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? b64chars[v & 63] : '=';
    }
    out[o] = 0;
}

/* ---------- the socket ---------------------------------------------------- */

static int tcp_connect(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((unsigned short)port) };
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) {
        close(s);
        return -1;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return s;
}

static int send_all(int fd, const unsigned char *p, size_t n)
{
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* GET a small page from Chromium's debugging port */
static int http_get(int port, const char *path, char *out, size_t cap)
{
    int s = tcp_connect(port);
    if (s < 0) return -1;
    char req[300];
    snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n\r\n", path, port);
    if (send_all(s, (unsigned char *)req, strlen(req)) != 0) {
        close(s);
        return -1;
    }
    size_t n = 0;
    for (;;) {
        struct pollfd p = { s, POLLIN, 0 };
        if (poll(&p, 1, 3000) <= 0) break;
        ssize_t r = recv(s, out + n, cap - n - 1, 0);
        if (r <= 0) break;
        n += (size_t)r;
        if (n + 1 >= cap) break;
    }
    close(s);
    out[n] = 0;
    char *body = strstr(out, "\r\n\r\n");
    if (!body) return -1;
    memmove(out, body + 4, strlen(body + 4) + 1);
    return 0;
}

static int ws_open(int port, const char *path)
{
    int s = tcp_connect(port);
    if (s < 0) return -1;
    unsigned char key[16];
    for (int i = 0; i < 16; i++) key[i] = (unsigned char)(rand() & 255);
    char k64[40], req[600];
    b64_encode(key, 16, k64);
    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
             path, port, k64);
    if (send_all(s, (unsigned char *)req, strlen(req)) != 0) {
        close(s);
        return -1;
    }
    char resp[1024];
    size_t n = 0;
    while (n < sizeof resp - 1) {
        struct pollfd p = { s, POLLIN, 0 };
        if (poll(&p, 1, 4000) <= 0) break;
        ssize_t r = recv(s, resp + n, 1, 0); /* one byte at a time, so nothing after the header is lost */
        if (r <= 0) break;
        n += (size_t)r;
        resp[n] = 0;
        if (n >= 4 && !memcmp(resp + n - 4, "\r\n\r\n", 4)) break;
    }
    if (!strstr(resp, " 101")) {
        close(s);
        return -1;
    }
    return s;
}

static int ws_send(const char *text)
{
    size_t n = strlen(text);
    unsigned char *f = malloc(n + 14);
    if (!f) return -1;
    size_t o = 0;
    f[o++] = 0x81;
    if (n < 126) f[o++] = (unsigned char)(0x80 | n);
    else if (n < 65536) f[o++] = 0x80 | 126, f[o++] = (unsigned char)(n >> 8), f[o++] = (unsigned char)n;
    else {
        f[o++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) f[o++] = (unsigned char)((uint64_t)n >> (8 * i));
    }
    unsigned char mask[4] = { (unsigned char)rand(), (unsigned char)rand(), (unsigned char)rand(), (unsigned char)rand() };
    memcpy(f + o, mask, 4);
    o += 4;
    for (size_t i = 0; i < n; i++) f[o + i] = (unsigned char)text[i] ^ mask[i & 3];
    int rc = send_all(B.fd, f, o + n);
    free(f);
    return rc;
}

static void ws_pong(const unsigned char *p, size_t n)
{
    unsigned char f[200];
    if (n > 100) n = 100;
    unsigned char mask[4] = { 1, 2, 3, 4 };
    f[0] = 0x8A;
    f[1] = (unsigned char)(0x80 | n);
    memcpy(f + 2, mask, 4);
    for (size_t i = 0; i < n; i++) f[6 + i] = p[i] ^ mask[i & 3];
    send_all(B.fd, f, 6 + n);
}

/* ---------- commands out -------------------------------------------------- */

static int pend_add(int kind, int tab)
{
    int id = ++B.next_id;
    if (kind != P_NONE) {
        for (int i = 0; i < PN; i++)
            if (!B.pend[i].id) {
                B.pend[i].id = id;
                B.pend[i].kind = kind;
                B.pend[i].tab = tab;
                break;
            }
    }
    return id;
}

static void cdp(const char *session, const char *method, int kind, int tab, const char *fmt, ...)
{
    char params[3000];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(params, sizeof params, fmt ? fmt : "{}", ap);
    va_end(ap);
    char *msg = malloc(strlen(params) + 400);
    if (!msg) return;
    int id = pend_add(kind, tab);
    if (session && *session) sprintf(msg, "{\"id\":%d,\"method\":\"%s\",\"params\":%s,\"sessionId\":\"%s\"}", id, method, params, session);
    else sprintf(msg, "{\"id\":%d,\"method\":\"%s\",\"params\":%s}", id, method, params);
    ws_send(msg);
    free(msg);
}

static const char *active_session(void)
{
    int a = B.st.active;
    return a >= 0 && a < B.st.ntabs && B.tab[a].attached ? B.tab[a].sess : "";
}

static void apply_view(const char *sess)
{
    int ch = B.css_w * B.H / (B.W ? B.W : 1);
    double dsf = (double)B.W / (double)B.css_w;
    cdp(sess, "Emulation.setDeviceMetricsOverride", P_NONE, 0, "{\"width\":%d,\"height\":%d,\"deviceScaleFactor\":%.4f,\"mobile\":%s}", B.css_w, ch, dsf,
        B.mobile ? "true" : "false");
    char ua[800];
    jesc(B.mobile ? B.ua_mobile : B.ua, ua, sizeof ua);
    cdp(sess, "Emulation.setUserAgentOverride", P_NONE, 0, "{\"userAgent\":\"%s\"}", ua);
}

static void start_cast(int i)
{
    if (i < 0 || i >= B.st.ntabs || !B.tab[i].attached) return;
    cdp(B.tab[i].sess, "Page.startScreencast", P_NONE, 0, "{\"format\":\"jpeg\",\"quality\":60,\"maxWidth\":%d,\"maxHeight\":%d,\"everyNthFrame\":1}", B.W, B.H);
    B.tab[i].shown = 1;
}

static void stop_cast(int i)
{
    if (i < 0 || i >= B.st.ntabs || !B.tab[i].attached || !B.tab[i].shown) return;
    cdp(B.tab[i].sess, "Page.stopScreencast", P_NONE, 0, "{}");
    B.tab[i].shown = 0;
}

static void switch_to(int i)
{
    if (i < 0 || i >= B.st.ntabs) return;
    pthread_mutex_lock(&B.lock);
    int old = B.st.active;
    B.st.active = i;
    B.st.can_back = B.tab[i].hist_i > 0;
    B.st.can_forward = B.tab[i].hist_i + 1 < B.tab[i].hist_n;
    pthread_mutex_unlock(&B.lock);
    if (old != i) stop_cast(old);
    cdp(NULL, "Target.activateTarget", P_NONE, 0, "{\"targetId\":\"%s\"}", B.tab[i].tid);
    start_cast(i);
}

static int tab_by_target(const char *tid)
{
    for (int i = 0; i < B.st.ntabs; i++)
        if (!strcmp(B.tab[i].tid, tid)) return i;
    return -1;
}

static int tab_by_session(const char *sess)
{
    for (int i = 0; i < B.st.ntabs; i++)
        if (B.tab[i].attached && !strcmp(B.tab[i].sess, sess)) return i;
    return -1;
}

static void run_cmd(const Cmd *c)
{
    const char *s = active_session();
    char esc[2400];
    switch (c->type) {
    case C_NAV:
        jesc(c->s, esc, sizeof esc);
        if (*s) cdp(s, "Page.navigate", P_NONE, 0, "{\"url\":\"%s\"}", esc);
        break;
    case C_BACK: if (*s) cdp(s, "Runtime.evaluate", P_NONE, 0, "{\"expression\":\"history.back()\"}"); break;
    case C_FWD: if (*s) cdp(s, "Runtime.evaluate", P_NONE, 0, "{\"expression\":\"history.forward()\"}"); break;
    case C_RELOAD: if (*s) cdp(s, "Page.reload", P_NONE, 0, "{}"); break;
    case C_STOP: if (*s) cdp(s, "Page.stopLoading", P_NONE, 0, "{}"); break;
    case C_NEWTAB:
        jesc(c->s, esc, sizeof esc);
        B.want_active = 1;
        cdp(NULL, "Target.createTarget", P_NONE, 0, "{\"url\":\"%s\"}", esc);
        break;
    case C_CLOSE:
        if (c->a >= 0 && c->a < B.st.ntabs) cdp(NULL, "Target.closeTarget", P_NONE, 0, "{\"targetId\":\"%s\"}", B.tab[c->a].tid);
        break;
    case C_SWITCH: switch_to(c->a); break;
    case C_MOVE:
        if (*s) cdp(s, "Input.dispatchMouseEvent", P_NONE, 0, "{\"type\":\"mouseMoved\",\"x\":%d,\"y\":%d}", c->a * B.css_w / B.W, c->b * B.css_w / B.W);
        break;
    case C_CLICK:
        if (*s) {
            int x = c->a * B.css_w / B.W, y = c->b * B.css_w / B.W;
            cdp(s, "Input.dispatchMouseEvent", P_NONE, 0, "{\"type\":\"mouseMoved\",\"x\":%d,\"y\":%d}", x, y);
            cdp(s, "Input.dispatchMouseEvent", P_NONE, 0, "{\"type\":\"mousePressed\",\"x\":%d,\"y\":%d,\"button\":\"left\",\"buttons\":1,\"clickCount\":1}", x, y);
            cdp(s, "Input.dispatchMouseEvent", P_NONE, 0, "{\"type\":\"mouseReleased\",\"x\":%d,\"y\":%d,\"button\":\"left\",\"buttons\":0,\"clickCount\":1}", x, y);
        }
        break;
    case C_WHEEL:
        if (*s)
            cdp(s, "Input.dispatchMouseEvent", P_NONE, 0, "{\"type\":\"mouseWheel\",\"x\":%d,\"y\":%d,\"deltaX\":%d,\"deltaY\":%d}", c->a * B.css_w / B.W,
                c->b * B.css_w / B.W, c->c, c->d);
        break;
    case C_KEY:
        if (*s) {
            const char *k = c->s;
            int vk = !strcmp(k, "Enter") ? 13 : !strcmp(k, "Backspace") ? 8 : !strcmp(k, "Escape") ? 27 : !strcmp(k, "Tab") ? 9 : 0;
            const char *text = !strcmp(k, "Enter") ? ",\"text\":\"\\r\"" : "";
            cdp(s, "Input.dispatchKeyEvent", P_NONE, 0, "{\"type\":\"%s\",\"key\":\"%s\",\"code\":\"%s\",\"windowsVirtualKeyCode\":%d%s}", text[0] ? "keyDown" : "rawKeyDown", k, k, vk, text);
            cdp(s, "Input.dispatchKeyEvent", P_NONE, 0, "{\"type\":\"keyUp\",\"key\":\"%s\",\"code\":\"%s\",\"windowsVirtualKeyCode\":%d}", k, k, vk);
        }
        break;
    case C_VIEWPORT:
        B.css_w = c->a < 320 ? 320 : c->a > 2560 ? 2560 : c->a;
        B.mobile = c->b;
        for (int i = 0; i < B.st.ntabs; i++)
            if (B.tab[i].attached) apply_view(B.tab[i].sess);
        break;
    case C_CLEAR:
        if (*s) {
            cdp(s, "Network.clearBrowserCookies", P_NONE, 0, "{}");
            cdp(s, "Network.clearBrowserCache", P_NONE, 0, "{}");
        }
        break;
    case C_PROBE:
        if (*s)
            cdp(s, "Runtime.evaluate", P_PROBE, 0,
                "{\"returnByValue\":true,\"expression\":\"(function(){var e=document.activeElement;if(!e)return '';var t=e.tagName;"
                "if(t=='TEXTAREA'||(t=='INPUT'&&!/^(button|submit|checkbox|radio|image|reset|file|range|color)$/i.test(e.type))||e.isContentEditable)"
                "return 'T'+(e.type=='password'?'p':'t')+(e.isContentEditable?e.textContent:(e.value||''));return '';})()\"}");
        break;
    case C_SETFIELD:
        jesc(c->s, esc, sizeof esc);
        if (*s)
            cdp(s, "Runtime.evaluate", P_NONE, 0,
                "{\"expression\":\"(function(v){var e=document.activeElement;if(!e)return;if(e.isContentEditable){e.textContent=v}else{"
                "var p=e.tagName=='TEXTAREA'?HTMLTextAreaElement.prototype:HTMLInputElement.prototype;var d=Object.getOwnPropertyDescriptor(p,'value');"
                "if(d&&d.set)d.set.call(e,v);else e.value=v;}e.dispatchEvent(new Event('input',{bubbles:true}));"
                "e.dispatchEvent(new Event('change',{bubbles:true}));})(\\\"%s\\\")\"}",
                esc);
        break;
    }
}

static void enqueue(int type, int a, int b, int c, int d, const char *s)
{
    pthread_mutex_lock(&B.lock);
    int next = (B.qt + 1) % QN;
    if (next != B.qh) {
        Cmd *q = &B.q[B.qt];
        memset(q, 0, sizeof *q);
        q->type = type;
        q->a = a, q->b = b, q->c = c, q->d = d;
        if (s) snprintf(q->s, sizeof q->s, "%s", s);
        B.qt = next;
    }
    pthread_mutex_unlock(&B.lock);
}

/* ---------- messages in --------------------------------------------------- */

static void tab_remove(int i)
{
    pthread_mutex_lock(&B.lock);
    for (int k = i; k + 1 < B.st.ntabs; k++) {
        B.tab[k] = B.tab[k + 1];
        B.st.tabs[k] = B.st.tabs[k + 1];
    }
    B.st.ntabs--;
    int was_active = B.st.active == i;
    if (B.st.active > i) B.st.active--;
    if (B.st.active >= B.st.ntabs) B.st.active = B.st.ntabs - 1;
    pthread_mutex_unlock(&B.lock);
    if (B.st.ntabs == 0) {
        B.want_active = 1;
        cdp(NULL, "Target.createTarget", P_NONE, 0, "{\"url\":\"about:blank\"}");
    } else if (was_active) {
        int a = B.st.active;
        B.tab[a].shown = 0;
        switch_to(a);
    }
}

static void handle_frame(const char *msg, const char *sess)
{
    const char *d = jfind(msg, "data");
    if (!d || *d != '"') return;
    d++;
    const char *e = strchr(d, '"');
    if (!e) return;
    unsigned char *jpg = malloc((size_t)(e - d) / 4 * 3 + 8);
    if (!jpg) return;
    size_t jn = b64_decode(d, (size_t)(e - d), jpg);
    /* the acknowledgment number is the numeric sessionId in params */
    const char *p = msg;
    long ack = -1;
    while ((p = strstr(p, "\"sessionId\":"))) {
        p += 12;
        if (isdigit((unsigned char)*p)) ack = atol(p);
    }
    int ti = tab_by_session(sess);
    if (ack >= 0 && sess[0]) cdp(sess, "Page.screencastFrameAck", P_NONE, 0, "{\"sessionId\":%ld}", ack);
    if (ti == B.st.active) {
        SDL_Surface *s0 = IMG_Load_RW(SDL_RWFromConstMem(jpg, (int)jn), 1);
        if (s0) {
            SDL_Surface *s1 = SDL_ConvertSurfaceFormat(s0, SDL_PIXELFORMAT_RGBA32, 0);
            SDL_FreeSurface(s0);
            if (s1) {
                size_t need = (size_t)s1->w * (size_t)s1->h * 4;
                pthread_mutex_lock(&B.lock);
                if ((size_t)B.fcap < need) {
                    free(B.rgba);
                    B.rgba = malloc(need);
                    B.fcap = B.rgba ? (int)need : 0;
                }
                if (B.rgba) {
                    for (int y = 0; y < s1->h; y++) memcpy(B.rgba + (size_t)y * (size_t)s1->w * 4, (unsigned char *)s1->pixels + (size_t)y * (size_t)s1->pitch, (size_t)s1->w * 4);
                    B.fw = s1->w, B.fh = s1->h;
                    B.seq++;
                }
                pthread_mutex_unlock(&B.lock);
                SDL_FreeSurface(s1);
            }
        }
    }
    free(jpg);
}

static void handle_message(char *msg)
{
    if (!strncmp(msg, "{\"id\":", 6)) {
        int id = atoi(msg + 6);
        for (int i = 0; i < PN; i++) {
            if (B.pend[i].id != id) continue;
            int kind = B.pend[i].kind, tab = B.pend[i].tab;
            B.pend[i].id = 0;
            if (kind == P_ATTACH && tab >= 0 && tab < B.st.ntabs) {
                char sess[80];
                if (jstr(msg, "sessionId", sess, sizeof sess)) {
                    snprintf(B.tab[tab].sess, sizeof B.tab[tab].sess, "%s", sess);
                    B.tab[tab].attached = 1;
                    cdp(sess, "Page.enable", P_NONE, 0, "{}");
                    apply_view(sess);
                    if (tab == B.st.active) start_cast(tab);
                }
            } else if (kind == P_HIST && tab >= 0 && tab < B.st.ntabs) {
                const char *ci = jfind(msg, "currentIndex");
                int n = 0;
                for (const char *p = msg; (p = strstr(p, "\"userTypedURL\"")); p++) n++;
                pthread_mutex_lock(&B.lock);
                B.tab[tab].hist_i = ci ? atoi(ci) : 0;
                B.tab[tab].hist_n = n;
                if (tab == B.st.active) {
                    B.st.can_back = B.tab[tab].hist_i > 0;
                    B.st.can_forward = B.tab[tab].hist_i + 1 < n;
                }
                pthread_mutex_unlock(&B.lock);
            } else if (kind == P_TITLE && tab >= 0 && tab < B.st.ntabs) {
                char v[200];
                if (jstr(msg, "value", v, sizeof v) && v[0]) {
                    pthread_mutex_lock(&B.lock);
                    snprintf(B.st.tabs[tab].title, sizeof B.st.tabs[tab].title, "%s", v);
                    pthread_mutex_unlock(&B.lock);
                }
            } else if (kind == P_PROBE) {
                char v[1100];
                if (jstr(msg, "value", v, sizeof v) && v[0] == 'T') {
                    pthread_mutex_lock(&B.lock);
                    B.field_ready = 1;
                    B.field_pw = v[1] == 'p';
                    snprintf(B.field_text, sizeof B.field_text, "%s", v + 2);
                    pthread_mutex_unlock(&B.lock);
                }
            }
            return;
        }
        return;
    }
    char method[80];
    if (!jstr(msg, "method", method, sizeof method)) return;
    char sess[80];
    jsession(msg, sess, sizeof sess);

    if (!strcmp(method, "Page.screencastFrame")) {
        handle_frame(msg, sess);
    } else if (!strcmp(method, "Target.targetCreated") || !strcmp(method, "Target.targetInfoChanged")) {
        char type[24], tid[80], title[160], url[600];
        jstr(msg, "type", type, sizeof type);
        if (strcmp(type, "page") != 0) return;
        jstr(msg, "targetId", tid, sizeof tid);
        jstr(msg, "title", title, sizeof title);
        jstr(msg, "url", url, sizeof url);
        int i = tab_by_target(tid);
        if (i < 0) {
            if (B.st.ntabs >= BR_MAX_TABS) {
                cdp(NULL, "Target.closeTarget", P_NONE, 0, "{\"targetId\":\"%s\"}", tid);
                return;
            }
            pthread_mutex_lock(&B.lock);
            i = B.st.ntabs++;
            memset(&B.tab[i], 0, sizeof B.tab[i]);
            memset(&B.st.tabs[i], 0, sizeof B.st.tabs[i]);
            snprintf(B.tab[i].tid, sizeof B.tab[i].tid, "%s", tid);
            pthread_mutex_unlock(&B.lock);
            int front = B.want_active || B.st.ntabs == 1 || strstr(msg, "\"openerId\"");
            B.want_active = 0;
            if (front) {
                int old = B.st.active;
                if (B.st.ntabs > 1) stop_cast(old);
                B.st.active = i;
            }
            cdp(NULL, "Target.attachToTarget", P_ATTACH, i, "{\"targetId\":\"%s\",\"flatten\":true}", tid);
            if (front && B.st.ntabs > 1) cdp(NULL, "Target.activateTarget", P_NONE, 0, "{\"targetId\":\"%s\"}", tid);
        }
        pthread_mutex_lock(&B.lock);
        snprintf(B.st.tabs[i].title, sizeof B.st.tabs[i].title, "%s", title);
        snprintf(B.st.tabs[i].url, sizeof B.st.tabs[i].url, "%s", url);
        pthread_mutex_unlock(&B.lock);
    } else if (!strcmp(method, "Target.targetDestroyed")) {
        char tid[80];
        jstr(msg, "targetId", tid, sizeof tid);
        int i = tab_by_target(tid);
        if (i >= 0) tab_remove(i);
    } else if (!strcmp(method, "Page.frameStartedLoading") || !strcmp(method, "Page.frameStoppedLoading")) {
        int i = tab_by_session(sess);
        if (i >= 0) {
            int on = method[11] == 'S' && method[12] == 't' && method[13] == 'a';
            B.st.tabs[i].loading = on;
            if (!on) {
                cdp(sess, "Page.getNavigationHistory", P_HIST, i, "{}");
                cdp(sess, "Runtime.evaluate", P_TITLE, i, "{\"returnByValue\":true,\"expression\":\"document.title\"}");
            }
        }
    } else if (!strcmp(method, "Page.frameNavigated")) {
        int i = tab_by_session(sess);
        if (i >= 0) cdp(sess, "Page.getNavigationHistory", P_HIST, i, "{}");
    } else if (!strcmp(method, "Page.javascriptDialogOpening")) {
        cdp(sess, "Page.handleJavaScriptDialog", P_NONE, 0, "{\"accept\":true}");
    }
}

/* ---------- reading frames ------------------------------------------------ */

static int rb_fill(int ms)
{
    struct pollfd p = { B.fd, POLLIN, 0 };
    int r = poll(&p, 1, ms);
    if (r <= 0) return r;
    if (B.rcap - B.rn < 65536) {
        B.rcap = B.rcap ? B.rcap * 2 : 262144;
        B.rb = realloc(B.rb, B.rcap);
        if (!B.rb) return -1;
    }
    ssize_t n = recv(B.fd, B.rb + B.rn, B.rcap - B.rn, 0);
    if (n <= 0) return -1;
    B.rn += (size_t)n;
    return 1;
}

/* take every complete WebSocket frame that's waiting; -1 if the connection is gone */
static int ws_process(void)
{
    for (;;) {
        if (B.rn < 2) return 0;
        unsigned char b0 = B.rb[0], b1 = B.rb[1];
        size_t hl = 2, len = b1 & 127;
        if (len == 126) {
            if (B.rn < 4) return 0;
            len = (size_t)B.rb[2] << 8 | B.rb[3];
            hl = 4;
        } else if (len == 127) {
            if (B.rn < 10) return 0;
            len = 0;
            for (int i = 0; i < 8; i++) len = len << 8 | B.rb[2 + i];
            hl = 10;
        }
        if (len > (64u << 20)) return -1;
        if (B.rn < hl + len) return 0;
        int op = b0 & 15, fin = b0 >> 7;
        if (op == 8) return -1;
        if (op == 9) ws_pong(B.rb + hl, len);
        if (op == 1 || op == 0) {
            if (op == 1) B.mn = 0;
            if (B.mcap < B.mn + len + 1) {
                B.mcap = (B.mn + len + 1) * 2;
                B.msg = realloc(B.msg, B.mcap);
                if (!B.msg) return -1;
            }
            memcpy(B.msg + B.mn, B.rb + hl, len);
            B.mn += len;
            B.msg[B.mn] = 0;
            if (fin) handle_message(B.msg);
        }
        memmove(B.rb, B.rb + hl + len, B.rn - hl - len);
        B.rn -= hl + len;
    }
}

/* ---------- the worker ---------------------------------------------------- */

static void fail(const char *why)
{
    pthread_mutex_lock(&B.lock);
    B.st.state = 3;
    snprintf(B.st.error, sizeof B.st.error, "%s", why);
    pthread_mutex_unlock(&B.lock);
}

static void *worker(void *arg)
{
    (void)arg;
    char buf[8192];
    int up = 0;
    for (int i = 0; i < 300 && B.running; i++) { /* up to 30 s for Chromium to open its port */
        if (http_get(B.port, "/json/version", buf, sizeof buf) == 0 && strstr(buf, "webSocketDebuggerUrl")) {
            up = 1;
            break;
        }
        int st;
        if (waitpid(B.pid, &st, WNOHANG) == B.pid) {
            B.pid = 0;
            fail("The browser stopped as it was starting.");
            return NULL;
        }
        usleep(100000);
    }
    if (!up) {
        fail("The browser didn't start in time.");
        return NULL;
    }
    char ws[300], ver[160];
    jstr(buf, "webSocketDebuggerUrl", ws, sizeof ws);
    jstr(buf, "User-Agent", B.ua, sizeof B.ua);
    jstr(buf, "Browser", ver, sizeof ver);
    char *h = strstr(B.ua, "HeadlessChrome");
    if (h) {
        memmove(h + 6, h + 14, strlen(h + 14) + 1);
        memcpy(h, "Chrome", 6);
    }
    const char *major = strchr(ver, '/');
    snprintf(B.ua_mobile, sizeof B.ua_mobile, "Mozilla/5.0 (Linux; Android 13; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/%s Mobile Safari/537.36",
             major ? major + 1 : "120.0.0.0");
    const char *path = strstr(ws, "/devtools/");
    if (!path) {
        fail("The browser gave an unexpected address.");
        return NULL;
    }
    B.fd = ws_open(B.port, path);
    if (B.fd < 0) {
        fail("Couldn't connect to the browser.");
        return NULL;
    }
    cdp(NULL, "Browser.setDownloadBehavior", P_NONE, 0, "{\"behavior\":\"deny\"}");
    cdp(NULL, "Target.setDiscoverTargets", P_NONE, 0, "{\"discover\":true}");
    pthread_mutex_lock(&B.lock);
    B.st.state = 2;
    pthread_mutex_unlock(&B.lock);

    while (B.running) {
        int r = rb_fill(15);
        if (r < 0 || ws_process() < 0) {
            fail("The browser closed.");
            break;
        }
        for (;;) {
            Cmd c;
            pthread_mutex_lock(&B.lock);
            int have = B.qh != B.qt;
            if (have) {
                c = B.q[B.qh];
                B.qh = (B.qh + 1) % QN;
            }
            pthread_mutex_unlock(&B.lock);
            if (!have) break;
            run_cmd(&c);
        }
    }
    close(B.fd);
    B.fd = -1;
    return NULL;
}

/* ---------- the public side ----------------------------------------------- */

int br_find(const char *configured, char *path, size_t cap)
{
    const char *names[] = { configured && *configured ? configured : "", "chromium", "chromium-browser", "google-chrome", "google-chrome-stable" };
    const char *pathenv = getenv("PATH");
    for (size_t n = 0; n < sizeof names / sizeof *names; n++) {
        if (!names[n][0]) continue;
        if (strchr(names[n], '/')) {
            if (access(names[n], X_OK) == 0) {
                snprintf(path, cap, "%s", names[n]);
                return 0;
            }
            continue;
        }
        char *dup = strdup(pathenv ? pathenv : "/usr/bin:/bin");
        for (char *save = NULL, *d = strtok_r(dup, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
            char p[600];
            snprintf(p, sizeof p, "%s/%s", d, names[n]);
            if (access(p, X_OK) == 0) {
                snprintf(path, cap, "%s", p);
                free(dup);
                return 0;
            }
        }
        free(dup);
    }
    return -1;
}

int br_start(const char *path, const char *profile_dir, int width, int height)
{
    if (B.started) return 0;
    memset(&B.st, 0, sizeof B.st);
    memset(B.pend, 0, sizeof B.pend);
    B.qh = B.qt = 0;
    B.field_ready = 0;
    B.rn = 0;
    B.W = width > 1280 ? 1280 : width < 320 ? 320 : width;
    B.H = height * B.W / (width ? width : 1);
    B.css_w = 1024;
    B.mobile = 0;
    B.st.state = 1;
    B.st.active = 0;
    snprintf(B.path, sizeof B.path, "%s", path);
    snprintf(B.profile, sizeof B.profile, "%s", profile_dir);
    mkdir(profile_dir, 0700);
    IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG);
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    B.port = 9222 + (int)(getpid() % 700);
    char port[48], dir[700], size[48];
    snprintf(port, sizeof port, "--remote-debugging-port=%d", B.port);
    snprintf(dir, sizeof dir, "--user-data-dir=%s", profile_dir);
    snprintf(size, sizeof size, "--window-size=%d,%d", B.W, B.H);
    pid_t pid = fork();
    if (pid < 0) {
        fail("Couldn't start the browser.");
        return -1;
    }
    if (pid == 0) {
        setsid();
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) {
            dup2(dn, 0);
            dup2(dn, 1);
            dup2(dn, 2);
        }
        char *args[24];
        int n = 0;
        args[n++] = (char *)path;
        args[n++] = "--headless=new";
        args[n++] = port;
        args[n++] = dir;
        args[n++] = size;
        args[n++] = "--no-first-run";
        args[n++] = "--no-default-browser-check";
        args[n++] = "--disable-dev-shm-usage";
        args[n++] = "--disable-background-networking";
        args[n++] = "--disable-sync";
        args[n++] = "--disable-features=Translate,MediaRouter";
        args[n++] = "--password-store=basic";
        args[n++] = "--hide-scrollbars";
        if (geteuid() == 0) args[n++] = "--no-sandbox";
        args[n++] = "about:blank";
        args[n] = NULL;
        execvp(path, args);
        _exit(127);
    }
    B.pid = pid;
    B.running = 1;
    B.started = 1;
    if (pthread_create(&B.thread, NULL, worker, NULL) != 0) {
        B.running = 0;
        B.started = 0;
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        fail("Couldn't start the browser.");
        return -1;
    }
    return 0;
}

void br_stop(void)
{
    if (!B.started) return;
    B.running = 0;
    pthread_join(B.thread, NULL);
    if (B.pid > 0) {
        kill(-B.pid, SIGTERM);
        kill(B.pid, SIGTERM);
        for (int i = 0; i < 30; i++) {
            if (waitpid(B.pid, NULL, WNOHANG) == B.pid) {
                B.pid = 0;
                break;
            }
            usleep(100000);
        }
        if (B.pid > 0) {
            kill(-B.pid, SIGKILL);
            kill(B.pid, SIGKILL);
            waitpid(B.pid, NULL, 0);
        }
        B.pid = 0;
    }
    B.started = 0;
    pthread_mutex_lock(&B.lock);
    memset(&B.st, 0, sizeof B.st);
    B.seq = 0;
    B.fw = B.fh = 0;
    pthread_mutex_unlock(&B.lock);
}

void br_get(BrState *out)
{
    pthread_mutex_lock(&B.lock);
    *out = B.st;
    pthread_mutex_unlock(&B.lock);
}

int br_frame_lock(const unsigned char **rgba, int *w, int *h, unsigned *seq)
{
    pthread_mutex_lock(&B.lock);
    if (!B.rgba || !B.fw) {
        pthread_mutex_unlock(&B.lock);
        return 0;
    }
    *rgba = B.rgba, *w = B.fw, *h = B.fh, *seq = B.seq;
    return 1;
}

void br_frame_unlock(void) { pthread_mutex_unlock(&B.lock); }

/* what was typed in the address bar -> a page address */
static void normalize_url(const char *in, char *out, size_t cap)
{
    while (*in == ' ') in++;
    if (!*in) {
        snprintf(out, cap, "about:blank");
        return;
    }
    if (strstr(in, "://") || !strncmp(in, "about:", 6) || !strncmp(in, "data:", 5)) {
        snprintf(out, cap, "%s", in);
        return;
    }
    int has_space = strchr(in, ' ') != NULL, has_dot = strchr(in, '.') != NULL;
    if (!has_space && (has_dot || !strncmp(in, "localhost", 9))) {
        snprintf(out, cap, "https://%s", in);
        return;
    }
    size_t o = (size_t)snprintf(out, cap, "https://duckduckgo.com/?q=");
    for (; *in && o + 4 < cap; in++) {
        if (isalnum((unsigned char)*in) || *in == '-' || *in == '_' || *in == '.' || *in == '~') out[o++] = *in;
        else if (*in == ' ') out[o++] = '+';
        else o += (size_t)snprintf(out + o, cap - o, "%%%02X", (unsigned char)*in);
    }
    out[o] = 0;
}

void br_navigate(const char *url)
{
    char u[1100];
    normalize_url(url, u, sizeof u);
    enqueue(C_NAV, 0, 0, 0, 0, u);
}
void br_back(void) { enqueue(C_BACK, 0, 0, 0, 0, NULL); }
void br_forward(void) { enqueue(C_FWD, 0, 0, 0, 0, NULL); }
void br_reload(void) { enqueue(C_RELOAD, 0, 0, 0, 0, NULL); }
void br_stop_loading(void) { enqueue(C_STOP, 0, 0, 0, 0, NULL); }
void br_new_tab(const char *url)
{
    char u[1100];
    normalize_url(url && *url ? url : "about:blank", u, sizeof u);
    enqueue(C_NEWTAB, 0, 0, 0, 0, u);
}
void br_close_tab(int index) { enqueue(C_CLOSE, index, 0, 0, 0, NULL); }
void br_switch_tab(int index) { enqueue(C_SWITCH, index, 0, 0, 0, NULL); }
void br_mouse_move(int x, int y) { enqueue(C_MOVE, x, y, 0, 0, NULL); }
void br_click(int x, int y) { enqueue(C_CLICK, x, y, 0, 0, NULL); }
void br_wheel(int x, int y, int dx, int dy) { enqueue(C_WHEEL, x, y, dx, dy, NULL); }
void br_key(const char *name) { enqueue(C_KEY, 0, 0, 0, 0, name); }
void br_set_viewport(int css_width, int mobile) { enqueue(C_VIEWPORT, css_width, mobile, 0, 0, NULL); }
void br_clear_data(void) { enqueue(C_CLEAR, 0, 0, 0, 0, NULL); }
void br_probe_field(void) { enqueue(C_PROBE, 0, 0, 0, 0, NULL); }
void br_set_field(const char *text) { enqueue(C_SETFIELD, 0, 0, 0, 0, text); }

int br_field(int *password, char *text, size_t cap)
{
    pthread_mutex_lock(&B.lock);
    int r = B.field_ready;
    if (r) {
        *password = B.field_pw;
        snprintf(text, cap, "%s", B.field_text);
        B.field_ready = 0;
    }
    pthread_mutex_unlock(&B.lock);
    return r;
}
