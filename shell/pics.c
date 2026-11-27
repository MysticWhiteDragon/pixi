/* Picture decoding worker: SDL_image does the reading, this scales the picture
 * down (with an area average, so thumbnails stay smooth) and turns it upright
 * from its EXIF orientation. */
#include "pics.h"

#include <SDL.h>
#include <SDL_image.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_QUEUE 256
#define MAX_DONE 64
#define MAX_PIXELS (80L * 1000 * 1000)

typedef struct {
    char path[1024];
    int max_dim;
    unsigned stamp;
} Req;

typedef struct {
    char path[1024];
    int max_dim, w, h;
    unsigned char *rgba;
} Done;

static SDL_Thread *thread;
static SDL_mutex *lock;
static SDL_cond *wake;
static int quit;
static Req queue[MAX_QUEUE];
static int nqueue;
static Done done[MAX_DONE];
static int ndone;
static unsigned stamp_counter;

/* ---------- EXIF orientation -------------------------------------------- */

static unsigned rd16(const unsigned char *p, int le) { return le ? (unsigned)(p[0] | p[1] << 8) : (unsigned)(p[0] << 8 | p[1]); }
static unsigned rd32(const unsigned char *p, int le)
{
    return le ? (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24
              : (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | (unsigned)p[3];
}

static int exif_orientation(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 1;
    unsigned char buf[65536];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < 12 || buf[0] != 0xFF || buf[1] != 0xD8) return 1;
    size_t i = 2;
    while (i + 4 <= n) {
        if (buf[i] != 0xFF) return 1;
        unsigned marker = buf[i + 1];
        if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        if (marker == 0xDA || marker == 0xD9) return 1;
        size_t len = (size_t)rd16(buf + i + 2, 0);
        if (len < 2) return 1;
        if (marker == 0xE1 && i + 4 + 14 <= n && !memcmp(buf + i + 4, "Exif\0\0", 6)) {
            const unsigned char *t = buf + i + 10;
            size_t tmax = n - (i + 10);
            if (tmax < 8) return 1;
            int le = t[0] == 'I';
            if (!le && t[0] != 'M') return 1;
            size_t ifd = rd32(t + 4, le);
            if (ifd + 2 > tmax) return 1;
            unsigned count = rd16(t + ifd, le);
            for (unsigned k = 0; k < count; k++) {
                size_t e = ifd + 2 + (size_t)k * 12;
                if (e + 12 > tmax) return 1;
                if (rd16(t + e, le) == 0x0112) {
                    unsigned v = rd16(t + e + 8, le);
                    return v >= 1 && v <= 8 ? (int)v : 1;
                }
            }
            return 1;
        }
        i += 2 + len;
    }
    return 1;
}

/* ---------- scaling and turning ----------------------------------------- */

static unsigned char *box_scale(const unsigned char *src, int sw, int sh, int dw, int dh)
{
    unsigned char *out = malloc((size_t)dw * (size_t)dh * 4);
    if (!out) return NULL;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((int64_t)y * sh / dh), y1 = (int)(((int64_t)(y + 1) * sh + dh - 1) / dh);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > sh) y1 = sh;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((int64_t)x * sw / dw), x1 = (int)(((int64_t)(x + 1) * sw + dw - 1) / dw);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > sw) x1 = sw;
            unsigned long r = 0, g = 0, b = 0, a = 0, cnt = 0;
            for (int sy = y0; sy < y1; sy++) {
                const unsigned char *p = src + ((size_t)sy * (size_t)sw + (size_t)x0) * 4;
                for (int sx = x0; sx < x1; sx++, p += 4) {
                    r += p[0], g += p[1], b += p[2], a += p[3];
                    cnt++;
                }
            }
            unsigned char *o = out + ((size_t)y * (size_t)dw + (size_t)x) * 4;
            o[0] = (unsigned char)(r / cnt), o[1] = (unsigned char)(g / cnt);
            o[2] = (unsigned char)(b / cnt), o[3] = (unsigned char)(a / cnt);
        }
    }
    return out;
}

static unsigned char *orient(unsigned char *src, int *w, int *h, int o)
{
    if (o <= 1 || o > 8) return src;
    int sw = *w, sh = *h;
    int dw = o >= 5 ? sh : sw, dh = o >= 5 ? sw : sh;
    unsigned char *out = malloc((size_t)dw * (size_t)dh * 4);
    if (!out) return src;
    for (int y = 0; y < dh; y++) {
        for (int x = 0; x < dw; x++) {
            int sx, sy;
            switch (o) {
            case 2: sx = sw - 1 - x, sy = y; break;
            case 3: sx = sw - 1 - x, sy = sh - 1 - y; break;
            case 4: sx = x, sy = sh - 1 - y; break;
            case 5: sx = y, sy = x; break;
            case 6: sx = y, sy = sh - 1 - x; break;
            case 7: sx = sw - 1 - y, sy = sh - 1 - x; break;
            default: sx = sw - 1 - y, sy = x; break;
            }
            memcpy(out + ((size_t)y * (size_t)dw + (size_t)x) * 4, src + ((size_t)sy * (size_t)sw + (size_t)sx) * 4, 4);
        }
    }
    free(src);
    *w = dw, *h = dh;
    return out;
}

static unsigned char *decode(const char *path, int max_dim, int *ow, int *oh)
{
    SDL_Surface *raw = IMG_Load(path);
    if (!raw) return NULL;
    if ((long)raw->w * (long)raw->h > MAX_PIXELS) {
        SDL_FreeSurface(raw);
        return NULL;
    }
    SDL_Surface *s = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(raw);
    if (!s) return NULL;
    int w = s->w, h = s->h;
    /* the surface may have padding at the end of rows */
    unsigned char *pix = malloc((size_t)w * (size_t)h * 4);
    if (!pix) {
        SDL_FreeSurface(s);
        return NULL;
    }
    for (int y = 0; y < h; y++) memcpy(pix + (size_t)y * (size_t)w * 4, (unsigned char *)s->pixels + (size_t)y * (size_t)s->pitch, (size_t)w * 4);
    SDL_FreeSurface(s);
    int longest = w > h ? w : h;
    if (longest > max_dim) {
        int dw = (int)((int64_t)w * max_dim / longest), dh = (int)((int64_t)h * max_dim / longest);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        unsigned char *small = box_scale(pix, w, h, dw, dh);
        free(pix);
        if (!small) return NULL;
        pix = small, w = dw, h = dh;
    }
    size_t pl = strlen(path);
    if (pl > 4 && (!SDL_strcasecmp(path + pl - 4, ".jpg") || (pl > 5 && !SDL_strcasecmp(path + pl - 5, ".jpeg"))))
        pix = orient(pix, &w, &h, exif_orientation(path));
    *ow = w, *oh = h;
    return pix;
}

/* ---------- the worker -------------------------------------------------- */

static int worker(void *arg)
{
    (void)arg;
    SDL_LockMutex(lock);
    while (!quit) {
        if (nqueue == 0 || ndone >= MAX_DONE) {
            SDL_CondWaitTimeout(wake, lock, 500);
            continue;
        }
        int best = 0;
        for (int i = 1; i < nqueue; i++)
            if (queue[i].stamp > queue[best].stamp) best = i;
        Req r = queue[best];
        queue[best] = queue[--nqueue];
        SDL_UnlockMutex(lock);
        int w = 0, h = 0;
        unsigned char *px = decode(r.path, r.max_dim, &w, &h);
        SDL_LockMutex(lock);
        if (ndone < MAX_DONE) {
            Done *d = &done[ndone++];
            memcpy(d->path, r.path, sizeof d->path);
            d->max_dim = r.max_dim, d->w = w, d->h = h, d->rgba = px;
        } else {
            free(px);
        }
    }
    SDL_UnlockMutex(lock);
    return 0;
}

void pics_start(void)
{
    if (thread) return;
    lock = SDL_CreateMutex();
    wake = SDL_CreateCond();
    quit = 0;
    thread = SDL_CreateThread(worker, "pixi-pictures", NULL);
}

void pics_stop(void)
{
    if (!thread) return;
    SDL_LockMutex(lock);
    quit = 1;
    SDL_CondSignal(wake);
    SDL_UnlockMutex(lock);
    SDL_WaitThread(thread, NULL);
    thread = NULL;
    for (int i = 0; i < ndone; i++) free(done[i].rgba);
    ndone = nqueue = 0;
    SDL_DestroyCond(wake);
    SDL_DestroyMutex(lock);
}

void pics_request(const char *path, int max_dim)
{
    if (!thread || !path || strlen(path) >= sizeof queue[0].path) return;
    SDL_LockMutex(lock);
    unsigned stamp = ++stamp_counter;
    for (int i = 0; i < nqueue; i++)
        if (queue[i].max_dim == max_dim && !strcmp(queue[i].path, path)) {
            queue[i].stamp = stamp;
            SDL_UnlockMutex(lock);
            return;
        }
    if (nqueue == MAX_QUEUE) { /* drop the stalest */
        int worst = 0;
        for (int i = 1; i < nqueue; i++)
            if (queue[i].stamp < queue[worst].stamp) worst = i;
        queue[worst] = queue[--nqueue];
    }
    Req *r = &queue[nqueue++];
    snprintf(r->path, sizeof r->path, "%s", path);
    r->max_dim = max_dim;
    r->stamp = stamp;
    SDL_CondSignal(wake);
    SDL_UnlockMutex(lock);
}

int pics_take(char *path, int path_cap, int *max_dim, int *w, int *h, unsigned char **rgba)
{
    if (!thread) return 0;
    SDL_LockMutex(lock);
    if (ndone == 0) {
        SDL_UnlockMutex(lock);
        return 0;
    }
    Done d = done[0];
    memmove(&done[0], &done[1], sizeof(Done) * (size_t)(--ndone));
    SDL_CondSignal(wake);
    SDL_UnlockMutex(lock);
    snprintf(path, (size_t)path_cap, "%s", d.path);
    *max_dim = d.max_dim, *w = d.w, *h = d.h, *rgba = d.rgba;
    return 1;
}
