/*
 * pixi-shell: everything that talks to the system rather than the screen.
 * Configuration, installed apps, the game library, Wi-Fi, volume,
 * brightness, battery and system information.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#ifndef PIXI_SYS_H
#define PIXI_SYS_H

#include <stddef.h>
#include <sys/types.h>

/* ---------- small utilities ---------- */
void die(const char *fmt, ...);
void *xmalloc(size_t n);
void *xcalloc(size_t a, size_t b);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xasprintf(const char *fmt, ...);
void sncpy(char *dst, size_t cap, const char *src);
char *trim(char *s);
const char *home_dir(void);
char *expand_home(const char *p);
int file_exists(const char *p);
int is_dir(const char *p);
void mkdir_p(const char *path);
int in_path(const char *name);
int read_file_str(const char *path, char *out, size_t cap);
long read_file_long(const char *path, long fallback);
int list_has(const char *list, const char *word);
int list_next(const char **cursor, char *out, size_t cap);
const char *basename_of(const char *p);
unsigned hash_str(const char *s);
int natural_cmp(const char *a, const char *b);
int run_capture(char *const argv[], char *out, size_t cap, int timeout_ms);
char **argv_dup(char *const *a);
void argv_free(char **a);
char **exec_to_argv(const char *exec);

typedef struct {
    char **v;
    int n, cap;
} StrList;
void sl_add(StrList *l, const char *s);
void sl_free(StrList *l);

/* ---------- configuration ---------- */
typedef struct {
    char id[32], name[64], dirs[256], exts[160], cores[256], pkg[64];
} System;

typedef struct {
    char id[64], name[128], exec[512], icon[256], category[32];
} CustomApp;

typedef struct {
    char theme[32];        /* purple blue green yellow red teal indigo pink orange */
    int full_material;     /* 0 = Simple (silk, flat marks), 1 = full (water, bead marks) */
    int material_explicit; /* chosen by the user, so never changed automatically */
    int sounds;
    int confirm_east;
    int clock24;
    float ui_scale;        /* 0 = automatic */
    int show_terminal_apps;
    char hide[1024];
    char retroarch[256];
    char rom_root[512];
    char core_dirs[1024];
    int joy_ok, joy_back, joy_menu, joy_pgup, joy_pgdn, joy_start, joy_select, joy_guide;
    int debug_input;
    int category;          /* remembered */
} Config;

extern Config cfg;
extern System *systems;
extern int nsystems;
extern CustomApp *custom_apps;
extern int ncustom;

void cfg_defaults(void);
void cfg_load(const char *path);
void state_load(void);
void state_save(void);

/* ---------- desktop entries ---------- */
enum {
    CAT_SETTINGS, CAT_GAMES, CAT_INTERNET, CAT_MULTIMEDIA, CAT_GRAPHICS, CAT_OFFICE,
    CAT_DEVELOPMENT, CAT_EDUCATION, CAT_UTILITIES, CAT_SYSTEM, CAT_OTHER, CAT_COUNT
};
extern const char *cat_names[CAT_COUNT];
extern const char *cat_glyphs[CAT_COUNT];

typedef struct {
    char id[256];
    char name[256], generic[256], comment[256];
    char exec[1024], icon[256], categories[512];
    int category;
    int terminal;
} AppEntry;

typedef struct {
    AppEntry *v;
    int n, cap;
} AppList;

extern AppList apps;
void apps_scan(void);

/* ---------- game library ---------- */
char *find_core(const System *s);
void system_roms(const System *s, StrList *out, int limit);
void rom_title(const char *path, char *title, size_t tcap, char *tags, size_t gcap);

/* ---------- status ---------- */
int volume_get(void);
void volume_set(int pct);
int brightness_get(void);
int brightness_set(int pct);

typedef struct {
    char ssid[128];
    char security[64];
    int signal, secured, saved, active;
} WifiNet;
int wifi_available(void);
int wifi_radio(void);            /* 1 on, 0 off, -1 unknown */
void wifi_set_radio(int on);
void wifi_current(char *ssid, size_t cap);
int wifi_scan(WifiNet *out, int max, int rescan);
int wifi_connect(const char *ssid, const char *password, int saved, char *err, size_t errcap);
void wifi_disconnect(const char *ssid);

typedef struct {
    int battery;   /* -1 when there is none */
    int charging;
    int wifi;      /* -1 none, else 0..3 bars */
} SysStatus;
void status_read(SysStatus *st);

void ip_address(char *out, size_t cap);
void os_name(char *out, size_t cap);

#endif
