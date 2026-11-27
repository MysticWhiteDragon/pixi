/*
 * pixi-shell: system helpers. See sys.h.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#define _GNU_SOURCE
#include "sys.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <ifaddrs.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ======================================================================== */
/* Utilities                                                                */
/* ======================================================================== */

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("pixi-shell: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

void *xcalloc(size_t a, size_t b)
{
    void *p = calloc(a ? a : 1, b ? b : 1);
    if (!p) die("out of memory");
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

char *xstrdup(const char *s)
{
    if (!s) return NULL;
    char *d = strdup(s);
    if (!d) die("out of memory");
    return d;
}

char *xasprintf(const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = xmalloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return s;
}

void sncpy(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    if (!src) src = "";
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

const char *home_dir(void)
{
    const char *h = getenv("HOME");
    return (h && *h) ? h : "/";
}

char *expand_home(const char *p)
{
    if (!p) return NULL;
    if (p[0] == '~' && (p[1] == '/' || p[1] == 0)) return xasprintf("%s%s", home_dir(), p + 1);
    return xstrdup(p);
}

int file_exists(const char *p) { return p && access(p, F_OK) == 0; }

int is_dir(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

void mkdir_p(const char *path)
{
    char buf[1024];
    sncpy(buf, sizeof buf, path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

int in_path(const char *name)
{
    if (!name || !*name) return 0;
    if (strchr(name, '/')) return access(name, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    char buf[1024];
    const char *p = path;
    while (*p) {
        const char *c = strchr(p, ':');
        size_t len = c ? (size_t)(c - p) : strlen(p);
        if (len && len + strlen(name) + 2 < sizeof buf) {
            memcpy(buf, p, len);
            buf[len] = '/';
            strcpy(buf + len + 1, name);
            if (access(buf, X_OK) == 0) return 1;
        }
        if (!c) break;
        p = c + 1;
    }
    return 0;
}

int read_file_str(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    return (int)n;
}

long read_file_long(const char *path, long fallback)
{
    char buf[64];
    if (read_file_str(path, buf, sizeof buf) <= 0) return fallback;
    char *end;
    long v = strtol(buf, &end, 10);
    return end == buf ? fallback : v;
}

int list_has(const char *list, const char *word)
{
    if (!list || !word || !*word) return 0;
    size_t wl = strlen(word);
    const char *p = list;
    while (*p) {
        while (*p == ',' || *p == ';' || isspace((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && *p != ',' && *p != ';' && !isspace((unsigned char)*p)) p++;
        if ((size_t)(p - s) == wl && strncasecmp(s, word, wl) == 0) return 1;
    }
    return 0;
}

int list_next(const char **cursor, char *out, size_t cap)
{
    const char *p = *cursor;
    while (*p == ',' || isspace((unsigned char)*p)) p++;
    if (!*p) return 0;
    const char *s = p;
    while (*p && *p != ',') p++;
    size_t n = (size_t)(p - s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
    *cursor = p;
    return 1;
}

const char *basename_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

unsigned hash_str(const char *s)
{
    unsigned h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

int natural_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *ea = a, *eb = b;
            while (isdigit((unsigned char)*ea)) ea++;
            while (isdigit((unsigned char)*eb)) eb++;
            if (ea - a != eb - b) return (int)((ea - a) - (eb - b));
            for (; a < ea; a++, b++)
                if (*a != *b) return *a - *b;
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int run_capture(char *const argv[], char *out, size_t cap, int timeout_ms)
{
    char scratch[256];
    if (!out) {
        out = scratch;
        cap = sizeof scratch;
    }
    out[0] = 0;
    int pfd[2];
    if (pipe(pfd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[0]);
        close(pfd[1]);
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) dup2(dn, 0);
        setenv("LC_ALL", "C", 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    size_t len = 0;
    long start = now_ms();
    for (;;) {
        struct pollfd p = { pfd[0], POLLIN, 0 };
        int r = poll(&p, 1, 100);
        if (r > 0) {
            char tmp[1024];
            ssize_t n = read(pfd[0], tmp, sizeof tmp);
            if (n <= 0) break;
            size_t take = (size_t)n;
            if (len + take >= cap) take = cap - 1 - len;
            memcpy(out + len, tmp, take);
            len += take;
        } else if (r < 0 && errno != EINTR) {
            break;
        }
        if (timeout_ms > 0 && now_ms() - start > timeout_ms) {
            kill(pid, SIGKILL);
            break;
        }
    }
    close(pfd[0]);
    out[len] = 0;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

char **argv_dup(char *const *a)
{
    int n = 0;
    while (a[n]) n++;
    char **r = xcalloc((size_t)n + 1, sizeof(char *));
    for (int i = 0; i < n; i++) r[i] = xstrdup(a[i]);
    return r;
}

void argv_free(char **a)
{
    if (!a) return;
    for (int i = 0; a[i]; i++) free(a[i]);
    free(a);
}

char **exec_to_argv(const char *exec)
{
    char **argv = xcalloc(64, sizeof(char *));
    int n = 0;
    const char *p = exec;
    char buf[1024];
    while (*p && n < 62) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        size_t len = 0;
        int quoted = 0;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            char c = *p++;
            if (c == '"') {
                quoted = !quoted;
                continue;
            }
            if (c == '\\' && *p) {
                c = *p++;
            } else if (c == '%' && *p) {
                char code = *p++;
                if (code == '%') c = '%';
                else continue;
            }
            if (len < sizeof buf - 1) buf[len++] = c;
        }
        buf[len] = 0;
        if (len) argv[n++] = xstrdup(buf);
    }
    argv[n] = NULL;
    if (n == 0) {
        free(argv);
        return NULL;
    }
    return argv;
}

void sl_add(StrList *l, const char *s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = xrealloc(l->v, sizeof(char *) * (size_t)l->cap);
    }
    l->v[l->n++] = xstrdup(s);
}

void sl_free(StrList *l)
{
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

/* ======================================================================== */
/* Configuration                                                            */
/* ======================================================================== */

Config cfg;
System *systems;
int nsystems;
CustomApp *custom_apps;
int ncustom;

static const System default_systems[] = {
    { "nes",  "Nintendo Entertainment System", "nes,famicom",           "nes,fds,unf,unif,zip,7z",   "nestopia,fceumm,mesen",                  "libretro-nestopia" },
    { "snes", "Super Nintendo",                "snes,sfc,superfamicom", "sfc,smc,swc,fig,bs,zip,7z", "snes9x,bsnes_mercury_performance,bsnes", "libretro-snes9x" },
    { "gb",   "Game Boy",                      "gb",                    "gb,zip,7z",                 "gambatte,mgba,sameboy",                  "libretro-gambatte" },
    { "gbc",  "Game Boy Color",                "gbc",                   "gbc,zip,7z",                "gambatte,mgba,sameboy",                  "libretro-gambatte" },
    { "gba",  "Game Boy Advance",              "gba",                   "gba,zip,7z",                "mgba,vba_next,gpsp",                     "libretro-mgba" },
    { "md",   "Mega Drive / Genesis",          "md,genesis,megadrive",  "md,gen,smd,bin,zip,7z",     "genesis_plus_gx,picodrive",              "libretro-genesisplusgx" },
    { "sms",  "Master System",                 "sms,mastersystem",      "sms,zip,7z",                "genesis_plus_gx,picodrive",              "libretro-genesisplusgx" },
    { "gg",   "Game Gear",                     "gg,gamegear",           "gg,zip,7z",                 "genesis_plus_gx",                        "libretro-genesisplusgx" },
    { "pce",  "PC Engine / TurboGrafx-16",     "pce,tg16,pcengine",     "pce,sgx,cue,chd,zip",       "mednafen_pce_fast,mednafen_pce",         "libretro-beetle-pce-fast" },
    { "psx",  "PlayStation",                   "psx,ps1",               "cue,chd,pbp,m3u,iso",       "pcsx_rearmed,mednafen_psx,swanstation",  "libretro-beetle-psx" },
    { "psp",  "PlayStation Portable",          "psp",                   "iso,cso,pbp,chd",           "ppsspp",                                 "" },
    { "nds",  "Nintendo DS",                   "nds",                   "nds,zip",                   "desmume,melonds",                        "libretro-desmume" },
    { "n64",  "Nintendo 64",                   "n64",                   "n64,z64,v64,zip",           "mupen64plus_next,parallel_n64",          "" },
};

void cfg_defaults(void)
{
    memset(&cfg, 0, sizeof cfg);
    sncpy(cfg.theme, sizeof cfg.theme, "blue");
    cfg.full_material = 0; /* the Full water-and-glass material was removed: too slow on a Pi */
    cfg.sounds = 1;
    cfg.clock24 = 1;
    cfg.date_fmt = 0;
    cfg.battery_pct = 1;
    cfg.app_scheme = 0;
    cfg.category = CAT_GAMES;
    cfg.scan_removable = 1;
    sncpy(cfg.video_player, sizeof cfg.video_player,
          "cvlc --fullscreen --play-and-exit --no-video-title-show");
    sncpy(cfg.video_hwdec, sizeof cfg.video_hwdec, "no");
    cfg.video_skip = 10;
    cfg.video_skip_long = 60;
    sncpy(cfg.retroarch, sizeof cfg.retroarch, "retroarch");
    sncpy(cfg.rom_root, sizeof cfg.rom_root, "~/Games/roms");
    sncpy(cfg.pc_root, sizeof cfg.pc_root, "~/Games/pc");
    sncpy(cfg.core_dirs, sizeof cfg.core_dirs,
          "~/.config/retroarch/cores,/usr/lib/aarch64-linux-gnu/libretro,/usr/lib/arm-linux-gnueabihf/libretro,/usr/lib/libretro");
    sncpy(cfg.hide, sizeof cfg.hide,
          "pixi.desktop,foot-server.desktop,footclient.desktop,org.codeberg.dnkl.foot-server.desktop,"
          "org.codeberg.dnkl.footclient.desktop,display-im6.q16.desktop,debian-uxterm.desktop,debian-xterm.desktop");
    cfg.joy_ok = 0;
    cfg.joy_back = 1;
    cfg.joy_menu = 2;
    cfg.joy_pgup = 4;
    cfg.joy_square = 3;
    cfg.joy_pgdn = 5;
    cfg.joy_select = 8;
    cfg.joy_start = 9;
    cfg.joy_guide = 10;
    cfg.ftp_port = 2121;
    cfg.web_port = 80;
    nsystems = (int)(sizeof default_systems / sizeof default_systems[0]);
    systems = xmalloc(sizeof default_systems);
    memcpy(systems, default_systems, sizeof default_systems);
}

static int parse_bool(const char *v)
{
    return !strcasecmp(v, "1") || !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on");
}

static System *system_get(const char *id)
{
    for (int i = 0; i < nsystems; i++)
        if (!strcasecmp(systems[i].id, id)) return &systems[i];
    systems = xrealloc(systems, sizeof(System) * (size_t)(nsystems + 1));
    System *s = &systems[nsystems++];
    memset(s, 0, sizeof *s);
    sncpy(s->id, sizeof s->id, id);
    sncpy(s->name, sizeof s->name, id);
    sncpy(s->dirs, sizeof s->dirs, id);
    return s;
}

static CustomApp *custom_get(const char *id)
{
    for (int i = 0; i < ncustom; i++)
        if (!strcmp(custom_apps[i].id, id)) return &custom_apps[i];
    custom_apps = xrealloc(custom_apps, sizeof(CustomApp) * (size_t)(ncustom + 1));
    CustomApp *a = &custom_apps[ncustom++];
    memset(a, 0, sizeof *a);
    sncpy(a->id, sizeof a->id, id);
    sncpy(a->category, sizeof a->category, "other");
    return a;
}

static void cfg_set(const char *section, const char *key, const char *val)
{
    if (!*section || !strcasecmp(section, "general")) {
        if (!strcasecmp(key, "theme")) sncpy(cfg.theme, sizeof cfg.theme, val);
        else if (!strcasecmp(key, "material")) {
            /* accepted and ignored, so old config files still load */
        }
        else if (!strcasecmp(key, "sounds")) cfg.sounds = parse_bool(val);
        else if (!strcasecmp(key, "confirm_button")) cfg.confirm_east = !strcasecmp(val, "east");
        else if (!strcasecmp(key, "clock_24h")) cfg.clock24 = parse_bool(val);
        else if (!strcasecmp(key, "date_format")) {
            cfg.date_fmt = !strcasecmp(val, "dmy") ? 1 : !strcasecmp(val, "iso") ? 2 : !strcasecmp(val, "long") ? 3 : 0;
        }
        else if (!strcasecmp(key, "battery_percent")) cfg.battery_pct = parse_bool(val);
        else if (!strcasecmp(key, "ui_scale")) cfg.ui_scale = strtof(val, NULL);
        else if (!strcasecmp(key, "show_terminal_apps")) cfg.show_terminal_apps = parse_bool(val);
        else if (!strcasecmp(key, "hide")) sncpy(cfg.hide, sizeof cfg.hide, val);
        else fprintf(stderr, "pixi-shell: config: unknown key [general] %s\n", key);
    } else if (!strcasecmp(section, "retroarch")) {
        if (!strcasecmp(key, "binary")) sncpy(cfg.retroarch, sizeof cfg.retroarch, val);
        else if (!strcasecmp(key, "rom_root")) sncpy(cfg.rom_root, sizeof cfg.rom_root, val);
        else if (!strcasecmp(key, "pc_root")) sncpy(cfg.pc_root, sizeof cfg.pc_root, val);
        else if (!strcasecmp(key, "core_dirs")) sncpy(cfg.core_dirs, sizeof cfg.core_dirs, val);
    } else if (!strcasecmp(section, "media")) {
        if (!strcasecmp(key, "pictures")) sncpy(cfg.pictures, sizeof cfg.pictures, val);
        else if (!strcasecmp(key, "music")) sncpy(cfg.music, sizeof cfg.music, val);
        else if (!strcasecmp(key, "videos")) sncpy(cfg.videos, sizeof cfg.videos, val);
        else if (!strcasecmp(key, "scan_removable")) cfg.scan_removable = parse_bool(val);
        else if (!strcasecmp(key, "video_player")) sncpy(cfg.video_player, sizeof cfg.video_player, val);
        else if (!strcasecmp(key, "video_hwdec")) sncpy(cfg.video_hwdec, sizeof cfg.video_hwdec, val);
        else if (!strcasecmp(key, "video_skip")) cfg.video_skip = atoi(val) > 0 ? atoi(val) : 10;
        else if (!strcasecmp(key, "video_skip_long")) cfg.video_skip_long = atoi(val) > 0 ? atoi(val) : 60;
    } else if (!strcasecmp(section, "input")) {
        int v = atoi(val);
        if (!strcasecmp(key, "joy_ok")) cfg.joy_ok = v;
        else if (!strcasecmp(key, "joy_back")) cfg.joy_back = v;
        else if (!strcasecmp(key, "joy_menu")) cfg.joy_menu = v;
        else if (!strcasecmp(key, "joy_page_up")) cfg.joy_pgup = v;
        else if (!strcasecmp(key, "joy_page_down")) cfg.joy_pgdn = v;
        else if (!strcasecmp(key, "joy_select")) cfg.joy_select = v;
        else if (!strcasecmp(key, "joy_start")) cfg.joy_start = v;
        else if (!strcasecmp(key, "joy_guide")) cfg.joy_guide = v;
        else if (!strcasecmp(key, "joy_square")) cfg.joy_square = v;
        else if (!strcasecmp(key, "browser")) sncpy(cfg.browser_path, sizeof cfg.browser_path, val);
        else if (!strcasecmp(key, "debug")) cfg.debug_input = parse_bool(val);
        else if (!strcasecmp(key, "ftp_port") && v > 0 && v < 65536) cfg.ftp_port = v;
        else if (!strcasecmp(key, "web_port") && v > 0 && v < 65536) cfg.web_port = v;
    } else if (!strncasecmp(section, "system.", 7)) {
        System *s = system_get(section + 7);
        if (!strcasecmp(key, "name")) sncpy(s->name, sizeof s->name, val);
        else if (!strcasecmp(key, "dirs")) sncpy(s->dirs, sizeof s->dirs, val);
        else if (!strcasecmp(key, "extensions")) sncpy(s->exts, sizeof s->exts, val);
        else if (!strcasecmp(key, "cores")) sncpy(s->cores, sizeof s->cores, val);
        else if (!strcasecmp(key, "package")) sncpy(s->pkg, sizeof s->pkg, val);
    } else if (!strncasecmp(section, "app.", 4)) {
        CustomApp *a = custom_get(section + 4);
        if (!strcasecmp(key, "name")) sncpy(a->name, sizeof a->name, val);
        else if (!strcasecmp(key, "exec")) sncpy(a->exec, sizeof a->exec, val);
        else if (!strcasecmp(key, "icon")) sncpy(a->icon, sizeof a->icon, val);
        else if (!strcasecmp(key, "category")) sncpy(a->category, sizeof a->category, val);
    }
}

void cfg_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048], section[128] = "";
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) {
                *e = 0;
                sncpy(section, sizeof section, trim(s + 1));
            }
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        cfg_set(section, trim(s), trim(eq + 1));
    }
    fclose(f);
}

static char state_path[1024];

void state_load(void)
{
    snprintf(state_path, sizeof state_path, "%s/.config/pixi/state", home_dir());
    FILE *f = fopen(state_path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(line), *v = trim(eq + 1);
        if (!strcmp(k, "category")) {
            cfg.category = atoi(v);
            if (cfg.category < 0 || cfg.category >= CAT_COLUMNS) cfg.category = CAT_GAMES;
        }
        else if (!strcmp(k, "sounds")) cfg.sounds = atoi(v) != 0;
        else if (!strcmp(k, "theme")) sncpy(cfg.theme, sizeof cfg.theme, v);
        else if (!strcmp(k, "material")) {}
        else if (!strcmp(k, "confirm_east")) cfg.confirm_east = atoi(v) != 0;
        else if (!strcmp(k, "clock24")) cfg.clock24 = atoi(v) != 0;
        else if (!strcmp(k, "date_fmt")) cfg.date_fmt = atoi(v) < 0 || atoi(v) > 3 ? 0 : atoi(v);
        else if (!strcmp(k, "battery_pct")) cfg.battery_pct = atoi(v) != 0;
        else if (!strcmp(k, "app_scheme")) cfg.app_scheme = atoi(v) == 1 ? 1 : atoi(v) == 2 ? 2 : 0;
        else if (!strcmp(k, "video_out")) cfg.video_out = atoi(v) == 1 ? 1 : 0;
        else if (!strcmp(k, "wallpaper")) sncpy(cfg.wallpaper, sizeof cfg.wallpaper, v);
    }
    fclose(f);
}

void state_save(void)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/.config/pixi", home_dir());
    mkdir_p(dir);
    if (!*state_path) snprintf(state_path, sizeof state_path, "%s/state", dir);
    FILE *f = fopen(state_path, "w");
    if (!f) return;
    fprintf(f, "category=%d\nsounds=%d\ntheme=%s\nconfirm_east=%d\n", cfg.category, cfg.sounds, cfg.theme,
            cfg.confirm_east);
    fprintf(f, "clock24=%d\ndate_fmt=%d\nbattery_pct=%d\nvideo_out=%d\n", cfg.clock24, cfg.date_fmt, cfg.battery_pct,
            cfg.video_out);
    fprintf(f, "wallpaper=%s\n", cfg.wallpaper);
    fprintf(f, "app_scheme=%d\n", cfg.app_scheme);
    fclose(f);
}

/* ======================================================================== */
/* Desktop entries                                                          */
/* ======================================================================== */

const char *cat_names[CAT_COUNT] = { "Settings", "Apps", "Pictures", "Music", "Videos", "Games",
                                     "Internet", "Multimedia", "Graphics", "Office",
                                     "Development", "Education", "Utilities", "System", "Other" };
const char *cat_glyphs[CAT_COUNT] = { "category-settings", "category-software", "category-images", "category-music",
                                      "category-video", "category-games", "category-internet",
                                      "category-multimedia", "category-graphics", "category-office",
                                      "category-development", "category-education", "category-utilities",
                                      "category-system", "category-other" };

AppList apps;
static char lang_full[32], lang_short[8];

static int category_of(const char *cats)
{
    if (list_has(cats, "Game")) return CAT_GAMES;
    if (list_has(cats, "Network") || list_has(cats, "WebBrowser") || list_has(cats, "Email")) return CAT_INTERNET;
    if (list_has(cats, "AudioVideo") || list_has(cats, "Audio") || list_has(cats, "Video")) return CAT_MULTIMEDIA;
    if (list_has(cats, "Graphics")) return CAT_GRAPHICS;
    if (list_has(cats, "Office")) return CAT_OFFICE;
    if (list_has(cats, "Development")) return CAT_DEVELOPMENT;
    if (list_has(cats, "Education") || list_has(cats, "Science")) return CAT_EDUCATION;
    if (list_has(cats, "Utility") || list_has(cats, "Accessories")) return CAT_UTILITIES;
    if (list_has(cats, "System") || list_has(cats, "Settings")) return CAT_SYSTEM;
    return CAT_OTHER;
}

static int category_named(const char *n)
{
    for (int i = CAT_GAMES; i < CAT_COUNT; i++)
        if (!strcasecmp(n, cat_names[i])) return i;
    return CAT_OTHER;
}

static void scan_dir(const char *dir, StrList *seen)
{
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    char key_full[64], key_short[64];
    snprintf(key_full, sizeof key_full, "Name[%s]", lang_full);
    snprintf(key_short, sizeof key_short, "Name[%s]", lang_short);
    while ((de = readdir(dp))) {
        size_t l = strlen(de->d_name);
        if (l < 9 || strcmp(de->d_name + l - 8, ".desktop")) continue;
        int dup = 0;
        for (int i = 0; i < seen->n; i++)
            if (!strcmp(seen->v[i], de->d_name)) dup = 1;
        if (dup) continue;
        sl_add(seen, de->d_name); /* user entries mask system ones, even hidden ones */
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        AppEntry e;
        memset(&e, 0, sizeof e);
        sncpy(e.id, sizeof e.id, de->d_name);
        char line[2048], type[64] = "", tryexec[256] = "", name_loc[256] = "";
        int in_entry = 0, nodisplay = 0, hidden = 0, onlyshowin = 0, got_full = 0;
        while (fgets(line, sizeof line, f)) {
            char *s = trim(line);
            if (*s == '[') {
                in_entry = !strcmp(s, "[Desktop Entry]");
                continue;
            }
            if (!in_entry || *s == '#') continue;
            char *eq = strchr(s, '=');
            if (!eq) continue;
            *eq = 0;
            char *k = trim(s), *v = trim(eq + 1);
            if (!strcmp(k, "Name")) sncpy(e.name, sizeof e.name, v);
            else if (*lang_full && !strcmp(k, key_full)) { sncpy(name_loc, sizeof name_loc, v); got_full = 1; }
            else if (*lang_short && !got_full && !strcmp(k, key_short)) sncpy(name_loc, sizeof name_loc, v);
            else if (!strcmp(k, "GenericName")) sncpy(e.generic, sizeof e.generic, v);
            else if (!strcmp(k, "Comment")) sncpy(e.comment, sizeof e.comment, v);
            else if (!strcmp(k, "Exec")) sncpy(e.exec, sizeof e.exec, v);
            else if (!strcmp(k, "TryExec")) sncpy(tryexec, sizeof tryexec, v);
            else if (!strcmp(k, "Icon")) sncpy(e.icon, sizeof e.icon, v);
            else if (!strcmp(k, "Categories")) sncpy(e.categories, sizeof e.categories, v);
            else if (!strcmp(k, "Type")) sncpy(type, sizeof type, v);
            else if (!strcmp(k, "NoDisplay")) nodisplay = parse_bool(v);
            else if (!strcmp(k, "Hidden")) hidden = parse_bool(v);
            else if (!strcmp(k, "Terminal")) e.terminal = parse_bool(v);
            else if (!strcmp(k, "OnlyShowIn")) onlyshowin = !list_has(v, "Pixi");
        }
        fclose(f);
        if (*name_loc) sncpy(e.name, sizeof e.name, name_loc);
        if (strcmp(type, "Application") || nodisplay || hidden || onlyshowin || !*e.exec) continue;
        if (list_has(cfg.hide, e.id)) continue;
        if (e.terminal && !cfg.show_terminal_apps) continue;
        if (!strncmp(e.exec, "foot", 4) && (!e.exec[4] || e.exec[4] == ' ' || !strncmp(e.exec + 4, "client", 6) || !strncmp(e.exec + 4, "-server", 7)))
            continue; /* Pixi has its own terminal now */
        if (*tryexec && !in_path(tryexec)) continue;
        char **a = exec_to_argv(e.exec);
        if (!a) continue;
        int ok = in_path(a[0]);
        argv_free(a);
        if (!ok) continue;
        e.category = category_of(e.categories);
        if (apps.n == apps.cap) {
            apps.cap = apps.cap ? apps.cap * 2 : 64;
            apps.v = xrealloc(apps.v, sizeof(AppEntry) * (size_t)apps.cap);
        }
        apps.v[apps.n++] = e;
    }
    closedir(dp);
}

static int app_cmp(const void *a, const void *b)
{
    return natural_cmp(((const AppEntry *)a)->name, ((const AppEntry *)b)->name);
}

void apps_scan(void)
{
    apps.n = 0;
    const char *lang = getenv("LC_ALL");
    if (!lang || !*lang) lang = getenv("LC_MESSAGES");
    if (!lang || !*lang) lang = getenv("LANG");
    lang_full[0] = lang_short[0] = 0;
    if (lang && *lang && strcmp(lang, "C") && strcmp(lang, "POSIX") && strncmp(lang, "C.", 2)) {
        sncpy(lang_full, sizeof lang_full, lang);
        lang_full[strcspn(lang_full, ".@")] = 0;
        sncpy(lang_short, sizeof lang_short, lang_full);
        lang_short[strcspn(lang_short, "_")] = 0;
    }
    StrList seen = { 0 };
    char buf[1024];
    const char *xdh = getenv("XDG_DATA_HOME");
    if (xdh && *xdh) snprintf(buf, sizeof buf, "%s/applications", xdh);
    else snprintf(buf, sizeof buf, "%s/.local/share/applications", home_dir());
    scan_dir(buf, &seen);
    const char *dirs = getenv("XDG_DATA_DIRS");
    if (!dirs || !*dirs) dirs = "/usr/local/share:/usr/share";
    const char *p = dirs;
    while (*p) {
        const char *c = strchr(p, ':');
        size_t len = c ? (size_t)(c - p) : strlen(p);
        if (len && len < 900) {
            memcpy(buf, p, len);
            strcpy(buf + len, "/applications");
            scan_dir(buf, &seen);
        }
        if (!c) break;
        p = c + 1;
    }
    sl_free(&seen);
    for (int i = 0; i < ncustom; i++) {
        CustomApp *a = &custom_apps[i];
        if (!*a->exec) continue;
        if (apps.n == apps.cap) {
            apps.cap = apps.cap ? apps.cap * 2 : 64;
            apps.v = xrealloc(apps.v, sizeof(AppEntry) * (size_t)apps.cap);
        }
        AppEntry *e = &apps.v[apps.n++];
        memset(e, 0, sizeof *e);
        snprintf(e->id, sizeof e->id, "custom:%s", a->id);
        sncpy(e->name, sizeof e->name, *a->name ? a->name : a->id);
        sncpy(e->exec, sizeof e->exec, a->exec);
        sncpy(e->icon, sizeof e->icon, a->icon);
        e->category = category_named(a->category);
    }
    if (apps.n > 1) qsort(apps.v, (size_t)apps.n, sizeof(AppEntry), app_cmp);
}

/* ======================================================================== */
/* Game library                                                             */
/* ======================================================================== */

char *find_core(const System *s)
{
    const char *cp = s->cores;
    char core[256];
    while (list_next(&cp, core, sizeof core)) {
        if (strchr(core, '/')) {
            char *p = expand_home(core);
            if (access(p, R_OK) == 0) return p;
            free(p);
            continue;
        }
        const char *dp = cfg.core_dirs;
        char dir[512];
        while (list_next(&dp, dir, sizeof dir)) {
            char *d = expand_home(dir);
            char *p = xasprintf("%s/%s_libretro.so", d, core);
            free(d);
            if (access(p, R_OK) == 0) return p;
            free(p);
        }
        glob_t g;
        char pat[400];
        snprintf(pat, sizeof pat, "/usr/lib/*/libretro/%s_libretro.so", core);
        if (glob(pat, 0, NULL, &g) == 0 && g.gl_pathc > 0) {
            char *p = xstrdup(g.gl_pathv[0]);
            globfree(&g);
            return p;
        }
        globfree(&g);
    }
    return NULL;
}

static void scan_roms(const System *s, const char *dir, int depth, StrList *out, int limit)
{
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) && out->n < limit) {
        if (de->d_name[0] == '.') continue;
        char *p = xasprintf("%s/%s", dir, de->d_name);
        if (is_dir(p)) {
            if (depth < 2) scan_roms(s, p, depth + 1, out, limit);
        } else {
            const char *dot = strrchr(de->d_name, '.');
            if (dot && dot != de->d_name && list_has(s->exts, dot + 1)) sl_add(out, p);
        }
        free(p);
    }
    closedir(dp);
}

/* moves everything in src into dst (made if needed), merging folders both have; a file whose name is
 * already taken in dst stays where it was. src is removed if that empties it. */
static void move_into(const char *src, const char *dst)
{
    mkdir(dst, 0755);
    DIR *dp = opendir(src);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *a = xasprintf("%s/%s", src, de->d_name), *b = xasprintf("%s/%s", dst, de->d_name);
        struct stat sa, sb;
        if (lstat(b, &sb) != 0) {
            if (rename(a, b) != 0) fprintf(stderr, "pixi-shell: couldn't move %s: %s\n", a, strerror(errno));
        } else if (lstat(a, &sa) == 0 && S_ISDIR(sa.st_mode) && S_ISDIR(sb.st_mode)) {
            move_into(a, b); /* both have this folder: merge what's inside */
        }
        free(a), free(b);
    }
    closedir(dp);
    rmdir(src); /* only goes if it's empty now */
}

static int is_real_dir(const char *p)
{
    struct stat st;
    return lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Games live in ~/Games: console games in ~/Games/roms/<system>, PC games in ~/Games/pc/<game>.
 * Older layouts are moved in once: ~/roms (the first one), and system folders straight under
 * ~/Games (the one before this). ~/roms is left as a link to ~/Games/roms, so RetroArch playlists
 * and anything else that points there keep working. */
void games_folders_ensure(void)
{
    char games[1100], groms[1100], roms[1100], link[1100];
    snprintf(games, sizeof games, "%s/Games", home_dir());
    snprintf(groms, sizeof groms, "%s/Games/roms", home_dir());
    snprintf(roms, sizeof roms, "%s/roms", home_dir());
    mkdir(games, 0755);
    if (is_real_dir(roms)) {
        if (!is_real_dir(groms) && rename(roms, groms) == 0) fprintf(stderr, "pixi-shell: moved ~/roms to ~/Games/roms\n");
        else move_into(roms, groms);
    }
    mkdir(groms, 0755);
    for (int i = 0; i < nsystems; i++) { /* system folders that sat straight under ~/Games */
        const char *cp = systems[i].dirs;
        char d[256];
        while (list_next(&cp, d, sizeof d)) {
            if (d[0] == '/' || d[0] == '~' || !strcmp(d, "roms") || !strcmp(d, "pc")) continue;
            char *old = xasprintf("%s/%s", games, d), *now = xasprintf("%s/%s", groms, d);
            if (is_real_dir(old)) move_into(old, now);
            free(old), free(now);
        }
    }
    struct stat st;
    ssize_t n = readlink(roms, link, sizeof link - 1);
    if (n >= 0) link[n] = 0;
    if (lstat(roms, &st) != 0 || (S_ISLNK(st.st_mode) && (n < 0 || strcmp(link, "Games/roms")))) {
        unlink(roms);
        if (symlink("Games/roms", roms) != 0) fprintf(stderr, "pixi-shell: couldn't link ~/roms to ~/Games/roms\n");
    }
    /* the starter settings of older versions */
    if (!strcmp(cfg.rom_root, "~/roms") || !strcmp(cfg.rom_root, "~/roms/") || !strcmp(cfg.rom_root, "~/Games") ||
        !strcmp(cfg.rom_root, "~/Games/"))
        sncpy(cfg.rom_root, sizeof cfg.rom_root, "~/Games/roms");
    char *root = expand_home(cfg.rom_root);
    mkdir(root, 0755);
    for (int i = 0; i < nsystems; i++) {
        const char *cp = systems[i].dirs;
        char d[256];
        if (!list_next(&cp, d, sizeof d) || d[0] == '/' || d[0] == '~') continue; /* the first name is the folder */
        char *p = xasprintf("%s/%s", root, d);
        mkdir(p, 0755);
        free(p);
    }
    free(root);
    char *pc = expand_home(cfg.pc_root);
    mkdir(pc, 0755);
    free(pc);
}

/* ---- PC games ---------------------------------------------------------------------------------- */

#if defined(__aarch64__)
#define HOST_MACHINE 183 /* EM_AARCH64 */
#elif defined(__x86_64__)
#define HOST_MACHINE 62 /* EM_X86_64 */
#elif defined(__arm__)
#define HOST_MACHINE 40 /* EM_ARM */
#elif defined(__i386__)
#define HOST_MACHINE 3
#else
#define HOST_MACHINE 0
#endif

/* 2: a program for this machine, 1: a script, 0: neither (or a program for another machine) */
static int runnable_kind(const char *path)
{
    unsigned char h[20] = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n >= 2 && h[0] == '#' && h[1] == '!') return 1;
    size_t pl = strlen(path);
    if (pl > 3 && !strcmp(path + pl - 3, ".sh")) return 1; /* a script without a #! line runs with sh */
    if (n < 20 || memcmp(h, "\x7f"
                            "ELF",
                         4))
        return 0;
    unsigned type = h[5] == 1 ? (unsigned)(h[16] | h[17] << 8) : (unsigned)(h[17] | h[16] << 8);
    unsigned mach = h[5] == 1 ? (unsigned)(h[18] | h[19] << 8) : (unsigned)(h[19] | h[18] << 8);
    if (type != 2 && type != 3) return 0; /* executables and PIE executables only */
    if (HOST_MACHINE && mach != HOST_MACHINE) return 0;
    return 2;
}

static int name_has_ci(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return 0;
    for (const char *p = hay; *p; p++)
        if (!strncasecmp(p, needle, nl)) return 1;
    return 0;
}

/* Which file in a game's folder is the game: a launcher script (start.sh, run.sh ...) first, then a
 * program named like the folder (Doom/doom, Quake 3/quake3), then one with the folder's name in it
 * (Quake3/ioquake3.aarch64), then any program for this machine, then any script. Libraries and
 * programs built for another kind of machine are passed over. Files copied over FTP lose their
 * "executable" mark, so the contents decide, not the mark. */
int pc_game_find(const char *dir, char *exe, size_t ecap, char *icon, size_t icap)
{
    exe[0] = icon[0] = 0;
    const char *folder = basename_of(dir);
    char flat[256];
    size_t k = 0;
    for (const char *p = folder; *p && k + 1 < sizeof flat; p++)
        if (*p != ' ' && *p != '_' && *p != '-') flat[k++] = *p;
    flat[k] = 0;
    static const char *launchers[] = { "start.sh", "run.sh", "launch.sh", "play.sh", "game.sh", "start", "run", "launch", "play", NULL };
    static const char *icons[] = { "icon.png", "icon.svg", "cover.png", "cover.jpg", "folder.png", "folder.jpg", NULL };
    DIR *dp = opendir(dir);
    if (!dp) return 0;
    int best = 0;
    size_t best_len = 0;
    char named_icon[1100] = "";
    struct dirent *de;
    while ((de = readdir(dp))) {
        const char *nm = de->d_name;
        if (nm[0] == '.') continue;
        char *p = xasprintf("%s/%s", dir, nm);
        struct stat st;
        if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(p);
            continue;
        }
        size_t nl = strlen(nm);
        for (int i = 0; icons[i]; i++)
            if (!strcasecmp(nm, icons[i]) && !icon[0]) sncpy(icon, icap, p);
        if (nl > 4 && (!strcasecmp(nm + nl - 4, ".png") || !strcasecmp(nm + nl - 4, ".svg")) && strlen(folder) == nl - 4 &&
            !strncasecmp(nm, folder, nl - 4))
            sncpy(named_icon, sizeof named_icon, p); /* Doom/Doom.png */
        if (!strncmp(nm, "lib", 3) || strstr(nm, ".so") || (nl > 4 && !strcasecmp(nm + nl - 4, ".dll"))) {
            free(p);
            continue;
        }
        int kind = runnable_kind(p);
        if (!kind) {
            free(p);
            continue;
        }
        char stem[256];
        sncpy(stem, sizeof stem, nm);
        char *dot = strrchr(stem, '.');
        if (dot && dot != stem) *dot = 0;
        int score = kind == 2 ? 50 : 40;
        for (int i = 0; launchers[i]; i++)
            if (!strcasecmp(nm, launchers[i])) score = 110;
        if (score < 110) {
            if (!strcasecmp(stem, folder) || !strcasecmp(stem, flat)) score = 100;
            else if (name_has_ci(nm, flat) || name_has_ci(nm, folder)) score = 80;
        }
        if (score > best || (score == best && nl < best_len)) {
            best = score;
            best_len = nl;
            sncpy(exe, ecap, p);
        }
        free(p);
    }
    closedir(dp);
    if (!icon[0] && named_icon[0]) sncpy(icon, icap, named_icon);
    return best > 0;
}

void system_roms(const System *s, StrList *out, int limit)
{
    char *root = expand_home(cfg.rom_root);
    const char *cp = s->dirs;
    char d[256];
    while (list_next(&cp, d, sizeof d)) {
        char *path = (d[0] == '/' || d[0] == '~') ? expand_home(d) : xasprintf("%s/%s", root, d);
        scan_roms(s, path, 0, out, limit);
        free(path);
    }
    free(root);
}

void rom_title(const char *path, char *title, size_t tcap, char *tags, size_t gcap)
{
    char name[512];
    sncpy(name, sizeof name, basename_of(path));
    char *dot = strrchr(name, '.');
    if (dot && dot != name) *dot = 0;
    for (char *p = name; *p; p++)
        if (*p == '_') *p = ' ';
    tags[0] = 0;
    char *paren = NULL;
    for (char *p = name + 1; *p; p++) {
        if (*p == '(' || *p == '[') {
            paren = p;
            break;
        }
    }
    if (paren) {
        const char *q = paren;
        size_t tl = 0;
        while (*q) {
            if (*q == '(' || *q == '[') {
                q++;
                const char *e = q;
                while (*e && *e != ')' && *e != ']') e++;
                size_t n = (size_t)(e - q);
                if (n && tl + n + 8 < gcap) {
                    if (tl) {
                        memcpy(tags + tl, " \xC2\xB7 ", 4);
                        tl += 4;
                    }
                    memcpy(tags + tl, q, n);
                    tl += n;
                    tags[tl] = 0;
                }
                q = *e ? e + 1 : e;
            } else {
                q++;
            }
        }
        *paren = 0;
    }
    sncpy(title, tcap, trim(name));
    if (!*title) sncpy(title, tcap, basename_of(path));
}

/* ======================================================================== */
/* Pictures, music and videos                                               */
/* ======================================================================== */

static int ext_in(const char *name, const char *list)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return 0;
    return list_has(list, dot + 1);
}

int media_is(int kind, const char *name)
{
    switch (kind) {
    case MEDIA_PICTURES: return ext_in(name, "jpg,jpeg,png,bmp,gif,webp");
    case MEDIA_MUSIC: return ext_in(name, "mp3,ogg,oga,opus,flac,wav");
    case MEDIA_VIDEOS: return ext_in(name, "mp4,mkv,avi,mov,webm,m4v,mpg,mpeg,wmv,flv,ts,ogv");
    }
    return 0;
}

static void root_add(MediaRoot *out, int *n, int max, const char *label, const char *path)
{
    if (*n >= max || !is_dir(path)) return;
    for (int i = 0; i < *n; i++)
        if (!strcmp(out[i].path, path)) return;
    sncpy(out[*n].label, sizeof out[*n].label, label);
    sncpy(out[*n].path, sizeof out[*n].path, path);
    (*n)++;
}

int media_roots(int kind, MediaRoot *out, int max)
{
    static const char *home_names[3][3] = { { "Pictures", "Photos", NULL }, { "Music", NULL, NULL }, { "Videos", "Movies", NULL } };
    static const char *drive_names[3][4] = { { "Pictures", "DCIM", "Photos", NULL }, { "Music", NULL, NULL, NULL },
                                             { "Videos", "Movies", NULL, NULL } };
    static const char *titles[3] = { "Pictures", "Music", "Videos" };
    if (kind < 0 || kind > 2) return 0;
    int n = 0;
    char path[1100];
    for (int i = 0; home_names[kind][i]; i++) {
        snprintf(path, sizeof path, "%s/%s", home_dir(), home_names[kind][i]);
        root_add(out, &n, max, titles[kind], path);
    }
    const char *extra = kind == MEDIA_PICTURES ? cfg.pictures : kind == MEDIA_MUSIC ? cfg.music : cfg.videos;
    const char *cursor = extra;
    char one[512];
    while (list_next(&cursor, one, sizeof one)) {
        char *p = expand_home(one);
        root_add(out, &n, max, basename_of(p), p);
        free(p);
    }
    if (cfg.scan_removable) {
        static const char *mounts[] = { "/media/*/*", "/media/*", "/run/media/*/*", "/mnt/*" };
        for (size_t m = 0; m < sizeof mounts / sizeof *mounts; m++) {
            glob_t g;
            if (glob(mounts[m], 0, NULL, &g) != 0) continue;
            for (size_t i = 0; i < g.gl_pathc; i++) {
                for (int d = 0; drive_names[kind][d]; d++) {
                    snprintf(path, sizeof path, "%s/%s", g.gl_pathv[i], drive_names[kind][d]);
                    root_add(out, &n, max, basename_of(g.gl_pathv[i]), path);
                }
            }
            globfree(&g);
        }
    }
    return n;
}

static int str_cmp(const void *a, const void *b) { return natural_cmp(*(char *const *)a, *(char *const *)b); }

void media_list(const char *dir, int kind, StrList *dirs, StrList *files)
{
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;
        char path[1400];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        int dir_p = de->d_type == DT_DIR;
        if (de->d_type == DT_UNKNOWN || de->d_type == DT_LNK) dir_p = is_dir(path);
        if (dir_p) {
            if (dirs) sl_add(dirs, path);
        } else if (files && media_is(kind, de->d_name) && files->n < 20000) {
            sl_add(files, path);
        }
    }
    closedir(dp);
    if (dirs && dirs->n > 1) qsort(dirs->v, (size_t)dirs->n, sizeof(char *), str_cmp);
    if (files && files->n > 1) qsort(files->v, (size_t)files->n, sizeof(char *), str_cmp);
}

int mount_points(MediaRoot *out, int max)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    int n = 0;
    char line[1400];
    while (n < max && fgets(line, sizeof line, f)) {
        char dev[512], dir[512];
        if (sscanf(line, "%511s %511s", dev, dir) != 2) continue;
        /* /proc/mounts writes spaces in a path as \040 */
        char clean[512];
        size_t o = 0;
        for (size_t i = 0; dir[i] && o < sizeof clean - 1; i++) {
            if (dir[i] == '\\' && !strncmp(dir + i + 1, "040", 3)) {
                clean[o++] = ' ';
                i += 3;
            } else clean[o++] = dir[i];
        }
        clean[o] = 0;
        if (strncmp(clean, "/media/", 7) && strncmp(clean, "/run/media/", 11) && strncmp(clean, "/mnt/", 5)) continue;
        root_add(out, &n, max, basename_of(clean), clean);
    }
    fclose(f);
    return n;
}

int media_find_exe(const char *name)
{
    if (!name || !*name) return 0;
    if (strchr(name, '/')) return access(name, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    char buf[1200];
    const char *p = path;
    while (*p) {
        size_t n = strcspn(p, ":");
        snprintf(buf, sizeof buf, "%.*s/%s", (int)n, p, name);
        if (access(buf, X_OK) == 0) return 1;
        p += n + (p[n] ? 1 : 0);
    }
    return 0;
}


/* ======================================================================== */
/* Volume, brightness                                                       */
/* ======================================================================== */

int volume_get(void)
{
    char out[256];
    if (in_path("wpctl")) {
        char *a[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };
        if (run_capture(a, out, sizeof out, 2000) == 0) {
            char *p = strstr(out, "Volume:");
            if (p) return (int)lroundf(strtof(p + 7, NULL) * 100.0f);
        }
    }
    if (in_path("amixer")) {
        char *a[] = { "amixer", "sget", "Master", NULL };
        if (run_capture(a, out, sizeof out, 2000) == 0) {
            for (char *p = strchr(out, '['); p; p = strchr(p + 1, '['))
                if (isdigit((unsigned char)p[1]) && strchr(p, '%')) return atoi(p + 1);
        }
    }
    return -1;
}

void volume_set(int pct)
{
    char v[32];
    if (in_path("wpctl")) {
        snprintf(v, sizeof v, "%.2f", pct / 100.0);
        char *a[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", v, NULL };
        if (run_capture(a, NULL, 0, 2000) == 0) return;
    }
    if (in_path("amixer")) {
        snprintf(v, sizeof v, "%d%%", pct);
        char *a[] = { "amixer", "-q", "sset", "Master", v, NULL };
        run_capture(a, NULL, 0, 2000);
    }
}

int volume_muted(void)
{
    char out[256];
    if (in_path("wpctl")) {
        char *a[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL };
        if (run_capture(a, out, sizeof out, 2000) == 0) return strstr(out, "[MUTED]") != NULL;
    }
    if (in_path("amixer")) {
        char *a[] = { "amixer", "sget", "Master", NULL };
        if (run_capture(a, out, sizeof out, 2000) == 0) return strstr(out, "[off]") != NULL;
    }
    return 0;
}

void volume_toggle_mute(void)
{
    if (in_path("wpctl")) {
        char *a[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", NULL };
        if (run_capture(a, NULL, 0, 2000) == 0) return;
    }
    if (in_path("amixer")) {
        char *a[] = { "amixer", "-q", "sset", "Master", "toggle", NULL };
        run_capture(a, NULL, 0, 2000);
    }
}

static char backlight_dir[512];

static int backlight_find(void)
{
    if (*backlight_dir) return 1;
    DIR *dp = opendir("/sys/class/backlight");
    if (!dp) return 0;
    struct dirent *de;
    while ((de = readdir(dp))) {
        if (de->d_name[0] == '.') continue;
        snprintf(backlight_dir, sizeof backlight_dir, "/sys/class/backlight/%s", de->d_name);
        break;
    }
    closedir(dp);
    return *backlight_dir != 0;
}

int brightness_get(void)
{
    if (!backlight_find()) return -1;
    char p[600];
    snprintf(p, sizeof p, "%s/max_brightness", backlight_dir);
    long mx = read_file_long(p, 0);
    snprintf(p, sizeof p, "%s/brightness", backlight_dir);
    long v = read_file_long(p, -1);
    if (mx <= 0 || v < 0) return -1;
    return (int)lround(v * 100.0 / mx);
}

int brightness_set(int pct)
{
    if (!backlight_find()) return -1;
    char p[600];
    snprintf(p, sizeof p, "%s/max_brightness", backlight_dir);
    long mx = read_file_long(p, 0);
    if (mx <= 0) return -1;
    long v = lround(pct * mx / 100.0);
    if (v < 1) v = 1;
    snprintf(p, sizeof p, "%s/brightness", backlight_dir);
    FILE *f = fopen(p, "w");
    if (!f) return -1;
    fprintf(f, "%ld\n", v);
    return fclose(f) == 0 ? 0 : -1;
}

/* ======================================================================== */
/* Wi-Fi (NetworkManager)                                                   */
/* ======================================================================== */

static int nm_split(char *line, char **fields, int max)
{
    int n = 0;
    char *w = line;
    fields[n++] = line;
    for (char *r = line; *r; r++) {
        if (*r == '\\' && r[1]) *w++ = *++r;
        else if (*r == ':' && n < max) {
            *w++ = 0;
            fields[n++] = w;
        } else *w++ = *r;
    }
    *w = 0;
    return n;
}

int wifi_available(void) { return in_path("nmcli"); }

int wifi_radio(void)
{
    if (!wifi_available()) return -1;
    char out[64];
    char *a[] = { "nmcli", "radio", "wifi", NULL };
    if (run_capture(a, out, sizeof out, 3000) != 0) return -1;
    return strncmp(trim(out), "enabled", 7) == 0;
}

void wifi_set_radio(int on)
{
    char *a[] = { "nmcli", "radio", "wifi", on ? "on" : "off", NULL };
    run_capture(a, NULL, 0, 8000);
}

/* ======================================================================== */
/* Video out: the USB port as a webcam showing the screen (pixi-videoout)   */
/* ======================================================================== */

#define VIDEOOUT_HELPER "/usr/local/lib/pixi/pixi-videoout"
#define AUDIO_ROUTE "/usr/local/lib/pixi/pixi-audio-route"

int videoout_running(void)
{
    char pid[32] = "";
    if (read_file_str("/run/pixi-uvc.pid", pid, sizeof pid) <= 0) return 0;
    long p = atol(trim(pid));
    if (p <= 1) return 0;
    char path[64], comm[64] = "";
    snprintf(path, sizeof path, "/proc/%ld/comm", p);
    return read_file_str(path, comm, sizeof comm) > 0 && strstr(comm, "pixi-uvc") != NULL;
}

int videoout_apply(int mode, char *msg, size_t n)
{
    if (msg && n) msg[0] = 0;
    if (access(VIDEOOUT_HELPER, X_OK) != 0) {
        if (msg) snprintf(msg, n, "Video out isn't installed yet. Run Update Pixi first.");
        return mode ? 1 : 0;
    }
    const char *rt = getenv("XDG_RUNTIME_DIR"), *wd = getenv("WAYLAND_DISPLAY");
    char *a[] = { "sudo", "-n", VIDEOOUT_HELPER, mode ? "start" : "stop", (char *)(rt ? rt : ""), (char *)(wd ? wd : "wayland-0"), NULL };
    char out[512];
    int rc = run_capture(a, out, sizeof out, 20000);
    if (access(AUDIO_ROUTE, X_OK) == 0 && (rc == 0 || !mode)) {
        /* sound for the PC: the script goes on in the background, this only starts or stops it */
        char *r[] = { AUDIO_ROUTE, mode ? "start" : "stop", NULL };
        char o2[128];
        run_capture(r, o2, sizeof o2, 5000);
    }
    if (msg && n) {
        /* the helper's last line is the sentence to show */
        char *e = out + strlen(out);
        while (e > out && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
        char *nl = strrchr(out, '\n');
        snprintf(msg, n, "%s", nl ? nl + 1 : out);
        if (rc != 0 && rc != 3 && !msg[0]) snprintf(msg, n, "Video out couldn't be changed.");
    }
    return rc;
}

/* ======================================================================== */
/* Bluetooth (rfkill, and bluetoothctl for the adapter)                     */
/* ======================================================================== */

/* 1 on, 0 off (blocked), -1 if there's no Bluetooth adapter to switch */
int bluetooth_radio(void)
{
    DIR *d = opendir("/sys/class/rfkill");
    if (!d) return -1;
    int found = 0, on = 1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "rfkill", 6) != 0) continue;
        char p[320], type[32] = "";
        snprintf(p, sizeof p, "/sys/class/rfkill/%s/type", e->d_name);
        if (read_file_str(p, type, sizeof type) <= 0 || strcmp(trim(type), "bluetooth") != 0) continue;
        found = 1;
        snprintf(p, sizeof p, "/sys/class/rfkill/%s/soft", e->d_name);
        long soft = read_file_long(p, 0);
        snprintf(p, sizeof p, "/sys/class/rfkill/%s/hard", e->d_name);
        long hard = read_file_long(p, 0);
        if (soft || hard) on = 0;
    }
    closedir(d);
    return found ? on : -1;
}

void bluetooth_set_radio(int on)
{
    if (in_path("rfkill")) {
        char *a[] = { "rfkill", on ? "unblock" : "block", "bluetooth", NULL };
        run_capture(a, NULL, 0, 5000);
    }
    if (in_path("bluetoothctl")) {
        char *a[] = { "bluetoothctl", "power", on ? "on" : "off", NULL };
        run_capture(a, NULL, 0, 5000);
    }
}

void wifi_current(char *ssid, size_t cap)
{
    ssid[0] = 0;
    if (!wifi_available()) return;
    static char out[16384];
    char *a[] = { "nmcli", "-t", "-f", "ACTIVE,SSID", "device", "wifi", "list", "--rescan", "no", NULL };
    if (run_capture(a, out, sizeof out, 4000) != 0) return;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *f[4];
        if (nm_split(line, f, 2) == 2 && !strcmp(f[0], "yes")) {
            sncpy(ssid, cap, f[1]);
            return;
        }
    }
}

int wifi_scan(WifiNet *out, int max, int rescan)
{
    static char saved[16384], list[65536];
    char *sa[] = { "nmcli", "-t", "-f", "NAME,TYPE", "connection", "show", NULL };
    if (run_capture(sa, saved, sizeof saved, 4000) != 0) saved[0] = 0;
    StrList names = { 0 };
    char *save = NULL;
    for (char *line = strtok_r(saved, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *f[4];
        if (nm_split(line, f, 2) == 2 && strstr(f[1], "wireless")) sl_add(&names, f[0]);
    }
    char *a[] = { "nmcli", "-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "device", "wifi", "list", "--rescan",
                  rescan ? "yes" : "no", NULL };
    if (run_capture(a, list, sizeof list, 20000) != 0) list[0] = 0;
    int n = 0;
    save = NULL;
    for (char *line = strtok_r(list, "\n", &save); line && n < max; line = strtok_r(NULL, "\n", &save)) {
        char *f[6];
        if (nm_split(line, f, 4) < 4 || !*f[1]) continue;
        int active = strchr(f[0], '*') != NULL, signal = atoi(f[2]);
        int dupe = -1;
        for (int i = 0; i < n; i++)
            if (!strcmp(out[i].ssid, f[1])) dupe = i;
        if (dupe >= 0) {
            if (signal > out[dupe].signal) out[dupe].signal = signal;
            if (active) out[dupe].active = 1;
            continue;
        }
        WifiNet *w = &out[n++];
        memset(w, 0, sizeof *w);
        sncpy(w->ssid, sizeof w->ssid, f[1]);
        sncpy(w->security, sizeof w->security, f[3]);
        w->signal = signal;
        w->active = active;
        w->secured = *f[3] && strcmp(f[3], "--") != 0;
        for (int i = 0; i < names.n; i++)
            if (!strcmp(names.v[i], f[1])) w->saved = 1;
    }
    sl_free(&names);
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            int ki = out[i].active ? 1000 : out[i].signal, kj = out[j].active ? 1000 : out[j].signal;
            if (kj > ki) {
                WifiNet t = out[i];
                out[i] = out[j];
                out[j] = t;
            }
        }
    }
    return n;
}

int wifi_connect(const char *ssid, const char *password, int saved, char *err, size_t errcap)
{
    char out[1024];
    int rc;
    if (saved && !password) {
        char *a[] = { "nmcli", "connection", "up", "id", (char *)ssid, NULL };
        rc = run_capture(a, out, sizeof out, 45000);
    } else if (password) {
        char *a[] = { "nmcli", "device", "wifi", "connect", (char *)ssid, "password", (char *)password, NULL };
        rc = run_capture(a, out, sizeof out, 45000);
    } else {
        char *a[] = { "nmcli", "device", "wifi", "connect", (char *)ssid, NULL };
        rc = run_capture(a, out, sizeof out, 45000);
    }
    if (rc != 0) {
        char *msg = trim(out);
        char *e = strstr(msg, "Error: ");
        sncpy(err, errcap, e ? e + 7 : (*msg ? msg : "NetworkManager reported an error."));
        if (password) {
            char *d[] = { "nmcli", "connection", "delete", "id", (char *)ssid, NULL };
            run_capture(d, NULL, 0, 5000);
        }
    }
    return rc;
}

void wifi_forget(const char *ssid)
{
    char *a[] = { "nmcli", "connection", "delete", "id", (char *)ssid, NULL };
    run_capture(a, NULL, 0, 10000);
}

/* a new password for a saved network */
int wifi_set_password(const char *ssid, const char *password, char *err, size_t errcap)
{
    char out[512];
    char *a[] = { "nmcli", "connection", "modify", "id", (char *)ssid, "wifi-sec.psk", (char *)password, NULL };
    int rc = run_capture(a, out, sizeof out, 10000);
    if (rc != 0) {
        char *msg = trim(out);
        char *e = strstr(msg, "Error: ");
        sncpy(err, errcap, e ? e + 7 : (*msg ? msg : "NetworkManager reported an error."));
    }
    return rc;
}

/* address, gateway and DNS of a connected network, as lines of text */
void wifi_info(const char *ssid, char *out, size_t cap)
{
    out[0] = 0;
    char raw[2048];
    char *a[] = { "nmcli", "-t", "-f", "IP4.ADDRESS,IP4.GATEWAY,IP4.DNS", "connection", "show", "id", (char *)ssid, NULL };
    if (run_capture(a, raw, sizeof raw, 5000) != 0) return;
    size_t o = 0;
    for (char *save = NULL, *ln = strtok_r(raw, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        char *colon = strchr(ln, ':');
        if (!colon || !colon[1]) continue;
        const char *label = !strncmp(ln, "IP4.ADDRESS", 11) ? "IP address" : !strncmp(ln, "IP4.GATEWAY", 11) ? "Gateway" : !strncmp(ln, "IP4.DNS", 7) ? "DNS" : NULL;
        if (label && o + 80 < cap) o += (size_t)snprintf(out + o, cap - o, "\n%s: %s", label, colon + 1);
    }
}

int wifi_saved(WifiConn *out, int max)
{
    static char raw[16384];
    char *a[] = { "nmcli", "-t", "-f", "NAME,TYPE,ACTIVE", "connection", "show", NULL };
    if (run_capture(a, raw, sizeof raw, 5000) != 0) return 0;
    int n = 0;
    char *save = NULL;
    for (char *line = strtok_r(raw, "\n", &save); line && n < max; line = strtok_r(NULL, "\n", &save)) {
        char *f[4];
        if (nm_split(line, f, 3) < 3 || !strstr(f[1], "wireless")) continue;
        sncpy(out[n].name, sizeof out[n].name, f[0]);
        out[n].active = !strcmp(f[2], "yes");
        n++;
    }
    /* the connected one first, then by name */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (out[j].active > out[i].active || (out[j].active == out[i].active && strcasecmp(out[j].name, out[i].name) < 0)) {
                WifiConn t = out[i];
                out[i] = out[j];
                out[j] = t;
            }
    return n;
}

static void nm_unescape(char *s)
{
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '\\' && r[1]) r++;
        *w++ = *r;
    }
    *w = 0;
}

void wifi_details(const char *name, WifiDetails *d)
{
    memset(d, 0, sizeof *d);
    d->signal = d->channel = -1;
    sncpy(d->name, sizeof d->name, name);
    char raw[4096], keymgmt[64] = "";
    char *a[] = { "nmcli", "-t", "-f", "GENERAL.STATE,802-11-wireless.ssid,802-11-wireless-security.key-mgmt,IP4.ADDRESS,IP4.GATEWAY,IP4.DNS",
                  "connection", "show", "id", (char *)name, NULL };
    if (run_capture(a, raw, sizeof raw, 5000) == 0) {
        char *save = NULL;
        for (char *ln = strtok_r(raw, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
            char *colon = strchr(ln, ':');
            if (!colon) continue;
            *colon = 0;
            char *v = colon + 1;
            nm_unescape(v);
            if (!strcmp(ln, "GENERAL.STATE")) d->active = !strcmp(v, "activated");
            else if (!strcmp(ln, "802-11-wireless.ssid")) sncpy(d->ssid, sizeof d->ssid, v);
            else if (!strcmp(ln, "802-11-wireless-security.key-mgmt")) sncpy(keymgmt, sizeof keymgmt, v);
            else if (!strncmp(ln, "IP4.ADDRESS", 11) && !d->ip[0]) sncpy(d->ip, sizeof d->ip, v);
            else if (!strcmp(ln, "IP4.GATEWAY")) sncpy(d->gateway, sizeof d->gateway, v);
            else if (!strncmp(ln, "IP4.DNS", 7)) {
                size_t o = strlen(d->dns);
                if (o + strlen(v) + 3 < sizeof d->dns) snprintf(d->dns + o, sizeof d->dns - o, "%s%s", o ? ", " : "", v);
            }
        }
    }
    char *slash = strchr(d->ip, '/');
    if (slash) *slash = 0; /* 192.168.1.20/24 -> 192.168.1.20 */
    sncpy(d->security, sizeof d->security,
          !keymgmt[0] ? "None" : !strcmp(keymgmt, "wpa-psk") ? "WPA/WPA2 Personal" : !strcmp(keymgmt, "sae") ? "WPA3 Personal"
          : !strcmp(keymgmt, "none") ? "WEP" : !strcmp(keymgmt, "wpa-eap") ? "WPA Enterprise" : keymgmt);
    if (!d->active) return;
    /* the access point it's on: its address, channel, signal and what it really uses */
    static char list[32768];
    char *b[] = { "nmcli", "-t", "-f", "IN-USE,BSSID,CHAN,SIGNAL,SECURITY", "device", "wifi", "list", "--rescan", "no", NULL };
    if (run_capture(b, list, sizeof list, 5000) != 0) return;
    char *save = NULL;
    for (char *ln = strtok_r(list, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
        char *f[6];
        if (nm_split(ln, f, 5) < 5 || !strchr(f[0], '*')) continue;
        sncpy(d->bssid, sizeof d->bssid, f[1]);
        d->channel = atoi(f[2]);
        d->signal = atoi(f[3]);
        if (*f[4] && strcmp(f[4], "--")) sncpy(d->security, sizeof d->security, f[4]);
        break;
    }
}

int wifi_connect_hidden(const char *ssid, const char *password, char *err, size_t errcap)
{
    char out[1024];
    int rc;
    if (password && *password) {
        char *a[] = { "nmcli", "device", "wifi", "connect", (char *)ssid, "password", (char *)password, "hidden", "yes", NULL };
        rc = run_capture(a, out, sizeof out, 45000);
    } else {
        char *a[] = { "nmcli", "device", "wifi", "connect", (char *)ssid, "hidden", "yes", NULL };
        rc = run_capture(a, out, sizeof out, 45000);
    }
    if (rc != 0) {
        char *msg = trim(out);
        char *e = strstr(msg, "Error: ");
        sncpy(err, errcap, e ? e + 7 : (*msg ? msg : "NetworkManager reported an error."));
        char *d[] = { "nmcli", "connection", "delete", "id", (char *)ssid, NULL };
        run_capture(d, NULL, 0, 5000);
    }
    return rc;
}

void wifi_disconnect(const char *ssid)
{
    char *a[] = { "nmcli", "connection", "down", "id", (char *)ssid, NULL };
    run_capture(a, NULL, 0, 15000);
}

/* ======================================================================== */
/* SysStatus and information                                                   */
/* ======================================================================== */

void status_read(SysStatus *st)
{
    st->battery = -1;
    st->charging = 0;
    st->wifi = -1;
    DIR *dp = opendir("/sys/class/power_supply");
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp))) {
            if (de->d_name[0] == '.') continue;
            char p[512], type[64] = "";
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", de->d_name);
            read_file_str(p, type, sizeof type);
            if (strncmp(type, "Battery", 7)) continue;
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", de->d_name);
            st->battery = (int)read_file_long(p, -1);
            char s[64] = "";
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", de->d_name);
            read_file_str(p, s, sizeof s);
            st->charging = !strncmp(s, "Charging", 8) || !strncmp(s, "Full", 4);
            break;
        }
        closedir(dp);
    }
    FILE *f = fopen("/proc/net/wireless", "r");
    if (f) {
        char line[256];
        int ln = 0;
        while (fgets(line, sizeof line, f)) {
            if (++ln <= 2) continue;
            char ifn[32];
            int stat_;
            float link;
            if (sscanf(line, " %31[^:]: %x %f", ifn, &stat_, &link) == 3) {
                int q = (int)link;
                st->wifi = q < 30 ? 0 : q < 45 ? 1 : q < 58 ? 2 : 3;
                break;
            }
        }
        fclose(f);
    }
}

void ip_address(char *out, size_t cap)
{
    struct ifaddrs *ifa = NULL;
    out[0] = 0;
    if (getifaddrs(&ifa) == 0) {
        size_t len = 0;
        for (struct ifaddrs *p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !strcmp(p->ifa_name, "lo")) continue;
            char a[64];
            inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, a, sizeof a);
            int n = snprintf(out + len, cap - len, "%s%s", len ? ", " : "", a);
            if (n < 0 || (size_t)n >= cap - len) break;
            len += (size_t)n;
        }
        freeifaddrs(ifa);
    }
    if (!out[0]) sncpy(out, cap, "Not connected");
}

void os_name(char *out, size_t cap)
{
    char osr[4096];
    sncpy(out, cap, "Linux");
    if (read_file_str("/etc/os-release", osr, sizeof osr) <= 0) return;
    char *p = strstr(osr, "PRETTY_NAME=");
    if (!p) return;
    p += 12;
    if (*p == '"') p++;
    size_t n = strcspn(p, "\"\n");
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
}
