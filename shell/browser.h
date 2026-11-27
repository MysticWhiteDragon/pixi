/* A web browser engine for the shell: Chromium, run without a window and driven over its DevTools protocol.
 * The shell draws everything itself (page picture, tabs, menus, cursor); this file only talks to Chromium. */
#ifndef PIXI_BROWSER_H
#define PIXI_BROWSER_H

#include <stddef.h>

#define BR_MAX_TABS 16

typedef struct {
    char title[160], url[600];
    int loading;
} BrTab;

typedef struct {
    int state; /* 0 = off, 1 = starting, 2 = ready, 3 = failed */
    char error[200];
    int ntabs, active;
    BrTab tabs[BR_MAX_TABS];
    int can_back, can_forward;
} BrState;

/* Find a Chromium-family browser. Returns 0 and fills path if there is one. */
int br_find(const char *configured, char *path, size_t cap);
int br_start(const char *path, const char *profile_dir, int width, int height);
void br_stop(void);
void br_get(BrState *out);

/* The newest page picture, RGBA. Hold the lock while reading it. */
int br_frame_lock(const unsigned char **rgba, int *w, int *h, unsigned *seq);
void br_frame_unlock(void);

/* Commands. They go to Chromium in order. */
void br_navigate(const char *url);
void br_back(void);
void br_forward(void);
void br_reload(void);
void br_stop_loading(void);
void br_new_tab(const char *url);
void br_close_tab(int index);
void br_switch_tab(int index);
void br_mouse_move(int x, int y);
void br_click(int x, int y);
void br_wheel(int x, int y, int dx, int dy);
void br_key(const char *name); /* "Enter", "Backspace", "Escape", "Tab" */
void br_set_viewport(int css_width, int mobile);
void br_clear_data(void);

/* Ask what's under the cursor after a click. If it's a text box, br_field() reports it once. */
void br_probe_field(void);
int br_field(int *password, char *text, size_t cap); /* 1 once per probe that found a text box */
void br_set_field(const char *text);

#endif
