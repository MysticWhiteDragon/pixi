/*
 * pixi-shell - the Pixi ("Pi Cross Interface") shell.
 *
 * A Wayland client of the pixi compositor, drawn with OpenGL ES 3 through
 * SDL2. Its look follows LineXinBar's shell (GPL-3.0-only): the same layout
 * measurements, theme colours, glyphs, sounds and GPU materials, ported to C.
 *
 * The compositor shows every application full screen. This shell is the
 * start screen (categories across, items down), the guide overlay drawn over
 * a running application, and the launch screen in between; it asks the
 * compositor which of those is on screen through the pixi_shell_v1 protocol.
 *
 * Copyright (C) 2026 Pixi contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _GNU_SOURCE
#include <SDL.h>
#include <SDL_image.h>
#include <SDL_mixer.h>
#include <SDL_syswm.h>
#include <SDL_ttf.h>
#include <GLES3/gl3.h>
#include <wayland-client.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "pixi-shell-v1-client-protocol.h"
#include "shaders.h"
#include "sys.h"

#define PIXI_VERSION "1.0.0"
#ifndef PIXI_DATADIR
#define PIXI_DATADIR "/usr/local/share/pixi"
#endif

/* ======================================================================== */
/* LineXinBar's layout, in pixels at 1080 lines (crates/lxb-desktop/src/ui.rs) */
/* ======================================================================== */

#define REF_H 1080.0f
#define CATEGORY_SPACING 200.0f
#define ITEM_SPACING 124.0f
#define CATEGORY_ICON 84.0f
#define CATEGORY_ICON_FOCUSED 148.0f
#define ITEM_ICON 64.0f
#define ITEM_ICON_FOCUSED 105.0f
#define ITEM_DISC 1.4707913f /* sqrt(2) * 1.04 */
#define CATEGORY_DISC 1.30f
#define GLYPH_DEPTH 0.075f
#define CATEGORY_LABEL 26.0f
#define CATEGORY_LABEL_ABOVE 3.0f
#define CATEGORY_LABEL_BELOW 25.0f
#define ITEM_GAP_ABOVE 168.0f
#define CATEGORY_HALF (CATEGORY_ICON_FOCUSED * CATEGORY_DISC / 2.0f)
#define CATEGORY_TO_LABEL (CATEGORY_HALF + CATEGORY_LABEL_ABOVE + CATEGORY_LABEL)
#define ITEM_GAP_BELOW (CATEGORY_TO_LABEL + CATEGORY_LABEL_BELOW + ITEM_ICON_FOCUSED * ITEM_DISC / 2.0f)
#define CORNER_INSET 48.0f
#define CORNER_TOP 36.0f
#define CORNER_CLOCK 24.0f
#define CORNER_MARK 32.0f
#define BAR_CROSS_X 0.22f
#define BAR_CROSS_Y 0.30f
#define COLUMN_MARGIN (ITEM_ICON_FOCUSED * ITEM_DISC / 2.0f + 24.0f)
#define DEPTH_SHRINK 0.86f
#define DEPTH_HAZE 0.72f
#define CATEGORY_MIN_ALPHA 0.62f
#define PULSE_PERIOD 1.8f
#define DEPTH_CONTROL 9.0f
#define DEPTH_PANEL 22.0f
#define FROST_PANEL 0.95f
#define FROST_CONTROL 0.2f
#define GLOSS_FULL 1.0f
#define GLOSS_QUIET 0.45f
#define DEPTH_SIDEBAR 15.0f
#define FROST_SIDEBAR 0.46f
#define GLOSS_SIDEBAR 0.66f
#define SIDEBAR_STAIN 0.38f
#define HINT_GLYPH 27.0f
#define HINT_LABEL 19.0f
#define HINT_GAP 8.0f
#define HINT_PADDING 15.0f
#define HINT_MARGIN 26.0f
#define GUIDE_ROW_HEIGHT 76.0f
#define GUIDE_BAR_HEIGHT 62.0f
#define GUIDE_TILE 68.0f
#define GUIDE_TILE_GAP 14.0f
#define GUIDE_TILE_GLYPH 0.62f
#define GUIDE_TILE_RADIUS 0.30f
#define GUIDE_MARGIN 24.0f
#define GUIDE_PADDING 30.0f
#define POWER_BUTTON 66.0f
#define POWER_WIDTH 460.0f
#define POWER_ROW 64.0f
#define LAUNCH_ICON 168.0f
#define LAUNCH_DOTS 12
#define LAUNCH_SPIN 1.4f
#define LAUNCH_NAME 62.0f
#define LAUNCH_DOING 25.0f
#define SQUIRCLE 4.0f
#define CIRCLE 2.0f

/* ======================================================================== */
/* Theme (crates/lxb-desktop/src/theme.rs), sRGB as authored                */
/* ======================================================================== */

typedef struct {
    const char *name, *label;
    unsigned accent, accent_soft, accent_deep, glass, glass_raised, text_soft;
    unsigned sky[4], glow;
} ThemeDef;

static const ThemeDef THEMES[] = {
    { "purple", "Purple", 0x8B5CF6, 0xC4B5FD, 0x4C1D95, 0x140B26, 0xB9A7E8, 0xD6CBEF, { 0x1B1140, 0x060314, 0x2A1252, 0x0A0620 }, 0x5B21B6 },
    { "blue", "Blue", 0x3B82F6, 0x93C5FD, 0x1E3A8A, 0x081026, 0xA7C6E8, 0xCBDDEF, { 0x0F1B40, 0x030614, 0x122A52, 0x060A20 }, 0x1E40AF },
    { "green", "Green", 0x16A34A, 0x91D39F, 0x14532D, 0x06170D, 0xA7C8B1, 0xC7DFCE, { 0x082114, 0x020A05, 0x0A3018, 0x030F08 }, 0x14532D },
    { "yellow", "Yellow", 0xCA8A04, 0xDFBC73, 0x713F12, 0x151002, 0xD8C389, 0xE9DFC3, { 0x292008, 0x0D0A02, 0x382B0A, 0x151003 }, 0x713F12 },
    { "red", "Red", 0xEF4444, 0xFCA5A5, 0x7F1D1D, 0x230707, 0xD8A7A7, 0xE9CCCC, { 0x3A0D0D, 0x100202, 0x501010, 0x1E0505 }, 0x991B1B },
    { "teal", "Teal", 0x14B8A6, 0x5EEAD4, 0x134E4A, 0x051A19, 0xA7CFC9, 0xC7E0DB, { 0x08302B, 0x020F0D, 0x0B4038, 0x041614 }, 0x115E59 },
    { "indigo", "Indigo", 0xA5B4FC, 0xC7D2FE, 0x312E81, 0x0D0E22, 0xB3B9DE, 0xD5D9F2, { 0x101128, 0x030409, 0x15163A, 0x070813 }, 0x252270 },
    { "pink", "Pink", 0xEC4899, 0xF9A8D4, 0x831843, 0x200714, 0xE0A7C4, 0xF0CCE0, { 0x2C0A1E, 0x0C0207, 0x3E0D28, 0x160410 }, 0x861042 },
    { "orange", "Orange", 0xF97316, 0xFDBA74, 0x7C2D12, 0x1D0B02, 0xE8C3A7, 0xF0DAC4, { 0x2A1306, 0x0D0602, 0x3A1A08, 0x160A03 }, 0x7C2D12 },
};
#define NTHEMES ((int)(sizeof THEMES / sizeof THEMES[0]))

typedef struct { float r, g, b, a; } Col;

static struct {
    Col accent, accent_soft, accent_deep, glass, glass_raised, text, text_soft;
    float sky[4][3], accent3[3][3], glow[3];
} th;

static float lin(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

static Col hexcol(unsigned rgb, float a)
{
    Col c = { lin(((rgb >> 16) & 255) / 255.0f), lin(((rgb >> 8) & 255) / 255.0f), lin((rgb & 255) / 255.0f), a };
    return c;
}

static Col with_alpha(Col c, float a)
{
    c.a = a;
    return c;
}

static const ThemeDef *theme_def(void)
{
    for (int i = 0; i < NTHEMES; i++)
        if (!strcasecmp(THEMES[i].name, cfg.theme)) return &THEMES[i];
    return &THEMES[0];
}

static void theme_apply(void)
{
    const ThemeDef *t = theme_def();
    th.accent = hexcol(t->accent, 1);
    th.accent_soft = hexcol(t->accent_soft, 1);
    th.accent_deep = hexcol(t->accent_deep, 1);
    th.glass = hexcol(t->glass, 1);
    th.glass_raised = hexcol(t->glass_raised, 1);
    th.text = hexcol(0xFFFFFF, 1);
    th.text_soft = hexcol(t->text_soft, 1);
    for (int i = 0; i < 4; i++) {
        Col c = hexcol(t->sky[i], 1);
        th.sky[i][0] = c.r, th.sky[i][1] = c.g, th.sky[i][2] = c.b;
    }
    Col a3[3] = { th.accent, th.accent_soft, th.accent_deep };
    for (int i = 0; i < 3; i++) th.accent3[i][0] = a3[i].r, th.accent3[i][1] = a3[i].g, th.accent3[i][2] = a3[i].b;
    Col g = hexcol(t->glow, 1);
    th.glow[0] = g.r, th.glow[1] = g.g, th.glow[2] = g.b;
}

/* ======================================================================== */
/* Window and GL                                                            */
/* ======================================================================== */

static SDL_Window *win;
static SDL_GLContext glctx;
static int W, H;            /* drawable size in pixels */
static float S = 1;         /* layout scale */
static float now_s;         /* seconds since start */
static unsigned frame_no;
static char datadir[1024];
static int windowed_w, windowed_h;

static GLuint prog_bg, prog_copy, prog_present, prog_quad;
static GLuint vao_empty, vao_quad, vbo_corners, vbo_inst;
static GLuint fb_bg, tex_bg, fb_scene, tex_scene, tex_white;
static int fb_w, fb_h;

static GLuint compile(GLenum type, const char *src, const char *name)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        die("shader %s failed to compile:\n%s", name, log);
    }
    return s;
}

static GLuint link_prog(const char *vs, const char *fs, const char *name)
{
    GLuint p = glCreateProgram();
    GLuint v = compile(GL_VERTEX_SHADER, vs, name), f = compile(GL_FRAGMENT_SHADER, fs, name);
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        die("shader %s failed to link:\n%s", name, log);
    }
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

static void make_target(GLuint *fb, GLuint *tex, int w, int h, int mips)
{
    if (*tex) glDeleteTextures(1, tex);
    if (*fb) glDeleteFramebuffers(1, fb);
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    int levels = 1;
    if (mips) {
        int m = w > h ? w : h;
        while (m > 1 && levels < 6) {
            m >>= 1;
            levels++;
        }
    }
    glTexStorage2D(GL_TEXTURE_2D, levels, GL_SRGB8_ALPHA8, w, h);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, fb);
    glBindFramebuffer(GL_FRAMEBUFFER, *fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) die("render target incomplete");
}

static void targets_fit(void)
{
    if (fb_w == W && fb_h == H) return;
    fb_w = W;
    fb_h = H;
    make_target(&fb_bg, &tex_bg, W, H, 1);
    make_target(&fb_scene, &tex_scene, W, H, 0);
}

static void set_common_uniforms(GLuint p, float rw, float rh, float blur)
{
    glUniform2f(glGetUniformLocation(p, "u_res"), rw, rh);
    glUniform1f(glGetUniformLocation(p, "u_time"), now_s);
    glUniform1f(glGetUniformLocation(p, "u_blur"), blur);
    glUniform3fv(glGetUniformLocation(p, "u_sky"), 4, &th.sky[0][0]);
    glUniform3fv(glGetUniformLocation(p, "u_accent"), 3, &th.accent3[0][0]);
    glUniform3fv(glGetUniformLocation(p, "u_glow"), 1, th.glow);
    glUniform4f(glGetUniformLocation(p, "u_style"), cfg.full_material ? 0.0f : 1.0f, cfg.full_material ? 0.0f : 1.0f, 0, 0);
}

static void gl_init(void)
{
    prog_bg = link_prog(VS_FULLSCREEN, FS_BACKGROUND, "background");
    prog_copy = link_prog(VS_FULLSCREEN, FS_BACKDROP_COPY, "copy");
    prog_present = link_prog(VS_FULLSCREEN, FS_PRESENT, "present");
    prog_quad = link_prog(VS_QUAD, FS_QUAD, "quad");
    glGenVertexArrays(1, &vao_empty);
    glGenVertexArrays(1, &vao_quad);
    glBindVertexArray(vao_quad);
    static const float corners[12] = { 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 0, 1 };
    glGenBuffers(1, &vbo_corners);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_corners);
    glBufferData(GL_ARRAY_BUFFER, sizeof corners, corners, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glGenBuffers(1, &vbo_inst);
    glBindVertexArray(0);
    unsigned char white[4] = { 255, 255, 255, 255 };
    glGenTextures(1, &tex_white);
    glBindTexture(GL_TEXTURE_2D, tex_white);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

/* ---------- quads ------------------------------------------------------- */

typedef struct {
    float rect[4], uv[4], color[4], shape[4], material[4], misc[4];
} QuadInst;

#define MAXQ 4096
static QuadInst qbuf[MAXQ];
static GLuint qtex[MAXQ];
static int qn;
static float quad_blur; /* the softness the wallpaper is drawn at, for glass */
static float quad_hole[4], quad_hole_radius; /* the guide's card, where nothing is drawn */

enum { KIND_PLAIN = 0, KIND_GLYPH = 1, KIND_GLOW = 2 };

static QuadInst *quad_new(GLuint tex)
{
    if (qn >= MAXQ) return NULL;
    QuadInst *q = &qbuf[qn];
    memset(q, 0, sizeof *q);
    q->uv[2] = q->uv[3] = 1;
    q->material[3] = 1;
    q->misc[0] = CIRCLE;
    q->misc[2] = -1;
    qtex[qn++] = tex ? tex : tex_white;
    return q;
}

static void set4(float *d, float a, float b, float c, float e) { d[0] = a, d[1] = b, d[2] = c, d[3] = e; }

/* A pane of glass (or a flat rounded shape when thickness and gloss are 0). */
static void pane(float x, float y, float w, float h, float radius, float power, Col c, float thickness, float frost,
                 float gloss, float fade, float curve)
{
    if (fade <= 0.003f || w <= 0 || h <= 0) return;
    QuadInst *q = quad_new(0);
    if (!q) return;
    set4(q->rect, x, y, w, h);
    set4(q->color, c.r, c.g, c.b, c.a);
    set4(q->shape, radius, 0, 0, frost);
    set4(q->material, thickness, quad_blur, gloss, fade);
    set4(q->misc, power, curve, -1, KIND_PLAIN);
}

static void outline(float x, float y, float w, float h, float radius, float border, Col c, float fade)
{
    if (fade <= 0.003f) return;
    QuadInst *q = quad_new(0);
    if (!q) return;
    set4(q->rect, x, y, w, h);
    set4(q->color, c.r, c.g, c.b, c.a);
    set4(q->shape, radius, border, 0, 0);
    set4(q->material, 0, 0, 0, fade);
    set4(q->misc, CIRCLE, 0, -1, KIND_PLAIN);
}

static void glow(float cx, float cy, float size, Col c, float fade)
{
    if (fade <= 0.003f || c.a <= 0.003f) return;
    QuadInst *q = quad_new(0);
    if (!q) return;
    set4(q->rect, cx - size / 2, cy - size / 2, size, size);
    set4(q->color, c.r, c.g, c.b, c.a);
    set4(q->material, 0, 0, 0, fade);
    set4(q->misc, CIRCLE, 0, -1, KIND_GLOW);
}

static void picture(GLuint tex, float x, float y, float w, float h, Col c, float fade)
{
    if (!tex || fade <= 0.003f) return;
    QuadInst *q = quad_new(tex);
    if (!q) return;
    set4(q->rect, x, y, w, h);
    set4(q->color, c.r, c.g, c.b, c.a);
    set4(q->material, 0, 0, 0, fade);
    set4(q->misc, CIRCLE, 0, -1, KIND_PLAIN);
}

static void quads_flush(void)
{
    if (!qn) return;
    glUseProgram(prog_quad);
    set_common_uniforms(prog_quad, (float)W, (float)H, quad_blur);
    glUniform1i(glGetUniformLocation(prog_quad, "u_tex"), 0);
    glUniform1i(glGetUniformLocation(prog_quad, "u_backdrop"), 1);
    glUniform4fv(glGetUniformLocation(prog_quad, "u_hole"), 1, quad_hole);
    glUniform1f(glGetUniformLocation(prog_quad, "u_hole_radius"), quad_hole_radius);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_bg);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(vao_quad);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_inst);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(QuadInst) * (size_t)qn), qbuf, GL_STREAM_DRAW);
    int start = 0;
    while (start < qn) {
        int end = start + 1;
        while (end < qn && qtex[end] == qtex[start]) end++;
        size_t base = sizeof(QuadInst) * (size_t)start;
        for (int a = 0; a < 6; a++) {
            glEnableVertexAttribArray((GLuint)(1 + a));
            glVertexAttribPointer((GLuint)(1 + a), 4, GL_FLOAT, GL_FALSE, sizeof(QuadInst),
                                  (const void *)(base + sizeof(float) * 4 * (size_t)a));
            glVertexAttribDivisor((GLuint)(1 + a), 1);
        }
        glBindTexture(GL_TEXTURE_2D, qtex[start]);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, end - start);
        start = end;
    }
    glBindVertexArray(0);
    qn = 0;
}

/* ---------- glyphs (distance fields) ------------------------------------ */

typedef struct {
    char name[64];
    GLuint tex;
} GlyphTex;
static GlyphTex glyph_cache[256];
static int nglyphs;

static GLuint glyph_tex(const char *name)
{
    if (!name || !*name) return 0;
    for (int i = 0; i < nglyphs; i++)
        if (!strcmp(glyph_cache[i].name, name)) return glyph_cache[i].tex;
    if (nglyphs >= 256) return 0;
    GlyphTex *g = &glyph_cache[nglyphs++];
    sncpy(g->name, sizeof g->name, name);
    g->tex = 0;
    char path[1200];
    snprintf(path, sizeof path, "%s/glyphs/%s.png", datadir, name);
    SDL_Surface *raw = IMG_Load(path);
    if (!raw) {
        fprintf(stderr, "pixi-shell: missing glyph %s\n", path);
        return 0;
    }
    SDL_Surface *s = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(raw);
    if (!s) return 0;
    unsigned char *red = xmalloc((size_t)(s->w * s->h));
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) red[y * s->w + x] = ((unsigned char *)s->pixels)[y * s->pitch + x * 4];
    glGenTextures(1, &g->tex);
    glBindTexture(GL_TEXTURE_2D, g->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, s->w, s->h, 0, GL_RED, GL_UNSIGNED_BYTE, red);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(red);
    SDL_FreeSurface(s);
    return g->tex;
}

/* One of the shell's own marks, shaded out of its own shape. */
static void glyph(const char *name, float cx, float cy, float size, Col c, float fade)
{
    GLuint t = glyph_tex(name);
    if (!t || fade <= 0.003f) return;
    QuadInst *q = quad_new(t);
    if (!q) return;
    set4(q->rect, cx - size / 2, cy - size / 2, size, size);
    set4(q->color, c.r, c.g, c.b, c.a);
    set4(q->shape, 0, 0, 0, 0);
    set4(q->material, size * GLYPH_DEPTH, 0, GLOSS_FULL, fade);
    set4(q->misc, CIRCLE, 0, -1, KIND_GLYPH);
}

/* ---------- application icons ------------------------------------------- */

typedef struct {
    char *name;
    GLuint tex;
} IconTex;
static IconTex *icon_cache;
static int nicons, cap_icons;

static int find_icon_file(const char *name, char *out, size_t cap)
{
    if (!name || !*name) return 0;
    if (name[0] == '/' || name[0] == '~') {
        char *p = expand_home(name);
        int ok = access(p, R_OK) == 0;
        if (ok) sncpy(out, cap, p);
        free(p);
        return ok;
    }
    static const char *sizes[] = { "256x256", "192x192", "128x128", "scalable", "96x96", "64x64", "48x48" };
    static const char *ctxs[] = { "apps", "places", "devices", "categories" };
    static const char *exts[] = { "png", "svg" };
    char bases[3][512];
    snprintf(bases[0], sizeof bases[0], "%s/.local/share/icons/hicolor", home_dir());
    snprintf(bases[1], sizeof bases[1], "/usr/local/share/icons/hicolor");
    snprintf(bases[2], sizeof bases[2], "/usr/share/icons/hicolor");
    for (int b = 0; b < 3; b++) {
        if (!is_dir(bases[b])) continue;
        for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++)
            for (size_t c = 0; c < sizeof ctxs / sizeof *ctxs; c++)
                for (int e = 0; e < 2; e++) {
                    snprintf(out, cap, "%s/%s/%s/%s.%s", bases[b], sizes[s], ctxs[c], name, exts[e]);
                    if (access(out, R_OK) == 0) return 1;
                }
    }
    for (int e = 0; e < 2; e++) {
        snprintf(out, cap, "/usr/share/pixmaps/%s.%s", name, exts[e]);
        if (access(out, R_OK) == 0) return 1;
    }
    return 0;
}

static GLuint icon_tex(const char *name)
{
    if (!name || !*name) return 0;
    for (int i = 0; i < nicons; i++)
        if (!strcmp(icon_cache[i].name, name)) return icon_cache[i].tex;
    if (nicons == cap_icons) {
        cap_icons = cap_icons ? cap_icons * 2 : 32;
        icon_cache = xrealloc(icon_cache, sizeof(IconTex) * (size_t)cap_icons);
    }
    IconTex *ic = &icon_cache[nicons++];
    ic->name = xstrdup(name);
    ic->tex = 0;
    char path[1024];
    if (!find_icon_file(name, path, sizeof path)) return 0;
    SDL_Surface *raw = NULL;
    size_t pl = strlen(path);
    if (pl > 4 && !strcasecmp(path + pl - 4, ".svg")) {
        SDL_RWops *rw = SDL_RWFromFile(path, "rb");
        if (rw) {
            raw = IMG_LoadSizedSVG_RW(rw, 256, 256);
            SDL_RWclose(rw);
        }
    } else {
        raw = IMG_Load(path);
    }
    if (!raw) return 0;
    SDL_Surface *s = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(raw);
    if (!s) return 0;
    glGenTextures(1, &ic->tex);
    glBindTexture(GL_TEXTURE_2D, ic->tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, s->pitch / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, s->w, s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    SDL_FreeSurface(s);
    return ic->tex;
}

/* ---------- text -------------------------------------------------------- */

typedef struct TextTex {
    char *s;
    int size, bold;
    GLuint tex;
    int w, h;
    unsigned last;
    struct TextTex *next;
} TextTex;

#define TEXT_BUCKETS 512
static TextTex *text_cache[TEXT_BUCKETS];

typedef struct {
    int size, bold, cjk;
    TTF_Font *font;
} FontEntry;
static FontEntry fonts[64];
static int nfonts;

static const char *cjk_regular[] = { "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", NULL };
static const char *cjk_bold[] = { "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
                                  "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", NULL };

static int needs_cjk(const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p >= 0xE2 && *p < 0xF0 && p[1] && p[2]) {
            unsigned cp = ((unsigned)(*p & 0x0F) << 12) | ((unsigned)(p[1] & 0x3F) << 6) | (unsigned)(p[2] & 0x3F);
            if (cp >= 0x2E80) return 1;
        }
    }
    return 0;
}

static TTF_Font *font_get(int size, int bold, int cjk)
{
    if (size < 6) size = 6;
    for (int i = 0; i < nfonts; i++)
        if (fonts[i].size == size && fonts[i].bold == bold && fonts[i].cjk == cjk) return fonts[i].font;
    TTF_Font *f = NULL;
    if (cjk) {
        for (const char **p = bold ? cjk_bold : cjk_regular; *p && !f; p++) f = TTF_OpenFontIndex(*p, size, 0);
    }
    if (!f) {
        char path[1200];
        snprintf(path, sizeof path, "%s/fonts/%s", datadir, bold ? "Roboto-Bold.ttf" : "Roboto-Regular.ttf");
        f = TTF_OpenFont(path, size);
        if (!f) die("couldn't open font %s: %s", path, TTF_GetError());
    }
    TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
    if (nfonts < 64) {
        fonts[nfonts].size = size;
        fonts[nfonts].bold = bold;
        fonts[nfonts].cjk = cjk;
        fonts[nfonts].font = f;
        nfonts++;
    }
    return f;
}

static int text_px(float size) { return (int)lroundf(size); }

static int text_width(const char *s, float size, int bold)
{
    if (!s || !*s) return 0;
    int w = 0;
    TTF_SizeUTF8(font_get(text_px(size), bold, needs_cjk(s)), s, &w, NULL);
    return w;
}

static TextTex *text_get(const char *s, float fsize, int bold)
{
    if (!s || !*s) return NULL;
    int size = text_px(fsize);
    unsigned h = (hash_str(s) ^ (unsigned)(size * 2654435761u) ^ (unsigned)bold) % TEXT_BUCKETS;
    for (TextTex *t = text_cache[h]; t; t = t->next) {
        if (t->size == size && t->bold == bold && !strcmp(t->s, s)) {
            t->last = frame_no;
            return t;
        }
    }
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *raw = TTF_RenderUTF8_Blended(font_get(size, bold, needs_cjk(s)), s, white);
    if (!raw) return NULL;
    SDL_Surface *surf = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(raw);
    if (!surf) return NULL;
    TextTex *t = xcalloc(1, sizeof *t);
    t->s = xstrdup(s);
    t->size = size;
    t->bold = bold;
    t->w = surf->w;
    t->h = surf->h;
    t->last = frame_no;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, surf->pitch / 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, surf->w, surf->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    SDL_FreeSurface(surf);
    t->next = text_cache[h];
    text_cache[h] = t;
    return t;
}

static void text_cache_trim(void)
{
    for (int b = 0; b < TEXT_BUCKETS; b++) {
        TextTex **pp = &text_cache[b];
        while (*pp) {
            TextTex *t = *pp;
            if (frame_no - t->last > 900) {
                *pp = t->next;
                glDeleteTextures(1, &t->tex);
                free(t->s);
                free(t);
            } else {
                pp = &t->next;
            }
        }
    }
}

static const char *fit_text(const char *s, float size, int bold, int maxw, char *buf, size_t cap)
{
    if (!s) return "";
    if (maxw <= 0 || text_width(s, size, bold) <= maxw) return s;
    size_t len = strlen(s), lo = 0, hi = len;
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2, m = mid;
        while (m > 0 && ((unsigned char)s[m] & 0xC0) == 0x80) m--;
        char tmp[1024];
        if (m + 4 > sizeof tmp) m = sizeof tmp - 4;
        memcpy(tmp, s, m);
        strcpy(tmp + m, "\xE2\x80\xA6");
        if (text_width(tmp, size, bold) <= maxw) lo = mid;
        else hi = mid - 1;
    }
    size_t m = lo;
    while (m > 0 && ((unsigned char)s[m] & 0xC0) == 0x80) m--;
    while (m > 0 && s[m - 1] == ' ') m--;
    if (m + 4 > cap) m = cap - 4;
    memcpy(buf, s, m);
    strcpy(buf + m, "\xE2\x80\xA6");
    return buf;
}

enum { AL_LEFT, AL_CENTER, AL_RIGHT };

/* Text with its top edge at y. Returns the width drawn. */
static float text(const char *s, float x, float y, float size, int bold, Col c, float fade, int align, int maxw)
{
    if (!s || !*s || fade <= 0.003f) return 0;
    char buf[1024];
    const char *shown = fit_text(s, size, bold, maxw, buf, sizeof buf);
    TextTex *t = text_get(shown, size, bold);
    if (!t) return 0;
    float dx = x;
    if (align == AL_CENTER) dx -= t->w * 0.5f;
    else if (align == AL_RIGHT) dx -= (float)t->w;
    /* SDL_ttf's surface carries the ascent above the cap height; lift it so
     * `y` is close to where the capitals start, as LineXinBar positions it. */
    float lift = size * 0.18f;
    picture(t->tex, floorf(dx), floorf(y - lift), (float)t->w, (float)t->h, c, fade);
    return (float)t->w;
}

static int wrap_text(const char *s, float size, int maxw, char lines[][256], int maxlines)
{
    int n = 0;
    while (*s && n < maxlines) {
        if (*s == '\n') {
            lines[n++][0] = 0;
            s++;
            continue;
        }
        size_t best = 0, last_space = 0, i = 0;
        char tmp[256];
        while (s[i] && s[i] != '\n') {
            size_t j = i + 1;
            while (s[j] && ((unsigned char)s[j] & 0xC0) == 0x80) j++;
            if (j >= sizeof tmp) break;
            memcpy(tmp, s, j);
            tmp[j] = 0;
            if (text_width(tmp, size, 0) > maxw && best > 0) break;
            best = j;
            if (s[i] == ' ') last_space = i;
            i = j;
        }
        size_t cut = best;
        if (s[best] && s[best] != '\n' && last_space > 0) cut = last_space;
        if (cut == 0) cut = best ? best : 1;
        size_t copy = cut < 255 ? cut : 255;
        memcpy(lines[n], s, copy);
        lines[n][copy] = 0;
        n++;
        s += cut;
        while (*s == ' ') s++;
        if (*s == '\n') s++;
    }
    return n;
}

/* ======================================================================== */
/* Sounds (LineXinBar's)                                                    */
/* ======================================================================== */

enum { SND_STEP, SND_BACK, SND_SELECT, SND_GUIDE_STEP, SND_GUIDE_SELECT, SND_KEY, SND_LAUNCH, SND_GUIDE_OPEN, SND_ERROR, SND_COUNT };
static const char *snd_files[SND_COUNT] = { "press", "press-back", "press-selected", "press-guide", "press-guide-selected",
                                            "keyboard-click", "app-launch", "guide-open", "error" };
static Mix_Chunk *sounds[SND_COUNT];
static int audio_ok;

static void audio_init(void)
{
    if (Mix_OpenAudio(48000, MIX_DEFAULT_FORMAT, 2, 1024) != 0) {
        fprintf(stderr, "pixi-shell: no audio: %s\n", Mix_GetError());
        return;
    }
    Mix_AllocateChannels(8);
    for (int i = 0; i < SND_COUNT; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/sounds/%s.ogg", datadir, snd_files[i]);
        sounds[i] = Mix_LoadWAV(path);
    }
    audio_ok = 1;
}

static void snd(int which)
{
    if (!audio_ok || !cfg.sounds || which < 0 || which >= SND_COUNT || !sounds[which]) return;
    Mix_PlayChannel(-1, sounds[which], 0);
}

/* ======================================================================== */
/* The compositor (pixi_shell_v1)                                           */
/* ======================================================================== */

enum { MODE_HOME = 0, MODE_OVERLAY = 1, MODE_APP = 2 };

typedef struct {
    uint32_t id;
    char app_id[128], title[256];
    int pid;
} Win;

static struct wl_display *wl_dpy;
static struct wl_event_queue *wl_queue;
static struct pixi_shell_v1 *pixi;
static Win wins[64];
static int nwins;
static uint32_t front_id;
static int comp_mode = MODE_HOME;
static int guide_requested;

static Win *win_find(uint32_t id)
{
    for (int i = 0; i < nwins; i++)
        if (wins[i].id == id) return &wins[i];
    return NULL;
}

static void on_window(void *data, struct pixi_shell_v1 *p, uint32_t id, const char *app_id, const char *title, int32_t pid);
static void on_window_closed(void *data, struct pixi_shell_v1 *p, uint32_t id);
static void on_front(void *data, struct pixi_shell_v1 *p, uint32_t id);
static void on_mode(void *data, struct pixi_shell_v1 *p, uint32_t mode);
static void on_guide(void *data, struct pixi_shell_v1 *p);

static const struct pixi_shell_v1_listener pixi_listener = { on_window, on_window_closed, on_front, on_mode, on_guide };

static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    if (!strcmp(iface, pixi_shell_v1_interface.name)) {
        pixi = wl_registry_bind(reg, name, &pixi_shell_v1_interface, 1);
        pixi_shell_v1_add_listener(pixi, &pixi_listener, NULL);
    }
}

static void reg_remove(void *data, struct wl_registry *reg, uint32_t name) {}

static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void comp_connect(void)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_WAYLAND) {
        fprintf(stderr, "pixi-shell: not running on Wayland; the compositor's controls are off\n");
        return;
    }
    wl_dpy = info.info.wl.display;
    wl_queue = wl_display_create_queue(wl_dpy);
    struct wl_display *wrapped = wl_proxy_create_wrapper(wl_dpy);
    wl_proxy_set_queue((struct wl_proxy *)wrapped, wl_queue);
    struct wl_registry *reg = wl_display_get_registry(wrapped);
    wl_proxy_wrapper_destroy(wrapped);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip_queue(wl_dpy, wl_queue);
    if (pixi) wl_display_roundtrip_queue(wl_dpy, wl_queue);
    else fprintf(stderr, "pixi-shell: the compositor doesn't offer pixi_shell_v1; running on its own\n");
}

static void comp_pump(void)
{
    if (!wl_dpy) return;
    wl_display_dispatch_queue_pending(wl_dpy, wl_queue);
    wl_display_flush(wl_dpy);
}

static void comp_set_mode(int mode)
{
    if (pixi) {
        pixi_shell_v1_set_mode(pixi, (uint32_t)mode);
        wl_display_flush(wl_dpy);
    } else {
        comp_mode = mode == MODE_APP ? MODE_HOME : mode;
    }
}

static void comp_activate(uint32_t id)
{
    if (!pixi) return;
    pixi_shell_v1_activate(pixi, id);
    wl_display_flush(wl_dpy);
}

static void comp_close(uint32_t id)
{
    if (!pixi) return;
    pixi_shell_v1_close(pixi, id);
    wl_display_flush(wl_dpy);
}

static void comp_quit(void)
{
    if (!pixi) return;
    pixi_shell_v1_quit(pixi);
    wl_display_flush(wl_dpy);
}

/* ======================================================================== */
/* Menus                                                                    */
/* ======================================================================== */

enum { K_LAUNCH, K_FOLDER, K_ACTION, K_INFO };
enum {
    A_NONE, A_VOLUME, A_BRIGHTNESS, A_SOUNDS, A_THEME, A_MATERIAL, A_CONFIRM, A_WIFI_NET, A_WIFI_RADIO,
    A_RESCAN, A_NOCORE, A_LOGOUT, A_REBOOT, A_SHUTDOWN
};
enum { IF_SECURED = 1, IF_SAVED = 2, IF_ACTIVE = 4 };

typedef struct Menu Menu;
typedef void (*BuildFn)(Menu *m);

typedef struct {
    char *title, *subtitle, *glyph, *icon, *data, *detail;
    int kind, action, arg, flags;
    char **argv;
    BuildFn build;
} Item;

struct Menu {
    char *title;
    Item *items;
    int n, cap, sel;
    float pos;
    BuildFn build;
    int arg;
    char *data, *empty;
    int cat; /* for top-level columns */
};

static Item *menu_add(Menu *m, const char *title, const char *subtitle, const char *glyph_name, int kind)
{
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->items = xrealloc(m->items, sizeof(Item) * (size_t)m->cap);
    }
    Item *it = &m->items[m->n++];
    memset(it, 0, sizeof *it);
    it->title = xstrdup(title ? title : "");
    it->subtitle = subtitle && *subtitle ? xstrdup(subtitle) : NULL;
    it->glyph = glyph_name ? xstrdup(glyph_name) : NULL;
    it->kind = kind;
    return it;
}

static void menu_clear(Menu *m)
{
    for (int i = 0; i < m->n; i++) {
        Item *it = &m->items[i];
        free(it->title), free(it->subtitle), free(it->glyph), free(it->icon), free(it->data), free(it->detail);
        argv_free(it->argv);
    }
    m->n = 0;
    free(m->empty);
    m->empty = NULL;
}

static void menu_free(Menu *m)
{
    menu_clear(m);
    free(m->items);
    free(m->title);
    free(m->data);
    free(m);
}

static int item_cmp(const void *a, const void *b) { return natural_cmp(((const Item *)a)->title, ((const Item *)b)->title); }

static void menu_rebuild(Menu *m)
{
    char keep[256] = "";
    if (m->sel >= 0 && m->sel < m->n) sncpy(keep, sizeof keep, m->items[m->sel].title);
    int old = m->sel;
    menu_clear(m);
    if (m->build) m->build(m);
    m->sel = 0;
    int found = 0;
    for (int i = 0; i < m->n; i++)
        if (!strcmp(m->items[i].title, keep)) {
            m->sel = i;
            found = 1;
            break;
        }
    if (!found) m->sel = old < m->n ? (old < 0 ? 0 : old) : (m->n ? m->n - 1 : 0);
    m->pos = (float)m->sel;
}

/* ---------- content ----------------------------------------------------- */

static void add_app(Menu *m, const AppEntry *e)
{
    char **a = exec_to_argv(e->exec);
    if (!a) return;
    if (e->terminal) {
        int n = 0;
        while (a[n]) n++;
        char **t = xcalloc((size_t)n + 3, sizeof(char *));
        t[0] = xstrdup("foot");
        t[1] = xstrdup("-e");
        for (int i = 0; i < n; i++) t[i + 2] = a[i];
        free(a);
        a = t;
    }
    Item *it = menu_add(m, e->name, *e->generic ? e->generic : e->comment, NULL, K_LAUNCH);
    it->icon = *e->icon ? xstrdup(e->icon) : NULL;
    it->glyph = xstrdup("launch");
    it->argv = a;
    it->detail = xstrdup(e->exec);
}

static void build_system(Menu *m)
{
    if (m->arg < 0 || m->arg >= nsystems) return;
    System *s = &systems[m->arg];
    StrList roms = { 0 };
    system_roms(s, &roms, 20000);
    char *core = find_core(s);
    for (int i = 0; i < roms.n; i++) {
        char title[256], tags[256];
        rom_title(roms.v[i], title, sizeof title, tags, sizeof tags);
        Item *it = menu_add(m, title, *tags ? tags : s->name, "category-games", core ? K_LAUNCH : K_INFO);
        it->data = xstrdup(roms.v[i]);
        it->detail = xstrdup(roms.v[i]);
        it->arg = m->arg;
        if (core) {
            char *a[] = { cfg.retroarch, "-f", "-L", core, roms.v[i], NULL };
            it->argv = argv_dup(a);
        } else {
            it->action = A_NOCORE;
        }
    }
    free(core);
    sl_free(&roms);
    if (m->n > 1) qsort(m->items, (size_t)m->n, sizeof(Item), item_cmp);
    m->empty = xstrdup("No games found in this folder");
}

static void build_games(Menu *m)
{
    for (int i = 0; i < apps.n; i++)
        if (apps.v[i].category == CAT_GAMES) add_app(m, &apps.v[i]);
    for (int pass = 1; pass >= 0; pass--) {
        const char *want = pass ? "retroarch" : "steamlink";
        for (int i = 0; i < m->n; i++) {
            if (m->items[i].argv && !strcmp(basename_of(m->items[i].argv[0]), want)) {
                Item tmp = m->items[i];
                memmove(&m->items[1], &m->items[0], sizeof(Item) * (size_t)i);
                m->items[0] = tmp;
                break;
            }
        }
    }
    int first = m->n, any = 0;
    for (int i = 0; i < nsystems; i++) {
        StrList roms = { 0 };
        system_roms(&systems[i], &roms, 20000);
        if (roms.n > 0) {
            char sub[64];
            snprintf(sub, sizeof sub, roms.n == 1 ? "%d game" : "%d games", roms.n);
            Item *it = menu_add(m, systems[i].name, sub, "file-folder", K_FOLDER);
            it->build = build_system;
            it->arg = i;
            any = 1;
        }
        sl_free(&roms);
    }
    if (m->n - first > 1) qsort(m->items + first, (size_t)(m->n - first), sizeof(Item), item_cmp);
    if (!any) {
        char *root = expand_home(cfg.rom_root);
        char sub[600];
        snprintf(sub, sizeof sub, "Copy game files into %s/<system>", root);
        Item *it = menu_add(m, "Add games", sub, "add", K_INFO);
        it->detail = xasprintf("Put game files in folders under %s, one per system:\n"
                               "nes  snes  gb  gbc  gba  md  sms  gg  pce  psx  psp  nds  n64\n\n"
                               "They appear here the next time this list is opened.", root);
        free(root);
    }
}

static void build_apps_cat(Menu *m)
{
    for (int i = 0; i < apps.n; i++)
        if (apps.v[i].category == m->cat) add_app(m, &apps.v[i]);
    m->empty = xstrdup("Nothing installed here");
}

static void build_wifi(Menu *m)
{
    if (!wifi_available()) {
        menu_add(m, "NetworkManager isn't installed", "Install the network-manager package", "setting-info", K_INFO);
        return;
    }
    int radio = wifi_radio();
    Item *r = menu_add(m, "Wi-Fi", radio > 0 ? "On" : "Off", "setting-wifi", K_ACTION);
    r->action = A_WIFI_RADIO;
    r->arg = radio > 0;
    if (radio <= 0) return;
    WifiNet nets[48];
    int n = wifi_scan(nets, 48, 1);
    for (int i = 0; i < n; i++) {
        WifiNet *w = &nets[i];
        char sub[160];
        snprintf(sub, sizeof sub, "%s%s", w->active ? "Connected" : w->saved ? "Saved" : w->secured ? "Secured" : "Open",
                 w->secured && !w->active ? "" : "");
        const char *g = w->signal >= 67 ? "signal-strong" : w->signal >= 34 ? "signal-fair" : "signal-weak";
        Item *it = menu_add(m, w->ssid, sub, w->active ? "chosen" : g, K_ACTION);
        it->action = A_WIFI_NET;
        it->data = xstrdup(w->ssid);
        it->flags = (w->secured ? IF_SECURED : 0) | (w->saved ? IF_SAVED : 0) | (w->active ? IF_ACTIVE : 0);
        it->detail = xasprintf("Signal %d%%\nSecurity: %s", w->signal, w->secured ? w->security : "none");
    }
    if (n == 0) menu_add(m, "No networks found", "Go back and open this again to look once more", "signal-weak", K_INFO);
}

static void build_info(Menu *m)
{
    char buf[512], v[256];
    gethostname(v, sizeof v);
    menu_add(m, "Device name", v, "setting-name", K_INFO);
    ip_address(buf, sizeof buf);
    menu_add(m, "IP address", buf, "setting-address", K_INFO);
    os_name(v, sizeof v);
    menu_add(m, "Operating system", v, "setting-system", K_INFO);
    struct utsname u;
    if (uname(&u) == 0) {
        snprintf(buf, sizeof buf, "%s %s", u.release, u.machine);
        menu_add(m, "Kernel", buf, "setting-system", K_INFO);
    }
    char model[256];
    if (read_file_str("/proc/device-tree/model", model, sizeof model) > 0) menu_add(m, "Hardware", model, "setting-info", K_INFO);
    long t = read_file_long("/sys/class/thermal/thermal_zone0/temp", -1);
    if (t > 0) {
        snprintf(buf, sizeof buf, "%.1f \xC2\xB0" "C", t / 1000.0);
        menu_add(m, "Temperature", buf, "setting-info", K_INFO);
    }
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        double unit = si.mem_unit ? si.mem_unit : 1;
        snprintf(buf, sizeof buf, "%.0f MB free of %.0f MB", (si.freeram + si.bufferram) * unit / 1048576.0,
                 si.totalram * unit / 1048576.0);
        menu_add(m, "Memory", buf, "setting-info", K_INFO);
    }
    struct statvfs sv;
    if (statvfs(home_dir(), &sv) == 0) {
        snprintf(buf, sizeof buf, "%.1f GB free of %.1f GB", (double)sv.f_bavail * sv.f_frsize / 1e9,
                 (double)sv.f_blocks * sv.f_frsize / 1e9);
        menu_add(m, "Storage", buf, "file-drive", K_INFO);
    }
    menu_add(m, "Pixi", "Version " PIXI_VERSION " \xC2\xB7 look and materials from LineXinBar", "logo", K_INFO);
}

static void build_themes(Menu *m)
{
    for (int i = 0; i < NTHEMES; i++) {
        Item *it = menu_add(m, THEMES[i].label, NULL, !strcasecmp(cfg.theme, THEMES[i].name) ? "chosen" : "unchosen", K_ACTION);
        it->action = A_THEME;
        it->arg = i;
    }
}

static void build_settings(Menu *m)
{
    char ssid[128] = "", sub[160];
    wifi_current(ssid, sizeof ssid);
    Item *it = menu_add(m, "Wi-Fi", *ssid ? ssid : "Not connected", "setting-wifi", K_FOLDER);
    it->build = build_wifi;
    int vol = volume_get();
    if (vol >= 0) snprintf(sub, sizeof sub, "%d%%", vol);
    else sncpy(sub, sizeof sub, "Unavailable");
    it = menu_add(m, "Volume", sub, vol == 0 ? "volume-muted" : "volume", K_ACTION);
    it->action = A_VOLUME;
    int br = brightness_get();
    if (br >= 0) {
        snprintf(sub, sizeof sub, "%d%%", br);
        it = menu_add(m, "Brightness", sub, "brightness", K_ACTION);
        it->action = A_BRIGHTNESS;
    }
    it = menu_add(m, "Accent colour", theme_def()->label, "setting-accent", K_FOLDER);
    it->build = build_themes;
    free(it->title);
    it->title = xstrdup("Accent color");
    it = menu_add(m, "Theme", cfg.full_material ? "Full (water and glass)" : "Simple (for slower machines)", "setting-theme", K_ACTION);
    it->action = A_MATERIAL;
    it = menu_add(m, "Menu sounds", cfg.sounds ? "On" : "Off", cfg.sounds ? "volume" : "volume-muted", K_ACTION);
    it->action = A_SOUNDS;
    it = menu_add(m, "Confirm button", cfg.confirm_east ? "Right button" : "Bottom button",
                  cfg.confirm_east ? "pad-east" : "pad-south", K_ACTION);
    it->action = A_CONFIRM;
    it = menu_add(m, "Refresh games and apps", "Look for newly installed apps and games", "refresh", K_ACTION);
    it->action = A_RESCAN;
    it = menu_add(m, "System information", NULL, "setting-info", K_FOLDER);
    it->build = build_info;
}

/* ---------- top-level columns ------------------------------------------- */

static Menu cats[CAT_COUNT];
static int vis[CAT_COUNT], nvis; /* visible category indices, left to right */
static int cur;                  /* index into vis */

static void cat_build(Menu *m)
{
    if (m->cat == CAT_SETTINGS) build_settings(m);
    else if (m->cat == CAT_GAMES) build_games(m);
    else build_apps_cat(m);
}

static void categories_rebuild(void)
{
    int keep_cat = nvis ? vis[cur] : cfg.category;
    apps_scan();
    nvis = 0;
    for (int c = 0; c < CAT_COUNT; c++) {
        Menu *m = &cats[c];
        if (!m->title) {
            m->title = xstrdup(cat_names[c]);
            m->cat = c;
            m->build = cat_build;
        }
        menu_rebuild(m);
        if (c == CAT_SETTINGS || c == CAT_GAMES || m->n > 0) vis[nvis++] = c;
    }
    cur = 0;
    for (int i = 0; i < nvis; i++)
        if (vis[i] == keep_cat) cur = i;
}

/* ======================================================================== */
/* State                                                                    */
/* ======================================================================== */

#define MAX_DEPTH 8
static Menu *stack[MAX_DEPTH];
static int depth;
static float catpos, dpos;
static int running = 1;

enum { D_NONE, D_MESSAGE, D_CONFIRM, D_SLIDER, D_OSK, D_BUSY };
static struct {
    int kind;
    char title[256], text[1024], ctx[256];
    int choice, action, value, min, max, step;
    char buf[128];
    int r, c, layer;
    float anim;
} dlg;

/* The guide overlay. */
enum { G_TILE, G_SLIDER, G_ENTRY, G_POWER };
enum { GE_RESUME, GE_CLOSE, GE_SWITCH, GE_HOME, GE_START };
typedef struct {
    int what;
    uint32_t id;
    char label[300];
} GuideEntry;
static struct {
    int open, row, tile, entry, power_open, power_sel;
    float anim;
    GuideEntry entries[40];
    int nentries;
    int volume;
} guide;

/* Launching: shown until the application's window arrives. */
static struct {
    int active;
    pid_t pid;
    char name[256];
    char icon[256], glyph[64];
    float started;
    int windows_before;
} launch;

static volatile sig_atomic_t got_sigchld, got_term;
static SysStatus status;
static float status_next;
static int hidden; /* the compositor isn't showing us */

static void show_message(const char *title, const char *body)
{
    memset(&dlg, 0, sizeof dlg);
    dlg.kind = D_MESSAGE;
    sncpy(dlg.title, sizeof dlg.title, title);
    sncpy(dlg.text, sizeof dlg.text, body ? body : "");
}

static void show_confirm(const char *title, const char *body, int action, const char *ctx)
{
    memset(&dlg, 0, sizeof dlg);
    dlg.kind = D_CONFIRM;
    sncpy(dlg.title, sizeof dlg.title, title);
    sncpy(dlg.text, sizeof dlg.text, body ? body : "");
    dlg.action = action;
    sncpy(dlg.ctx, sizeof dlg.ctx, ctx ? ctx : "");
}

static void render_frame(void);

static void show_busy(const char *title, const char *body)
{
    memset(&dlg, 0, sizeof dlg);
    dlg.kind = D_BUSY;
    dlg.anim = 1;
    sncpy(dlg.title, sizeof dlg.title, title);
    sncpy(dlg.text, sizeof dlg.text, body ? body : "");
    if (!hidden) {
        render_frame();
        render_frame();
    }
}

/* ======================================================================== */
/* Drawing the start screen                                                 */
/* ======================================================================== */

static float lerp(float a, float b, float t) { return a + (b - a) * t; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static float item_y(float offset, float cross_y, float gap_above, float gap_below, float spacing)
{
    if (offset >= 0) return cross_y + gap_below + offset * spacing;
    if (offset <= -1) return cross_y - gap_above + (offset + 1) * spacing;
    return cross_y + gap_below + offset * (gap_below + gap_above);
}

/* An item's mark: the app's own picture where it has one, else a glyph. */
static void item_mark(const Item *it, float cx, float cy, float size, float alpha)
{
    GLuint t = it->icon ? icon_tex(it->icon) : 0;
    if (t) {
        float s = size * 0.92f;
        picture(t, cx - s / 2, cy - s / 2, s, s, with_alpha(th.text, 1), alpha);
    } else {
        glyph(it->glyph ? it->glyph : "launch", cx, cy, size, th.text, alpha);
    }
}

static void draw_column(Menu *m, float x, float alpha, int focused_column)
{
    float cross_y = H * BAR_CROSS_Y;
    float spacing = ITEM_SPACING * S, gap_above = ITEM_GAP_ABOVE * S, gap_below = ITEM_GAP_BELOW * S;
    if (m->n == 0) {
        text(m->empty ? m->empty : "Nothing here", x - ITEM_ICON * S * 0.5f, cross_y + gap_below - 12 * S, 22 * S, 0,
             th.text_soft, alpha * 0.8f, AL_LEFT, (int)(W - x - 40 * S));
        return;
    }
    int lo = (int)floorf(m->pos) - 4, hi = (int)ceilf(m->pos) + 8;
    if (lo < 0) lo = 0;
    if (hi > m->n - 1) hi = m->n - 1;
    float text_x = x + (ITEM_ICON_FOCUSED * ITEM_DISC / 2.0f + 12.0f) * S;
    int maxw = (int)(W - text_x - CORNER_INSET * S * 0.5f);
    for (int i = lo; i <= hi; i++) {
        Item *it = &m->items[i];
        float off = (float)i - m->pos;
        float y = item_y(off, cross_y, gap_above, gap_below, spacing);
        if (y < -ITEM_SPACING * S || y > H + ITEM_SPACING * S) continue;
        float distance = fminf(fabsf(off), 1.0f);
        float focus = 1.0f - distance;
        float icon = lerp(ITEM_ICON, ITEM_ICON_FOCUSED, focus) * S;
        float a = alpha * (off < -0.5f ? 0.55f : 1.0f);
        /* fade under the hints and above the top */
        if (y > H - 70 * S) a *= clampf(1.0f - (y - (H - 70 * S)) / (50 * S), 0, 1);
        if (y < 40 * S) a *= clampf(y / (40 * S), 0, 1);
        if (a <= 0.01f) continue;
        if (distance < 0.5f && focused_column && dlg.kind == D_NONE) {
            float disc = icon * ITEM_DISC;
            float lit = 1.0f - distance * 2.0f;
            pane(x - disc / 2, y - disc / 2, disc, disc, disc / 2, CIRCLE, with_alpha(th.accent, 0.13f * lit),
                 DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_FULL, a * lit, 0);
        }
        item_mark(it, x, y, icon, a);
        int show_sub = distance < 0.5f && it->subtitle;
        float tsize = lerp(24, 30, focus) * S;
        int bold = distance < 0.5f;
        if (show_sub) {
            text(it->title, text_x, y - tsize * 0.92f, tsize, bold, th.text, a, AL_LEFT, maxw);
            text(it->subtitle, text_x, y + 6 * S, 20 * S, 0, th.text_soft, a, AL_LEFT, maxw);
        } else {
            text(it->title, text_x, y - tsize * 0.5f, tsize, bold, th.text, a, AL_LEFT, maxw);
        }
        if (it->kind == K_FOLDER && distance < 0.5f) {
            float gx = (float)W - CORNER_INSET * S * 0.6f;
            glyph("arrow-right", gx, y, 30 * S, th.text_soft, a * 0.8f);
        }
    }
}

static void draw_corner(void)
{
    char buf[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[16];
    if (cfg.clock24) strftime(hm, sizeof hm, "%H:%M", &tm);
    else {
        strftime(hm, sizeof hm, "%I:%M", &tm);
        if (hm[0] == '0') memmove(hm, hm + 1, strlen(hm));
    }
    snprintf(buf, sizeof buf, "%d/%d %s", tm.tm_mon + 1, tm.tm_mday, hm);
    float size = CORNER_CLOCK * S;
    float x = (float)W - CORNER_INSET * S, y = CORNER_TOP * S;
    float w = text(buf, x, y, size, 0, th.text, 0.85f, AL_RIGHT, 0);
    x -= w + 12 * S;
    float mark = CORNER_MARK * S, my = y + size * 0.42f;
    if (status.battery >= 0) {
        const char *b = status.charging ? "battery-charging" : status.battery > 85 ? "battery-full" : status.battery > 60 ? "battery-high"
                        : status.battery > 35 ? "battery-half" : status.battery > 12 ? "battery-low" : "battery-empty";
        char pct[16];
        snprintf(pct, sizeof pct, "%d%%", status.battery);
        float pw = text(pct, x, y, size, 0, th.text, 0.85f, AL_RIGHT, 0);
        x -= pw + 4 * S;
        glyph(b, x - mark / 2, my, mark * 1.25f, th.text, 0.85f);
        x -= mark * 1.25f + 12 * S;
    }
    if (status.wifi >= 0) {
        const char *g = status.wifi >= 3 ? "setting-wifi" : status.wifi == 2 ? "signal-strong" : status.wifi == 1 ? "signal-fair" : "signal-weak";
        glyph(g, x - mark / 2, my, mark, th.text, 0.85f);
    }
}

typedef struct {
    const char *glyph_name, *label;
} Hint;

static void draw_hints(const Hint *h, int n)
{
    float x = (float)W - HINT_MARGIN * S, y = (float)H - HINT_MARGIN * S - HINT_GLYPH * S * 0.5f;
    for (int i = n - 1; i >= 0; i--) {
        float g = HINT_GLYPH * S;
        glyph(h[i].glyph_name, x - g / 2, y, g, th.text, 0.85f);
        x -= g + HINT_GAP * S;
        float w = text(h[i].label, x, y - HINT_LABEL * S * 0.55f, HINT_LABEL * S, 0, th.text, 0.85f, AL_RIGHT, 0);
        x -= w + HINT_PADDING * S * 2;
    }
}

static const char *pad_ok(void) { return cfg.confirm_east ? "pad-east" : "pad-south"; }
static const char *pad_back(void) { return cfg.confirm_east ? "pad-south" : "pad-east"; }

static void draw_categories(float shift, float fade_others, float fade)
{
    float cross_x = W * BAR_CROSS_X, cross_y = H * BAR_CROSS_Y;
    float pulse = 0.5f + 0.5f * sinf(now_s * 2 * (float)M_PI / PULSE_PERIOD);
    for (int i = 0; i < nvis; i++) {
        float off = (float)i - catpos;
        float x = cross_x + off * CATEGORY_SPACING * S + shift;
        if (x < -CATEGORY_SPACING * S || x > W + CATEGORY_SPACING * S) continue;
        float distance = fminf(fabsf(off), 1.0f);
        float focus = 1.0f - distance;
        float alpha = clampf(1.0f - fabsf(off) * 0.10f, CATEGORY_MIN_ALPHA, 1.0f);
        if (i != cur) alpha *= 1.0f - fade_others;
        alpha *= fade;
        if (alpha <= 0.01f) continue;
        float icon = lerp(CATEGORY_ICON, CATEGORY_ICON_FOCUSED, focus) * S;
        float disc = icon * CATEGORY_DISC;
        pane(x - disc / 2, cross_y - disc / 2, disc, disc, disc / 2, SQUIRCLE, with_alpha(th.glass_raised, 0.05f),
             DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_QUIET, alpha * 0.85f, 0);
        if (i == cur) {
            float lit = (1.0f - distance) * (1.0f - fade_others) * fade;
            float gsize = icon * (2.0f + 0.2f * pulse);
            glow(x, cross_y, gsize, with_alpha(th.accent, (0.28f + 0.24f * pulse) * lit), 1);
            pane(x - disc / 2, cross_y - disc / 2, disc, disc, disc / 2, SQUIRCLE, with_alpha(th.accent, 0.12f),
                 DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_FULL, lit, 0);
            text(cat_names[vis[i]], x, cross_y + (CATEGORY_ICON_FOCUSED * CATEGORY_DISC / 2 + CATEGORY_LABEL_ABOVE) * S,
                 CATEGORY_LABEL * S, 1, th.text, alpha * (1 - distance * 2), AL_CENTER, 0);
        }
        glyph(cat_glyphs[vis[i]], x, cross_y, icon, th.text, alpha);
    }
}

static void draw_home(float alpha)
{
    float cross_x = W * BAR_CROSS_X;
    float step = cross_x - COLUMN_MARGIN * S;
    float d1 = fminf(dpos, 1.0f);
    draw_categories(-dpos * step, d1, alpha);
    for (int L = 0; L <= depth; L++) {
        Menu *m = stack[L];
        float x = cross_x + ((float)L - dpos) * step;
        if (L < depth) {
            if (L == 0 || m->sel >= m->n) continue;
            float s = ITEM_ICON_FOCUSED * S * DEPTH_SHRINK;
            item_mark(&m->items[m->sel], x, H * BAR_CROSS_Y, s, DEPTH_HAZE * alpha);
        } else {
            float a = alpha * (1.0f - fminf(fabsf((float)depth - dpos), 1.0f) * 0.7f);
            /* the column belongs to nobody while the row glides */
            if (depth == 0) a *= 1.0f - fminf(fabsf(catpos - (float)cur), 1.0f) * 0.9f;
            draw_column(m, x, a, 1);
        }
    }
}

/* ======================================================================== */
/* Dialogs                                                                  */
/* ======================================================================== */

static const char *osk_rows[3][4] = {
    { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.@" },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM_.@" },
    { "!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?", "`~@#&*+=_-" },
};
static const char *osk_special[5] = { "Shift", "#+=", "Space", "Delete", "Done" };

static void draw_dialog(void)
{
    if (dlg.kind == D_NONE) return;
    float a = dlg.anim;
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.62f }, 0, 0, 0, a, 0);
    if (dlg.kind == D_OSK) {
        float pw = W * 0.88f, ph = H * 0.74f, px = (W - pw) / 2, py = H * 0.08f;
        pane(px, py, pw, ph, 28 * S, SQUIRCLE, with_alpha(th.glass, 0.55f), DEPTH_PANEL * S, FROST_PANEL, GLOSS_QUIET, a, 1);
        text(dlg.title, px + 28 * S, py + 22 * S, 24 * S, 1, th.text, a, AL_LEFT, (int)(pw - 56 * S));
        float fy = py + 62 * S, fh = 52 * S;
        pane(px + 24 * S, fy, pw - 48 * S, fh, fh / 2, SQUIRCLE, with_alpha(th.glass, 0.6f), DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_QUIET, a, 0);
        char shown[160];
        snprintf(shown, sizeof shown, "%s%s", dlg.buf, (frame_no / 30) % 2 ? "|" : "");
        text(shown, px + 44 * S, fy + fh / 2 - 12 * S, 24 * S, 0, th.text, a, AL_LEFT, (int)(pw - 90 * S));
        float gx = px + 24 * S, gy = fy + fh + 14 * S, gw = pw - 48 * S;
        float kw = gw / 10, kh = (py + ph - 20 * S - gy) / 5;
        for (int r = 0; r < 5; r++) {
            int cols = r < 4 ? 10 : 5;
            for (int c = 0; c < cols; c++) {
                float w = r < 4 ? kw : kw * 2, kx = gx + c * w, ky = gy + r * kh;
                int sel = dlg.r == r && dlg.c == c;
                int on = r == 4 && ((c == 0 && dlg.layer == 1) || (c == 1 && dlg.layer == 2));
                Col kc = sel ? with_alpha(th.accent, 0.35f) : on ? with_alpha(th.accent, 0.18f) : with_alpha(th.glass_raised, 0.06f);
                pane(kx + 3 * S, ky + 3 * S, w - 6 * S, kh - 6 * S, 12 * S, SQUIRCLE, kc, DEPTH_CONTROL * S * 0.6f, FROST_CONTROL,
                     sel ? GLOSS_FULL : GLOSS_QUIET, a, 0);
                char label[16];
                if (r < 4) {
                    label[0] = osk_rows[dlg.layer][r][c];
                    label[1] = 0;
                } else sncpy(label, sizeof label, osk_special[c]);
                float ts = (r < 4 ? 24 : 18) * S;
                text(label, kx + w / 2, ky + kh / 2 - ts * 0.5f, ts, sel, th.text, a, AL_CENTER, 0);
            }
        }
        return;
    }
    float pw = fminf(W * 0.70f, 760 * S), px = (W - pw) / 2;
    char lines[10][256];
    int nl = dlg.text[0] ? wrap_text(dlg.text, 21 * S, (int)(pw - 64 * S), lines, 10) : 0;
    float ph = 96 * S + nl * 30 * S + (dlg.kind == D_CONFIRM || dlg.kind == D_SLIDER ? 80 * S : 0);
    float py = (H - ph) / 2 - (1 - a) * 20 * S;
    pane(px, py, pw, ph, 28 * S, SQUIRCLE, with_alpha(th.glass, 0.55f), DEPTH_PANEL * S, FROST_PANEL, GLOSS_QUIET, a, 1);
    text(dlg.title, px + 32 * S, py + 30 * S, 28 * S, 1, th.text, a, AL_LEFT, (int)(pw - 64 * S));
    float ty = py + 80 * S;
    for (int i = 0; i < nl; i++, ty += 30 * S) text(lines[i], px + 32 * S, ty, 21 * S, 0, th.text_soft, a, AL_LEFT, 0);
    if (dlg.kind == D_BUSY) {
        float cx = px + pw - 50 * S, cy = py + 46 * S;
        for (int i = 0; i < LAUNCH_DOTS; i++) {
            float ang = (float)i / LAUNCH_DOTS * 2 * (float)M_PI;
            float lit = fmodf((float)i / LAUNCH_DOTS - now_s * LAUNCH_SPIN + 10.0f, 1.0f);
            pane(cx + cosf(ang) * 16 * S - 3 * S, cy + sinf(ang) * 16 * S - 3 * S, 6 * S, 6 * S, 3 * S, CIRCLE, th.text, 0, 0, 0,
                 a * (0.25f + 0.75f * lit), 0);
        }
    } else if (dlg.kind == D_CONFIRM) {
        const char *labels[2] = { "Yes", "No" };
        float bw = 150 * S, bh = 56 * S, by = py + ph - bh - 24 * S;
        for (int i = 0; i < 2; i++) {
            float bx = px + pw - 32 * S - (2 - i) * (bw + 14 * S) + 14 * S;
            int sel = dlg.choice == i;
            pane(bx, by, bw, bh, bh / 2, SQUIRCLE, sel ? with_alpha(th.accent, 0.55f) : with_alpha(th.glass_raised, 0.06f),
                 DEPTH_CONTROL * S, FROST_CONTROL, sel ? GLOSS_FULL : GLOSS_QUIET, a, 0);
            text(labels[i], bx + bw / 2, by + bh / 2 - 12 * S, 24 * S, sel, th.text, a, AL_CENTER, 0);
        }
    } else if (dlg.kind == D_SLIDER) {
        float sx = px + 32 * S, sw = pw - 150 * S, sy = py + ph - 52 * S;
        float fr = (float)(dlg.value - dlg.min) / (float)(dlg.max - dlg.min);
        pane(sx, sy - 6 * S, sw, 12 * S, 6 * S, CIRCLE, with_alpha(th.glass_raised, 0.18f), 0, 0, 0, a, 0);
        pane(sx, sy - 6 * S, sw * fr, 12 * S, 6 * S, CIRCLE, th.accent_soft, 0, 0, 0, a, 0);
        pane(sx + sw * fr - 14 * S, sy - 14 * S, 28 * S, 28 * S, 14 * S, CIRCLE, with_alpha(th.text, 0.9f), DEPTH_CONTROL * S * 0.5f, 0,
             GLOSS_FULL, a, 0);
        char v[16];
        snprintf(v, sizeof v, "%d%%", dlg.value);
        text(v, px + pw - 32 * S, sy - 16 * S, 28 * S, 1, th.text, a, AL_RIGHT, 0);
    }
}

/* ======================================================================== */
/* The guide                                                                */
/* ======================================================================== */

static Win *front_win(void) { return front_id ? win_find(front_id) : NULL; }

static void guide_entries(void)
{
    guide.nentries = 0;
    Win *f = front_win();
    GuideEntry *e;
    if (f) {
        e = &guide.entries[guide.nentries++];
        e->what = GE_RESUME;
        e->id = f->id;
        sncpy(e->label, sizeof e->label, "Resume");
        e = &guide.entries[guide.nentries++];
        e->what = GE_CLOSE;
        e->id = f->id;
        snprintf(e->label, sizeof e->label, "Close %s", *f->title ? f->title : *f->app_id ? f->app_id : "application");
    }
    for (int i = nwins - 1; i >= 0 && guide.nentries < 38; i--) {
        if (f && wins[i].id == f->id) continue;
        e = &guide.entries[guide.nentries++];
        e->what = GE_SWITCH;
        e->id = wins[i].id;
        snprintf(e->label, sizeof e->label, "Switch to %s", *wins[i].title ? wins[i].title : wins[i].app_id);
    }
    e = &guide.entries[guide.nentries++];
    e->what = f ? GE_HOME : GE_START;
    sncpy(e->label, sizeof e->label, f ? "Start screen" : "Start screen");
    if (guide.entry >= guide.nentries) guide.entry = guide.nentries - 1;
}

static void guide_open(void)
{
    if (guide.open) return;
    memset(&guide, 0, sizeof guide);
    guide.open = 1;
    guide.row = G_ENTRY;
    guide.volume = volume_get();
    guide_entries();
    if (comp_mode == MODE_APP || front_id) comp_set_mode(front_id ? MODE_OVERLAY : MODE_HOME);
    snd(SND_GUIDE_OPEN);
}

static void guide_close(int resume)
{
    guide.open = 0;
    guide.power_open = 0;
    if (resume && front_id) comp_set_mode(MODE_APP);
    else comp_set_mode(MODE_HOME);
    snd(SND_BACK);
}

static void guide_card(float *x, float *y, float *w, float *h)
{
    float panel_r = GUIDE_MARGIN * S + fmaxf(392 * S, W * 0.27f);
    float avail = W - panel_r - GUIDE_MARGIN * S * 2;
    float cw = fminf(avail * 0.92f, W * 0.54f), ch = cw * H / W;
    *w = cw;
    *h = ch;
    *x = panel_r + (avail - cw) / 2 + GUIDE_MARGIN * S;
    *y = (H - ch) / 2 - 10 * S;
}

static void draw_guide(void)
{
    float a = guide.anim;
    float pm = GUIDE_MARGIN * S, pw = fmaxf(392 * S, W * 0.27f), ph = H - pm * 2;
    float px = pm - (1 - a) * (pw + pm), py = pm;
    pane(px, py, pw, ph, 28 * S, SQUIRCLE, with_alpha(th.glass, SIDEBAR_STAIN), DEPTH_SIDEBAR * S, FROST_SIDEBAR, GLOSS_SIDEBAR, a, 1.0f);
    float ix = px + GUIDE_PADDING * S * 0.8f, iw = pw - GUIDE_PADDING * S * 1.6f;

    /* clock, date, what's running */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[16], date[32];
    if (cfg.clock24) strftime(hm, sizeof hm, "%H:%M", &tm);
    else strftime(hm, sizeof hm, "%I:%M", &tm);
    strftime(date, sizeof date, "%a %b %e", &tm);
    text(hm, ix, py + 30 * S, 34 * S, 1, th.text, a, AL_LEFT, 0);
    text(date, ix + iw, py + 36 * S, 18 * S, 0, th.text_soft, a, AL_RIGHT, 0);
    Win *f = front_win();
    char running_label[300];
    if (f) snprintf(running_label, sizeof running_label, "Playing %s", *f->title ? f->title : f->app_id);
    else sncpy(running_label, sizeof running_label, "Nothing is running");
    text(running_label, ix, py + 80 * S, 18 * S, 0, th.text_soft, a, AL_LEFT, (int)iw);

    /* quick tiles */
    static const char *tiles[4] = { "setting-wifi", "brightness", "setting-accent", "setting-info" };
    float ty = py + 124 * S, tsz = GUIDE_TILE * S;
    float gap = (iw - tsz * 4) / 3;
    for (int i = 0; i < 4; i++) {
        float tx = ix + i * (tsz + gap);
        int sel = guide.row == G_TILE && guide.tile == i && !guide.power_open;
        pane(tx, ty, tsz, tsz, tsz * GUIDE_TILE_RADIUS, SQUIRCLE, sel ? with_alpha(th.accent, 0.45f) : with_alpha(th.glass_raised, 0.08f),
             DEPTH_CONTROL * S, FROST_CONTROL, sel ? GLOSS_FULL : GLOSS_QUIET, a, 0);
        glyph(tiles[i], tx + tsz / 2, ty + tsz / 2, tsz * GUIDE_TILE_GLYPH, th.text, a);
    }

    /* volume bar */
    float vy = ty + tsz + 16 * S, vh = GUIDE_BAR_HEIGHT * S * 0.75f;
    int vsel = guide.row == G_SLIDER && !guide.power_open;
    pane(ix, vy, iw, vh, vh / 2, SQUIRCLE, vsel ? with_alpha(th.accent, 0.35f) : with_alpha(th.glass_raised, 0.08f), DEPTH_CONTROL * S,
         FROST_CONTROL, vsel ? GLOSS_FULL : GLOSS_QUIET, a, 0);
    glyph(guide.volume == 0 ? "volume-muted" : "volume", ix + vh * 0.62f, vy + vh / 2, vh * 0.62f, th.text, a);
    float sx = ix + vh * 1.2f, sw = iw - vh * 1.65f;
    float fr = guide.volume < 0 ? 0 : guide.volume / 100.0f;
    pane(sx, vy + vh / 2 - 3 * S, sw, 6 * S, 3 * S, CIRCLE, with_alpha(th.text, 0.25f), 0, 0, 0, a, 0);
    pane(sx, vy + vh / 2 - 3 * S, sw * fr, 6 * S, 3 * S, CIRCLE, th.text, 0, 0, 0, a, 0);
    pane(sx + sw * fr - 8 * S, vy + vh / 2 - 8 * S, 16 * S, 16 * S, 8 * S, CIRCLE, th.text, 0, 0, 0, a, 0);

    /* separator and entries */
    float ey = vy + vh + 26 * S;
    pane(ix, ey - 14 * S, iw, 1.5f * S, 0.75f * S, CIRCLE, with_alpha(th.text, 0.18f), 0, 0, 0, a, 0);
    float rh = GUIDE_ROW_HEIGHT * S * 0.72f;
    float power_y = py + ph - POWER_BUTTON * S - GUIDE_PADDING * S * 0.6f;
    for (int i = 0; i < guide.nentries; i++) {
        float ry = ey + i * (rh + 8 * S);
        if (ry + rh > power_y - 8 * S) break;
        int sel = guide.row == G_ENTRY && guide.entry == i && !guide.power_open;
        if (sel)
            pane(ix, ry, iw, rh, rh / 2, SQUIRCLE, with_alpha(th.accent, 0.62f), DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_FULL, a, 0);
        text(guide.entries[i].label, ix + 24 * S, ry + rh / 2 - 12 * S, 23 * S, sel, th.text, a * (sel ? 1 : 0.85f), AL_LEFT,
             (int)(iw - 48 * S));
    }

    /* power */
    float pb = POWER_BUTTON * S;
    int psel = guide.row == G_POWER || guide.power_open;
    pane(ix, power_y, pb, pb, pb / 2, CIRCLE, psel ? with_alpha(th.accent, 0.45f) : with_alpha(th.glass_raised, 0.10f), DEPTH_CONTROL * S,
         FROST_CONTROL, psel ? GLOSS_FULL : GLOSS_QUIET, a, 0);
    glyph("shutdown", ix + pb / 2, power_y + pb / 2, pb * 0.52f, th.text, a);

    /* the running application, seen through the card */
    if (f) {
        float cx, cy, cw, ch;
        guide_card(&cx, &cy, &cw, &ch);
        glow(cx + cw / 2, cy + ch / 2, fmaxf(cw, ch) * 1.25f, with_alpha(th.accent, 0.22f), a);
        outline(cx - 3 * S, cy - 3 * S, cw + 6 * S, ch + 6 * S, 22 * S, 3 * S, th.accent_soft, a * 0.9f);
        text(*f->title ? f->title : f->app_id, cx, cy + ch + 16 * S, 22 * S, 0, th.text, a, AL_LEFT, (int)cw);
    } else {
        /* Nothing running: the card stands for the start screen. */
        float cx, cy, cw, ch;
        guide_card(&cx, &cy, &cw, &ch);
        glow(cx + cw / 2, cy + ch / 2, fmaxf(cw, ch) * 1.1f, with_alpha(th.accent, 0.16f), a);
        pane(cx, cy, cw, ch, 20 * S, SQUIRCLE, with_alpha(th.glass_raised, 0.06f), DEPTH_PANEL * S, FROST_CONTROL, GLOSS_QUIET, a, 1);
        glyph("category-games", cx + cw / 2, cy + ch / 2, fminf(cw, ch) * 0.34f, th.text, a * 0.9f);
        text("Start screen", cx, cy + ch + 16 * S, 22 * S, 0, th.text, a, AL_LEFT, (int)cw);
    }

    if (guide.power_open) {
        static const char *labels[3] = { "Log out", "Restart", "Shut down" };
        static const char *glyphs3[3] = { "sign-out", "refresh", "shutdown" };
        float mw = POWER_WIDTH * S * 0.8f, mh = POWER_ROW * S * 3 + 24 * S;
        float mx = ix, my = power_y - mh - 12 * S;
        pane(mx, my, mw, mh, 24 * S, SQUIRCLE, with_alpha(th.glass, 0.6f), DEPTH_PANEL * S, FROST_PANEL, GLOSS_QUIET, a, 1);
        for (int i = 0; i < 3; i++) {
            float ry = my + 12 * S + i * POWER_ROW * S;
            int sel = guide.power_sel == i;
            if (sel)
                pane(mx + 10 * S, ry + 4 * S, mw - 20 * S, POWER_ROW * S - 8 * S, (POWER_ROW * S - 8 * S) / 2, SQUIRCLE,
                     with_alpha(th.accent, 0.6f), DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_FULL, a, 0);
            glyph(glyphs3[i], mx + 42 * S, ry + POWER_ROW * S / 2, 34 * S, th.text, a);
            text(labels[i], mx + 76 * S, ry + POWER_ROW * S / 2 - 12 * S, 23 * S, sel, th.text, a, AL_LEFT, 0);
        }
    }
}

/* ======================================================================== */
/* The launch screen                                                        */
/* ======================================================================== */

static void draw_launch(void)
{
    float cx = W / 2.0f, cy = H * 0.42f;
    float icon = LAUNCH_ICON * S;
    float t = now_s - launch.started;
    float a = clampf(t / 0.25f, 0, 1);
    GLuint tex = *launch.icon ? icon_tex(launch.icon) : 0;
    if (tex) picture(tex, cx - icon / 2, cy - icon / 2, icon, icon, th.text, a);
    else glyph(*launch.glyph ? launch.glyph : "launch", cx, cy, icon, th.text, a);
    float ring = icon * 0.5f + 46 * S;
    for (int i = 0; i < LAUNCH_DOTS; i++) {
        float ang = (float)i / LAUNCH_DOTS * 2 * (float)M_PI - (float)M_PI / 2;
        float lit = fmodf((float)i / LAUNCH_DOTS + now_s * LAUNCH_SPIN + 10.0f, 1.0f);
        float r = (4 + 3 * lit) * S;
        pane(cx + cosf(ang) * ring - r, cy + sinf(ang) * ring - r, r * 2, r * 2, r, CIRCLE, th.accent_soft, 0, 0, 0,
             a * (0.2f + 0.8f * lit), 0);
    }
    text(launch.name, cx, cy + ring + 30 * S, LAUNCH_NAME * S * 0.62f, 1, th.text, a, AL_CENTER, (int)(W * 0.72f));
    text("Starting\xE2\x80\xA6", cx, cy + ring + 30 * S + LAUNCH_NAME * S * 0.62f + 14 * S, LAUNCH_DOING * S, 0, th.text_soft, a,
         AL_CENTER, 0);
}

/* ======================================================================== */
/* Frame                                                                    */
/* ======================================================================== */

static void render_frame(void)
{
    SDL_GL_GetDrawableSize(win, &W, &H);
    if (W < 16 || H < 16) return;
    float k = cfg.ui_scale > 0 ? cfg.ui_scale : (H <= 600 ? 1.35f : H <= 800 ? 1.15f : 1.0f);
    S = H / REF_H * k;
    targets_fit();
    frame_no++;

    int overlay = guide.open && front_id;
    float soften = guide.open ? guide.anim : 0;
    quad_blur = soften;

    /* 1. the wallpaper, into its own target with a blur chain for the glass */
    glBindFramebuffer(GL_FRAMEBUFFER, fb_bg);
    glViewport(0, 0, W, H);
    glDisable(GL_BLEND);
    glUseProgram(prog_bg);
    set_common_uniforms(prog_bg, (float)W, (float)H, soften);
    glBindVertexArray(vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, tex_bg);
    glGenerateMipmap(GL_TEXTURE_2D);

    /* 2. the scene */
    glBindFramebuffer(GL_FRAMEBUFFER, fb_scene);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(prog_copy);
    glUniform1i(glGetUniformLocation(prog_copy, "u_tex"), 0);
    glUniform2f(glGetUniformLocation(prog_copy, "u_res"), (float)W, (float)H);
    glUniform1f(glGetUniformLocation(prog_copy, "u_alpha"), 1.0f);
    if (overlay) {
        float cx, cy, cw, ch;
        guide_card(&cx, &cy, &cw, &ch);
        quad_hole[0] = cx, quad_hole[1] = cy, quad_hole[2] = cw, quad_hole[3] = ch * guide.anim;
        quad_hole_radius = 20 * S;
    } else {
        memset(quad_hole, 0, sizeof quad_hole);
    }
    glUniform4fv(glGetUniformLocation(prog_copy, "u_hole"), 1, quad_hole);
    glUniform1f(glGetUniformLocation(prog_copy, "u_hole_radius"), quad_hole_radius);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_bg);
    glBindVertexArray(vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    if (launch.active) {
        draw_launch();
    } else {
        float home_alpha = 1.0f - (guide.open ? guide.anim * 0.85f : 0);
        if (!guide.open && stack[0]) draw_home(home_alpha);
        if (guide.open) draw_guide();
        draw_corner();
        if (guide.open) {
            Hint h[2] = { { pad_ok(), "Select" }, { pad_back(), "Back" } };
            draw_hints(h, 2);
        } else if (dlg.kind == D_NONE) {
            Hint h[3] = { { pad_ok(), "Select" }, { "pad-north", "Options" }, { "pad-guide", "Guide" } };
            if (depth > 0) {
                h[1].glyph_name = pad_back();
                h[1].label = "Back";
            }
            draw_hints(h, 3);
        }
        draw_dialog();
    }
    quads_flush();

    /* 3. to the display */
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    glDisable(GL_BLEND);
    glUseProgram(prog_present);
    glUniform1i(glGetUniformLocation(prog_present, "u_tex"), 0);
    glUniform2f(glGetUniformLocation(prog_present, "u_res"), (float)W, (float)H);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_scene);
    glBindVertexArray(vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    SDL_GL_SwapWindow(win);
}

/* ======================================================================== */
/* Launching                                                                */
/* ======================================================================== */

static void launch_item(const Item *it)
{
    if (!it->argv || !it->argv[0]) return;
    snd(SND_LAUNCH);
    pid_t pid = fork();
    if (pid < 0) {
        show_message("Couldn't start", strerror(errno));
        return;
    }
    if (pid == 0) {
        setsid();
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        if (chdir(home_dir()) != 0) {}
        execvp(it->argv[0], it->argv);
        fprintf(stderr, "pixi-shell: couldn't run %s: %s\n", it->argv[0], strerror(errno));
        _exit(127);
    }
    memset(&launch, 0, sizeof launch);
    launch.active = 1;
    launch.pid = pid;
    launch.started = now_s;
    launch.windows_before = nwins;
    sncpy(launch.name, sizeof launch.name, it->title);
    sncpy(launch.icon, sizeof launch.icon, it->icon ? it->icon : "");
    sncpy(launch.glyph, sizeof launch.glyph, it->glyph ? it->glyph : "launch");
}

static void reap_children(void)
{
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        if (launch.active && pid == launch.pid) {
            int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            launch.active = 0;
            if (code != 0 && now_s - launch.started < 20) {
                char msg[600];
                snprintf(msg, sizeof msg, "%s stopped before it opened a window%s.", launch.name,
                         code == 127 ? " (it may not be installed)" : "");
                show_message("Couldn't start", msg);
                snd(SND_ERROR);
            }
        }
    }
}

/* ---------- protocol events -------------------------------------------- */

static void on_window(void *data, struct pixi_shell_v1 *p, uint32_t id, const char *app_id, const char *title, int32_t pid)
{
    Win *w = win_find(id);
    if (!w && nwins < 64) {
        w = &wins[nwins++];
        memset(w, 0, sizeof *w);
        w->id = id;
    }
    if (!w) return;
    sncpy(w->app_id, sizeof w->app_id, app_id);
    sncpy(w->title, sizeof w->title, title);
    w->pid = pid;
    if (launch.active) launch.active = 0; /* the window is here: the compositor brings it forward */
    if (guide.open) guide_entries();
}

static void on_window_closed(void *data, struct pixi_shell_v1 *p, uint32_t id)
{
    for (int i = 0; i < nwins; i++) {
        if (wins[i].id == id) {
            memmove(&wins[i], &wins[i + 1], sizeof(Win) * (size_t)(nwins - i - 1));
            nwins--;
            break;
        }
    }
    if (guide.open) guide_entries();
}

static void on_front(void *data, struct pixi_shell_v1 *p, uint32_t id) { front_id = id; }

static void on_mode(void *data, struct pixi_shell_v1 *p, uint32_t mode)
{
    comp_mode = (int)mode;
    if (mode == MODE_APP) {
        guide.open = 0;
        launch.active = 0;
    }
}

static void on_guide(void *data, struct pixi_shell_v1 *p) { guide_requested = 1; }

/* ======================================================================== */
/* Actions                                                                  */
/* ======================================================================== */

static void push_menu(Item *it)
{
    if (depth + 1 >= MAX_DEPTH) return;
    Menu *c = xcalloc(1, sizeof *c);
    c->title = xstrdup(it->title);
    c->build = it->build;
    c->arg = it->arg;
    c->data = xstrdup(it->data);
    if (it->build == build_wifi) show_busy("Wi-Fi", "Looking for networks\xE2\x80\xA6");
    if (c->build) c->build(c);
    if (dlg.kind == D_BUSY) dlg.kind = D_NONE;
    stack[++depth] = c;
    snd(SND_SELECT);
}

static void pop_menu(void)
{
    if (depth == 0) return;
    menu_free(stack[depth]);
    stack[depth--] = NULL;
    snd(SND_BACK);
    if (stack[depth]->build == cat_build || stack[depth]->build == build_settings) menu_rebuild(stack[depth]);
}

static void wifi_join(const char *ssid, const char *password, int saved)
{
    char title[300], err[600];
    snprintf(title, sizeof title, "Connecting to %s", ssid);
    show_busy(title, "This can take up to 30 seconds.");
    int rc = wifi_connect(ssid, password, saved, err, sizeof err);
    if (stack[depth]->build == build_wifi) menu_rebuild(stack[depth]);
    if (rc == 0) {
        snprintf(title, sizeof title, "Connected to %s", ssid);
        show_message(title, "");
        snd(SND_SELECT);
    } else {
        show_message("Couldn't connect", err);
        snd(SND_ERROR);
    }
}

static void do_action(Item *it)
{
    char buf[600];
    switch (it->action) {
    case A_VOLUME: {
        int v = volume_get();
        if (v < 0) {
            show_message("Volume", "No audio control was found. Install pipewire and wireplumber.");
            snd(SND_ERROR);
            return;
        }
        memset(&dlg, 0, sizeof dlg);
        dlg.kind = D_SLIDER;
        dlg.action = A_VOLUME;
        sncpy(dlg.title, sizeof dlg.title, "Volume");
        dlg.min = 0, dlg.max = 100, dlg.step = 5, dlg.value = v > 100 ? 100 : v;
        snd(SND_SELECT);
        break;
    }
    case A_BRIGHTNESS: {
        int v = brightness_get();
        memset(&dlg, 0, sizeof dlg);
        dlg.kind = D_SLIDER;
        dlg.action = A_BRIGHTNESS;
        sncpy(dlg.title, sizeof dlg.title, "Brightness");
        dlg.min = 5, dlg.max = 100, dlg.step = 5, dlg.value = v < 5 ? 5 : v;
        snd(SND_SELECT);
        break;
    }
    case A_SOUNDS:
        cfg.sounds = !cfg.sounds;
        state_save();
        menu_rebuild(stack[depth]);
        snd(SND_SELECT);
        break;
    case A_THEME:
        sncpy(cfg.theme, sizeof cfg.theme, THEMES[it->arg].name);
        theme_apply();
        state_save();
        menu_rebuild(stack[depth]);
        snd(SND_SELECT);
        break;
    case A_MATERIAL:
        cfg.full_material = !cfg.full_material;
        cfg.material_explicit = 1;
        state_save();
        menu_rebuild(stack[depth]);
        snd(SND_SELECT);
        break;
    case A_CONFIRM:
        cfg.confirm_east = !cfg.confirm_east;
        state_save();
        menu_rebuild(stack[depth]);
        snd(SND_SELECT);
        break;
    case A_RESCAN:
        show_busy("Refreshing", "Looking for apps and games\xE2\x80\xA6");
        categories_rebuild();
        stack[0] = &cats[vis[cur]];
        dlg.kind = D_NONE;
        snd(SND_SELECT);
        break;
    case A_WIFI_RADIO:
        show_busy("Wi-Fi", it->arg ? "Turning Wi-Fi off\xE2\x80\xA6" : "Turning Wi-Fi on\xE2\x80\xA6");
        wifi_set_radio(!it->arg);
        if (!it->arg) SDL_Delay(2500);
        menu_rebuild(stack[depth]);
        dlg.kind = D_NONE;
        snd(SND_SELECT);
        break;
    case A_WIFI_NET:
        if (it->flags & IF_ACTIVE) {
            snprintf(buf, sizeof buf, "Disconnect from %s?", it->data);
            show_confirm(buf, "", A_WIFI_NET, it->data);
        } else if (it->flags & IF_SAVED) {
            wifi_join(it->data, NULL, 1);
        } else if (it->flags & IF_SECURED) {
            memset(&dlg, 0, sizeof dlg);
            dlg.kind = D_OSK;
            snprintf(dlg.title, sizeof dlg.title, "Password for %s", it->data);
            sncpy(dlg.ctx, sizeof dlg.ctx, it->data);
            dlg.r = 1;
            SDL_StartTextInput();
        } else {
            wifi_join(it->data, NULL, 0);
        }
        snd(SND_SELECT);
        break;
    case A_NOCORE: {
        const System *s = &systems[it->arg];
        if (*s->pkg) snprintf(buf, sizeof buf, "No emulator core was found for %s. Install one with:\nsudo apt install %s", s->name, s->pkg);
        else snprintf(buf, sizeof buf, "No emulator core was found for %s. Add one (%s) to ~/.config/retroarch/cores.", s->name, s->cores);
        show_message("Emulator missing", buf);
        snd(SND_ERROR);
        break;
    }
    default:
        break;
    }
}

static void power_action(int which)
{
    if (which == 0) {
        state_save();
        comp_quit();
        running = 0;
        return;
    }
    char out[512];
    char *a[] = { "systemctl", which == 1 ? "reboot" : "poweroff", NULL };
    guide.open = 0;
    show_busy(which == 1 ? "Restarting" : "Shutting down", NULL);
    state_save();
    if (run_capture(a, out, sizeof out, 10000) != 0) show_message("Couldn't do that", trim(out));
}

static void confirm_yes(int action, const char *ctx)
{
    if (action == A_WIFI_NET) {
        show_busy("Wi-Fi", "Disconnecting\xE2\x80\xA6");
        wifi_disconnect(ctx);
        menu_rebuild(stack[depth]);
        dlg.kind = D_NONE;
    } else if (action == A_LOGOUT) power_action(0);
    else if (action == A_REBOOT) power_action(1);
    else if (action == A_SHUTDOWN) power_action(2);
}

static void osk_type(const char *s)
{
    size_t l = strlen(dlg.buf);
    if (l + strlen(s) < sizeof dlg.buf - 1) strcat(dlg.buf, s);
    snd(SND_KEY);
}

static void osk_backspace(void)
{
    size_t l = strlen(dlg.buf);
    while (l > 0) {
        l--;
        if (((unsigned char)dlg.buf[l] & 0xC0) != 0x80) break;
    }
    dlg.buf[l] = 0;
}

static void osk_done(void)
{
    char ssid[256], pw[128];
    sncpy(ssid, sizeof ssid, dlg.ctx);
    sncpy(pw, sizeof pw, dlg.buf);
    SDL_StopTextInput();
    memset(&dlg, 0, sizeof dlg);
    if (strlen(pw) < 8) {
        show_message("Password too short", "Wi-Fi passwords are at least 8 characters.");
        snd(SND_ERROR);
        return;
    }
    wifi_join(ssid, pw, 0);
}

/* ======================================================================== */
/* Input                                                                    */
/* ======================================================================== */

enum { ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_OK, ACT_BACK, ACT_MENU, ACT_PGUP, ACT_PGDN, ACT_START, ACT_GUIDE, ACT_COUNT };

static void dialog_action(int a)
{
    switch (dlg.kind) {
    case D_BUSY:
        break;
    case D_MESSAGE:
        if (a == ACT_OK || a == ACT_BACK || a == ACT_START) {
            dlg.kind = D_NONE;
            snd(SND_BACK);
        }
        break;
    case D_CONFIRM:
        if (a == ACT_LEFT || a == ACT_RIGHT || a == ACT_UP || a == ACT_DOWN) {
            dlg.choice = !dlg.choice;
            snd(SND_STEP);
        } else if (a == ACT_OK) {
            int yes = dlg.choice == 0, act = dlg.action;
            char ctx[256];
            sncpy(ctx, sizeof ctx, dlg.ctx);
            dlg.kind = D_NONE;
            snd(yes ? SND_SELECT : SND_BACK);
            if (yes) confirm_yes(act, ctx);
        } else if (a == ACT_BACK) {
            dlg.kind = D_NONE;
            snd(SND_BACK);
        }
        break;
    case D_SLIDER: {
        int v = dlg.value;
        if (a == ACT_LEFT || a == ACT_DOWN) v -= dlg.step;
        else if (a == ACT_RIGHT || a == ACT_UP) v += dlg.step;
        else if (a == ACT_OK || a == ACT_BACK || a == ACT_START) {
            dlg.kind = D_NONE;
            snd(SND_BACK);
            if (stack[depth]->build == cat_build) menu_rebuild(stack[depth]);
            break;
        }
        v = v < dlg.min ? dlg.min : v > dlg.max ? dlg.max : v;
        if (v != dlg.value) {
            dlg.value = v;
            if (dlg.action == A_VOLUME) volume_set(v);
            else if (dlg.action == A_BRIGHTNESS && brightness_set(v) != 0) {
                show_message("Brightness", "Pixi can't change the backlight. Rerun install.sh to add the backlight rule, then reboot.");
                snd(SND_ERROR);
                break;
            }
            snd(SND_STEP);
        }
        break;
    }
    case D_OSK: {
        int cols = dlg.r < 4 ? 10 : 5;
        switch (a) {
        case ACT_UP:
            if (dlg.r > 0) {
                if (dlg.r == 4) dlg.c *= 2;
                dlg.r--;
            }
            snd(SND_STEP);
            break;
        case ACT_DOWN:
            if (dlg.r < 4) {
                dlg.r++;
                if (dlg.r == 4) dlg.c /= 2;
            }
            snd(SND_STEP);
            break;
        case ACT_LEFT: dlg.c = (dlg.c + cols - 1) % cols; snd(SND_STEP); break;
        case ACT_RIGHT: dlg.c = (dlg.c + 1) % cols; snd(SND_STEP); break;
        case ACT_OK:
            if (dlg.r < 4) {
                char s[2] = { osk_rows[dlg.layer][dlg.r][dlg.c], 0 };
                osk_type(s);
                if (dlg.layer == 1) dlg.layer = 0;
            } else if (dlg.c == 0) dlg.layer = dlg.layer == 1 ? 0 : 1;
            else if (dlg.c == 1) dlg.layer = dlg.layer == 2 ? 0 : 2;
            else if (dlg.c == 2) osk_type(" ");
            else if (dlg.c == 3) osk_backspace();
            else if (dlg.c == 4) osk_done();
            break;
        case ACT_BACK:
            if (dlg.buf[0]) osk_backspace(), snd(SND_KEY);
            else {
                SDL_StopTextInput();
                dlg.kind = D_NONE;
                snd(SND_BACK);
            }
            break;
        case ACT_MENU: dlg.layer = dlg.layer == 1 ? 0 : 1; snd(SND_STEP); break;
        case ACT_PGUP: dlg.layer = dlg.layer == 2 ? 0 : 2; snd(SND_STEP); break;
        case ACT_START: osk_done(); break;
        }
        break;
    }
    }
}

static void guide_action(int a)
{
    if (guide.power_open) {
        if (a == ACT_UP && guide.power_sel > 0) guide.power_sel--, snd(SND_GUIDE_STEP);
        else if (a == ACT_DOWN && guide.power_sel < 2) guide.power_sel++, snd(SND_GUIDE_STEP);
        else if (a == ACT_BACK) guide.power_open = 0, snd(SND_BACK);
        else if (a == ACT_OK) {
            snd(SND_GUIDE_SELECT);
            static const int acts[3] = { A_LOGOUT, A_REBOOT, A_SHUTDOWN };
            static const char *q[3] = { "Log out of Pixi?", "Restart now?", "Shut down now?" };
            guide.power_open = 0;
            show_confirm(q[guide.power_sel], guide.power_sel == 0 ? "Running applications will be closed." : NULL,
                         acts[guide.power_sel], NULL);
        }
        return;
    }
    switch (a) {
    case ACT_GUIDE:
    case ACT_BACK:
        guide_close(1);
        break;
    case ACT_UP:
        if (guide.row == G_POWER) {
            guide.row = G_ENTRY;
            guide.entry = guide.nentries - 1;
        } else if (guide.row == G_ENTRY && guide.entry > 0) guide.entry--;
        else if (guide.row == G_ENTRY) guide.row = G_SLIDER;
        else if (guide.row == G_SLIDER) guide.row = G_TILE;
        snd(SND_GUIDE_STEP);
        break;
    case ACT_DOWN:
        if (guide.row == G_TILE) guide.row = G_SLIDER;
        else if (guide.row == G_SLIDER) guide.row = G_ENTRY, guide.entry = 0;
        else if (guide.row == G_ENTRY && guide.entry < guide.nentries - 1) guide.entry++;
        else if (guide.row == G_ENTRY) guide.row = G_POWER;
        snd(SND_GUIDE_STEP);
        break;
    case ACT_LEFT:
    case ACT_RIGHT: {
        int d = a == ACT_LEFT ? -1 : 1;
        if (guide.row == G_TILE) {
            guide.tile = (guide.tile + 4 + d) % 4;
            snd(SND_GUIDE_STEP);
        } else if (guide.row == G_SLIDER && guide.volume >= 0) {
            int v = guide.volume + d * 5;
            v = v < 0 ? 0 : v > 100 ? 100 : v;
            if (v != guide.volume) {
                guide.volume = v;
                volume_set(v);
                snd(SND_GUIDE_STEP);
            }
        }
        break;
    }
    case ACT_OK:
        snd(SND_GUIDE_SELECT);
        if (guide.row == G_POWER) {
            guide.power_open = 1;
            guide.power_sel = 0;
        } else if (guide.row == G_TILE) {
            /* Tiles jump to the matching place on the start screen. */
            static const char *targets[4] = { "Wi-Fi", "Brightness", "Accent color", "System information" };
            guide_close(0);
            while (depth > 0) pop_menu();
            for (int i = 0; i < nvis; i++)
                if (vis[i] == CAT_SETTINGS) cur = i;
            stack[0] = &cats[CAT_SETTINGS];
            menu_rebuild(stack[0]);
            for (int i = 0; i < stack[0]->n; i++)
                if (!strcmp(stack[0]->items[i].title, targets[guide.tile])) stack[0]->sel = i;
        } else if (guide.row == G_ENTRY && guide.entry < guide.nentries) {
            GuideEntry *e = &guide.entries[guide.entry];
            if (e->what == GE_RESUME) guide_close(1);
            else if (e->what == GE_CLOSE) {
                comp_close(e->id);
                guide_close(0);
            } else if (e->what == GE_SWITCH) {
                guide.open = 0;
                comp_activate(e->id);
            } else guide_close(0);
        }
        break;
    }
}

static void switch_category(int i)
{
    if (i < 0 || i >= nvis || i == cur) return;
    cur = i;
    cfg.category = vis[i];
    stack[0] = &cats[vis[i]];
    if (vis[i] == CAT_SETTINGS) menu_rebuild(stack[0]);
    snd(SND_STEP);
}

static void handle_action(int a)
{
    if (a == ACT_GUIDE) {
        if (guide.open) guide_close(1);
        else guide_open();
        return;
    }
    if (hidden) return; /* an application is in front */
    if (launch.active) {
        if (a == ACT_BACK) launch.active = 0; /* stop waiting; the app may still open */
        return;
    }
    if (dlg.kind != D_NONE) {
        dialog_action(a);
        return;
    }
    if (guide.open) {
        guide_action(a);
        return;
    }
    Menu *m = stack[depth];
    Item *it = (m->n && m->sel < m->n) ? &m->items[m->sel] : NULL;
    switch (a) {
    case ACT_UP: if (m->sel > 0) m->sel--, snd(SND_STEP); break;
    case ACT_DOWN: if (m->sel < m->n - 1) m->sel++, snd(SND_STEP); break;
    case ACT_PGUP: if (m->sel > 0) m->sel = m->sel > 6 ? m->sel - 6 : 0, snd(SND_STEP); break;
    case ACT_PGDN: if (m->sel < m->n - 1) m->sel = m->sel + 6 < m->n - 1 ? m->sel + 6 : m->n - 1, snd(SND_STEP); break;
    case ACT_LEFT: if (depth == 0) switch_category(cur - 1); else pop_menu(); break;
    case ACT_RIGHT:
        if (depth == 0) switch_category(cur + 1);
        else if (it && it->kind == K_FOLDER) push_menu(it);
        break;
    case ACT_BACK: if (depth > 0) pop_menu(); break;
    case ACT_OK:
    case ACT_START:
        if (!it) break;
        if (it->kind == K_LAUNCH) launch_item(it);
        else if (it->kind == K_FOLDER) push_menu(it);
        else if (it->kind == K_ACTION || it->action) do_action(it);
        else if (it->detail) show_message(it->title, it->detail), snd(SND_SELECT);
        break;
    case ACT_MENU:
        if (it && it->detail) show_message(it->title, it->detail), snd(SND_SELECT);
        break;
    }
}

#define REPEAT_DELAY 320
#define REPEAT_RATE 75
static int held[ACT_COUNT];
static Uint32 next_rep[ACT_COUNT];
static int axis_state[8][2], hat_state[8];
static int select_held, start_held;
static SDL_GameController *pads[8];
static SDL_Joystick *joys[8];
static Uint32 quiet_until;

static void act_press(int a)
{
    if (a < 0 || a >= ACT_COUNT) return;
    held[a] = 1;
    next_rep[a] = SDL_GetTicks() + REPEAT_DELAY;
    if (SDL_GetTicks() < quiet_until && a != ACT_GUIDE) return;
    handle_action(a);
}

static void act_release(int a)
{
    if (a >= 0 && a < ACT_COUNT) held[a] = 0;
}

static void repeat_tick(Uint32 now)
{
    static const int rep[] = { ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_PGUP, ACT_PGDN };
    for (size_t i = 0; i < sizeof rep / sizeof *rep; i++) {
        int a = rep[i];
        if (held[a] && now >= next_rep[a]) {
            next_rep[a] = now + REPEAT_RATE;
            handle_action(a);
        }
    }
}

/* Select + Start together is the guide, for controllers without a guide button. */
static void combo_check(void)
{
    if (select_held && start_held) {
        select_held = start_held = 0;
        held[ACT_START] = held[ACT_MENU] = 0;
        handle_action(ACT_GUIDE);
    }
}

static int key_action(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP: return ACT_UP;
    case SDLK_DOWN: return ACT_DOWN;
    case SDLK_LEFT: return ACT_LEFT;
    case SDLK_RIGHT: return ACT_RIGHT;
    case SDLK_RETURN: case SDLK_KP_ENTER: return ACT_OK;
    case SDLK_ESCAPE: return ACT_BACK;
    case SDLK_BACKSPACE: return dlg.kind == D_OSK ? -1 : ACT_BACK;
    case SDLK_TAB: return ACT_MENU;
    case SDLK_PAGEUP: return ACT_PGUP;
    case SDLK_PAGEDOWN: return ACT_PGDN;
    case SDLK_SPACE: return dlg.kind == D_OSK ? -1 : ACT_OK;
    case SDLK_F1: case SDLK_HOME: return ACT_GUIDE;
    default: return -1;
    }
}

static int pad_action(int b)
{
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A: return cfg.confirm_east ? ACT_BACK : ACT_OK;
    case SDL_CONTROLLER_BUTTON_B: return cfg.confirm_east ? ACT_OK : ACT_BACK;
    case SDL_CONTROLLER_BUTTON_X: return ACT_PGUP;
    case SDL_CONTROLLER_BUTTON_Y: return ACT_MENU;
    case SDL_CONTROLLER_BUTTON_BACK: return ACT_MENU;
    case SDL_CONTROLLER_BUTTON_START: return ACT_START;
    case SDL_CONTROLLER_BUTTON_GUIDE: return ACT_GUIDE;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return ACT_PGUP;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return ACT_PGDN;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return ACT_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return ACT_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return ACT_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ACT_RIGHT;
    default: return -1;
    }
}

static int joy_action(int b)
{
    if (b == cfg.joy_ok) return cfg.confirm_east ? ACT_BACK : ACT_OK;
    if (b == cfg.joy_back) return cfg.confirm_east ? ACT_OK : ACT_BACK;
    if (b == cfg.joy_menu) return ACT_MENU;
    if (b == cfg.joy_pgup) return ACT_PGUP;
    if (b == cfg.joy_pgdn) return ACT_PGDN;
    if (b == cfg.joy_start) return ACT_START;
    if (b == cfg.joy_select) return ACT_MENU;
    if (b == cfg.joy_guide) return ACT_GUIDE;
    return -1;
}

static void open_inputs(void)
{
    for (int i = 0; i < SDL_NumJoysticks() && i < 8; i++) {
        if (SDL_IsGameController(i)) {
            if (!pads[i]) pads[i] = SDL_GameControllerOpen(i);
            if (cfg.debug_input && pads[i]) printf("input: controller %d: %s\n", i, SDL_GameControllerName(pads[i]));
        } else {
            if (!joys[i]) joys[i] = SDL_JoystickOpen(i);
            if (cfg.debug_input && joys[i]) printf("input: joystick %d (raw buttons): %s\n", i, SDL_JoystickName(joys[i]));
        }
    }
}

static int dev_slot(SDL_JoystickID id)
{
    for (int i = 0; i < 8; i++) {
        SDL_Joystick *j = pads[i] ? SDL_GameControllerGetJoystick(pads[i]) : joys[i];
        if (j && SDL_JoystickInstanceID(j) == id) return i;
    }
    return 0;
}

static void axis_input(int slot, int axis, int value)
{
    if (axis > 1 || slot < 0 || slot >= 8) return;
    int nv = value < -16000 ? -1 : value > 16000 ? 1 : (abs(value) < 9000 ? 0 : axis_state[slot][axis]);
    int ov = axis_state[slot][axis];
    if (nv == ov) return;
    int neg = axis == 0 ? ACT_LEFT : ACT_UP, pos = axis == 0 ? ACT_RIGHT : ACT_DOWN;
    if (ov == -1) act_release(neg);
    if (ov == 1) act_release(pos);
    axis_state[slot][axis] = nv;
    if (nv == -1) act_press(neg);
    if (nv == 1) act_press(pos);
}

static void hat_input(int slot, int v)
{
    if (slot < 0 || slot >= 8) return;
    int ov = hat_state[slot];
    const int bits[4] = { SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT };
    const int acts[4] = { ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT };
    for (int i = 0; i < 4; i++) {
        if ((ov & bits[i]) && !(v & bits[i])) act_release(acts[i]);
        if (!(ov & bits[i]) && (v & bits[i])) act_press(acts[i]);
    }
    hat_state[slot] = v;
}

static void handle_event(SDL_Event *e)
{
    switch (e->type) {
    case SDL_QUIT: running = 0; break;
    case SDL_KEYDOWN:
        if (e->key.repeat) break;
        if (dlg.kind == D_OSK) {
            if (e->key.keysym.sym == SDLK_BACKSPACE) { osk_backspace(); break; }
            if (e->key.keysym.sym == SDLK_RETURN || e->key.keysym.sym == SDLK_KP_ENTER) { osk_done(); break; }
        }
        act_press(key_action(e->key.keysym.sym));
        break;
    case SDL_KEYUP: act_release(key_action(e->key.keysym.sym)); break;
    case SDL_TEXTINPUT: if (dlg.kind == D_OSK) osk_type(e->text.text); break;
    case SDL_CONTROLLERBUTTONDOWN:
        if (cfg.debug_input) printf("input: controller button %d\n", e->cbutton.button);
        if (e->cbutton.button == SDL_CONTROLLER_BUTTON_BACK) select_held = 1;
        if (e->cbutton.button == SDL_CONTROLLER_BUTTON_START) start_held = 1;
        if (select_held && start_held) { combo_check(); break; }
        if (hidden && e->cbutton.button != SDL_CONTROLLER_BUTTON_GUIDE) break;
        if (e->cbutton.button == SDL_CONTROLLER_BUTTON_BACK || e->cbutton.button == SDL_CONTROLLER_BUTTON_START) {
            held[pad_action(e->cbutton.button)] = 1; /* acted on at release, so the combo can win */
            break;
        }
        act_press(pad_action(e->cbutton.button));
        break;
    case SDL_CONTROLLERBUTTONUP:
        if (e->cbutton.button == SDL_CONTROLLER_BUTTON_BACK || e->cbutton.button == SDL_CONTROLLER_BUTTON_START) {
            int a = pad_action(e->cbutton.button);
            int was = held[a] && (e->cbutton.button == SDL_CONTROLLER_BUTTON_BACK ? select_held : start_held);
            if (e->cbutton.button == SDL_CONTROLLER_BUTTON_BACK) select_held = 0;
            else start_held = 0;
            held[a] = 0;
            if (was && !hidden) handle_action(a);
            break;
        }
        act_release(pad_action(e->cbutton.button));
        break;
    case SDL_CONTROLLERAXISMOTION:
        if (!hidden && e->caxis.axis <= SDL_CONTROLLER_AXIS_LEFTY) axis_input(dev_slot(e->caxis.which), e->caxis.axis, e->caxis.value);
        break;
    case SDL_JOYBUTTONDOWN:
        if (SDL_GameControllerFromInstanceID(e->jbutton.which)) break;
        if (cfg.debug_input) printf("input: joystick button %d\n", e->jbutton.button);
        if (e->jbutton.button == cfg.joy_select) select_held = 1;
        if (e->jbutton.button == cfg.joy_start) start_held = 1;
        if (select_held && start_held) { combo_check(); break; }
        if (hidden && joy_action(e->jbutton.button) != ACT_GUIDE) break;
        act_press(joy_action(e->jbutton.button));
        break;
    case SDL_JOYBUTTONUP:
        if (SDL_GameControllerFromInstanceID(e->jbutton.which)) break;
        if (e->jbutton.button == cfg.joy_select) select_held = 0;
        if (e->jbutton.button == cfg.joy_start) start_held = 0;
        act_release(joy_action(e->jbutton.button));
        break;
    case SDL_JOYHATMOTION:
        if (!hidden && !SDL_GameControllerFromInstanceID(e->jhat.which)) hat_input(dev_slot(e->jhat.which), e->jhat.value);
        break;
    case SDL_JOYAXISMOTION:
        if (!hidden && !SDL_GameControllerFromInstanceID(e->jaxis.which))
            axis_input(dev_slot(e->jaxis.which), e->jaxis.axis, e->jaxis.value);
        break;
    case SDL_CONTROLLERDEVICEADDED:
    case SDL_JOYDEVICEADDED: open_inputs(); break;
    case SDL_CONTROLLERDEVICEREMOVED:
    case SDL_JOYDEVICEREMOVED:
        for (int i = 0; i < 8; i++) {
            if (pads[i] && !SDL_GameControllerGetAttached(pads[i])) SDL_GameControllerClose(pads[i]), pads[i] = NULL;
            if (joys[i] && !SDL_JoystickGetAttached(joys[i])) SDL_JoystickClose(joys[i]), joys[i] = NULL;
        }
        memset(held, 0, sizeof held);
        break;
    }
}

/* ======================================================================== */
/* Test script (development): --script "right;down;ok;wait:30;shot:a.png"  */
/* ======================================================================== */

static char *script_pos;
static int script_wait;

static void screenshot(const char *path)
{
    unsigned char *px = xmalloc((size_t)W * (size_t)H * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_RGBA32);
    for (int y = 0; y < H; y++) memcpy((unsigned char *)s->pixels + y * s->pitch, px + (size_t)(H - 1 - y) * W * 4, (size_t)W * 4);
    if (IMG_SavePNG(s, path) == 0) printf("screenshot: %s\n", path);
    SDL_FreeSurface(s);
    free(px);
}

static void script_step(void)
{
    if (!script_pos) return;
    if (script_wait > 0) {
        script_wait--;
        return;
    }
    while (*script_pos == ';' || *script_pos == ' ') script_pos++;
    if (!*script_pos) {
        script_pos = NULL;
        running = 0;
        return;
    }
    char cmd[512];
    size_t n = strcspn(script_pos, ";");
    if (n >= sizeof cmd) n = sizeof cmd - 1;
    memcpy(cmd, script_pos, n);
    cmd[n] = 0;
    script_pos += n;
    static const struct { const char *name; int act; } map[] = {
        { "up", ACT_UP }, { "down", ACT_DOWN }, { "left", ACT_LEFT }, { "right", ACT_RIGHT }, { "ok", ACT_OK },
        { "back", ACT_BACK }, { "menu", ACT_MENU }, { "pgup", ACT_PGUP }, { "pgdn", ACT_PGDN }, { "start", ACT_START },
        { "guide", ACT_GUIDE },
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(cmd, map[i].name)) {
            handle_action(map[i].act);
            script_wait = 12;
            return;
        }
    if (!strncmp(cmd, "wait:", 5)) script_wait = atoi(cmd + 5);
    else if (!strncmp(cmd, "shot:", 5)) screenshot(cmd + 5);
    else if (!strncmp(cmd, "type:", 5)) osk_type(cmd + 5);
    else if (!strncmp(cmd, "theme:", 6)) sncpy(cfg.theme, sizeof cfg.theme, cmd + 6), theme_apply();
    else if (!strcmp(cmd, "full")) cfg.full_material = 1;
    else if (!strcmp(cmd, "quit")) running = 0;
    else fprintf(stderr, "pixi-shell: script: unknown step '%s'\n", cmd);
}

/* ======================================================================== */
/* main                                                                     */
/* ======================================================================== */

static void on_sigchld(int s) { (void)s; got_sigchld = 1; }
static void on_term(int s) { (void)s; got_term = 1; }

static void find_datadir(const char *argv0)
{
    const char *env = getenv("PIXI_DATA");
    if (env && *env) {
        sncpy(datadir, sizeof datadir, env);
        return;
    }
    /* next to the binary while developing: shell/../assets */
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            char probe[1200];
            snprintf(probe, sizeof probe, "%s/../assets/glyphs/category-games.png", exe);
            if (file_exists(probe)) {
                snprintf(datadir, sizeof datadir, "%s/../assets", exe);
                return;
            }
        }
    }
    (void)argv0;
    sncpy(datadir, sizeof datadir, PIXI_DATADIR);
}

static void usage(void)
{
    puts("pixi-shell " PIXI_VERSION " - the Pixi shell (normally started by the pixi compositor)\n\n"
         "usage: pixi-shell [options]\n"
         "  --config FILE      settings file (default ~/.config/pixi/pixi.conf)\n"
         "  --windowed WxH     window size, for testing outside the compositor\n"
         "  --debug-input      print controller buttons as they are pressed\n"
         "  --version          print the version");
}

int main(int argc, char **argv)
{
    char config_path[1024];
    snprintf(config_path, sizeof config_path, "%s/.config/pixi/pixi.conf", home_dir());
    const char *script = NULL;
    int debug_input = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc) sncpy(config_path, sizeof config_path, argv[++i]);
        else if (!strcmp(argv[i], "--windowed") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &windowed_w, &windowed_h) != 2) die("--windowed needs WIDTHxHEIGHT");
        } else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--debug-input")) debug_input = 1;
        else if (!strcmp(argv[i], "--version")) { puts("pixi-shell " PIXI_VERSION); return 0; }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else { fprintf(stderr, "pixi-shell: unknown option %s\n", argv[i]); usage(); return 2; }
    }
    cfg_defaults();
    cfg_load(config_path);
    state_load();
    if (debug_input) cfg.debug_input = 1;
    if (cfg.debug_input) setvbuf(stdout, NULL, _IOLBF, 0);
    theme_apply();
    find_datadir(argv[0]);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = on_term;
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    setenv("SDL_VIDEO_WAYLAND_WMCLASS", "pixi-shell", 1);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_VIDEO_EGL_ALLOW_TRANSPARENCY, "1");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint("SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR", "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) die("SDL_Init: %s", SDL_GetError());
    if (TTF_Init() != 0) die("TTF_Init: %s", TTF_GetError());
    IMG_Init(IMG_INIT_PNG);
    char db[1024];
    snprintf(db, sizeof db, "%s/.config/pixi/gamecontrollerdb.txt", home_dir());
    if (file_exists(db)) SDL_GameControllerAddMappingsFromFile(db);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    int ww = windowed_w > 0 ? windowed_w : 1280, wh = windowed_h > 0 ? windowed_h : 720;
    win = SDL_CreateWindow("Pixi", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
                           SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | (windowed_w > 0 ? 0 : SDL_WINDOW_BORDERLESS));
    if (!win) die("couldn't open a window: %s", SDL_GetError());
    glctx = SDL_GL_CreateContext(win);
    if (!glctx) die("couldn't create an OpenGL ES 3.0 context: %s", SDL_GetError());
    SDL_GL_SetSwapInterval(1);
    SDL_ShowCursor(SDL_DISABLE);
    gl_init();
    audio_init();
    open_inputs();
    comp_connect();

    show_busy("Pixi", "Looking for apps and games\xE2\x80\xA6");
    categories_rebuild();
    dlg.kind = D_NONE;
    stack[0] = &cats[vis[cur]];
    catpos = (float)cur;
    script_pos = script ? xstrdup(script) : NULL;

    Uint32 start = SDL_GetTicks(), last = start;
    while (running && !got_term) {
        comp_pump();
        if (guide_requested) {
            guide_requested = 0;
            handle_action(ACT_GUIDE);
        }
        hidden = pixi && comp_mode == MODE_APP && !guide.open;
        SDL_Event e;
        if (hidden) {
            if (SDL_WaitEventTimeout(&e, 100)) handle_event(&e);
        }
        while (SDL_PollEvent(&e)) handle_event(&e);
        if (got_sigchld) {
            got_sigchld = 0;
            reap_children();
        }
        Uint32 now = SDL_GetTicks();
        repeat_tick(now);
        script_step();
        if (!running) break;
        float dt = script_pos ? 1.0f / 60.0f : fminf((now - last) / 1000.0f, 0.1f);
        last = now;
        now_s = script_pos ? now_s + dt : (now - start) / 1000.0f;

        float k = 1.0f - expf(-14.0f * dt);
        catpos += ((float)cur - catpos) * k;
        dpos += ((float)depth - dpos) * (1.0f - expf(-13.0f * dt));
        Menu *m = stack[depth];
        m->pos += ((float)m->sel - m->pos) * (1.0f - expf(-16.0f * dt));
        dlg.anim = dlg.kind ? dlg.anim + (1 - dlg.anim) * (1.0f - expf(-18.0f * dt)) : 0;
        guide.anim = guide.open ? guide.anim + (1 - guide.anim) * (1.0f - expf(-12.0f * dt)) : 0;
        if (launch.active && now_s - launch.started > 25) launch.active = 0;
        if (now_s >= status_next) {
            status_read(&status);
            status_next = now_s + 3;
        }
        if (frame_no % 600 == 0) text_cache_trim();
        if (hidden) continue;
        Uint64 t0 = SDL_GetPerformanceCounter();
        render_frame();
        /* Full material on a machine that can't keep up: fall back to Simple, once. */
        static double slow_sum;
        static int slow_n;
        if (!script_pos && !cfg.material_explicit && cfg.full_material && now_s > 3 && slow_n < 240) {
            slow_sum += (double)(SDL_GetPerformanceCounter() - t0) / (double)SDL_GetPerformanceFrequency();
            if (++slow_n == 240 && slow_sum / slow_n > 0.024) {
                fprintf(stderr, "pixi-shell: %.1f ms a frame in the Full theme; switching to Simple\n", slow_sum / slow_n * 1000);
                cfg.full_material = 0;
                cfg.material_explicit = 1;
                state_save();
            }
        }
    }
    state_save();
    Mix_CloseAudio();
    SDL_GL_DeleteContext(glctx);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
