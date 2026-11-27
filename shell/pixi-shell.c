/*
 * pixi-shell - the Pixi ("Pi Cross Interface") shell.
 *
 * A Wayland client of the pixi compositor, drawn with OpenGL ES 3 through
 * SDL2. Its look follows LineXinBar's shell (GPL-3.0-only): the same layout
 * measurements, theme colours, glyphs, sounds and GPU materials, ported to C.
 *
 * The compositor shows every application full screen. This shell is the
 * home screen (categories across, items down), the guide overlay drawn over
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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
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
#include "ftp.h"
#include "web.h"
#include "video.h"
#include "browser.h"
#include "terminal.h"
#include "pics.h"
#include <qrencode.h>

#ifndef PIXI_VERSION /* set by the Makefile to the build date, in UTC */
#define PIXI_VERSION "unknown"
#endif
#ifndef PIXI_DATADIR
#define PIXI_DATADIR "/usr/local/share/pixi"
#endif

/* ======================================================================== */
/* LineXinBar's layout, in pixels at 1080 lines (crates/lxb-desktop/src/ui.rs) */
/* ======================================================================== */

#define REF_H 1080.0f
#define CATEGORY_SPACING 168.0f
#define ITEM_SPACING 88.0f
#define CATEGORY_ICON 70.0f
#define CATEGORY_ICON_FOCUSED 120.0f
#define ITEM_ICON 46.0f
#define ITEM_ICON_FOCUSED 74.0f
#define ITEM_DISC 1.4707913f /* sqrt(2) * 1.04 */
#define CATEGORY_DISC 1.30f
#define GLYPH_DEPTH 0.075f
#define CATEGORY_LABEL 22.0f
#define CATEGORY_LABEL_ABOVE 3.0f
#define CATEGORY_LABEL_BELOW 18.0f
#define ITEM_GAP_ABOVE 130.0f
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
#define HINT_GLYPH 40.0f
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
    { "silver", "Silver", 0x9CA3AF, 0xB1B6BE, 0x393C42, 0x0D0E0F, 0xC5C6C9, 0xDDDEDF, { 0x191A1D, 0x08090A, 0x222428, 0x0D0E0F }, 0x42464D },
    { "gold", "Gold", 0xEAB308, 0xF7D878, 0x685312, 0x161206, 0xDCD2B2, 0xE9E3D3, { 0x2C2409, 0x0F0C03, 0x3D310D, 0x171305 }, 0x796115 },
    { "lime", "Lime", 0x84CC16, 0xC3EE81, 0x456218, 0x0F1507, 0xCBD9B5, 0xE0E7D5, { 0x1E2A0C, 0x0A0E04, 0x293A10, 0x101606 }, 0x50731C },
    { "emerald", "Emerald", 0x10B981, 0x7FF1CB, 0x17644A, 0x071510, 0xB4DACD, 0xD4E8E1, { 0x0B2B20, 0x040E0B, 0x0F3B2C, 0x061611 }, 0x1A7457 },
    { "cyan", "Cyan", 0x06B6D4, 0x78E5F8, 0x125C69, 0x061416, 0xB2D6DC, 0xD3E6E9, { 0x09272C, 0x030D0F, 0x0D363D, 0x051517 }, 0x156B7A },
    { "sky", "Sky", 0x0EA5E9, 0x7BCEF4, 0x154D66, 0x071115, 0xB3CEDB, 0xD4E2E8, { 0x0A212B, 0x030B0E, 0x0E2E3C, 0x051117 }, 0x185977 },
    { "violet", "Violet", 0x7C3AED, 0xA97FF0, 0x331763, 0x0C0715, 0xC2B4DA, 0xDBD4E8, { 0x170B2A, 0x08040E, 0x1F0F3B, 0x0C0616 }, 0x3C1B74 },
    { "fuchsia", "Fuchsia", 0xD946EF, 0xE27FF1, 0x5A1764, 0x130715, 0xD5B4DA, 0xE5D4E8, { 0x260B2B, 0x0D040E, 0x350F3B, 0x140616 }, 0x691A74 },
    { "rose", "Rose", 0xF43F5E, 0xF47B90, 0x661422, 0x160709, 0xDBB3BA, 0xE8D4D7, { 0x2B0A10, 0x0E0305, 0x3C0E16, 0x170508 }, 0x771828 },
    { "crimson", "Crimson", 0xBE123C, 0xF0809B, 0x63172A, 0x15070A, 0xD9B4BD, 0xE7D4D9, { 0x2A0B13, 0x0E0406, 0x3A101A, 0x16060A }, 0x741B31 },
    { "brown", "Brown", 0xA16207, 0xD6BD99, 0x524129, 0x120F0A, 0xD1C9BD, 0xE3DFD9, { 0x231C12, 0x0C0906, 0x312719, 0x120F0A }, 0x5F4C2F },
    { "slate", "Slate", 0x64748B, 0xABB5C4, 0x353C45, 0x0D0E10, 0xC3C6CB, 0xDCDDE0, { 0x171A1E, 0x08090A, 0x20242A, 0x0C0E10 }, 0x3E4651 },
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
static GLuint fb_snap, tex_snap; /* a small picture of the home screen, for the app switcher */
static int snap_w, snap_h;
static int snap_have;
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
    if (tex_snap) glDeleteTextures(1, &tex_snap), glDeleteFramebuffers(1, &fb_snap);
    snap_w = 512;
    snap_h = (int)(512.0f * (float)H / (float)W + 0.5f);
    make_target(&fb_snap, &tex_snap, snap_w, snap_h, 0);
    snap_have = 0;
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

/* A choice mark: kind 1 is a radio button (a ring, with a dot when it is the one in force), kind 2 a
 * check box (a square, with a tick when it is on); unticked ones are just the empty shape. Kind 0 is the
 * old badge, shown only for the value in force. */
static void mark_draw(int kind, int on, float cx, float cy, float size, Col c, float fade)
{
    if (kind == 1) glyph(on ? "radio-on" : "radio-off", cx, cy, size, c, fade);
    else if (kind == 2) glyph(on ? "checkbox-on" : "checkbox-off", cx, cy, size, c, fade);
    else if (on) glyph("chosen", cx, cy, size, c, fade);
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
    snprintf(out, cap, "%s/.cache/pixi/store-icons/%s.png", home_dir(), name); /* the App Store's downloaded icons */
    return access(out, R_OK) == 0;
}

/* forget the icons that weren't found, so ones that have arrived since are looked for again */
static void icon_retry(void)
{
    int k = 0;
    for (int i = 0; i < nicons; i++) {
        if (icon_cache[i].tex) icon_cache[k++] = icon_cache[i];
        else free(icon_cache[i].name);
    }
    nicons = k;
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

static const char *mono_regular[] = { "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/noto/NotoSansMono-Regular.ttf",
                                      "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", NULL };
static const char *mono_bold[] = { "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", "/usr/share/fonts/truetype/noto/NotoSansMono-Bold.ttf",
                                   "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf", NULL };
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
        for (const char **p = (bold & 1) ? cjk_bold : cjk_regular; *p && !f; p++) f = TTF_OpenFontIndex(*p, size, 0);
    } else if (bold & 2) { /* the terminal's monospaced face */
        for (const char **p = (bold & 1) ? mono_bold : mono_regular; *p && !f; p++) f = TTF_OpenFont(*p, size);
    }
    if (!f) {
        char path[1200];
        snprintf(path, sizeof path, "%s/fonts/%s", datadir, (bold & 1) ? "Roboto-Bold.ttf" : "Roboto-Regular.ttf");
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

/* How a window is controlled from the gamepad: as it is, or as a keyboard and mouse. */
enum { PAD_GAMEPAD = 0, PAD_MOUSE = 1 };

typedef struct {
    uint32_t id;
    char app_id[128], title[256];
    int pid;
    int padmode;
    char dsk[160];   /* the desktop id it was launched from; empty for a window nobody here launched (never paused) */
    float born;      /* when it appeared */
    int stopped;     /* its processes are paused (SIGSTOP) */
} Win;

static struct wl_display *wl_dpy;
static struct wl_event_queue *wl_queue;
static struct pixi_shell_v1 *pixi;
static uint32_t pixi_ver; /* the protocol version bound; the pointer and key requests need 3, screenshots 4 */
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
        pixi_ver = version < 4 ? version : 4;
        pixi = wl_registry_bind(reg, name, &pixi_shell_v1_interface, pixi_ver);
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

static void bg_wake_id(uint32_t id);

static void comp_close(uint32_t id)
{
    if (!pixi) return;
    bg_wake_id(id); /* a paused program can't answer "please close" */
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

enum { ACT_UP, ACT_DOWN, ACT_LEFT, ACT_RIGHT, ACT_OK, ACT_BACK, ACT_MENU, ACT_PGUP, ACT_PGDN, ACT_START, ACT_GUIDE, ACT_SQUARE, ACT_COUNT };
enum { K_LAUNCH, K_FOLDER, K_ACTION, K_INFO };
enum {
    A_NONE, A_VOLUME, A_BRIGHTNESS, A_SOUNDS, A_THEME, A_CONFIRM, A_WIFI_NET, A_WIFI_RADIO,
    A_RESCAN, A_NOCORE, A_LOGOUT, A_REBOOT, A_SHUTDOWN,
    A_UPDATES, A_SERVERS, A_WALL_SET, A_WALL_DEFAULT, A_VIEW, A_SLIDESHOW, A_PLAY, A_PLAYALL, A_SHUFFLEALL, A_NOWPLAYING, A_PLAYVIDEO, A_UPDATE, A_UPDATE_PIXI, A_DELETE, A_CLOCK, A_DATEFMT, A_BATPCT, A_SYSINFO, A_GAMEPAD, A_STORE_OPEN, A_STORE_REMOVE, A_STORE_SEARCH,
    A_ACCENT, A_FTP, A_FILE_RENAME, A_FILE_MKDIR, A_BROWSER, A_POWER, A_WIFI_FORGET, A_WIFI_EDIT, A_BR_ADDR, A_BR_FIELD, A_BR_INSTALL, A_TERMINAL, A_TERM_APP, A_VIDEOOUT, A_SUDO_PASS, A_NETWORK, A_NET_SSID, A_NET_HIDPW, A_WEB, A_WEB_EDIT, A_WEB_RELOAD, A_WEB_RESET, A_WEB_INSTALL, A_ED_DISCARD, A_SSH, A_PIAPPS_INSTALL, A_APP_TYPE, A_APP_BG, A_APP_HIDE, A_APPLIST
};
enum { IF_SECURED = 1, IF_SAVED = 2, IF_ACTIVE = 4, IF_FILE = 8, IF_APP = 16, IF_ROM = 32, IF_MEDIA = 64, IF_PC = 128 };

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

static int appset_hidden(const char *id);

static void add_app(Menu *m, const AppEntry *e)
{
    if (appset_hidden(e->id)) return; /* hidden from the home screen (Options > Hide Item, or the Apps List) */
    char **a = exec_to_argv(e->exec);
    if (!a) return;
    /* command-line apps open in Pixi's own terminal */
    Item *it = menu_add(m, e->name, *e->generic ? e->generic : e->comment, NULL, e->terminal ? K_ACTION : K_LAUNCH);
    if (e->terminal) it->action = A_TERM_APP;
    it->icon = *e->icon ? xstrdup(e->icon) : NULL;
    it->glyph = xstrdup("launch");
    it->argv = a;
    it->detail = xstrdup(e->exec);
    it->flags |= IF_APP;
    it->data = xstrdup(e->id);
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
        it->flags |= IF_ROM;
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

/* Games > Console: the systems that have games, each opening its list */
static void build_consoles(Menu *m)
{
    for (int i = 0; i < nsystems; i++) {
        StrList roms = { 0 };
        system_roms(&systems[i], &roms, 20000);
        if (roms.n > 0) {
            char sub[64];
            snprintf(sub, sizeof sub, roms.n == 1 ? "%d game" : "%d games", roms.n);
            Item *it = menu_add(m, systems[i].name, sub, "file-folder", K_FOLDER);
            it->build = build_system;
            it->arg = i;
        }
        sl_free(&roms);
    }
    if (m->n > 1) qsort(m->items, (size_t)m->n, sizeof(Item), item_cmp);
    if (!m->n) {
        char *root = expand_home(cfg.rom_root);
        char sub[600];
        snprintf(sub, sizeof sub, "Copy game files into %s/<system>", root);
        Item *it = menu_add(m, "Add games", sub, "add", K_INFO);
        it->detail = xasprintf("Put game files in folders under %s, one per system:\n"
                               "nes  snes  gb  gbc  gba  md  sms  gg  pce  psx  psp  nds  n64\n\n"
                               "Copy them on this device or over File transfer (FTP), into Games/roms.\n"
                               "They appear here the next time this list is opened.", root);
        free(root);
    }
}

/* Games > PC: one folder per game under ~/Games/pc, run with the program inside it */
static void build_pc(Menu *m)
{
    char *root = expand_home(cfg.pc_root);
    StrList dirs = { 0 };
    DIR *dp = opendir(root);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp))) {
            if (de->d_name[0] == '.') continue;
            char *p = xasprintf("%s/%s", root, de->d_name);
            struct stat st;
            if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) sl_add(&dirs, p);
            free(p);
        }
        closedir(dp);
    }
    for (int i = 0; i < dirs.n; i++) {
        char exe[1100], icon[1100], title[256];
        sncpy(title, sizeof title, basename_of(dirs.v[i]));
        for (char *p = title; *p; p++)
            if (*p == '_') *p = ' ';
        char pcid[300];
        snprintf(pcid, sizeof pcid, "pc:%s", basename_of(dirs.v[i]));
        if (appset_hidden(pcid)) continue;
        if (pc_game_find(dirs.v[i], exe, sizeof exe, icon, sizeof icon)) {
            Item *it = menu_add(m, title, basename_of(exe), "launch", K_LAUNCH);
            char *a[] = { exe, NULL };
            it->argv = argv_dup(a);
            it->icon = icon[0] ? xstrdup(icon) : NULL;
            it->flags |= IF_APP | IF_PC;
            it->data = xasprintf("pc:%s", basename_of(dirs.v[i])); /* its App Settings (controls) are kept under this */
            it->detail = xasprintf("Folder: %s\nRuns: %s", dirs.v[i], basename_of(exe));
        } else {
            Item *it = menu_add(m, title, "No program found in this folder", "launch", K_INFO);
            it->icon = icon[0] ? xstrdup(icon) : NULL;
            it->detail = xasprintf("Pixi couldn't find a program to run in\n%s\n\n"
                                   "Put the game's program for this device there (named like the folder, e.g. Doom/doom), "
                                   "or a start.sh script that starts it.", dirs.v[i]);
        }
    }
    sl_free(&dirs);
    if (m->n > 1) qsort(m->items, (size_t)m->n, sizeof(Item), item_cmp);
    if (!m->n) {
        Item *it = menu_add(m, "Add games", "Copy a folder for each game into Games/pc", "add", K_INFO);
        it->detail = xasprintf("Put each PC game in its own folder under %s, with its program inside:\n"
                               "%s/Doom/doom\n%s/Quake3/quake3\n\n"
                               "The program named like the folder runs, or a start.sh if there is one. An icon.png in the "
                               "folder is used as its icon. Copy games on this device or over File transfer (FTP).", root, root, root);
    }
    free(root);
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
    /* Console: ~/Games/roms/<system>.  PC: ~/Games/pc/<game>, each with its program inside. */
    int nsys = 0, npc = 0;
    for (int i = 0; i < nsystems; i++) {
        StrList roms = { 0 };
        system_roms(&systems[i], &roms, 1);
        if (roms.n > 0) nsys++;
        sl_free(&roms);
    }
    char *pcroot = expand_home(cfg.pc_root);
    DIR *dp = opendir(pcroot);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)))
            if (de->d_name[0] != '.') {
                char *p = xasprintf("%s/%s", pcroot, de->d_name);
                struct stat st;
                if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) npc++;
                free(p);
            }
        closedir(dp);
    }
    free(pcroot);
    char sub[64];
    if (nsys) snprintf(sub, sizeof sub, nsys == 1 ? "1 system" : "%d systems", nsys);
    else snprintf(sub, sizeof sub, "No games yet");
    Item *it = menu_add(m, "Console", sub, "category-games", K_FOLDER);
    it->build = build_consoles;
    if (npc) snprintf(sub, sizeof sub, npc == 1 ? "1 game" : "%d games", npc);
    else snprintf(sub, sizeof sub, "No games yet");
    it = menu_add(m, "PC", sub, "setting-display", K_FOLDER);
    it->build = build_pc;
}

static void build_apps_folder(Menu *m)
{
    for (int i = 0; i < apps.n; i++)
        if (apps.v[i].category == m->arg) add_app(m, &apps.v[i]);
    m->empty = xstrdup("Nothing installed here");
}

static void build_files_top(Menu *m);

static void build_apps_top(Menu *m)
{
    if (!appset_hidden("pixi:browser")) {
        Item *web_item = menu_add(m, "Web Browser", "Browse the web", "category-internet", K_ACTION);
        web_item->action = A_BROWSER;
    }
    if (!appset_hidden("pixi:terminal")) {
        Item *term_item = menu_add(m, "Terminal", "A command line", "category-terminal", K_ACTION);
        term_item->action = A_TERMINAL;
    }
    int total = 0, counts[CAT_COUNT] = { 0 };
    for (int i = 0; i < apps.n; i++)
        if (apps.v[i].category >= CAT_INTERNET && !appset_hidden(apps.v[i].id)) counts[apps.v[i].category]++, total++;
    if (total <= 8) {
        for (int i = 0; i < apps.n; i++)
            if (apps.v[i].category >= CAT_INTERNET) add_app(m, &apps.v[i]);
    } else {
        for (int c = CAT_INTERNET; c < CAT_COUNT; c++) {
            if (!counts[c]) continue;
            char sub[48];
            snprintf(sub, sizeof sub, counts[c] == 1 ? "%d app" : "%d apps", counts[c]);
            Item *it = menu_add(m, cat_names[c], sub, cat_glyphs[c], K_FOLDER);
            it->build = build_apps_folder;
            it->arg = c;
        }
    }
    m->empty = xstrdup("No apps found");
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

/* ---------- clock and date text ------------------------------------------ */

static void clock_time_text(char *out, size_t cap, const struct tm *tm)
{
    if (cfg.clock24) {
        strftime(out, cap, "%H:%M", tm);
    } else {
        strftime(out, cap, "%I:%M %p", tm);
        if (out[0] == '0') memmove(out, out + 1, strlen(out));
    }
}

static void clock_date_text(char *out, size_t cap, const struct tm *tm, int fmt)
{
    switch (fmt) {
    case 1: snprintf(out, cap, "%d/%d", tm->tm_mday, tm->tm_mon + 1); break;
    case 2: strftime(out, cap, "%Y-%m-%d", tm); break;
    case 3: { /* Sun. Jan. 25 */
        char wd[16], mo[16];
        strftime(wd, sizeof wd, "%a", tm);
        strftime(mo, sizeof mo, "%b", tm);
        snprintf(out, cap, "%s. %s. %d", wd, mo, tm->tm_mday);
        break;
    }
    default: snprintf(out, cap, "%d/%d", tm->tm_mon + 1, tm->tm_mday); break;
    }
}

/* SSH is the system's own service; the installer lets this account switch it on and off */
static int ssh_active(void)
{
    return system("systemctl is-active --quiet ssh 2>/dev/null") == 0;
}

static void sysinfo_text(char *out, size_t cap)
{
    char buf[512], v[256];
    size_t o = 0;
#define LINE(label, ...)                                                          \
    do {                                                                          \
        snprintf(buf, sizeof buf, __VA_ARGS__);                                   \
        o += (size_t)snprintf(out + o, cap - o, "%s%s: %s", o ? "\n" : "", label, buf); \
        if (o >= cap) o = cap - 1;                                                \
    } while (0)
    gethostname(v, sizeof v);
    LINE("Device name", "%s", v);
    char ip[128];
    ip_address(ip, sizeof ip);
    LINE("IP address", "%s", ip);
    os_name(v, sizeof v);
    LINE("Operating system", "%s", v);
    struct utsname u;
    if (uname(&u) == 0) LINE("Kernel", "%s %s", u.release, u.machine);
    char model[256];
    if (read_file_str("/proc/device-tree/model", model, sizeof model) > 0) LINE("Hardware", "%s", model);
    long t = read_file_long("/sys/class/thermal/thermal_zone0/temp", -1);
    if (t > 0) LINE("Temperature", "%.1f \xC2\xB0" "C", t / 1000.0);
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        double unit = si.mem_unit ? si.mem_unit : 1;
        LINE("Memory", "%.0f MB free of %.0f MB", (si.freeram + si.bufferram) * unit / 1048576.0, si.totalram * unit / 1048576.0);
    }
    struct statvfs sv;
    if (statvfs(home_dir(), &sv) == 0)
        LINE("Storage", "%.1f GB free of %.1f GB", (double)sv.f_bavail * sv.f_frsize / 1e9, (double)sv.f_blocks * sv.f_frsize / 1e9);
    if (access("/usr/sbin/sshd", X_OK) == 0) LINE("SSH", "%s", ssh_active() ? "On" : "Off");
    LINE("Pixi", "version " PIXI_VERSION);
#undef LINE
}

static void strip_ext(char *s);
static int ftp_running(void);
static int web_ui_running(void);

/* Settings > Wallpaper: the original background, then the pictures to choose from */
static void build_wallpaper_dir(Menu *m);

static void wallpaper_fill(Menu *m, const char *dir)
{
    StrList dirs = { 0 }, files = { 0 };
    media_list(dir, MEDIA_PICTURES, &dirs, &files);
    for (int i = 0; i < dirs.n; i++) {
        Item *it = menu_add(m, basename_of(dirs.v[i]), NULL, "file-folder", K_FOLDER);
        it->build = build_wallpaper_dir;
        it->data = xstrdup(dirs.v[i]);
    }
    for (int i = 0; i < files.n; i++) {
        char title[256];
        sncpy(title, sizeof title, basename_of(files.v[i]));
        strip_ext(title);
        Item *it = menu_add(m, title, !strcmp(files.v[i], cfg.wallpaper) ? "Current wallpaper" : NULL, "category-images", K_ACTION);
        it->action = A_WALL_SET;
        it->data = xstrdup(files.v[i]);
    }
    if (!m->n) m->empty = xstrdup("No pictures in this folder");
    sl_free(&dirs);
    sl_free(&files);
}

static void build_wallpaper_dir(Menu *m) { wallpaper_fill(m, m->data); }

static void build_wallpaper(Menu *m)
{
    Item *it = menu_add(m, "Original", cfg.wallpaper[0] ? "The moving background in the accent color" : "Current wallpaper", "setting-theme", K_ACTION);
    it->action = A_WALL_DEFAULT;
    MediaRoot roots[16];
    int n = media_roots(MEDIA_PICTURES, roots, 16);
    if (n == 0) {
        it = menu_add(m, "Add pictures", "~/Pictures", "add", K_INFO);
        it->detail = xstrdup("Copy pictures into ~/Pictures (or over File transfer), then choose one here.");
    } else if (n == 1) wallpaper_fill(m, roots[0].path);
    else
        for (int i = 0; i < n; i++) {
            it = menu_add(m, roots[i].label, roots[i].path, "file-folder", K_FOLDER);
            it->build = build_wallpaper_dir;
            it->data = xstrdup(roots[i].path);
        }
}

static void build_settings(Menu *m)
{
    char ssid[128] = "", sub[160];
    Item *it;
    wifi_current(ssid, sizeof ssid);
    if (!appset_hidden("pixi:files")) {
        it = menu_add(m, "Files", "Browse your files and drives", "file-folder", K_FOLDER);
        it->build = build_files_top;
    }
    it = menu_add(m, "Network", *ssid ? ssid : "Not connected", "setting-wifi", K_ACTION);
    it->action = A_NETWORK;
    {
        const char *vd = !cfg.video_out ? "None" : videoout_running() ? "USB webcam (UVC): on" : "USB webcam (UVC)";
        it = menu_add(m, "Video Out", vd, "setting-display", K_ACTION);
        it->action = A_VIDEOOUT;
    }
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
    snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", theme_def()->label, cfg.wallpaper[0] ? basename_of(cfg.wallpaper) : "Original wallpaper");
    it = menu_add(m, "Layout", sub, "setting-appearance", K_ACTION); /* the accent color and the wallpaper */
    it->action = A_ACCENT;
    {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char hm[32], d[32];
        clock_time_text(hm, sizeof hm, &tm);
        clock_date_text(d, sizeof d, &tm, cfg.date_fmt);
        snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", cfg.clock24 ? "24 Hour" : "12 Hour", d);
        it = menu_add(m, "Information Bar", sub, "setting-schedule", K_ACTION); /* the clock, date and battery up top */
        it->action = A_CLOCK;
    }
    it = menu_add(m, "Confirm Button", cfg.confirm_east ? "Right button" : "Bottom button",
                  cfg.confirm_east ? "pad-east" : "pad-south", K_ACTION);
    it->action = A_CONFIRM;
    it = menu_add(m, "Menu Sounds", cfg.sounds ? "On" : "Off", cfg.sounds ? "volume" : "volume-muted", K_ACTION);
    it->action = A_SOUNDS;
    it = menu_add(m, "Servers", ftp_running() && web_ui_running() ? "FTP and Web Server: on" : ftp_running() ? "FTP Server: on" : web_ui_running() ? "Web Server: on" : "FTP and Web Server", "setting-network", K_ACTION);
    it->action = A_SERVERS;
    it = menu_add(m, "Gamepad Tester", "Check every button and stick", "category-games", K_ACTION);
    it->action = A_GAMEPAD;
    it = menu_add(m, "App Store", "Find and install apps", "category-software", K_ACTION); /* Triangle: Refresh Apps & Games */
    it->action = A_STORE_OPEN;
    it = menu_add(m, "System Information", NULL, "setting-info", K_ACTION);
    it->action = A_SYSINFO;
    it = menu_add(m, "Update", "Update the system or Pixi", "setting-update-system", K_ACTION);
    it->action = A_UPDATES;
    it = menu_add(m, "Power", "Log out, restart, shut down", "shutdown", K_ACTION);
    it->action = A_POWER;
}

/* ---------- pictures: thumbnails and full-size textures ------------------ */

#define THUMB_DIM 160
#define THUMB_CAP 160
#define FULL_CAP 6

typedef struct {
    char *path;
    int dim, w, h, failed;
    GLuint tex;
    unsigned used;
} TexEnt;

static TexEnt thumbs[THUMB_CAP], fulls[FULL_CAP];
static unsigned tex_clock;

static TexEnt *tc_find(TexEnt *c, int cap, const char *path, int dim)
{
    for (int i = 0; i < cap; i++)
        if (c[i].path && (dim == 0 || c[i].dim == dim) && !strcmp(c[i].path, path)) {
            c[i].used = ++tex_clock;
            return &c[i];
        }
    return NULL;
}

static TexEnt *tc_slot(TexEnt *c, int cap)
{
    int pick = 0;
    for (int i = 0; i < cap; i++) {
        if (!c[i].path) {
            pick = i;
            break;
        }
        if (c[i].used < c[pick].used) pick = i;
    }
    TexEnt *e = &c[pick];
    if (e->tex) glDeleteTextures(1, &e->tex);
    free(e->path);
    memset(e, 0, sizeof *e);
    return e;
}

static void tc_store(const char *path, int dim, int w, int h, unsigned char *rgba)
{
    int is_thumb = dim == THUMB_DIM;
    TexEnt *c = is_thumb ? thumbs : fulls;
    int cap = is_thumb ? THUMB_CAP : FULL_CAP;
    TexEnt *e = tc_find(c, cap, path, dim);
    if (!e) e = tc_slot(c, cap);
    else if (e->tex) glDeleteTextures(1, &e->tex), e->tex = 0;
    free(e->path);
    e->path = xstrdup(path);
    e->dim = dim;
    e->used = ++tex_clock;
    e->w = w, e->h = h;
    e->failed = rgba == NULL;
    if (!rgba) return;
    glGenTextures(1, &e->tex);
    glBindTexture(GL_TEXTURE_2D, e->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (is_thumb) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* ---------- the wallpaper: a picture instead of the moving background --- */

static struct {
    GLuint tex;
    int w, h;           /* the screen size it was cropped for */
    int dim, pending, failed;
    char path[1024];    /* the picture the texture (or the request) is for */
} wall;

/* crops the picture to the screen's shape, darkens it a little so the menus stay readable,
 * and uploads it bottom row first (it's drawn the way the background target is laid out) */
static void wallpaper_upload(int w, int h, const unsigned char *rgba)
{
    float sa = (float)W / (float)H, ia = (float)w / (float)h;
    int cw = w, ch = h, cx = 0, cy = 0;
    if (ia > sa) cw = (int)((float)h * sa), cx = (w - cw) / 2;
    else ch = (int)((float)w / sa), cy = (h - ch) / 2;
    if (cw < 1 || ch < 1) return;
    unsigned char *out = xmalloc((size_t)cw * (size_t)ch * 4);
    for (int y = 0; y < ch; y++) {
        const unsigned char *src = rgba + ((size_t)(cy + y) * (size_t)w + (size_t)cx) * 4;
        unsigned char *dst = out + (size_t)(ch - 1 - y) * (size_t)cw * 4;
        for (int x = 0; x < cw * 4; x += 4) {
            dst[x] = (unsigned char)(src[x] * 0.82f);
            dst[x + 1] = (unsigned char)(src[x + 1] * 0.82f);
            dst[x + 2] = (unsigned char)(src[x + 2] * 0.82f);
            dst[x + 3] = 255;
        }
    }
    if (!wall.tex) glGenTextures(1, &wall.tex);
    glBindTexture(GL_TEXTURE_2D, wall.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, cw, ch, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(out);
    wall.w = W, wall.h = H;
}

/* a decoded picture: 1 if it was the wallpaper (and so was used here) */
static int wallpaper_take(const char *path, int dim, int w, int h, const unsigned char *rgba)
{
    if (!wall.pending || dim != wall.dim || strcmp(path, wall.path)) return 0;
    wall.pending = 0;
    if (strcmp(path, cfg.wallpaper)) return 1; /* changed meanwhile */
    if (!rgba) {
        wall.failed = 1;
        if (wall.tex) glDeleteTextures(1, &wall.tex), wall.tex = 0;
        return 1;
    }
    wallpaper_upload(w, h, rgba);
    return 1;
}

/* once a frame: ask for the picture when it's new, or when the screen changed size */
static void wallpaper_tick(void)
{
    if (!cfg.wallpaper[0]) {
        if (wall.tex) glDeleteTextures(1, &wall.tex), wall.tex = 0;
        wall.path[0] = 0;
        return;
    }
    int changed = strcmp(wall.path, cfg.wallpaper) != 0;
    if (changed) wall.failed = 0;
    if (wall.pending || wall.failed) return;
    if (changed || (wall.tex && (wall.w != W || wall.h != H)) || !wall.tex) {
        if (changed && wall.tex) glDeleteTextures(1, &wall.tex), wall.tex = 0;
        sncpy(wall.path, sizeof wall.path, cfg.wallpaper);
        wall.dim = (W > H ? W : H) * 2; /* enough to fill the screen after cropping, either way round */
        wall.pending = 1;
        pics_request(wall.path, wall.dim);
    }
}

static TexEnt *thumb_lookup(const char *path)
{
    TexEnt *e = tc_find(thumbs, THUMB_CAP, path, THUMB_DIM);
    return e && e->tex ? e : NULL;
}

/* Draw a picture as large as it fits in the box, keeping its shape. */
static void draw_fit(const TexEnt *e, float x, float y, float bw, float bh, float fade)
{
    if (!e || !e->tex || e->w < 1 || e->h < 1) return;
    float sc = fminf(bw / (float)e->w, bh / (float)e->h);
    float w = (float)e->w * sc, h = (float)e->h * sc;
    picture(e->tex, x + (bw - w) / 2, y + (bh - h) / 2, w, h, with_alpha(th.text, 1), fade);
}

/* ---------- the viewer ------------------------------------------------- */

static void show_message(const char *title, const char *body);

static struct {
    int open, idx, dim, slide, info;
    Menu *m;
    float t_change, slide_at, info_until;
    char prev[1024];
} viewer;

static int viewer_step(int from, int dir)
{
    Menu *m = viewer.m;
    for (int k = 1; k <= m->n; k++) {
        int i = ((from + dir * k) % m->n + m->n) % m->n;
        if (m->items[i].action == A_VIEW) return i;
    }
    return from;
}

static int viewer_count(int *pos)
{
    int n = 0, p = 0;
    for (int i = 0; i < viewer.m->n; i++)
        if (viewer.m->items[i].action == A_VIEW) {
            n++;
            if (i <= viewer.idx) p = n;
        }
    *pos = p;
    return n;
}

static void viewer_show(int idx)
{
    if (idx != viewer.idx && viewer.m->items[viewer.idx].data)
        sncpy(viewer.prev, sizeof viewer.prev, viewer.m->items[viewer.idx].data);
    viewer.idx = idx;
    viewer.t_change = now_s;
    viewer.slide_at = now_s + 5.0f;
    viewer.info_until = now_s + 2.5f;
}

static void viewer_open(Menu *m, int idx, int slide)
{
    memset(&viewer, 0, sizeof viewer);
    viewer.open = 1;
    viewer.m = m;
    viewer.idx = idx;
    int d = W > H ? W : H;
    viewer.dim = d < 640 ? 640 : d > 2048 ? 2048 : (d + 63) / 64 * 64;
    viewer.slide = slide;
    viewer.t_change = now_s;
    viewer.slide_at = now_s + 5.0f;
    viewer.info_until = now_s + 3.0f;
    snd(SND_SELECT);
}

static void viewer_action(int a)
{
    switch (a) {
    case ACT_LEFT: case ACT_UP: case ACT_PGUP:
        viewer_show(viewer_step(viewer.idx, -1));
        snd(SND_STEP);
        break;
    case ACT_RIGHT: case ACT_DOWN: case ACT_PGDN:
        viewer_show(viewer_step(viewer.idx, 1));
        snd(SND_STEP);
        break;
    case ACT_OK: case ACT_START:
        viewer.slide = !viewer.slide;
        viewer.slide_at = now_s + 5.0f;
        viewer.info_until = now_s + 2.5f;
        snd(SND_SELECT);
        break;
    case ACT_MENU:
        viewer.info = !viewer.info;
        snd(SND_SELECT);
        break;
    case ACT_BACK:
        viewer.open = 0;
        snd(SND_BACK);
        break;
    }
}

/* Keep the wanted pictures coming: the one on screen last, so it decodes first. */
static void viewer_want(void)
{
    if (!viewer.open) return;
    int nxt = viewer_step(viewer.idx, 1), prv = viewer_step(viewer.idx, -1);
    int order[3] = { prv, nxt, viewer.idx };
    for (int i = 0; i < 3; i++) {
        const char *p = viewer.m->items[order[i]].data;
        if (!p) continue;
        if (!tc_find(fulls, FULL_CAP, p, viewer.dim)) pics_request(p, viewer.dim);
        if (!tc_find(thumbs, THUMB_CAP, p, THUMB_DIM)) pics_request(p, THUMB_DIM);
    }
}

/* ---------- thumbnails for the list that's showing ---------------------- */

static void thumbs_want(Menu *m)
{
    if (!m || m->n == 0) return;
    int lo = m->sel - 7, hi = m->sel + 7;
    if (lo < 0) lo = 0;
    if (hi > m->n - 1) hi = m->n - 1;
    /* farthest first, so the nearest ends up the most urgent */
    for (int d = 7; d >= 0; d--) {
        int ids[2] = { m->sel + d, m->sel - d };
        for (int k = 0; k < (d ? 2 : 1); k++) {
            int i = ids[k];
            if (i < lo || i > hi) continue;
            Item *it = &m->items[i];
            if ((it->action != A_VIEW && it->action != A_WALL_SET) || !it->data) continue;
            if (!tc_find(thumbs, THUMB_CAP, it->data, THUMB_DIM)) pics_request(it->data, THUMB_DIM);
        }
    }
}

/* ---------- the music player -------------------------------------------- */

static struct {
    int active, paused, shuffle, repeat; /* repeat: 0 off, 1 all, 2 one */
    Mix_Music *mus;
    char **q;
    int nq, *order, pos;
    char title[256], artist[256], album[256];
    double dur;
} mp;

static struct {
    int open;
} np;

static void strip_ext(char *s)
{
    char *dot = strrchr(s, '.');
    if (dot && dot != s) *dot = 0;
}

static void music_shuffle_order(int first)
{
    for (int i = 0; i < mp.nq; i++) mp.order[i] = i;
    if (mp.shuffle) {
        for (int i = mp.nq - 1; i > 0; i--) {
            int j = rand() % (i + 1), t = mp.order[i];
            mp.order[i] = mp.order[j];
            mp.order[j] = t;
        }
        for (int i = 0; i < mp.nq; i++)
            if (mp.order[i] == first) {
                mp.order[i] = mp.order[0];
                mp.order[0] = first;
                break;
            }
        mp.pos = 0;
    } else {
        mp.pos = first;
    }
}

static void music_halt(void)
{
    if (mp.mus) {
        Mix_HaltMusic();
        Mix_FreeMusic(mp.mus);
        mp.mus = NULL;
    }
}

static int music_load_current(void)
{
    music_halt();
    const char *path = mp.q[mp.order[mp.pos]];
    mp.mus = Mix_LoadMUS(path);
    if (!mp.mus || Mix_PlayMusic(mp.mus, 1) != 0) {
        if (mp.mus) Mix_FreeMusic(mp.mus);
        mp.mus = NULL;
        return 0;
    }
    Mix_VolumeMusic(MIX_MAX_VOLUME);
    const char *t = Mix_GetMusicTitleTag(mp.mus), *ar = Mix_GetMusicArtistTag(mp.mus), *al = Mix_GetMusicAlbumTag(mp.mus);
    if (t && *t) sncpy(mp.title, sizeof mp.title, t);
    else {
        sncpy(mp.title, sizeof mp.title, basename_of(path));
        strip_ext(mp.title);
    }
    sncpy(mp.artist, sizeof mp.artist, ar ? ar : "");
    sncpy(mp.album, sizeof mp.album, al ? al : "");
    mp.dur = Mix_MusicDuration(mp.mus);
    mp.paused = 0;
    mp.active = 1;
    return 1;
}

static void music_stop(void)
{
    music_halt();
    for (int i = 0; i < mp.nq; i++) free(mp.q[i]);
    free(mp.q);
    free(mp.order);
    mp.q = NULL, mp.order = NULL, mp.nq = 0, mp.pos = 0;
    mp.active = mp.paused = 0;
    np.open = 0;
}

/* Move to another track; keeps trying if a file won't play. Returns 0 at the end of the queue. */
static int music_go(int dir, int automatic)
{
    if (mp.nq == 0) return 0;
    if (automatic && mp.repeat == 2) return music_load_current();
    for (int tries = 0; tries < mp.nq; tries++) {
        mp.pos += dir;
        if (mp.pos >= mp.nq || mp.pos < 0) {
            if (mp.repeat == 0 && automatic) {
                music_halt();
                mp.active = 0;
                mp.paused = 0;
                return 0;
            }
            mp.pos = dir > 0 ? 0 : mp.nq - 1;
        }
        if (music_load_current()) return 1;
    }
    music_stop();
    return 0;
}

static void music_start(const char *const *paths, int n, int start, int shuffle)
{
    if (!audio_ok) {
        show_message("No sound", "Pixi couldn't open an audio device, so music can't play.");
        snd(SND_ERROR);
        return;
    }
    music_stop();
    mp.q = xcalloc((size_t)n, sizeof(char *));
    mp.order = xcalloc((size_t)n, sizeof(int));
    for (int i = 0; i < n; i++) mp.q[i] = xstrdup(paths[i]);
    mp.nq = n;
    mp.shuffle = shuffle;
    music_shuffle_order(start);
    int tries = 0;
    while (!music_load_current()) {
        if (++tries >= n) {
            music_stop();
            show_message("Couldn't play", "None of these files could be played.");
            snd(SND_ERROR);
            return;
        }
        mp.pos = (mp.pos + 1) % mp.nq;
    }
    np.open = 1;
}

static void music_toggle(void)
{
    if (!mp.active || !mp.mus) return;
    if (mp.paused) Mix_ResumeMusic();
    else Mix_PauseMusic();
    mp.paused = !mp.paused;
}

static void music_seek(double delta)
{
    if (!mp.active || !mp.mus) return;
    double p = Mix_GetMusicPosition(mp.mus);
    if (p < 0) return;
    p += delta;
    if (p < 0) p = 0;
    if (mp.dur > 0 && p > mp.dur - 1) p = mp.dur - 1;
    Mix_SetMusicPosition(p);
}

static void music_set_shuffle(int on)
{
    if (mp.nq == 0) return;
    int cur = mp.order[mp.pos];
    mp.shuffle = on;
    music_shuffle_order(cur);
}

static void music_tick(void)
{
    if (mp.active && !mp.paused && mp.mus && !Mix_PlayingMusic()) music_go(1, 1);
    if (!mp.active) np.open = np.open && mp.nq > 0;
}

static void np_action(int a)
{
    switch (a) {
    case ACT_OK: case ACT_START:
        if (mp.active) music_toggle();
        else if (mp.nq) {
            mp.pos = 0;
            music_go(0, 0);
        }
        snd(SND_SELECT);
        break;
    case ACT_LEFT:
        if (mp.active && mp.mus && Mix_GetMusicPosition(mp.mus) > 3.0) music_seek(-1e9);
        else music_go(-1, 0);
        snd(SND_STEP);
        break;
    case ACT_RIGHT:
        music_go(1, 0);
        snd(SND_STEP);
        break;
    case ACT_UP:
        mp.repeat = (mp.repeat + 1) % 3;
        snd(SND_SELECT);
        break;
    case ACT_DOWN:
        music_set_shuffle(!mp.shuffle);
        snd(SND_SELECT);
        break;
    case ACT_PGUP: music_seek(-10); break;
    case ACT_PGDN: music_seek(10); break;
    case ACT_MENU:
        music_stop();
        snd(SND_BACK);
        break;
    case ACT_BACK:
        np.open = 0;
        snd(SND_BACK);
        break;
    }
}

/* ---------- the file browser --------------------------------------------- */

typedef struct {
    char *path;
    off_t size;
    time_t mtime;
    int dir;
} FileEnt;

static int fileent_cmp(const void *a, const void *b)
{
    const FileEnt *x = a, *y = b;
    if (x->dir != y->dir) return y->dir - x->dir;
    return natural_cmp(basename_of(x->path), basename_of(y->path));
}

static void human_size(char *out, size_t cap, double b)
{
    if (b < 1024) snprintf(out, cap, "%.0f B", b);
    else if (b < 1048576) snprintf(out, cap, "%.0f KB", b / 1024);
    else if (b < 1073741824.0) snprintf(out, cap, "%.1f MB", b / 1048576);
    else snprintf(out, cap, "%.2f GB", b / 1073741824.0);
}

static void build_files_dir(Menu *m);

static void build_files_dir(Menu *m)
{
    DIR *dp = opendir(m->data);
    if (!dp) {
        m->empty = xasprintf("Can't open this folder: %s", strerror(errno));
        return;
    }
    FileEnt *list = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dp)) && n < 5000) {
        if (de->d_name[0] == '.') continue;
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", m->data, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            list = xrealloc(list, sizeof *list * (size_t)cap);
        }
        list[n].path = xstrdup(path);
        list[n].size = st.st_size;
        list[n].mtime = st.st_mtime;
        list[n].dir = S_ISDIR(st.st_mode);
        n++;
    }
    closedir(dp);
    if (n > 1) qsort(list, (size_t)n, sizeof *list, fileent_cmp);
    for (int i = 0; i < n; i++) {
        const char *name = basename_of(list[i].path);
        char sub[96], sz[32], when[32];
        struct tm tm;
        localtime_r(&list[i].mtime, &tm);
        strftime(when, sizeof when, "%Y-%m-%d", &tm);
        Item *it;
        if (list[i].dir) {
            it = menu_add(m, name, when, "file-folder", K_FOLDER);
            it->build = build_files_dir;
            it->data = xstrdup(list[i].path);
        } else {
            human_size(sz, sizeof sz, (double)list[i].size);
            snprintf(sub, sizeof sub, "%s \xC2\xB7 %s", sz, when);
            int kind = media_is(MEDIA_PICTURES, name) ? 0 : media_is(MEDIA_MUSIC, name) ? 1 : media_is(MEDIA_VIDEOS, name) ? 2 : -1;
            static const char *g[3] = { "category-images", "category-music", "category-video" };
            static const int a[3] = { A_VIEW, A_PLAY, A_PLAYVIDEO };
            it = menu_add(m, name, sub, kind >= 0 ? g[kind] : "file-page", kind >= 0 ? K_ACTION : K_INFO);
            if (kind >= 0) it->action = a[kind];
            it->data = xstrdup(list[i].path);
            it->detail = xasprintf("%s\n%s\nModified %s", list[i].path, sz, when);
        }
        it->flags |= IF_FILE;
        free(list[i].path);
    }
    free(list);
    m->empty = xstrdup("This folder is empty");
}

static void build_files_top(Menu *m)
{
    Item *it = menu_add(m, "Home", home_dir(), "file-folder", K_FOLDER);
    it->build = build_files_dir;
    it->data = xstrdup(home_dir());
    MediaRoot drives[16];
    int n = mount_points(drives, 16);
    for (int i = 0; i < n; i++) {
        it = menu_add(m, drives[i].label, drives[i].path, "file-drive", K_FOLDER);
        it->build = build_files_dir;
        it->data = xstrdup(drives[i].path);
    }
    it = menu_add(m, "This device", "/", "file-drive", K_FOLDER);
    it->build = build_files_dir;
    it->data = xstrdup("/");
}

/* ---------- media menus --------------------------------------------------- */

static void build_media_dir(Menu *m);

static void media_fill(Menu *m, int kind, const char *dir)
{
    StrList dirs = { 0 }, files = { 0 };
    media_list(dir, kind, &dirs, &files);
    if (kind == MEDIA_PICTURES && files.n > 1) {
        Item *it = menu_add(m, "Slideshow", "Show every picture here in turn", "media-play", K_ACTION);
        it->action = A_SLIDESHOW;
    }
    if (kind == MEDIA_MUSIC && files.n > 0) {
        Item *it = menu_add(m, "Play all", NULL, "media-play", K_ACTION);
        it->action = A_PLAYALL;
        if (files.n > 1) {
            it = menu_add(m, "Shuffle all", NULL, "media-next", K_ACTION);
            it->action = A_SHUFFLEALL;
        }
    }
    for (int i = 0; i < dirs.n; i++) {
        Item *it = menu_add(m, basename_of(dirs.v[i]), NULL, "file-folder", K_FOLDER);
        it->build = build_media_dir;
        it->data = xstrdup(dirs.v[i]);
        it->arg = kind;
    }
    static const char *file_glyph[3] = { "category-images", "category-music", "category-video" };
    static const int file_action[3] = { A_VIEW, A_PLAY, A_PLAYVIDEO };
    for (int i = 0; i < files.n; i++) {
        char title[256], sub[16] = "";
        sncpy(title, sizeof title, basename_of(files.v[i]));
        const char *dot = strrchr(title, '.');
        if (dot) {
            for (int k = 0; dot[1 + k] && k < 6; k++) sub[k] = (char)toupper((unsigned char)dot[1 + k]);
            strip_ext(title);
        }
        Item *it = menu_add(m, title, sub, file_glyph[kind], K_ACTION);
        it->action = file_action[kind];
        it->data = xstrdup(files.v[i]);
        it->detail = xstrdup(files.v[i]);
        it->flags |= IF_MEDIA;
    }
    static const char *empty[3] = { "No pictures in this folder", "No music in this folder", "No videos in this folder" };
    m->empty = xstrdup(empty[kind]);
    sl_free(&dirs);
    sl_free(&files);
}

static void build_media_dir(Menu *m) { media_fill(m, m->arg, m->data); }

static void build_media_top(Menu *m)
{
    int kind = m->cat - CAT_PICTURES;
    if (kind == MEDIA_MUSIC && mp.nq > 0) {
        char sub[300];
        snprintf(sub, sizeof sub, "%s%s", mp.paused ? "Paused \xC2\xB7 " : "", mp.title);
        Item *it = menu_add(m, "Now playing", sub, mp.paused ? "media-pause" : "media-play", K_ACTION);
        it->action = A_NOWPLAYING;
    }
    MediaRoot roots[16];
    int n = media_roots(kind, roots, 16);
    static const char *what[3] = { "pictures", "music", "videos" };
    static const char *home[3] = { "~/Pictures", "~/Music", "~/Videos" };
    if (n == 0) {
        char t[64];
        snprintf(t, sizeof t, "Add %s", what[kind]);
        Item *it = menu_add(m, t, home[kind], "add", K_INFO);
        it->detail = xasprintf("Copy %s into %s, or plug in a USB drive or card with a folder of that name.\n\n"
                               "Extra folders can be listed in pixi.conf under [media].", what[kind], home[kind]);
        return;
    }
    if (n == 1) {
        media_fill(m, kind, roots[0].path);
        return;
    }
    for (int i = 0; i < n; i++) {
        int removable = !strncmp(roots[i].path, "/media/", 7) || !strncmp(roots[i].path, "/run/media/", 11) || !strncmp(roots[i].path, "/mnt/", 5);
        Item *it = menu_add(m, roots[i].label, roots[i].path, removable ? "file-drive" : "file-folder", K_FOLDER);
        it->build = build_media_dir;
        it->data = xstrdup(roots[i].path);
        it->arg = kind;
    }
    m->empty = xstrdup("Nothing here yet");
}

/* Called every frame: pictures that finished decoding, the slideshow, the end of a song. */
static void media_tick(int visible)
{
    music_tick();
    if (!visible) return;
    int budget = 6;
    char path[1100];
    int dim, w, h;
    unsigned char *rgba;
    while (budget-- > 0 && pics_take(path, sizeof path, &dim, &w, &h, &rgba)) {
        if (!wallpaper_take(path, dim, w, h, rgba)) tc_store(path, dim, w, h, rgba);
        free(rgba);
    }
    if (viewer.open) {
        if (viewer.slide && now_s >= viewer.slide_at) viewer_show(viewer_step(viewer.idx, 1));
        viewer_want();
    }
}

/* ---------- top-level columns ------------------------------------------- */

static Menu cats[CAT_COUNT];
static int vis[CAT_COUNT], nvis; /* visible category indices, left to right */
static int cur;                  /* index into vis */

static void cat_build(Menu *m)
{
    if (m->cat == CAT_SETTINGS) build_settings(m);
    else if (m->cat == CAT_APPS) build_apps_top(m);
    else if (m->cat >= CAT_PICTURES && m->cat <= CAT_VIDEOS) build_media_top(m);
    else build_games(m);
}

static void categories_rebuild(void)
{
    int keep_cat = nvis ? vis[cur] : cfg.category;
    apps_scan();
    nvis = 0;
    for (int c = 0; c < CAT_COLUMNS; c++) {
        Menu *m = &cats[c];
        if (!m->title) {
            m->title = xstrdup(cat_names[c]);
            m->cat = c;
            m->build = cat_build;
        }
        menu_rebuild(m);
        vis[nvis++] = c;
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

/* after something was hidden or shown: every column of the home screen, and the folders open in it, are built again */
static void homescreen_refresh(void)
{
    for (int i = 0; i < nvis; i++) menu_rebuild(&cats[vis[i]]);
    for (int d = 1; d <= depth; d++)
        if (stack[d]) menu_rebuild(stack[d]);
}

static int osk_start_pressed;
enum { D_NONE, D_MESSAGE, D_CONFIRM, D_SLIDER, D_OSK, D_BUSY, D_UPDATE, D_CHECK };
/* 1 while the shell shows only a keyboard, over an app that stays visible behind it */
static int scene_transparent;

static struct {
    int kind;
    char title[256], text[1024], ctx[256];
    int choice, action, value, min, max, step;
    char buf[640];
    int r, c, layer, shift, autocap, mask;
    Uint32 shift_ms;
    float anim;
} dlg;

/* The system update, running in the background while a dialog shows how it's going. */
static struct {
    int active, done, ok, exited, code, count, eof, kind;
    pid_t pid;
    int fd;
    char buf[2048];
    size_t blen;
    char lines[6][200];
    int nlines;
    float pct, started;
    char phase[160], detail[160], fail[200], result[400], name[96];
} upd;

/* A question from a program that would have used a yad window (Pi-Apps' installers do): shown in Pixi's own dialog,
 * since a window of its own can't be reached with the gamepad. See yad_client and yad_open. */
#define YD_MAX 24
static struct {
    int fd, n, sel, list, radio, first;
    char text[640];
    struct { char label[110]; int check, button, code; } it[YD_MAX];
} yd = { -1, 0, 0, 0, 0, 0, "", { { "", 0, 0, 0 } } };

/* The guide overlay. */
enum { G_TILE, G_SLIDER, G_BRIGHT, G_MEDIA, G_ENTRY, G_POWER, G_SWITCH };
enum { GE_RESUME, GE_CLOSE, GE_SWITCH, GE_HOME, GE_START };
typedef struct {
    int what;
    uint32_t id;
    char label[300];
} GuideEntry;
typedef struct {
    uint32_t id; /* 0 is the home screen */
    char label[300];
} GuideCard;
static struct {
    int open, row, tile, entry, power_open, power_sel;
    float anim;
    GuideEntry entries[40];
    int nentries;
    int volume, brightness, mbtn;
    int wifi_on, bt_on; /* 1 on, 0 off, -1 not available */
    GuideCard cards[40];
    int ncards, card;
    float cpos;
    int previews_sent;
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
static void draw_volhud(void);
static int store_is_open(void);
static int store_is_running(void);
static void store_set_open(int open);
static void store_quit(void);
static void handle_action(int a);
static int pm_active_fwd(void);
static int pm_r_down; /* R is held, in Keyboard + Mouse mode */
static int tri_with_r; /* Triangle went down while R was held: the keyboard combo */
static int side_is_open(void);
static void draw_side(void);
static void side_action(int a);
static void side_open_accent(void);
static void side_open_files(Menu *m, Item *it);
static void side_open_power(void);
static void side_open_choice(int setting);
static void side_open_wifi(Item *it);
static void side_open_item(Item *it);
static void side_open_updates(void);
static void net_changed(int connected);
static void net_connect_saved(const char *name, int active);
static void net_show_info(const char *name);
static void net_open(void);
static int net_is_open(void);
static void draw_net(void);
static void net_action(int a);
static void side_open_servers(void);
static int ssh_set(int on);
static void sysinfo_refresh(void);
static void wallpaper_set(const char *path);
static void do_action(Item *it);
static int ftp_ui_is_open(void);
static int aps_is_open(void);
static int alist_is_open(void);
static void alist_open(void);
static void alist_action(int a);
static void draw_alist(void);
static void aps_open(const char *id, const char *name);
static void aps_action(int a);
static void draw_appsettings(void);
static void draw_ftp(void);
static int browser_is_open(void);
static void draw_browser(void);
static void browser_action(int a);
static void browser_tick(void);
static void browser_open(void);
static void browser_quit(void);
static int video_is_open(void);
static int sysinfo_is_open(void);
static int gptest_is_open(void);
static void gptest_open(void);
static void draw_gptest(void);
static void gptest_action(int a);
static void gptest_tick(void);
static void draw_sysinfo(void);
static void sysinfo_open(void);
static void draw_video(void);
static void video_action(int a);
static void video_tick(void);
static void video_frame(void);
static void video_close(void);

/* The apps Pixi draws itself show up in the Guide beside the windows of other programs. */
#define APP_BROWSER 0xFFFF0001u
#define APP_VIDEO 0xFFFF0002u
#define APP_MUSIC 0xFFFF0003u
#define APP_STORE 0xFFFF0004u
#define APP_TERMINAL 0xFFFF0005u
#define APP_FTP 0xFFFF0006u
#define APP_WEB 0xFFFF0007u
static int app_is_ours(uint32_t id);
static int app_running(uint32_t id);
static uint32_t app_front(void);
static void app_hide_all(void);
static void app_switch(uint32_t id);
static void app_close(uint32_t id);
static const char *app_label(uint32_t id);
static void app_draw_card(uint32_t id, float x, float y, float w, float h, float fade);
static void browser_address_done(const char *text);
static void browser_field_done(const char *text, int enter);
static void ftp_ui_action(int a);
static void ftp_ui_open(void);
static int web_ui_is_open(void);
static int web_ui_running(void);
static void web_ui_open(void);
static void web_ui_stop(void);
static void web_ui_action(int a);
static void web_edit_config(void);
static void web_reload_now(void);
static int ed_is_open(void);
static void draw_editor(void);
static void ed_action(int a);
static void ed_repeat_key(int a);
static void ed_close(void);
static void ed_abandon(void);
static void web_reset_now(void);
static void web_tick(void);
static void draw_web(void);
static void file_delete_now(const char *path);
static void file_rename_done(const char *old, const char *name);
static void file_mkdir_done(const char *dir, const char *name);
static void store_open(void);
static void store_remove_now(const char *pkg);
static void piapps_install_start(void);
static void piapps_job(int install, const char *name);
static void store_search_done(const char *q);
static void draw_store(void);
static void term_draw_card(float x, float y, float w, float h, float fade);
static int term_is_open(void);
static int term_is_running(void);
static void term_set_open(int open);
static void term_quit(void);
static void term_open(char **argv, const char *name);
static void term_action(int a);
static void term_tick(void);
static void term_repeat(Uint32 now);
static int term_child_hook(pid_t pid, int status);
static int term_sdl_key(const SDL_KeyboardEvent *k);
static void term_sdl_text(const char *s);
static void draw_terminal(void);
static void term_script_text(const char *s);

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
/* Drawing the home screen                                                 */
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
    if ((it->action == A_VIEW || it->action == A_WALL_SET) && it->data) {
        TexEnt *t = thumb_lookup(it->data);
        if (t) {
            float s = size * 0.98f;
            draw_fit(t, cx - s / 2, cy - s / 2, s, s, alpha);
            return;
        }
    }
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
        float tsize = lerp(20, 24, focus) * S;
        int bold = distance < 0.5f;
        if (show_sub) {
            text(it->title, text_x, y - tsize * 0.92f, tsize, bold, th.text, a, AL_LEFT, maxw);
            text(it->subtitle, text_x, y + 4 * S, 16 * S, 0, th.text_soft, a, AL_LEFT, maxw);
        } else {
            text(it->title, text_x, y - tsize * 0.5f, tsize, bold, th.text, a, AL_LEFT, maxw);
        }
        if (it->kind == K_FOLDER && distance < 0.5f) {
            float gx = (float)W - CORNER_INSET * S * 0.6f;
            glyph("arrow-right", gx, y, 24 * S, th.text_soft, a * 0.8f);
        }
    }
}

static void draw_corner(void)
{
    char buf[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[32], dt[32];
    clock_time_text(hm, sizeof hm, &tm);
    clock_date_text(dt, sizeof dt, &tm, cfg.date_fmt);
    snprintf(buf, sizeof buf, "%s %s", dt, hm);
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
        if (cfg.battery_pct) {
            float pw = text(pct, x, y, size, 0, th.text, 0.85f, AL_RIGHT, 0);
            x -= pw + 4 * S;
        }
        glyph(b, x - mark / 2, my, mark * 1.25f, th.text, 0.85f);
        x -= mark * 1.25f + 12 * S;
    }
    if (status.wifi >= 0) {
        const char *g = status.wifi >= 3 ? "setting-wifi" : status.wifi == 2 ? "signal-strong" : status.wifi == 1 ? "signal-fair" : "signal-weak";
        glyph(g, x - mark / 2, my, mark, th.text, 0.85f);
    }
}

/* The top of a full-screen page like File Transfer: a mark and title on the left, the status on the right. */
static void draw_page_header(const char *glyph_name, const char *title)
{
    glyph(glyph_name, 54 * S, 40 * S, 34 * S, th.text, 1);
    text(title, 90 * S, 27 * S, 30 * S, 1, th.text, 1, AL_LEFT, (int)(W * 0.55f));
    pane(0, 74 * S, (float)W, 1.5f * S, 0, CIRCLE, with_alpha(th.text, 0.35f), 0, 0, 0, 1, 0);
    draw_corner();
}

typedef struct {
    const char *glyph_name, *label;
} Hint;

static void draw_hints_at(const Hint *h, int n, int left)
{
    float y = (float)H - HINT_MARGIN * S - HINT_GLYPH * S * 0.5f, g = HINT_GLYPH * S, sh = 1.6f * S;
    const Col dark = { 0, 0, 0, 1 };
    if (left) { /* from the left edge, first hint first */
        float x = HINT_MARGIN * S;
        for (int i = 0; i < n; i++) {
            glyph(h[i].glyph_name, x + g / 2 + sh, y + sh, g, dark, 0.55f);
            glyph(h[i].glyph_name, x + g / 2, y, g, th.text, 0.95f);
            x += g + HINT_GAP * S;
            text(h[i].label, x + sh, y - HINT_LABEL * S * 0.55f + sh, HINT_LABEL * S, 0, dark, 0.7f, AL_LEFT, 0);
            float w = text(h[i].label, x, y - HINT_LABEL * S * 0.55f, HINT_LABEL * S, 0, th.text, 0.95f, AL_LEFT, 0);
            x += w + HINT_PADDING * S * 2;
        }
        return;
    }
    float x = (float)W - HINT_MARGIN * S;
    for (int i = n - 1; i >= 0; i--) {
        glyph(h[i].glyph_name, x - g / 2 + sh, y + sh, g, dark, 0.55f);
        glyph(h[i].glyph_name, x - g / 2, y, g, th.text, 0.95f);
        x -= g + HINT_GAP * S;
        text(h[i].label, x + sh, y - HINT_LABEL * S * 0.55f + sh, HINT_LABEL * S, 0, dark, 0.7f, AL_RIGHT, 0);
        float w = text(h[i].label, x, y - HINT_LABEL * S * 0.55f, HINT_LABEL * S, 0, th.text, 0.95f, AL_RIGHT, 0);
        x -= w + HINT_PADDING * S * 2;
    }
}

static void draw_hints(const Hint *h, int n) { draw_hints_at(h, n, 0); }

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
/* Pictures and music on screen                                             */
/* ======================================================================== */

static void fmt_time(char *out, size_t cap, double s)
{
    if (s < 0) s = 0;
    int t = (int)s;
    if (t >= 3600) snprintf(out, cap, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(out, cap, "%d:%02d", t / 60, t % 60);
}

static void draw_viewer(void)
{
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.94f }, 0, 0, 0, 1, 0);
    Item *it = &viewer.m->items[viewer.idx];
    float fade = clampf((now_s - viewer.t_change) / 0.35f, 0, 1);
    float bx = 0, by = 0, bw = (float)W, bh = (float)H;
    if (fade < 1 && viewer.prev[0]) {
        TexEnt *p = tc_find(fulls, FULL_CAP, viewer.prev, viewer.dim);
        if (!p || !p->tex) p = thumb_lookup(viewer.prev);
        draw_fit(p, bx, by, bw, bh, 1);
    }
    TexEnt *cur = tc_find(fulls, FULL_CAP, it->data, viewer.dim);
    if (!cur || !cur->tex) cur = thumb_lookup(it->data);
    int failed = cur && cur->failed;
    TexEnt *f = tc_find(fulls, FULL_CAP, it->data, viewer.dim);
    if (f && f->failed) failed = 1;
    if (cur && cur->tex) draw_fit(cur, bx, by, bw, bh, fade);
    if (!(cur && cur->tex)) {
        text(failed ? "This picture can't be shown" : "Loading\xE2\x80\xA6", W / 2.0f, H / 2.0f - 12 * S, 24 * S, 0, th.text_soft, 1,
             AL_CENTER, 0);
    } else if (f && f->failed) {
        text("Couldn't load the full picture", W / 2.0f, H * 0.12f, 20 * S, 0, th.text_soft, 1, AL_CENTER, 0);
    }
    if (viewer.info || now_s < viewer.info_until) {
        float a = viewer.info ? 1.0f : clampf((viewer.info_until - now_s) / 0.5f, 0, 1);
        float ph = 74 * S;
        pane(0, H - ph, (float)W, ph, 0, CIRCLE, (Col){ 0, 0, 0, 0.55f }, 0, 0, 0, a, 0);
        char name[256], cnt[48];
        sncpy(name, sizeof name, basename_of(it->data));
        int pos;
        int n = viewer_count(&pos);
        snprintf(cnt, sizeof cnt, "%d / %d%s", pos, n, viewer.slide ? "  \xC2\xB7  Slideshow" : "");
        text(name, 28 * S, H - ph + 12 * S, 24 * S, 1, th.text, a, AL_LEFT, (int)(W * 0.62f));
        text(cnt, 28 * S, H - ph + 42 * S, 18 * S, 0, th.text_soft, a, AL_LEFT, 0);
    }
    Hint h[3] = { { pad_ok(), viewer.slide ? "Pause slideshow" : "Slideshow" }, { "pad-north", "Info" }, { pad_back(), "Back" } };
    if (viewer.info || now_s < viewer.info_until) draw_hints(h, 3);
}

static void draw_nowplaying(void)
{
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.5f }, 0, 0, 0, 1, 0);
    float pulse = 0.5f + 0.5f * sinf(now_s * 2 * (float)M_PI / PULSE_PERIOD);
    float cx = W * 0.22f, cy = H * 0.42f, disc = fminf(W * 0.3f, H * 0.48f);
    if (mp.active && !mp.paused) glow(cx, cy, disc * (1.7f + 0.15f * pulse), with_alpha(th.accent, 0.35f + 0.2f * pulse), 1);
    pane(cx - disc / 2, cy - disc / 2, disc, disc, disc * 0.22f, SQUIRCLE, with_alpha(th.accent, 0.14f), DEPTH_PANEL * S, FROST_PANEL,
         GLOSS_FULL, 1, 0);
    glyph("category-music", cx, cy, disc * 0.62f, th.text, 1);

    float x = W * 0.42f, maxw = (int)(W * 0.52f);
    float y = H * 0.20f;
    if (!mp.active) {
        text("Stopped", x, y, 34 * S, 1, th.text, 1, AL_LEFT, (int)maxw);
        text("Press the confirm button to play again", x, y + 48 * S, 20 * S, 0, th.text_soft, 1, AL_LEFT, (int)maxw);
    } else {
        text(mp.title, x, y, 34 * S, 1, th.text, 1, AL_LEFT, (int)maxw);
        if (mp.artist[0]) text(mp.artist, x, y + 48 * S, 24 * S, 0, th.text, 0.9f, AL_LEFT, (int)maxw);
        if (mp.album[0]) text(mp.album, x, y + 82 * S, 20 * S, 0, th.text_soft, 1, AL_LEFT, (int)maxw);
    }
    double pos = mp.active && mp.mus ? Mix_GetMusicPosition(mp.mus) : 0;
    if (pos < 0) pos = 0;
    float by = H * 0.55f, bw = maxw;
    pane(x, by, bw, 8 * S, 4 * S, CIRCLE, with_alpha(th.glass_raised, 0.2f), 0, 0, 0, 1, 0);
    if (mp.dur > 0) {
        float fr = clampf((float)(pos / mp.dur), 0, 1);
        pane(x, by, fmaxf(bw * fr, 8 * S), 8 * S, 4 * S, CIRCLE, th.accent_soft, 0, 0, 0, 1, 0);
        pane(x + bw * fr - 8 * S, by - 4 * S, 16 * S, 16 * S, 8 * S, CIRCLE, with_alpha(th.text, 0.95f), 0, 0, 0, 1, 0);
    }
    char a[32], b[32];
    fmt_time(a, sizeof a, pos);
    if (mp.dur > 0) fmt_time(b, sizeof b, mp.dur);
    else sncpy(b, sizeof b, "--:--");
    text(a, x, by + 18 * S, 18 * S, 0, th.text_soft, 1, AL_LEFT, 0);
    text(b, x + bw, by + 18 * S, 18 * S, 0, th.text_soft, 1, AL_RIGHT, 0);

    float ry = H * 0.72f, gap = 92 * S, mx = x + bw / 2;
    glyph("media-previous", mx - gap, ry, 46 * S, th.text, 0.9f);
    float bsz = 76 * S;
    pane(mx - bsz / 2, ry - bsz / 2, bsz, bsz, bsz / 2, CIRCLE, with_alpha(th.accent, 0.35f), DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_FULL,
         1, 0);
    glyph(mp.active && !mp.paused ? "media-pause" : "media-play", mx, ry, 52 * S, th.text, 1);
    glyph("media-next", mx + gap, ry, 46 * S, th.text, 0.9f);

    char st[160], pos_txt[48] = "";
    if (mp.nq > 0) snprintf(pos_txt, sizeof pos_txt, "Track %d of %d", mp.pos + 1, mp.nq);
    static const char *rep[3] = { "Repeat off", "Repeat all", "Repeat one" };
    snprintf(st, sizeof st, "%s  \xC2\xB7  Shuffle %s  \xC2\xB7  %s", pos_txt, mp.shuffle ? "on" : "off", rep[mp.repeat]);
    text(st, x + bw / 2, H * 0.82f, 18 * S, 0, th.text_soft, 1, AL_CENTER, (int)bw);
    text("Left/Right: previous or next   Up: repeat   Down: shuffle   L/R: skip 10 s", W / 2.0f, H - 70 * S - 18 * S, 16 * S, 0,
         th.text_soft, 0.85f, AL_CENTER, (int)(W * 0.92f));
    Hint h[3] = { { pad_ok(), mp.paused || !mp.active ? "Play" : "Pause" }, { "pad-north", "Stop" }, { pad_back(), "Back" } };
    draw_hints(h, 3);
}

/* a small "now playing" pill on the home screen */
static void draw_music_pill(float alpha)
{
    if (!mp.active || np.open) return;
    float h = 40 * S, x = CORNER_INSET * S * 0.6f, y = H - 22 * S - h - 8 * S;
    float tw = (float)text_width(mp.title, 20 * S, 0);
    float maxtw = W * 0.34f;
    if (tw > maxtw) tw = maxtw;
    float w = 56 * S + tw + 18 * S;
    pane(x, y, w, h, h / 2, SQUIRCLE, with_alpha(th.glass, 0.5f), DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_QUIET, alpha, 0);
    glyph(mp.paused ? "media-pause" : "media-play", x + 26 * S, y + h / 2, 24 * S, th.text, alpha);
    text(mp.title, x + 46 * S, y + h / 2 - 11 * S, 20 * S, 0, th.text, alpha, AL_LEFT, (int)maxtw);
}

/* ======================================================================== */
/* Dialogs                                                                  */
/* ======================================================================== */

/* ---- The PC keyboard ------------------------------------------------------------------------------
 * One layout for both on-screen keyboards, the text-box one and the terminal's: a 75% block (with
 * Backspace at the end of the top row and one Page Up / Page Down key by Enter), and on the right a
 * 4 x 4 block with Fn, Delete, ` and Esc over the number row, which is laid out like a number pad
 * (7 8 9 -, 4 5 6 =, 1 2 3 0).
 * The triggers are modifiers while a keyboard is up: L is Shift while held (tap it twice for Caps
 * Lock), R is Ctrl while held (tap it twice for Alt on the next key), and with both held the number
 * block is F1 to F12, in reading order: F1-F4 on the 7 8 9 - row, F5-F8 on 4 5 6 =, F9-F12 on 1 2 3 0. */

enum {
    KY_CHAR, KY_TAB, KY_CAPS, KY_SHIFT, KY_ENTER, KY_CTRL, KY_SUPER, KY_ALT, KY_SPACE, KY_FN,
    KY_UP, KY_DOWN, KY_LEFT, KY_RIGHT, KY_PGUP, KY_PGDN, KY_HOME, KY_BKSP, KY_DEL, KY_ESC
};

typedef struct {
    const char *base, *shifted; /* what it types, without and with Shift (letters: shifted is NULL) */
    int type, fkey;             /* fkey: the F key it becomes with L + R (or Fn) */
    float w, x;                 /* width and position, in key widths */
    int pad_block;              /* first key of the right-hand block: a gap goes before it */
} PKey;

#define PK_ROWS 4
#define PK_MAXC 20
#define PK_UNITS 20.5f /* 16 for the main block, half a key of gap, 4 for the block on the right */

#define K(b, sh, t, f, w) { b, sh, t, f, w, 0, 0 }
#define P(b, sh, t, f, w) { b, sh, t, f, w, 0, 1 } /* first key of the block on the right */
static const PKey pk_rows[PK_ROWS][PK_MAXC] = {
    { K("Tab", 0, KY_TAB, 0, 1.5f),
      K("q", 0, KY_CHAR, 0, 1), K("w", 0, KY_CHAR, 0, 1), K("e", 0, KY_CHAR, 0, 1), K("r", 0, KY_CHAR, 0, 1),
      K("t", 0, KY_CHAR, 0, 1), K("y", 0, KY_CHAR, 0, 1), K("u", 0, KY_CHAR, 0, 1), K("i", 0, KY_CHAR, 0, 1),
      K("o", 0, KY_CHAR, 0, 1), K("p", 0, KY_CHAR, 0, 1), K("[", "{", KY_CHAR, 0, 1), K("]", "}", KY_CHAR, 0, 1),
      K("\\", "|", KY_CHAR, 0, 1.5f), K("", 0, KY_BKSP, 0, 1),
      P("Fn", 0, KY_FN, 0, 1), K("Delete", "Insert", KY_DEL, 0, 1), K("`", "~", KY_CHAR, 0, 1), K("Esc", 0, KY_ESC, 0, 1) },
    { K("Caps Lock", 0, KY_CAPS, 0, 1.75f),
      K("a", 0, KY_CHAR, 0, 1), K("s", 0, KY_CHAR, 0, 1), K("d", 0, KY_CHAR, 0, 1), K("f", 0, KY_CHAR, 0, 1),
      K("g", 0, KY_CHAR, 0, 1), K("h", 0, KY_CHAR, 0, 1), K("j", 0, KY_CHAR, 0, 1), K("k", 0, KY_CHAR, 0, 1),
      K("l", 0, KY_CHAR, 0, 1), K(";", ":", KY_CHAR, 0, 1), K("'", "\"", KY_CHAR, 0, 1),
      K("Enter", 0, KY_ENTER, 0, 2.25f), K("Pg", "Pg", KY_PGUP, 0, 1), /* Page Up; with Shift, Page Down */
      P("7", "&", KY_CHAR, 1, 1), K("8", "*", KY_CHAR, 2, 1), K("9", "(", KY_CHAR, 3, 1), K("-", "_", KY_CHAR, 4, 1) },
    { K("Shift", 0, KY_SHIFT, 0, 2.25f),
      K("z", 0, KY_CHAR, 0, 1), K("x", 0, KY_CHAR, 0, 1), K("c", 0, KY_CHAR, 0, 1), K("v", 0, KY_CHAR, 0, 1),
      K("b", 0, KY_CHAR, 0, 1), K("n", 0, KY_CHAR, 0, 1), K("m", 0, KY_CHAR, 0, 1),
      K(",", "<", KY_CHAR, 0, 1), K(".", ">", KY_CHAR, 0, 1), K("/", "?", KY_CHAR, 0, 1),
      K("Shift", 0, KY_SHIFT, 0, 1.75f), K("", 0, KY_UP, 0, 1), K("Home", "End", KY_HOME, 0, 1),
      P("4", "$", KY_CHAR, 5, 1), K("5", "%", KY_CHAR, 6, 1), K("6", "^", KY_CHAR, 7, 1), K("=", "+", KY_CHAR, 8, 1) },
    { K("Ctrl", 0, KY_CTRL, 0, 1.25f), K("", 0, KY_SUPER, 0, 1.25f), K("Alt", 0, KY_ALT, 0, 1.25f),
      K(" ", 0, KY_SPACE, 0, 6.25f),
      K("Alt", 0, KY_ALT, 0, 1), K("Fn", 0, KY_FN, 0, 1), K("Ctrl", 0, KY_CTRL, 0, 1),
      K("", 0, KY_LEFT, 0, 1), K("", 0, KY_DOWN, 0, 1), K("", 0, KY_RIGHT, 0, 1),
      P("1", "!", KY_CHAR, 9, 1), K("2", "@", KY_CHAR, 10, 1), K("3", "#", KY_CHAR, 11, 1), K("0", ")", KY_CHAR, 12, 1) },
};
#undef K
#undef P

static struct {
    int caps;                  /* Caps Lock: stays until turned off */
    int shift, ctrl, alt, fn;  /* tapped on screen (or R twice for Alt): for the next key only */
    int lhold, rhold;          /* the triggers, while held */
    Uint32 l_at, r_at;         /* when each was last pressed, for the double tap */
    int l_used, r_used;        /* a key was typed (or the other trigger joined in) while it was held */
} pk;

static void pk_reset(void) { memset(&pk, 0, sizeof pk); }
static int pk_fnmode(void) { return pk.fn || (pk.lhold && pk.rhold); }
static int pk_shifted(void) { return !pk_fnmode() && (pk.lhold || pk.shift); }
static int pk_ctrl(void) { return !pk_fnmode() && (pk.rhold || pk.ctrl); }

/* the keys with their positions; cnt gets the number in each row */
static void pk_layout(PKey rows[PK_ROWS][PK_MAXC], int cnt[PK_ROWS])
{
    for (int r = 0; r < PK_ROWS; r++) {
        float x = 0;
        cnt[r] = 0;
        for (int c = 0; c < PK_MAXC && pk_rows[r][c].w > 0; c++) {
            rows[r][c] = pk_rows[r][c];
            if (rows[r][c].pad_block) x = 16.5f;
            rows[r][c].x = x;
            x += rows[r][c].w;
            cnt[r]++;
        }
    }
}

/* D-pad movement: left and right go along the row (and on into the other block), up and down to the
 * key nearest above or below */
static void pk_nav(int a, int *r, int *c)
{
    PKey rows[PK_ROWS][PK_MAXC];
    int cnt[PK_ROWS];
    pk_layout(rows, cnt);
    if (*r < 0 || *r >= PK_ROWS) *r = 0;
    if (*c < 0 || *c >= cnt[*r]) *c = 0;
    if (a == ACT_LEFT) *c = (*c + cnt[*r] - 1) % cnt[*r];
    else if (a == ACT_RIGHT) *c = (*c + 1) % cnt[*r];
    else if (a == ACT_UP || a == ACT_DOWN) {
        int nr = *r + (a == ACT_UP ? -1 : 1);
        if (nr < 0 || nr >= PK_ROWS) return;
        float cx = rows[*r][*c].x + rows[*r][*c].w / 2, best = 1e9f;
        int bc = 0;
        for (int i = 0; i < cnt[nr]; i++) {
            float d = fabsf(rows[nr][i].x + rows[nr][i].w / 2 - cx);
            if (d < best) best = d, bc = i;
        }
        *r = nr, *c = bc;
    } else return;
    snd(SND_STEP);
}

static const PKey *pk_at(int r, int c)
{
    static PKey rows[PK_ROWS][PK_MAXC];
    int cnt[PK_ROWS];
    pk_layout(rows, cnt);
    if (r < 0 || r >= PK_ROWS || c < 0 || c >= cnt[r]) return NULL;
    return &rows[r][c];
}

/* what a key press means */
typedef struct {
    int type;     /* PK_*; KY_CHAR with text or fkey set */
    char text[8]; /* the character to type */
    int fkey;     /* F1..F12, or 0 */
    int shift, ctrl, alt;
} PKOut;

/* Modifier keys change the state and return 0. Anything else fills out and returns 1; the one-shot
 * modifiers are used up by it. */
static int pk_press(const PKey *k, PKOut *o)
{
    memset(o, 0, sizeof *o);
    switch (k->type) {
    case KY_SHIFT: pk.shift = !pk.shift; snd(SND_STEP); return 0;
    case KY_CAPS: pk.caps = !pk.caps; snd(SND_STEP); return 0;
    case KY_CTRL: pk.ctrl = !pk.ctrl; snd(SND_STEP); return 0;
    case KY_ALT: pk.alt = !pk.alt; snd(SND_STEP); return 0;
    case KY_FN: pk.fn = !pk.fn; snd(SND_STEP); return 0;
    default: break;
    }
    o->type = k->type;
    o->shift = pk_shifted();
    o->ctrl = pk_ctrl();
    o->alt = pk.alt;
    if (k->type == KY_CHAR) {
        if (pk_fnmode() && k->fkey) o->fkey = k->fkey;
        else if (!k->shifted) { /* a letter: Shift and Caps Lock cancel out, like a real keyboard */
            sncpy(o->text, sizeof o->text, k->base);
            if ((o->shift != 0) != (pk.caps != 0)) o->text[0] = (char)(o->text[0] - 32);
        } else sncpy(o->text, sizeof o->text, o->shift ? k->shifted : k->base);
    } else if (k->type == KY_SPACE) sncpy(o->text, sizeof o->text, " ");
    pk.shift = pk.ctrl = pk.alt = pk.fn = 0;
    if (pk.lhold) pk.l_used = 1;
    if (pk.rhold) pk.r_used = 1;
    return 1;
}

/* a trigger went down or up while a keyboard is showing */
static void pk_trigger(int right, int down)
{
    Uint32 now = SDL_GetTicks();
    int *hold = right ? &pk.rhold : &pk.lhold, *used = right ? &pk.r_used : &pk.l_used, *other = right ? &pk.lhold : &pk.rhold;
    Uint32 *at = right ? &pk.r_at : &pk.l_at;
    if (!down) {
        *hold = 0;
        return;
    }
    /* a second quick tap, when the first one typed nothing and wasn't part of L + R */
    if (!*other && !*used && *at && now - *at < 350) {
        if (right) pk.alt = !pk.alt;
        else pk.caps = !pk.caps;
        snd(SND_STEP);
        *at = 0; /* a third tap starts over */
    } else *at = now;
    *hold = 1;
    *used = 0;
    if (*other) { /* L + R: neither counts as a tap */
        pk.l_used = pk.r_used = 1;
        pk.l_at = pk.r_at = 0;
    }
}

/* Keys that do nothing in a text box (it has no cursor to move and no programs to send keys to). */
static int pk_usable(const PKey *k, int textbox)
{
    if (!textbox) return 1;
    switch (k->type) {
    case KY_CHAR: return !(pk_fnmode() && k->fkey);
    case KY_TAB: case KY_CTRL: case KY_ALT: case KY_FN: case KY_SUPER: case KY_UP: case KY_DOWN: case KY_LEFT:
    case KY_RIGHT: case KY_PGUP: case KY_PGDN: case KY_HOME: case KY_DEL: return 0;
    default: return 1;
    }
}

/* draws the keys into a box; sr, sc is the highlighted key */
static void pk_draw(float x0, float y0, float w, float h, int sr, int sc, float a, int textbox)
{
    PKey rows[PK_ROWS][PK_MAXC];
    int cnt[PK_ROWS];
    pk_layout(rows, cnt);
    float u = w / PK_UNITS, rh = h / PK_ROWS, gap = 2.5f * S;
    int fnm = pk_fnmode(), sh = pk_shifted();
    for (int r = 0; r < PK_ROWS; r++) {
        for (int c = 0; c < cnt[r]; c++) {
            const PKey *k = &rows[r][c];
            float kx = x0 + k->x * u, ky = y0 + r * rh, kw = k->w * u, kh = rh;
            int sel = sr == r && sc == c, use = pk_usable(k, textbox);
            int armed = (k->type == KY_SHIFT && sh) || (k->type == KY_CAPS && pk.caps) || (k->type == KY_CTRL && pk_ctrl()) ||
                        (k->type == KY_ALT && pk.alt) || (k->type == KY_FN && fnm);
            int fn = k->type != KY_CHAR && k->type != KY_SPACE;
            Col kc = sel ? with_alpha(th.accent, 0.70f)
                         : armed ? with_alpha(th.accent, 0.50f)
                         : k->type == KY_ENTER ? with_alpha(th.accent, 0.45f)
                         : (fnm && k->fkey && use) ? with_alpha(th.accent, 0.22f)
                         : fn ? with_alpha(th.glass_raised, 0.16f) : with_alpha(th.glass_raised, 0.09f);
            pane(kx + gap, ky + gap, kw - 2 * gap, kh - 2 * gap, 9 * S, SQUIRCLE, kc, DEPTH_CONTROL * S * 0.6f, FROST_CONTROL,
                 sel ? GLOSS_FULL : GLOSS_QUIET, a, 0);
            float cx = kx + kw / 2, cy = ky + kh / 2, la = a * (use ? 1.0f : 0.35f);
            float gs = fminf(kh, kw) * 0.42f;
            switch (k->type) {
            case KY_BKSP: glyph("arrow-left", cx, cy, gs, th.text, la); continue;
            case KY_UP: glyph("arrow-up", cx, cy, gs, th.text, la); continue;
            case KY_DOWN: glyph("arrow-down", cx, cy, gs, th.text, la); continue;
            case KY_LEFT: glyph("arrow-left", cx, cy, gs, th.text, la); continue;
            case KY_RIGHT: glyph("arrow-right", cx, cy, gs, th.text, la); continue;
            case KY_SUPER: glyph("file-home", cx, cy, gs, th.text, la); continue;
            case KY_SPACE:
                pane(cx - kw * 0.18f, cy - 1 * S, kw * 0.36f, 2.5f * S, 1.25f * S, CIRCLE, with_alpha(th.text, 0.45f), 0, 0, 0, la, 0);
                continue;
            default: break;
            }
            if (k->type == KY_CHAR && fnm && k->fkey) { /* L + R: the F keys */
                char f[8];
                snprintf(f, sizeof f, "F%d", k->fkey);
                float ts = 17 * S;
                text(f, cx, cy - ts * 0.55f, ts, 1, th.text, la, AL_CENTER, 0);
            } else if (k->type == KY_CHAR && !k->shifted) { /* a letter */
                char l[4] = { k->base[0], 0 };
                if ((sh != 0) != (pk.caps != 0)) l[0] = (char)(l[0] - 32);
                float ts = 22 * S;
                text(l, cx, cy - ts * 0.55f, ts, sel, th.text, la, AL_CENTER, 0);
            } else if (k->shifted && k->type == KY_CHAR) { /* two legends, Shift's on top; the one it types is bright */
                float ts = 15 * S, bs = 19 * S;
                text(k->shifted, cx, ky + kh * 0.12f, ts, sh && sel, th.text, la * (sh ? 1.0f : 0.5f), AL_CENTER, 0);
                text(k->base, cx, ky + kh * 0.47f, bs, !sh && sel, th.text, la * (sh ? 0.4f : 1.0f), AL_CENTER, 0);
            } else if (k->type == KY_PGUP) { /* Pg with an up arrow over Pg with a down arrow; Shift picks the lower one */
                float ts = 12.5f * S, gsz = ts * 1.1f, tw = (float)text_width("Pg", ts, 0), lx = cx - (tw + gsz + 2 * S) / 2;
                float ya = cy - ts * 1.15f, yb = cy + ts * 0.05f, ua = la * (sh ? 0.45f : 1.0f), da = la * (sh ? 1.0f : 0.45f);
                text("Pg", lx, ya, ts, !sh && sel, th.text, ua, AL_LEFT, 0);
                glyph("arrow-up", lx + tw + 2 * S + gsz / 2, ya + ts * 0.55f, gsz, th.text, ua);
                text("Pg", lx, yb, ts, sh && sel, th.text, da, AL_LEFT, 0);
                glyph("arrow-down", lx + tw + 2 * S + gsz / 2, yb + ts * 0.55f, gsz, th.text, da);
            } else if (k->shifted) { /* Delete / Insert, Home / End */
                float ts = 12.5f * S;
                text(k->base, cx, cy - ts * 1.15f, ts, !sh && sel, th.text, la * (sh ? 0.45f : 1.0f), AL_CENTER, (int)(kw - 4 * S));
                text(k->shifted, cx, cy + ts * 0.05f, ts, sh && sel, th.text, la * (sh ? 1.0f : 0.45f), AL_CENTER, (int)(kw - 4 * S));
            } else if (strchr(k->base, '\n')) { /* Page Up, Page Down */
                char top[16];
                const char *nl = strchr(k->base, '\n');
                size_t n = (size_t)(nl - k->base);
                if (n >= sizeof top) n = sizeof top - 1;
                memcpy(top, k->base, n), top[n] = 0;
                float ts = 12.5f * S;
                text(top, cx, cy - ts * 1.15f, ts, sel, th.text, la, AL_CENTER, (int)(kw - 4 * S));
                text(nl + 1, cx, cy + ts * 0.05f, ts, sel, th.text, la, AL_CENTER, (int)(kw - 4 * S));
            } else {
                float ts = 15 * S;
                text(k->base, cx, cy - ts * 0.55f, ts, sel || k->type == KY_ENTER, th.text, la, AL_CENTER, (int)(kw - 4 * S));
                if (k->type == KY_CAPS && pk.caps) /* the light on a Caps Lock key */
                    pane(kx + kw - gap - 10 * S, ky + gap + 6 * S, 5 * S, 5 * S, 2.5f * S, CIRCLE, th.text, 0, 0, 0, la, 0);
            }
        }
    }
}

/* the bar of hints under the text box and above the terminal's keys */
static const char *pk_hint(int textbox)
{
    return textbox ? "L: Shift   L twice: Caps Lock   Triangle: backspace   Square: space   Start: done"
                   : "L: Shift   R: Ctrl   L+R: F1-F12   Square+D-pad: arrows   Square+stick: scroll";
}

/* The text-box keyboard: the PC keyboard above, with the text so far in a bar just over it. */
static int osk_sentence_start(void)
{
    size_t n = strlen(dlg.buf);
    if (n == 0) return 1;
    return n >= 2 && dlg.buf[n - 1] == ' ' && strchr(".!?", dlg.buf[n - 2]);
}

/* called when the keyboard opens, with the title, action and text already set */
static void osk_begin(void)
{
    pk_reset();
    dlg.autocap = dlg.action == A_FILE_RENAME || dlg.action == A_FILE_MKDIR || (dlg.action == A_BR_FIELD && !strstr(dlg.title, "assword"));
    dlg.mask = strstr(dlg.title, "assword") != NULL;
    pk.shift = dlg.autocap && osk_sentence_start() ? 1 : 0;
    dlg.r = 0;
    dlg.c = 1; /* Q */
}

static void osk_after_edit(void)
{
    if (dlg.autocap && !pk.caps) pk.shift = osk_sentence_start() ? 1 : 0;
}

/* the tail of the text that fits in a width */
static const char *osk_tail(const char *s, float size, int maxw)
{
    while (*s && text_width(s, size, 0) > maxw) {
        s++;
        while (((unsigned char)*s & 0xC0) == 0x80) s++;
    }
    return s;
}

static void draw_osk(float a)
{
    float ph = floorf(H * 0.40f), y0 = (float)H - ph, pad = 6 * S;
    if (dlg.action == A_APP_TYPE) { /* the keyboard over an app: just the keys, and a line of what the buttons do */
        const char *line = "L: Shift   R: Ctrl   Triangle: Backspace   Square: Space   Start: Enter   B or R+Triangle: close";
        float lw = (float)text_width(line, 15 * S, 0);
        pane(0, y0 - 30 * S, (float)W, 30 * S, 0, SQUIRCLE, with_alpha(th.glass, 0.88f), 0, 0, 0, a, 0);
        text(line, ((float)W - lw) / 2, y0 - 25 * S, 15 * S, 0, th.text_soft, a, AL_LEFT, 0);
        pane(0, y0, (float)W, ph + 40 * S, 0, SQUIRCLE, with_alpha(th.glass, 0.88f), DEPTH_PANEL * S, FROST_PANEL, GLOSS_QUIET, a, 1);
        pane(0, y0, (float)W, 2 * S, 0, CIRCLE, with_alpha(th.text, 0.30f), 0, 0, 0, a, 0);
        pk_draw(pad, y0 + pad, W - 2 * pad, ph - 2 * pad, dlg.r, dlg.c, a, 0);
        return;
    }
    if (!scene_transparent) pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.30f }, 0, 0, 0, a, 0); /* dims what is behind, unless that is an app */
    /* the text so far, just above the keys */
    float fh = 58 * S, fy = y0 - fh - 12 * S;
    pane(pad, fy, W - 2 * pad, fh, 16 * S, SQUIRCLE, with_alpha(th.glass, 0.82f), DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_QUIET, a, 0);
    const char *hint = pk_hint(1);
    float hw = (float)text_width(hint, 14 * S, 0);
    text(hint, W - pad - 22 * S - hw, fy + 7 * S, 14 * S, 0, th.text_soft, a * 0.8f, AL_LEFT, 0);
    text(dlg.title, pad + 22 * S, fy + 6 * S, 16 * S, 0, th.text_soft, a, AL_LEFT, (int)(W - 2 * pad - 44 * S - hw - 20 * S));
    char shown[700] = "";
    if (dlg.mask) {
        size_t o = 0;
        for (const char *p = dlg.buf; *p && o + 4 < sizeof shown; p++)
            if (((unsigned char)*p & 0xC0) != 0x80) memcpy(shown + o, "\xE2\x80\xA2", 3), o += 3;
        shown[o] = 0;
    } else sncpy(shown, sizeof shown, dlg.buf);
    int maxw = (int)(W - 2 * pad - 60 * S);
    const char *tail = osk_tail(shown, 26 * S, maxw);
    float tw = text(tail, pad + 22 * S, fy + 25 * S, 26 * S, 0, th.text, a, AL_LEFT, 0);
    if ((frame_no / 30) % 2) pane(pad + 22 * S + tw + 2 * S, fy + 27 * S, 2.5f * S, 26 * S, 1 * S, CIRCLE, th.text, 0, 0, 0, a, 0);
    /* the keys */
    pane(0, y0, (float)W, ph + 40 * S, 0, SQUIRCLE, with_alpha(th.glass, 0.88f), DEPTH_PANEL * S, FROST_PANEL, GLOSS_QUIET, a, 1);
    pane(0, y0, (float)W, 2 * S, 0, CIRCLE, with_alpha(th.text, 0.30f), 0, 0, 0, a, 0);
    pk_draw(pad, y0 + pad, W - 2 * pad, ph - 2 * pad, dlg.r, dlg.c, a, 1);
}

/* a question with a list to tick, or buttons to choose between */
static void draw_check(float a)
{
    float pw = fminf(W * 0.70f, 760 * S), px = (W - pw) / 2, rh = 54 * S;
    char lines[12][256];
    int nl = yd.text[0] ? wrap_text(yd.text, 21 * S, (int)(pw - 64 * S), lines, 12) : 0;
    int vis = yd.n < 6 ? yd.n : 6;
    float ph = 96 * S + nl * 30 * S + vis * rh + 24 * S;
    float py = (H - ph) / 2 - (1 - a) * 20 * S;
    pane(px, py, pw, ph, 28 * S, SQUIRCLE, with_alpha(th.glass, 0.55f), DEPTH_PANEL * S, FROST_PANEL, GLOSS_QUIET, a, 1);
    text(dlg.title, px + 32 * S, py + 30 * S, 28 * S, 1, th.text, a, AL_LEFT, (int)(pw - 64 * S));
    float ty = py + 80 * S;
    for (int i = 0; i < nl; i++, ty += 30 * S) text(lines[i], px + 32 * S, ty, 21 * S, 0, th.text_soft, a, AL_LEFT, 0);
    int top = yd.sel - vis / 2;
    if (top > yd.n - vis) top = yd.n - vis;
    if (top < 0) top = 0;
    for (int k = 0; k < vis; k++) {
        int i = top + k;
        float ry = ty + 8 * S + k * rh;
        int sel = i == yd.sel;
        pane(px + 20 * S, ry, pw - 40 * S, rh - 6 * S, (rh - 6 * S) / 2, SQUIRCLE, sel ? with_alpha(th.accent, 0.55f) : with_alpha(th.glass_raised, 0.06f),
             DEPTH_CONTROL * S, FROST_CONTROL, sel ? GLOSS_FULL : GLOSS_QUIET, a, 0);
        if (yd.it[i].button) {
            text(yd.it[i].label, px + pw / 2, ry + (rh - 6 * S) / 2 - 12 * S, 24 * S, sel, th.text, a, AL_CENTER, (int)(pw - 100 * S));
        } else {
            float bs = 32 * S, bx = px + 44 * S, by = ry + (rh - 6 * S - bs) / 2;
            mark_draw(yd.radio ? 1 : 2, yd.it[i].check, bx + bs / 2, by + bs / 2, bs, th.text, a);
            text(yd.it[i].label, bx + bs + 20 * S, ry + (rh - 6 * S) / 2 - 12 * S, 24 * S, sel, th.text, a, AL_LEFT, (int)(pw - 160 * S));
        }
    }
}

static void draw_dialog(void)
{
    if (dlg.kind == D_NONE) return;
    float a = dlg.anim;
    if (dlg.kind != D_OSK && dlg.kind != D_UPDATE) pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.62f }, 0, 0, 0, a, 0);
    if (dlg.kind == D_CHECK) {
        draw_check(a);
        return;
    }
    if (dlg.kind == D_OSK) {
        draw_osk(a);
        return;
    }
    if (dlg.kind == D_UPDATE) { /* a full page, like File Transfer */
        pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.28f }, 0, 0, 0, 1, 0);
        draw_page_header("setting-update-system", dlg.title);
        float cx = W / 2.0f, cy = H * 0.34f;
        text(upd.done ? (upd.ok ? "Done" : "Couldn't finish") : upd.phase, cx, cy - 22 * S, 42 * S, 1, th.text, 1, AL_CENTER, (int)(W * 0.9f));
        float lw = W * 0.46f, ly = cy + 54 * S;
        pane(cx - lw / 2, ly, lw, 4 * S, 2 * S, CIRCLE, with_alpha(th.text, 0.25f), 0, 0, 0, 1, 0);
        if (upd.pct > 0.001f || upd.done) {
            pane(cx - lw / 2, ly, fmaxf(lw * clampf(upd.pct, 0, 1), 4 * S), 4 * S, 2 * S, CIRCLE,
                 upd.done && !upd.ok ? with_alpha(th.text_soft, 0.6f) : with_alpha(th.text, 0.95f), 0, 0, 0, 1, 0);
        } else {
            float seg = lw * 0.28f, p = fmodf(now_s * 0.5f, 1.0f);
            float sx = cx - lw / 2 + (lw - seg) * (0.5f - 0.5f * cosf(p * 6.2832f));
            pane(sx, ly - 0.5f * S, seg, 5 * S, 2.5f * S, CIRCLE, with_alpha(th.text, 0.9f), 0, 0, 0, 1, 0);
        }
        float ty = ly + 44 * S, step = 36 * S;
        if (upd.pct > 0.001f || upd.done) {
            char pc[16];
            snprintf(pc, sizeof pc, "%d%%", (int)(clampf(upd.pct, 0, 1) * 100 + 0.5f));
            text(pc, cx, ty, 26 * S, 1, th.text, 1, AL_CENTER, 0);
        }
        if (upd.done) {
            char rl[4][256];
            int n = wrap_text(upd.result, 24 * S, (int)(W * 0.8f), rl, 4);
            for (int i = 0; i < n; i++) text(rl[i], cx, ty + step + i * 34 * S, 24 * S, 0, th.text, 1, AL_CENTER, 0);
            Hint h[1] = { { pad_ok(), "OK" } };
            draw_hints(h, 1);
        } else {
            if (upd.detail[0]) text(upd.detail, cx, ty + step, 21 * S, 0, th.text, 1, AL_CENTER, (int)(W * 0.9f));
            int first = upd.nlines > 4 ? upd.nlines - 4 : 0;
            for (int i = first; i < upd.nlines; i++)
                text(upd.lines[i], cx, ty + step * 1.9f + (i - first) * 26 * S, 18 * S, 0, th.text_soft, 0.9f, AL_CENTER, (int)(W * 0.9f));
            text("Please don't turn the device off.", cx, (float)H - 96 * S, 19 * S, 0, th.text_soft, 0.8f, AL_CENTER, (int)(W * 0.9f));
        }
        return;
    }
    float pw = fminf(W * 0.70f, 760 * S), px = (W - pw) / 2;
    char lines[12][256];
    int nl = dlg.text[0] ? wrap_text(dlg.text, 21 * S, (int)(pw - 64 * S), lines, 12) : 0;
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

static GuideCard *guide_selected(void)
{
    if (guide.ncards == 0) return NULL;
    if (guide.card < 0) guide.card = 0;
    if (guide.card >= guide.ncards) guide.card = guide.ncards - 1;
    return &guide.cards[guide.card];
}

/* The window the Guide's app buttons are about: the one picked in the switcher, or else the one in front.
 * Pixi's own apps (the browser, the terminal) aren't windows, so they have none. */
static Win *guide_target_win(void)
{
    GuideCard *c = guide_selected();
    if (c && c->id && !app_is_ours(c->id)) {
        Win *w = win_find(c->id);
        if (w) return w;
    }
    if (app_front()) return NULL;
    return front_win();
}

static void guide_tile_caption(int i, Win *w, char *out, size_t cap)
{
    switch (i) {
    case 0: snprintf(out, cap, "Wi-Fi: %s", guide.wifi_on < 0 ? "not available" : guide.wifi_on ? "On" : "Off"); break;
    case 1: snprintf(out, cap, "Bluetooth: %s", guide.bt_on < 0 ? "no adapter" : guide.bt_on ? "On" : "Off"); break;
    case 2:
        if (w) snprintf(out, cap, "Controls: %s", w->padmode == PAD_MOUSE ? "Keyboard + mouse" : "Gamepad");
        else snprintf(out, cap, "Gamepad / mouse mode (needs an app)");
        break;
    default:
        if (w) snprintf(out, cap, "Screenshot: %s", *w->title ? w->title : *w->app_id ? w->app_id : "this app");
        else snprintf(out, cap, "Screenshot (needs an app)");
        break;
    }
}

static void guide_add_card(uint32_t id, const char *label)
{
    if (guide.ncards >= 38) return;
    GuideCard *c = &guide.cards[guide.ncards++];
    c->id = id;
    sncpy(c->label, sizeof c->label, label);
}

static const char *win_label(const Win *w) { return *w->title ? w->title : *w->app_id ? w->app_id : "Application"; }

/* the card that's on the screen now: one of Pixi's own apps, or the front window */
static uint32_t guide_current(void) { return app_front() ? app_front() : front_id; }

static void guide_entries(void)
{
    /* the switcher's cards: what's on screen first, the other apps, then the home screen */
    uint32_t keep = guide.ncards ? guide_selected()->id : 0;
    int had = guide.ncards;
    guide.ncards = 0;
    uint32_t ours = app_front();
    Win *f = front_win();
    if (ours) guide_add_card(ours, app_label(ours));
    else if (f) guide_add_card(f->id, win_label(f));
    for (int i = nwins - 1; i >= 0; i--) {
        if (!ours && f && wins[i].id == f->id) continue;
        guide_add_card(wins[i].id, win_label(&wins[i]));
    }
    static const uint32_t apps[7] = { APP_BROWSER, APP_VIDEO, APP_MUSIC, APP_STORE, APP_TERMINAL, APP_FTP, APP_WEB };
    for (int i = 0; i < 7; i++)
        if (apps[i] != ours && app_running(apps[i])) guide_add_card(apps[i], app_label(apps[i]));
    guide_add_card(0, "Home screen");
    if (had) {
        guide.card = guide.ncards - 1;
        for (int i = 0; i < guide.ncards; i++)
            if (guide.cards[i].id == keep) guide.card = i;
    }

    /* the buttons on the left act on the card that's selected */
    guide.nentries = 0;
    GuideCard *sel = guide_selected();
    GuideEntry *e;
    uint32_t cur = guide_current();
    if (sel && sel->id) {
        e = &guide.entries[guide.nentries++];
        e->what = sel->id == cur ? GE_RESUME : GE_SWITCH;
        e->id = sel->id;
        sncpy(e->label, sizeof e->label, sel->id == cur ? "Resume" : "Switch to");
        e = &guide.entries[guide.nentries++];
        e->what = GE_CLOSE;
        e->id = sel->id;
        char closing[sizeof sel->label];
        sncpy(closing, sizeof closing, sel->label);
        snprintf(e->label, sizeof e->label, "Close %s", closing);
    }
    e = &guide.entries[guide.nentries++];
    e->what = cur ? GE_HOME : GE_START;
    sncpy(e->label, sizeof e->label, "Home screen");
    if (guide.entry >= guide.nentries) guide.entry = guide.nentries - 1;
}

static void guide_open(void)
{
    if (guide.open) return;
    memset(&guide, 0, sizeof guide);
    guide.open = 1;
    guide.row = G_SWITCH;
    guide.volume = volume_get();
    guide.brightness = brightness_get();
    guide.wifi_on = wifi_available() ? wifi_radio() : -1;
    guide.bt_on = bluetooth_radio();
    guide_entries();
    guide.card = 0; /* the running app, or the home screen when nothing runs */
    guide.cpos = 0;
    guide_entries();
    if (app_front()) comp_set_mode(MODE_HOME);
    else if (comp_mode == MODE_APP || front_id) comp_set_mode(front_id ? MODE_OVERLAY : MODE_HOME);
    snd(SND_GUIDE_OPEN);
}

static void comp_clear_previews(void)
{
    if (!pixi || !guide.previews_sent) return;
    pixi_shell_v1_previews_flush(pixi);
    pixi_shell_v1_previews_flush(pixi);
    wl_display_flush(wl_dpy);
    guide.previews_sent = 0;
}

static void guide_close(int resume)
{
    guide.open = 0;
    guide.power_open = 0;
    comp_clear_previews();
    if (!resume) app_hide_all();
    if (resume && front_id && !app_front()) comp_set_mode(MODE_APP);
    else comp_set_mode(MODE_HOME);
    snd(SND_BACK);
}

/* Where card i is, with how clearly it should show. Returns 0 when it is off the screen. */
static int guide_card_rect(int i, float *x, float *y, float *w, float *h, float *alpha, int *whole)
{
    float panel_r = GUIDE_MARGIN * S + fmaxf(392 * S, W * 0.27f);
    float avail = W - panel_r - GUIDE_MARGIN * S * 2;
    float cw = fminf(avail * 0.92f, W * 0.54f);
    float ch = cw * H / W;
    if (ch > H * 0.56f) ch = H * 0.56f, cw = ch * W / H;
    float step = ch + 64 * S;
    float off = (float)i - guide.cpos;
    float cy = H * 0.5f - ch * 0.5f - 14 * S + off * step;
    *w = cw, *h = ch;
    *x = panel_r + (avail - cw) / 2 + GUIDE_MARGIN * S;
    *y = cy;
    float top = 30 * S, bottom = H - 62 * S;
    *whole = cy >= top - 1 && cy + ch + 34 * S <= bottom;
    *alpha = clampf(1.0f - fabsf(off) * 0.42f, 0.35f, 1.0f);
    return cy + ch > -10 && cy < H + 10;
}

/* Ask the compositor to draw each running app, live, in its card. */
static void guide_send_previews(void)
{
    if (!pixi || !guide.open) return;
    int any = 0;
    for (int i = 0; i < guide.ncards; i++) {
        GuideCard *c = &guide.cards[i];
        if (!c->id || app_is_ours(c->id)) continue;
        float x, y, w, h, a;
        int whole;
        if (!guide_card_rect(i, &x, &y, &w, &h, &a, &whole) || !whole) continue;
        pixi_shell_v1_preview(pixi, c->id, (int32_t)x, (int32_t)y, (int32_t)w, (int32_t)h, (uint32_t)(a * guide.anim * 1000.0f));
        any = 1;
    }
    pixi_shell_v1_previews_flush(pixi);
    wl_display_flush(wl_dpy);
    if (any) guide.previews_sent = 1;
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
    char hm[32], date[32];
    clock_time_text(hm, sizeof hm, &tm);
    clock_date_text(date, sizeof date, &tm, cfg.date_fmt == 0 ? 3 : cfg.date_fmt);
    text(hm, ix, py + 30 * S, 34 * S, 1, th.text, a, AL_LEFT, 0);
    text(date, ix + iw, py + 36 * S, 18 * S, 0, th.text_soft, a, AL_RIGHT, 0);
    Win *f = front_win();
    char running_label[300];
    if (app_front()) snprintf(running_label, sizeof running_label, "Playing %s", app_label(app_front()));
    else if (f) snprintf(running_label, sizeof running_label, "Playing %s", *f->title ? f->title : f->app_id);
    else sncpy(running_label, sizeof running_label, "Nothing is running");
    text(running_label, ix, py + 80 * S, 18 * S, 0, th.text_soft, a, AL_LEFT, (int)iw);

    /* quick tiles: Wi-Fi, Bluetooth, gamepad or keyboard + mouse, and what the app is */
    Win *tw = guide_target_win();
    int pmode = tw ? tw->padmode : PAD_GAMEPAD;
    const char *tiles[4] = { "setting-wifi", "setting-bluetooth", pmode == PAD_MOUSE ? "setting-mouse" : "category-games", "screenshot" };
    int lit[4] = { guide.wifi_on > 0, guide.bt_on > 0, tw && pmode == PAD_MOUSE, 0 };
    float shade[4] = { guide.wifi_on > 0 ? 1.0f : 0.42f, guide.bt_on > 0 ? 1.0f : 0.42f, tw ? 1.0f : 0.42f, tw ? 1.0f : 0.42f };
    float ty = py + 124 * S, tsz = GUIDE_TILE * S;
    float gap = (iw - tsz * 4) / 3;
    for (int i = 0; i < 4; i++) {
        float tx = ix + i * (tsz + gap);
        int sel = guide.row == G_TILE && guide.tile == i && !guide.power_open;
        Col bg = sel ? with_alpha(th.accent, 0.45f) : lit[i] ? with_alpha(th.accent, 0.26f) : with_alpha(th.glass_raised, 0.08f);
        pane(tx, ty, tsz, tsz, tsz * GUIDE_TILE_RADIUS, SQUIRCLE, bg, DEPTH_CONTROL * S, FROST_CONTROL, sel ? GLOSS_FULL : GLOSS_QUIET, a, 0);
        glyph(tiles[i], tx + tsz / 2, ty + tsz / 2, tsz * GUIDE_TILE_GLYPH, th.text, a * shade[i]);
    }
    char cap[120];
    guide_tile_caption(guide.tile, tw, cap, sizeof cap);
    text(cap, ix + 4 * S, ty + tsz + 6 * S, 17 * S, 0, th.text_soft, a, AL_LEFT, (int)iw);

    /* volume and brightness bars */
    float vh = GUIDE_BAR_HEIGHT * S * 0.75f;
    float vy = ty + tsz + 34 * S;
    for (int k = 0; k < 2; k++) {
        int value = k == 0 ? guide.volume : guide.brightness;
        if (k == 1 && value < 0) break;
        int sel = guide.row == (k == 0 ? G_SLIDER : G_BRIGHT) && !guide.power_open;
        pane(ix, vy, iw, vh, vh / 2, SQUIRCLE, sel ? with_alpha(th.accent, 0.35f) : with_alpha(th.glass_raised, 0.08f), DEPTH_CONTROL * S,
             FROST_CONTROL, sel ? GLOSS_FULL : GLOSS_QUIET, a, 0);
        glyph(k == 0 ? (value == 0 ? "volume-muted" : "volume") : "brightness", ix + vh * 0.62f, vy + vh / 2, vh * 0.62f, th.text, a);
        float sx = ix + vh * 1.2f, sw = iw - vh * 1.65f;
        float fr = value < 0 ? 0 : value / 100.0f;
        pane(sx, vy + vh / 2 - 3 * S, sw, 6 * S, 3 * S, CIRCLE, with_alpha(th.text, 0.25f), 0, 0, 0, a, 0);
        pane(sx, vy + vh / 2 - 3 * S, sw * fr, 6 * S, 3 * S, CIRCLE, th.text, 0, 0, 0, a, 0);
        pane(sx + sw * fr - 8 * S, vy + vh / 2 - 8 * S, 16 * S, 16 * S, 8 * S, CIRCLE, th.text, 0, 0, 0, a, 0);
        vy += vh + 10 * S;
    }

    /* what's playing, with its controls */
    if (mp.nq > 0) {
        float mh = 84 * S;
        int msel = guide.row == G_MEDIA && !guide.power_open;
        pane(ix, vy, iw, mh, 22 * S, SQUIRCLE, with_alpha(th.glass_raised, 0.08f), DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_QUIET, a, 0);
        static const char *mg[3] = { "media-previous", NULL, "media-next" };
        for (int k = 0; k < 3; k++) {
            float bx = ix + iw / 2 + (k - 1) * 74 * S, by = vy + 28 * S, bs = (k == 1 ? 40 : 32) * S;
            int bsel = msel && guide.mbtn == k;
            if (k == 1 || bsel)
                pane(bx - bs * 0.8f, by - bs * 0.8f, bs * 1.6f, bs * 1.6f, bs * 0.8f, CIRCLE, with_alpha(th.accent, bsel ? 0.75f : 0.45f),
                     DEPTH_CONTROL * S, FROST_CONTROL, bsel ? GLOSS_FULL : GLOSS_QUIET, a, 0);
            glyph(k == 1 ? (mp.paused || !mp.active ? "media-play" : "media-pause") : mg[k], bx, by, bs, th.text, a * (bsel || k == 1 ? 1 : 0.75f));
        }
        text(mp.title, ix + 18 * S, vy + mh - 30 * S, 20 * S, 0, th.text, a, AL_LEFT, (int)(iw - 36 * S));
        vy += mh + 10 * S;
    }

    /* separator and the buttons */
    float ey = vy + 12 * S;
    pane(ix, ey - 8 * S, iw, 1.5f * S, 0.75f * S, CIRCLE, with_alpha(th.text, 0.18f), 0, 0, 0, a, 0);
    float rh = GUIDE_ROW_HEIGHT * S * 0.72f;
    float power_y = py + ph - POWER_BUTTON * S - GUIDE_PADDING * S * 0.6f;
    for (int i = 0; i < guide.nentries; i++) {
        float ry = ey + i * (rh + 6 * S);
        if (ry + rh > power_y - 4 * S) break;
        int sel = guide.row == G_ENTRY && guide.entry == i && !guide.power_open;
        pane(ix, ry, iw, rh, rh / 2, SQUIRCLE, sel ? with_alpha(th.accent, 0.62f) : with_alpha(th.glass_raised, 0.07f), DEPTH_CONTROL * S,
             FROST_CONTROL, sel ? GLOSS_FULL : GLOSS_QUIET, a, 0);
        text(guide.entries[i].label, ix + 24 * S, ry + rh / 2 - 12 * S, 23 * S, sel, th.text, a * (sel ? 1 : 0.85f), AL_LEFT,
             (int)(iw - 48 * S));
    }

    /* power */
    float pb = POWER_BUTTON * S;
    int psel = guide.row == G_POWER || guide.power_open;
    pane(ix, power_y, pb, pb, pb / 2, CIRCLE, psel ? with_alpha(th.accent, 0.45f) : with_alpha(th.glass_raised, 0.10f), DEPTH_CONTROL * S,
         FROST_CONTROL, psel ? GLOSS_FULL : GLOSS_QUIET, a, 0);
    glyph("shutdown", ix + pb / 2, power_y + pb / 2, pb * 0.52f, th.text, a);

    /* the app switcher: a card for every running app, and the home screen */
    for (int i = 0; i < guide.ncards; i++) {
        GuideCard *c = &guide.cards[i];
        float cx, cy, cw, ch, ca;
        int whole;
        if (!guide_card_rect(i, &cx, &cy, &cw, &ch, &ca, &whole)) continue;
        int sel = i == guide.card;
        float fa = a * ca;
        if (sel) glow(cx + cw / 2, cy + ch / 2, fmaxf(cw, ch) * 1.25f, with_alpha(th.accent, guide.row == G_SWITCH ? 0.30f : 0.16f), a);
        pane(cx, cy, cw, ch, 20 * S, SQUIRCLE, with_alpha(th.glass, 0.65f), DEPTH_PANEL * S, FROST_CONTROL, GLOSS_QUIET, fa, 1);
        if (!c->id) {
            if (snap_have && tex_snap) {
                QuadInst *q = quad_new(tex_snap);
                if (q) {
                    set4(q->rect, cx, cy, cw, ch);
                    set4(q->color, 1, 1, 1, 1);
                    set4(q->uv, 0, 1, 1, 0);
                    set4(q->shape, 20 * S, 0, 0, 0);
                    set4(q->material, 0, 0, 0, fa);
                    set4(q->misc, SQUIRCLE, 0, -1, KIND_PLAIN);
                }
            } else {
                glyph("category-games", cx + cw / 2, cy + ch / 2, fminf(cw, ch) * 0.34f, th.text, fa * 0.9f);
            }
        } else if (app_is_ours(c->id)) {
            app_draw_card(c->id, cx, cy, cw, ch, fa);
        } else if (!whole) {
            glyph("launch", cx + cw / 2, cy + ch / 2, fminf(cw, ch) * 0.28f, th.text, fa * 0.6f);
        }
        if (sel) outline(cx - 3 * S, cy - 3 * S, cw + 6 * S, ch + 6 * S, 22 * S, 3 * S, th.accent_soft, a * (guide.row == G_SWITCH ? 0.95f : 0.5f));
        text(c->label, cx, cy + ch + 14 * S, 22 * S, sel, th.text, fa, AL_LEFT, (int)cw);
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
    video_frame();
    frame_no++;

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
    wallpaper_tick();
    if (wall.tex) { /* a picture chosen as the wallpaper covers it */
        glUseProgram(prog_copy);
        glUniform1i(glGetUniformLocation(prog_copy, "u_tex"), 0);
        glUniform2f(glGetUniformLocation(prog_copy, "u_res"), (float)W, (float)H);
        glUniform1f(glGetUniformLocation(prog_copy, "u_alpha"), 1.0f);
        const float nohole[4] = { 0, 0, 0, 0 };
        glUniform4fv(glGetUniformLocation(prog_copy, "u_hole"), 1, nohole);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, wall.tex);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
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
    memset(quad_hole, 0, sizeof quad_hole);
    glUniform4fv(glGetUniformLocation(prog_copy, "u_hole"), 1, quad_hole);
    glUniform1f(glGetUniformLocation(prog_copy, "u_hole_radius"), quad_hole_radius);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_bg);
    glBindVertexArray(vao_empty);
    if (!(scene_transparent && dlg.kind == D_OSK)) glDrawArrays(GL_TRIANGLES, 0, 3); /* the wallpaper, unless the app is to show through */

    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    if (scene_transparent && dlg.kind == D_OSK && !guide.open) {
        draw_dialog(); /* just the keyboard */
    } else if (launch.active) {
        draw_launch();
    } else if (dlg.kind == D_UPDATE && !guide.open) {
        draw_dialog();
    } else if (ed_is_open() && !guide.open) {
        draw_editor();
        draw_dialog();
    } else if (gptest_is_open()) {
        draw_gptest();
    } else if (sysinfo_is_open() && !guide.open) {
        draw_sysinfo();
        draw_dialog();
    } else if (browser_is_open() && !guide.open) {
        draw_browser();
        draw_dialog();
    } else if (video_is_open() && !guide.open) {
        draw_video();
        draw_dialog();
    } else if (term_is_open() && !guide.open) {
        draw_terminal();
        draw_dialog();
    } else if (ftp_ui_is_open() && !guide.open) {
        draw_ftp();
        draw_dialog();
    } else if (web_ui_is_open() && !guide.open) {
        draw_web();
        draw_dialog();
    } else if (aps_is_open() && !guide.open) {
        draw_appsettings();
        draw_dialog();
    } else if (alist_is_open() && !guide.open) {
        draw_alist();
        draw_dialog();
    } else if (net_is_open() && !guide.open) {
        draw_net();
        draw_dialog();
    } else if (store_is_open() && !guide.open) {
        draw_store();
        draw_dialog();
    } else if (viewer.open && !guide.open) {
        draw_viewer();
        draw_dialog();
    } else if (np.open && !guide.open) {
        draw_nowplaying();
        draw_dialog();
    } else {
        float home_alpha = 1.0f - (guide.open ? guide.anim * 0.85f : 0);
        if (!guide.open && stack[0]) draw_home(home_alpha);
        if (!guide.open) draw_music_pill(home_alpha);
        if (guide.open) draw_guide();
        if (!side_is_open()) draw_corner(); /* the right-hand list covers it */
        draw_side();
        if (guide.open) {
            Hint h[2] = { { pad_ok(), "Select" }, { pad_back(), "Back" } };
            draw_hints(h, 2);
        } else if (dlg.kind == D_NONE && side_is_open()) {
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
    draw_volhud();
    quads_flush();

    /* a small picture of the home screen, for the app switcher */
    if (!guide.open && !launch.active && !viewer.open && !np.open && !store_is_open() && !browser_is_open() && !video_is_open() && !term_is_open() && !ftp_ui_is_open() && !web_ui_is_open() && !aps_is_open() && !alist_is_open() && !net_is_open() && !sysinfo_is_open() && !gptest_is_open() && dlg.kind == D_NONE && stack[0] && (frame_no % 20 == 1 || !snap_have)) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb_scene);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb_snap);
        glBlitFramebuffer(0, 0, W, H, 0, 0, snap_w, snap_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        snap_have = 1;
    }

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

/* ---------- settings kept for each app (~/.config/pixi/apps.conf) --------- */

/* One line per app: its desktop id, a tab, then key=value pairs. Only the way it's controlled so far. */
typedef struct {
    char id[160];
    int input;
    int sudo; /* Allow Auto sudo: 1 on, 0 off, -1 not set (on for GParted, off for the rest) */
    int bg;   /* Run in Background: 1 on, 0 off, -1 not set (on for music players, off for the rest) */
    int hide; /* Hide Item: kept off the home screen */
} AppSet;
static AppSet appsets[256];
static int nappsets, appsets_loaded;

static void appsets_path(char *out, size_t cap) { snprintf(out, cap, "%s/.config/pixi/apps.conf", home_dir()); }

/* Programs from the Apps list start with the controller as keyboard + mouse, since nearly all of them have no
 * gamepad support. Games added from a PC folder are made for a controller. Either can be changed per app. */
static int appset_default(const char *id)
{
    return id && !strncmp(id, "pc:", 3) ? PAD_GAMEPAD : PAD_MOUSE;
}

static void appsets_load(void)
{
    if (appsets_loaded) return;
    appsets_loaded = 1;
    char p[1200];
    appsets_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[400];
    while (fgets(line, sizeof line, f) && nappsets < 256) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        AppSet *a = &appsets[nappsets];
        memset(a, 0, sizeof *a);
        sncpy(a->id, sizeof a->id, line);
        char *kv = tab + 1;
        a->input = strstr(kv, "input=pad") ? PAD_GAMEPAD : strstr(kv, "input=mouse") ? PAD_MOUSE : appset_default(a->id);
        a->sudo = strstr(kv, "sudo=on") ? 1 : strstr(kv, "sudo=off") ? 0 : -1;
        a->bg = strstr(kv, "bg=on") ? 1 : strstr(kv, "bg=off") ? 0 : -1;
        a->hide = strstr(kv, "hide=on") != NULL;
        nappsets++;
    }
    fclose(f);
}

static void appsets_save(void)
{
    char d[1200], p[1200];
    snprintf(d, sizeof d, "%s/.config", home_dir());
    mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/.config/pixi", home_dir());
    mkdir(d, 0755);
    appsets_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < nappsets; i++) {
        const AppSet *a = &appsets[i];
        if (a->input == appset_default(a->id) && a->sudo < 0 && a->bg < 0 && !a->hide) continue;
        const char *in = a->input == appset_default(a->id) ? "" : a->input == PAD_MOUSE ? "input=mouse" : "input=pad";
        const char *su = a->sudo > 0 ? "sudo=on" : a->sudo == 0 ? "sudo=off" : "";
        const char *bg = a->bg > 0 ? "bg=on" : a->bg == 0 ? "bg=off" : "";
        const char *hd = a->hide ? "hide=on" : "";
        fprintf(f, "%s\t", a->id);
        const char *parts[4] = { in, su, bg, hd };
        int first = 1;
        for (int k = 0; k < 4; k++)
            if (*parts[k]) fprintf(f, "%s%s", first ? "" : " ", parts[k]), first = 0;
        fputc('\n', f);
    }
    fclose(f);
}

static int appset_input(const char *id)
{
    if (!id || !*id) return PAD_MOUSE;
    appsets_load();
    for (int i = 0; i < nappsets; i++)
        if (!strcmp(appsets[i].id, id)) return appsets[i].input;
    return appset_default(id);
}

static void appset_set_input(const char *id, int mode)
{
    if (!id || !*id) return;
    appsets_load();
    for (int i = 0; i < nappsets; i++)
        if (!strcmp(appsets[i].id, id)) {
            appsets[i].input = mode;
            appsets_save();
            return;
        }
    if (nappsets >= 256) return;
    sncpy(appsets[nappsets].id, sizeof appsets[nappsets].id, id);
    appsets[nappsets].sudo = -1;
    appsets[nappsets].bg = -1;
    appsets[nappsets++].input = mode;
    appsets_save();
}

/* Allow Auto sudo: the app is started through sudo, so it runs as root without typing sudo anywhere. */
static int appset_sudo(const char *id)
{
    if (!id || !*id) return 0;
    appsets_load();
    for (int i = 0; i < nappsets; i++)
        if (!strcmp(appsets[i].id, id) && appsets[i].sudo >= 0) return appsets[i].sudo;
    return strcasestr(id, "gparted") != NULL; /* it can't do anything without root */
}

static void appset_set_sudo(const char *id, int on)
{
    if (!id || !*id) return;
    appsets_load();
    for (int i = 0; i < nappsets; i++)
        if (!strcmp(appsets[i].id, id)) {
            appsets[i].sudo = on;
            appsets_save();
            return;
        }
    if (nappsets >= 256) return;
    sncpy(appsets[nappsets].id, sizeof appsets[nappsets].id, id);
    appsets[nappsets].input = appset_default(id);
    appsets[nappsets].bg = -1;
    appsets[nappsets++].sudo = on;
    appsets_save();
}

/* Run in Background: whether the app keeps running while it isn't the one in front. Off for nearly everything, so a
 * game or a browser that's been switched away from stops using the processor and the battery; on for music players. */
static int appset_bg(const char *id)
{
    if (!id || !*id) return 1;
    appsets_load();
    for (int i = 0; i < nappsets; i++)
        if (!strcmp(appsets[i].id, id) && appsets[i].bg >= 0) return appsets[i].bg;
    return strcasestr(id, "music") != NULL;
}

static void appset_set_bg(const char *id, int on)
{
    if (!id || !*id) return;
    appsets_load();
    for (int i = 0; i < nappsets; i++)
        if (!strcmp(appsets[i].id, id)) {
            appsets[i].bg = on;
            appsets_save();
            return;
        }
    if (nappsets >= 256) return;
    sncpy(appsets[nappsets].id, sizeof appsets[nappsets].id, id);
    appsets[nappsets].input = appset_default(id);
    appsets[nappsets].sudo = -1;
    appsets[nappsets++].bg = on;
    appsets_save();
}

/* Hide Item: the app, game or built-in is left off the home screen. It is still installed, and the Apps List shows it. */
static int appset_hidden(const char *id)
{
    if (!id || !*id) return 0;
    appsets_load();
    for (int i = 0; i < nappsets; i++)
        if (!strcmp(appsets[i].id, id)) return appsets[i].hide;
    return 0;
}

static void appset_set_hidden(const char *id, int on)
{
    if (!id || !*id) return;
    appsets_load();
    for (int i = 0; i < nappsets; i++)
        if (!strcmp(appsets[i].id, id)) {
            appsets[i].hide = on;
            return; /* the caller saves, once, when it has set a few */
        }
    if (!on || nappsets >= 256) return;
    memset(&appsets[nappsets], 0, sizeof appsets[nappsets]);
    sncpy(appsets[nappsets].id, sizeof appsets[nappsets].id, id);
    appsets[nappsets].input = appset_default(id);
    appsets[nappsets].sudo = -1;
    appsets[nappsets].bg = -1;
    appsets[nappsets++].hide = 1;
}

/* The window an app opens takes the way of controlling it that was chosen for the app. It may not
 * open straight away (some apps start through a script), so this waits a little for it. */
static struct {
    int mode;
    float until;
    char dsk[160];
} pm_pending;

static void launch_item(const Item *it)
{
    if (!it->argv || !it->argv[0]) return;
    snd(SND_LAUNCH);
    int as_root = (it->flags & IF_APP) && appset_sudo(it->data);
    pid_t pid = fork();
    if (pid < 0) {
        show_message("Couldn't start", strerror(errno));
        return;
    }
    if (pid == 0) {
        setsid();
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        if (cfg.app_scheme) setenv("GTK_THEME", cfg.app_scheme == 2 ? "Adwaita:dark" : "Adwaita:light", 1); /* Layout > Light/Dark Mode for Apps */
        if (it->flags & IF_PC) {
            /* a PC game runs from its own folder (it finds its data next to it), and files copied over
             * FTP arrive without their "executable" mark */
            char dir[1100];
            sncpy(dir, sizeof dir, it->argv[0]);
            char *slash = strrchr(dir, '/');
            if (slash) *slash = 0;
            if (chdir(dir) != 0) {}
            struct stat st;
            if (access(it->argv[0], X_OK) != 0 && stat(it->argv[0], &st) == 0) chmod(it->argv[0], st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH);
        } else if (chdir(home_dir()) != 0) {}
        if (as_root) {
            /* Allow Auto sudo: start it through sudo. A password, if sudo wants one, is asked for on
             * screen (SUDO_ASKPASS, see askpass_listen). Root is let onto Pixi's X display for the
             * window, and keeps the session's display and sound settings. */
            static const char script[] =
                "xhost +SI:localuser:root >/dev/null 2>&1\n"
                "sudo -A --preserve-env=DISPLAY,XDG_RUNTIME_DIR,WAYLAND_DISPLAY,XDG_SESSION_TYPE,PULSE_SERVER,"
                "SDL_VIDEODRIVER,QT_QPA_PLATFORM,GDK_BACKEND -- \"$@\"\n"
                "st=$?\n"
                "xhost -SI:localuser:root >/dev/null 2>&1\n"
                "exit $st\n";
            char *av[64];
            int n = 0;
            av[n++] = (char *)"sh";
            av[n++] = (char *)"-c";
            av[n++] = (char *)script;
            av[n++] = (char *)"sh";
            for (int i = 0; it->argv[i] && n < 63; i++) av[n++] = it->argv[i];
            av[n] = NULL;
            execvp("sh", av);
        }
        execvp(it->argv[0], it->argv);
        fprintf(stderr, "pixi-shell: couldn't run %s: %s\n", it->argv[0], strerror(errno));
        _exit(127);
    }
    pm_pending.mode = (it->flags & IF_APP) ? appset_input(it->data) : PAD_GAMEPAD;
    pm_pending.until = now_s + 30;
    /* what it's called in apps.conf, so Run in Background can be looked up (a game or emulator without an id is
     * keyed by its title) */
    sncpy(pm_pending.dsk, sizeof pm_pending.dsk, (it->flags & IF_APP) && it->data ? it->data : it->title);
    memset(&launch, 0, sizeof launch);
    launch.active = 1;
    launch.pid = pid;
    launch.started = now_s;
    launch.windows_before = nwins;
    sncpy(launch.name, sizeof launch.name, it->title);
    sncpy(launch.icon, sizeof launch.icon, it->icon ? it->icon : "");
    sncpy(launch.glyph, sizeof launch.glyph, it->glyph ? it->glyph : "launch");
}

/* ---------- updating the system ------------------------------------------ */

static void upd_log(const char *line)
{
    if (!*line) return;
    if (upd.nlines == 6) {
        memmove(upd.lines[0], upd.lines[1], sizeof upd.lines[0] * 5);
        upd.nlines = 5;
    }
    sncpy(upd.lines[upd.nlines++], sizeof upd.lines[0], line);
}

/* what the installer says, turned into a phase and a rough progress: true if it was one of its headings */
static int pixi_update_line(const char *line)
{
    static const struct { const char *prefix; float pct; } steps[] = {
        { "  Installing Wayland", 0.28f }, { "  Installing RetroArch", 0.40f }, { "  Installing Chromium", 0.44f },
        { "  Installing emulator cores", 0.48f }, { "  Installing Steam Link", 0.52f }, { "  Building Pixi", 0.58f },
        { "  Installed:", 0.95f },
    };
    int n;
    if (line[0] == '[' && sscanf(line, "[%d/4]", &n) == 1 && n >= 1 && n <= 4) {
        const char *t = strchr(line, ']');
        t = t ? t + 1 : line;
        while (*t == ' ') t++;
        sncpy(upd.phase, sizeof upd.phase, t);
        upd.detail[0] = 0;
        float base = n == 1 ? 0.0f : n == 2 ? 0.05f : n == 3 ? 0.2f : 0.97f;
        if (base > upd.pct) upd.pct = base;
        return 1;
    }
    for (size_t i = 0; i < sizeof steps / sizeof *steps; i++)
        if (!strncmp(line, steps[i].prefix, strlen(steps[i].prefix))) {
            sncpy(upd.detail, sizeof upd.detail, line + 2);
            if (steps[i].pct > upd.pct) upd.pct = steps[i].pct;
            return 1;
        }
    if (!strncmp(line, "ERROR:", 6)) sncpy(upd.fail, sizeof upd.fail, line + 7);
    return 0;
}

/* installers print for a terminal: drop colors and keep only the last redraw of a progress line */
static void upd_clean(char *line)
{
    char *cr = strrchr(line, '\r');
    if (cr && cr[1]) memmove(line, cr + 1, strlen(cr + 1) + 1);
    char *o = line;
    for (const char *i = line; *i; i++) {
        if (*i == '\033' && i[1] == '[') {
            i += 2;
            while (*i && !isalpha((unsigned char)*i)) i++;
            if (!*i) break;
            continue;
        }
        *o++ = *i;
    }
    *o = 0;
}

static void upd_line(char *line)
{
    upd_clean(line);
    size_t l = strlen(line);
    while (l && (line[l - 1] == '\r' || line[l - 1] == ' ')) line[--l] = 0;
    if (!strncmp(line, "PHASE:", 6)) {
        sncpy(upd.phase, sizeof upd.phase, line + 6);
        upd.pct = 0;
        upd.detail[0] = 0;
    } else if (!strncmp(line, "COUNT:", 6)) {
        upd.count = atoi(line + 6);
    } else if (!strncmp(line, "FAIL:", 5)) {
        sncpy(upd.fail, sizeof upd.fail, line + 5);
    } else if (!strncmp(line, "dlstatus:", 9) || !strncmp(line, "pmstatus:", 9)) {
        const char *p = strchr(line + 9, ':');
        if (p) {
            char *end;
            double v = strtod(p + 1, &end);
            if (end != p + 1) {
                upd.pct = (float)(v / 100.0);
                if (*end == ':') sncpy(upd.detail, sizeof upd.detail, end + 1);
            }
        }
    } else if (upd.kind == 3 && pixi_update_line(line)) {
        upd_log(line);
    } else {
        upd_log(line);
        if (strstr(line, "a password is required") || strstr(line, "no tty present"))
            sncpy(upd.fail, sizeof upd.fail, "Updating needs passwordless sudo, which Raspberry Pi OS sets up by default.");
    }
}

static void upd_finish(void)
{
    upd.done = 1;
    upd.ok = upd.code == 0 && !upd.fail[0];
    if (upd.ok && upd.kind == 1) {
        snprintf(upd.result, sizeof upd.result, "Installed %s.", upd.name);
        upd.pct = 1;
        snd(SND_SELECT);
    } else if (upd.ok && upd.kind == 3) {
        sncpy(upd.result, sizeof upd.result, "Pixi is updated. Restart to start using it.");
        upd.pct = 1;
        snd(SND_SELECT);
    } else if (upd.ok && upd.kind == 2) {
        snprintf(upd.result, sizeof upd.result, "Removed %s.", upd.name);
        upd.pct = 1;
        snd(SND_SELECT);
    } else if (upd.ok) {
        if (upd.count == 0) sncpy(upd.result, sizeof upd.result, "Everything is up to date.");
        else snprintf(upd.result, sizeof upd.result, upd.count == 1 ? "Installed %d update." : "Installed %d updates.", upd.count);
        if (access("/var/run/reboot-required", F_OK) == 0)
            strncat(upd.result, " Restart to finish: open Settings, Power, then Restart.", sizeof upd.result - strlen(upd.result) - 1);
        upd.pct = 1;
        snd(SND_SELECT);
    } else {
        if (upd.fail[0]) {
            sncpy(upd.result, sizeof upd.result, upd.fail);
            if (upd.nlines && !strstr(upd.fail, "passwordless")) { /* say what apt itself said */
                char both[sizeof upd.result];
                snprintf(both, sizeof both, "%s %s", upd.fail, upd.lines[upd.nlines - 1]);
                sncpy(upd.result, sizeof upd.result, both);
            }
        } else snprintf(upd.result, sizeof upd.result, "That stopped (code %d). Check the connection and try again.", upd.code);
        snd(SND_ERROR);
    }
}

static void job_start(int kind, const char *title, const char *name, const char *script)
{
    int fds[2];
    if (pipe(fds) != 0) {
        show_message("Couldn't update", strerror(errno));
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        show_message("Couldn't update", strerror(errno));
        return;
    }
    if (pid == 0) {
        setsid();
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", script, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    memset(&upd, 0, sizeof upd);
    upd.kind = kind;
    sncpy(upd.name, sizeof upd.name, name ? name : "");
    upd.active = 1;
    upd.pid = pid;
    upd.fd = fds[0];
    upd.count = -1;
    upd.started = now_s;
    sncpy(upd.phase, sizeof upd.phase, "Starting\xE2\x80\xA6");
    memset(&dlg, 0, sizeof dlg);
    dlg.kind = D_UPDATE;
    sncpy(dlg.title, sizeof dlg.title, title);
}

static void update_start(void)
{
    static const char *script =
        "export DEBIAN_FRONTEND=noninteractive LC_ALL=C\n"
        "echo 'PHASE:Checking for updates'\n"
        "sudo -n apt-get -o APT::Status-Fd=1 update 2>&1 || { echo 'FAIL:Couldn'\"'\"'t check for updates. Is the device online?'; exit 1; }\n"
        "n=$(apt-get -s full-upgrade 2>/dev/null | grep -c '^Inst ')\n"
        "echo \"COUNT:$n\"\n"
        "if [ \"$n\" -gt 0 ]; then\n"
        "  echo \"PHASE:Installing $n updates\"\n"
        "  sudo -n apt-get -y -o APT::Status-Fd=1 -o Dpkg::Options::=--force-confold -o Dpkg::Options::=--force-confdef full-upgrade 2>&1"
        " || { echo 'FAIL:The update didn'\"'\"'t finish.'; exit 1; }\n"
        "fi\n";
    job_start(0, "Update OS", "", script);
}

/* Install or remove one package. The name is checked, so it can't carry anything into the shell. */
#define PIXI_INSTALL_DIR "/boot/firmware/install"
#define PIXI_INSTALL_SH PIXI_INSTALL_DIR "/install.sh"

/* Run the installer again, from the files in the install folder, with no questions asked. */
static void pixi_update_start(void)
{
    static const char *script =
        "export DEBIAN_FRONTEND=noninteractive LC_ALL=C\n"
        "echo 'PHASE:Starting the installer'\n"
        "sudo -n /usr/bin/bash " PIXI_INSTALL_SH " 2>&1 || { echo 'FAIL:The installer didn'\"'\"'t finish.'; exit 1; }\n";
    job_start(3, "Update Pixi", "", script);
}

static void package_job(int install, const char *pkg, const char *label)
{
    if (!strncmp(pkg, "pi:", 3)) {
        piapps_job(install, pkg + 3);
        return;
    }
    for (const char *p = pkg; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '.' && *p != '+' && *p != ' ') {
            show_message("Couldn't continue", "That package name isn't valid.");
            return;
        }
    if (!*pkg) return;
    char lab[96];
    size_t o = 0;
    for (const char *p = label; *p && o < sizeof lab - 1; p++)
        if (isalnum((unsigned char)*p) || *p == ' ' || *p == '-' || *p == '.') lab[o++] = *p;
    lab[o] = 0;
    char script[1200];
    snprintf(script, sizeof script,
             "export DEBIAN_FRONTEND=noninteractive LC_ALL=C\n"
             "echo 'PHASE:%s %s'\n"
             "sudo -n apt-get -y -o APT::Status-Fd=1 %s -- %s 2>&1 || { echo 'FAIL:Could not %s %s. If it cannot be found, run Update OS and try again.'; exit 1; }\n",
             install ? "Installing" : "Removing", lab, install ? "install" : "remove", pkg, install ? "install" : "remove", lab);
    job_start(install ? 1 : 2, install ? "Install" : "Remove", lab, script);
}

static void update_tick(void)
{
    if (!upd.active || upd.done) return;
    char tmp[1024];
    while (!upd.eof) {
        ssize_t n = read(upd.fd, tmp, sizeof tmp);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char ch = tmp[i];
                if (ch == '\n') {
                    upd.buf[upd.blen] = 0;
                    upd_line(upd.buf);
                    upd.blen = 0;
                } else if (upd.blen < sizeof upd.buf - 1) {
                    upd.buf[upd.blen++] = ch;
                }
            }
        } else if (n == 0) {
            upd.eof = 1;
        } else {
            if (errno == EINTR) continue;
            break; /* EAGAIN: nothing more for now */
        }
    }
    if (upd.eof && upd.exited) {
        close(upd.fd);
        upd.fd = -1;
        upd_finish();
    }
}

static void reap_children(void)
{
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        if (term_child_hook(pid, st)) continue;
        if (upd.active && pid == upd.pid) {
            upd.exited = 1;
            upd.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            continue;
        }
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
        w->padmode = PAD_MOUSE; /* a window nobody here launched (an installer's question, say) needs a pointer */
        w->born = now_s;
        if (pm_pending.until > now_s) {
            w->padmode = pm_pending.mode;
            sncpy(w->dsk, sizeof w->dsk, pm_pending.dsk);
            pm_pending.until = 0;
        }
    }
    if (!w) return;
    sncpy(w->app_id, sizeof w->app_id, app_id);
    sncpy(w->title, sizeof w->title, title);
    w->pid = pid;
    if (launch.active) launch.active = 0; /* the window is here: the compositor brings it forward */
    if (guide.open) guide_entries();
}

static void bg_wake_win(Win *w);

static void on_window_closed(void *data, struct pixi_shell_v1 *p, uint32_t id)
{
    for (int i = 0; i < nwins; i++) {
        if (wins[i].id == id) {
            bg_wake_win(&wins[i]);
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
        app_hide_all();
    }
}

/* ---------- Run in Background: pausing the apps that aren't in front ------- */

/* A program the shell launched has its own session (see launch_item), so stopping its process group stops the
 * program and everything it started. A window from a program that was started some other way is never paused. */
static pid_t win_group(const Win *w)
{
    if (w->pid <= 1) return 0;
    pid_t g = getpgid(w->pid);
    if (g <= 1 || g == getpgrp() || g == getpid()) return 0;
    return g;
}

static void bg_signal(const Win *w, int sig)
{
    pid_t g = win_group(w);
    if (g) kill(-g, sig);
    else if (w->pid > 1 && w->pid != getpid()) kill(w->pid, sig);
}

static void bg_wake_win(Win *w)
{
    if (!w->stopped) return;
    bg_signal(w, SIGCONT);
    w->stopped = 0;
    /* other windows of the same program share its group, so they run again too */
    pid_t g = win_group(w);
    for (int i = 0; i < nwins; i++)
        if (&wins[i] != w && g && win_group(&wins[i]) == g) wins[i].stopped = 0;
}

static void bg_wake_id(uint32_t id)
{
    Win *w = win_find(id);
    if (w) bg_wake_win(w);
}

static void bg_wake_all(void)
{
    for (int i = 0; i < nwins; i++) bg_wake_win(&wins[i]);
}

/* A window runs while it is the one in front (with Pixi showing apps, or the Guide over one), or when its program is
 * set to Run in Background. At the home screen nothing is in front, so the others pause. */
static int win_wants_run(const Win *w)
{
    if (!*w->dsk) return 1;                      /* not launched from here (an installer's question): leave it alone */
    if (now_s - w->born < 4.0f || launch.active) return 1; /* still starting up */
    if (appset_bg(w->dsk)) return 1;
    return w->id == front_id && comp_mode != MODE_HOME;
}

static void bg_tick(void)
{
    static float next;
    if (!pixi || now_s < next) return;
    next = now_s + 0.25f;
    for (int i = 0; i < nwins; i++) {
        Win *w = &wins[i];
        pid_t g = win_group(w);
        int run = win_wants_run(w);
        /* a program with several windows runs if any one of them should */
        for (int j = 0; j < nwins && !run; j++)
            if (j != i && g && win_group(&wins[j]) == g && win_wants_run(&wins[j])) run = 1;
        if (run && w->stopped) bg_wake_win(w);
        else if (!run && !w->stopped) {
            bg_signal(w, SIGSTOP);
            w->stopped = 1;
            for (int j = 0; j < nwins; j++)
                if (j != i && g && win_group(&wins[j]) == g) wins[j].stopped = 1;
        }
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
    net_changed(rc == 0);
    if (rc == 0) {
        snprintf(title, sizeof title, "Connected to %s", ssid);
        show_message(title, "");
        snd(SND_SELECT);
    } else {
        show_message("Couldn't connect", err);
        snd(SND_ERROR);
    }
}

/* If the built-in player can't start, hand the file to an outside player (VLC) instead. */
static void video_launch_external(const char *path, const char *title)
{
    char buf[512];
    char **a = exec_to_argv(cfg.video_player);
    if (!a || !a[0] || !media_find_exe(a[0])) {
        snprintf(buf, sizeof buf, "Pixi couldn't start its video player, and the outside player (%s) isn't installed either. Install it with:\nsudo apt install vlc-bin vlc-plugin-base",
                 a && a[0] ? a[0] : "none set");
        show_message("Video player missing", buf);
        snd(SND_ERROR);
        argv_free(a);
        return;
    }
    int n = 0;
    while (a[n]) n++;
    a = xrealloc(a, sizeof(char *) * (size_t)(n + 2));
    a[n] = xstrdup(path);
    a[n + 1] = NULL;
    Item tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.title = (char *)title;
    tmp.glyph = (char *)"category-video";
    tmp.argv = a;
    launch_item(&tmp);
    argv_free(a);
}

static int video_open_list_ok(const char *const *paths, int n, int start);

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
    case A_THEME:
        sncpy(cfg.theme, sizeof cfg.theme, THEMES[it->arg].name);
        theme_apply();
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
            osk_begin();
            SDL_StartTextInput();
        } else {
            wifi_join(it->data, NULL, 0);
        }
        snd(SND_SELECT);
        break;
    case A_VIEW:
        viewer_open(stack[depth], (int)(it - stack[depth]->items), 0);
        break;
    case A_SLIDESHOW:
        for (int i = 0; i < stack[depth]->n; i++)
            if (stack[depth]->items[i].action == A_VIEW) {
                viewer_open(stack[depth], i, 1);
                break;
            }
        break;
    case A_PLAY:
    case A_PLAYALL:
    case A_SHUFFLEALL: {
        Menu *m = stack[depth];
        const char **paths = xcalloc((size_t)m->n + 1, sizeof(char *));
        int n = 0, start = 0;
        for (int i = 0; i < m->n; i++)
            if (m->items[i].action == A_PLAY) {
                if (&m->items[i] == it) start = n;
                paths[n++] = m->items[i].data;
            }
        if (n > 0) {
            if (it->action == A_SHUFFLEALL) start = rand() % n;
            music_start(paths, n, start, it->action == A_SHUFFLEALL || mp.shuffle);
            snd(SND_SELECT);
        }
        free(paths);
        break;
    }
    case A_CLOCK:
    case A_DATEFMT:
    case A_BATPCT:
    case A_SOUNDS:
    case A_CONFIRM:
    case A_VIDEOOUT: side_open_choice(it->action); break;
    case A_STORE_OPEN: store_open(); break;
    case A_ACCENT: side_open_accent(); break;
    case A_FTP: ftp_ui_open(); break;
    case A_WEB: web_ui_open(); break;
    case A_WEB_EDIT: web_edit_config(); break;
    case A_WEB_RELOAD: web_reload_now(); break;
    case A_WEB_RESET:
        show_confirm("Restore the default settings?", "The web server's settings file is replaced with a fresh one. Your changes to it are lost.", A_WEB_RESET, "");
        dlg.choice = 1;
        break;
    case A_UPDATES: side_open_updates(); break;
    case A_NETWORK: net_open(); break;
    case A_SERVERS: side_open_servers(); break;
    case A_APPLIST: alist_open(); break;
    case A_WALL_SET: wallpaper_set(it->data); break;
    case A_WALL_DEFAULT: wallpaper_set(NULL); break;
    case A_BROWSER: browser_open(); break;
    case A_TERMINAL: term_open(NULL, NULL); break;
    case A_TERM_APP: term_open(it->argv, it->title); break;
    case A_SYSINFO: sysinfo_open(); break;
    case A_GAMEPAD: gptest_open(); break;
    case A_POWER: side_open_power(); break;
    case A_LOGOUT:
        show_confirm("Log out of Pixi?", "Running applications will be closed.", A_LOGOUT, "");
        snd(SND_SELECT);
        break;
    case A_REBOOT:
        show_confirm("Restart now?", "", A_REBOOT, "");
        snd(SND_SELECT);
        break;
    case A_SHUTDOWN:
        show_confirm("Shut down now?", "", A_SHUTDOWN, "");
        snd(SND_SELECT);
        break;
    case A_UPDATE:
        show_confirm("Update the system?",
                     "Pixi will check for updates and install them. Connect to Wi-Fi or a network cable first. "
                     "This can take a few minutes. Please don't turn the device off.",
                     A_UPDATE, "");
        snd(SND_SELECT);
        break;
    case A_UPDATE_PIXI:
        if (access(PIXI_INSTALL_SH, R_OK) != 0 ||
            (access(PIXI_INSTALL_DIR "/pixi-src.zip", R_OK) != 0 && access(PIXI_INSTALL_DIR "/pixi.zip", R_OK) != 0))
            show_message("Nothing to install", "Copy install.sh and pixi-src.zip into " PIXI_INSTALL_DIR " first, then try again.");
        else
            show_confirm("Update Pixi?",
                         "Pixi will run its installer from the files in " PIXI_INSTALL_DIR ". Connect to Wi-Fi or a network cable first. "
                         "This can take several minutes, and you'll be asked to restart afterwards.",
                         A_UPDATE_PIXI, "");
        snd(SND_SELECT);
        break;
    case A_NOWPLAYING:
        if (mp.nq > 0) np.open = 1;
        snd(SND_SELECT);
        break;
    case A_PLAYVIDEO: {
        /* the videos in this folder form the playlist, starting from the chosen one */
        Menu *m = stack[depth];
        const char **paths = xcalloc((size_t)m->n + 2, sizeof(char *));
        int n = 0, start = 0;
        for (int i = 0; i < m->n; i++)
            if (m->items[i].action == A_PLAYVIDEO && m->items[i].data) {
                if (!strcmp(m->items[i].data, it->data)) start = n;
                paths[n++] = m->items[i].data;
            }
        if (n == 0) paths[n++] = it->data;
        if (mp.active && !mp.paused) music_toggle();
        if (video_open_list_ok(paths, n, start) != 0) video_launch_external(it->data, it->title);
        free(paths);
        break;
    }
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
        net_changed(0);
        dlg.kind = D_NONE;
    } else if (action == A_UPDATE_PIXI) {
        pixi_update_start();
    } else if (action == A_UPDATE) {
        update_start();
    } else if (action == A_WIFI_FORGET) {
        show_busy("Wi-Fi", "Forgetting\xE2\x80\xA6");
        wifi_forget(ctx);
        if (stack[depth]->build == build_wifi) menu_rebuild(stack[depth]);
        net_changed(0);
        dlg.kind = D_NONE;
        snd(SND_SELECT);
    } else if (action == A_BR_INSTALL) {
        package_job(1, "chromium", "Chromium");
    } else if (action == A_WEB_INSTALL) {
        package_job(1, "nginx php-fpm php-cli php-sqlite3 php-curl php-mbstring php-xml", "Web server");
    } else if (action == A_WEB_RESET) {
        web_reset_now();
    } else if (action == A_ED_DISCARD) {
        ed_close();
    } else if (action == A_STORE_REMOVE) {
        store_remove_now(ctx);
    } else if (action == A_PIAPPS_INSTALL) {
        piapps_install_start();
    } else if (action == A_DELETE) {
        file_delete_now(ctx);
    } else if (action == A_LOGOUT) power_action(0);
    else if (action == A_REBOOT) power_action(1);
    else if (action == A_SHUTDOWN) power_action(2);
}

static void osk_type(const char *s)
{
    size_t l = strlen(dlg.buf);
    if (l + strlen(s) < sizeof dlg.buf - 1) strcat(dlg.buf, s);
    snd(SND_KEY);
    osk_after_edit();
}

static void osk_backspace(void)
{
    size_t l = strlen(dlg.buf);
    while (l > 0) {
        l--;
        if (((unsigned char)dlg.buf[l] & 0xC0) != 0x80) break;
    }
    dlg.buf[l] = 0;
    osk_after_edit();
}

/* ---------- passwords for sudo, typed on screen ---------------------------
 * sudo can't read a password from an app started by Pixi (there's no terminal), so it runs a helper
 * instead: SUDO_ASKPASS, and "Path askpass" in /etc/sudo.conf for apps that call sudo themselves. The
 * helper is this program run as "pixi-shell --askpass": it asks the running shell over a socket, the
 * shell shows its keyboard, and the password goes back the same way. Nothing is written to disk. */

static struct {
    int lfd, fd;       /* the listening socket, and the helper waiting for an answer */
    int over_app;      /* an app was in front: the shell came over it, and goes back after */
    int last_pid;      /* the sudo that asked last, to tell a wrong password from a new request */
    double last_at;
    int has_saved;     /* an install dialog was on screen: it comes back once the password is answered */
    int cached;        /* a password kept in memory while that install runs, so each sudo in it needn't ask again */
    char cache[640];
} ask = { -1, -1, 0, 0, 0, 0, 0, "" };
static __typeof__(dlg) ask_saved;

static void askpass_paths(char *sock, size_t scap, char *helper, size_t hcap)
{
    char dir[64];
    struct stat st;
    snprintf(dir, sizeof dir, "/run/user/%u", (unsigned)getuid());
    if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(sock, scap, "%s/pixi-askpass.sock", dir);
        if (helper) snprintf(helper, hcap, "%s/pixi-askpass", dir);
    } else {
        snprintf(sock, scap, "/tmp/pixi-askpass-%u.sock", (unsigned)getuid());
        if (helper) snprintf(helper, hcap, "/tmp/pixi-askpass-%u", (unsigned)getuid());
    }
}

/* at start: listen for the helper, and point sudo at it */
static void askpass_listen(void)
{
    char sock[200], helper[200];
    askpass_paths(sock, sizeof sock, helper, sizeof helper);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    sncpy(a.sun_path, sizeof a.sun_path, sock);
    unlink(sock);
    mode_t old = umask(077);
    int ok = bind(fd, (struct sockaddr *)&a, sizeof a) == 0 && listen(fd, 4) == 0;
    umask(old);
    if (!ok) {
        close(fd);
        return;
    }
    ask.lfd = fd;
    /* a helper that runs this very program, so it works wherever Pixi was installed or built */
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        FILE *f = fopen(helper, "w");
        if (f) {
            fprintf(f, "#!/bin/sh\nexec '%s' --askpass \"$@\"\n", exe);
            fclose(f);
            chmod(helper, 0700);
            setenv("SUDO_ASKPASS", helper, 1);
        }
    }
}

/* the answer: the password, or NULL for "cancelled" */
static void askpass_answer(const char *pw)
{
    if (ask.fd >= 0) {
        char buf[600];
        int n = pw ? snprintf(buf, sizeof buf, "OK %s\n", pw) : snprintf(buf, sizeof buf, "NO\n");
        if (n > 0 && write(ask.fd, buf, (size_t)n) < 0) {}
        explicit_bzero(buf, sizeof buf);
        close(ask.fd);
        ask.fd = -1;
    }
    if (ask.over_app) comp_set_mode(MODE_APP); /* back to the app that asked */
    ask.over_app = 0;
    if (ask.has_saved) {
        if (dlg.kind == D_NONE) dlg = ask_saved; /* the install dialog is back */
        ask.has_saved = 0;
    }
}

static void osk_open_for(int action, const char *title, const char *ctx, const char *initial);
static void pm_release_all_fwd(void);
static void appkb_tick(void);

/* the answer to a yad question goes back to the waiting program, and what was on screen before comes back */
static void yad_finish(int code)
{
    if (yd.fd >= 0) {
        char buf[600];
        size_t o = (size_t)snprintf(buf, sizeof buf, "OK %d\n", code);
        for (int i = 0; i < yd.n && o + 16 < sizeof buf; i++)
            if (!yd.it[i].button && yd.it[i].check) o += (size_t)snprintf(buf + o, sizeof buf - o, "SEL %d\n", i);
        if (code >= 0 && write(yd.fd, buf, o) < 0) {}
        close(yd.fd);
        yd.fd = -1;
    }
    if (dlg.kind == D_CHECK) dlg.kind = D_NONE;
    if (ask.over_app) comp_set_mode(MODE_APP);
    ask.over_app = 0;
    if (ask.has_saved) {
        if (dlg.kind == D_NONE) dlg = ask_saved;
        ask.has_saved = 0;
    }
}

static void yad_action(int a)
{
    int n = yd.n;
    if (n <= 0) {
        yad_finish(1);
        return;
    }
    if (a == ACT_UP || a == ACT_DOWN) {
        yd.sel = (yd.sel + (a == ACT_UP ? n - 1 : 1)) % n;
        snd(SND_STEP);
    } else if (a == ACT_OK) {
        if (yd.it[yd.sel].button) {
            snd(SND_SELECT);
            yad_finish(yd.it[yd.sel].code);
        } else {
            int v = !yd.it[yd.sel].check;
            if (yd.radio) {
                for (int i = 0; i < n; i++)
                    if (!yd.it[i].button) yd.it[i].check = 0;
            }
            yd.it[yd.sel].check = v;
            snd(SND_STEP);
        }
    } else if (a == ACT_START && yd.list) {
        snd(SND_SELECT);
        yad_finish(yd.first);
    } else if (a == ACT_BACK) {
        snd(SND_BACK);
        yad_finish(1);
    }
}

/* a request from the yad stand-in: lines of "KIND", "TITLE", "TEXT", "ROW", "BTN" and "END" */
static void yad_open(int fd, char *req)
{
    int in_job = dlg.kind == D_UPDATE && upd.active && !upd.done;
    if (dlg.kind != D_NONE && !in_job) {
        if (write(fd, "NO\n", 3) < 0) {}
        close(fd);
        return;
    }
    memset(&yd, 0, sizeof yd);
    yd.fd = fd;
    char title[256] = "";
    int nbtn = 0;
    for (char *line = strtok(req, "\n"); line; line = strtok(NULL, "\n")) {
        char *f1 = strchr(line, '\t');
        if (f1) *f1++ = 0;
        if (!strcmp(line, "KIND") && f1) yd.list = !strcmp(f1, "list");
        else if (!strcmp(line, "RADIO") && f1) yd.radio = atoi(f1);
        else if (!strcmp(line, "TITLE") && f1) sncpy(title, sizeof title, f1);
        else if (!strcmp(line, "TEXT") && f1) {
            sncpy(yd.text, sizeof yd.text, f1);
            for (char *c = yd.text; *c; c++)
                if (*c == '\x1e') *c = ' ';
        } else if (!strcmp(line, "ROW") && f1 && yd.n < YD_MAX - 4) {
            char *lab = strchr(f1, '\t');
            if (!lab) continue;
            *lab++ = 0;
            sncpy(yd.it[yd.n].label, sizeof yd.it[yd.n].label, lab);
            yd.it[yd.n].check = atoi(f1);
            yd.n++;
        } else if (!strcmp(line, "BTN") && f1 && yd.n < YD_MAX) {
            char *code = strchr(f1, '\t');
            if (!code) continue;
            *code++ = 0;
            sncpy(yd.it[yd.n].label, sizeof yd.it[yd.n].label, f1);
            yd.it[yd.n].button = 1;
            yd.it[yd.n].code = atoi(code);
            if (!nbtn++) yd.first = yd.it[yd.n].code;
            yd.n++;
        }
    }
    if (yd.n == 0 || (yd.n && !nbtn)) { /* always something to press */
        if (yd.n < YD_MAX) {
            sncpy(yd.it[yd.n].label, sizeof yd.it[yd.n].label, "OK");
            yd.it[yd.n].button = 1;
            yd.n++;
        }
    }
    if (in_job) {
        ask_saved = dlg;
        ask.has_saved = 1;
    }
    if (guide.open) guide_close(1);
    if (comp_mode == MODE_APP) {
        ask.over_app = 1;
        comp_set_mode(MODE_OVERLAY);
    }
    memset(&dlg, 0, sizeof dlg);
    dlg.kind = D_CHECK;
    sncpy(dlg.title, sizeof dlg.title, title[0] ? title : "Pi-Apps");
    yd.sel = 0;
    for (int i = 0; i < yd.n; i++)
        if (!yd.it[i].button) {
            yd.sel = i;
            break;
        }
    snd(SND_GUIDE_OPEN);
}

/* once a frame: a new request, or a password box that was closed some other way */
static void askpass_tick(void)
{
    appkb_tick();
    if (yd.fd >= 0) { /* a question is up: wait for its answer; if its dialog went some other way, cancel it */
        if (dlg.kind != D_CHECK) yad_finish(1);
        return;
    }
    if (ask.fd >= 0) {
        if (!(dlg.kind == D_OSK && dlg.action == A_SUDO_PASS)) askpass_answer(NULL);
        return;
    }
    if (ask.cached && (!upd.active || upd.done)) { /* the install is over: forget the password */
        explicit_bzero(ask.cache, sizeof ask.cache);
        ask.cached = 0;
    }
    if (ask.lfd < 0) return;
    int fd = accept4(ask.lfd, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) return;
    /* the helper writes "pid<TAB>who<TAB>prompt" straight away */
    struct timeval tv = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char req[8192];
    size_t got = 0;
    while (got + 1 < sizeof req) {
        ssize_t r = read(fd, req + got, sizeof req - 1 - got);
        if (r <= 0) break;
        got += (size_t)r;
        if (memchr(req, '\n', got) && (strncmp(req, "YAD\n", 4) || strstr(req, "\nEND\n"))) break;
    }
    req[got] = 0;
    if (!strncmp(req, "YAD\n", 4)) {
        yad_open(fd, req + 4);
        return;
    }
    char *nl = strchr(req, '\n');
    if (nl) *nl = 0;
    char *who = strchr(req, '\t');
    int in_job = dlg.kind == D_UPDATE && upd.active && !upd.done; /* an install is running: the box can go over it */
    if (!who || (dlg.kind != D_NONE && !in_job)) { /* garbled, or something else is on screen: say no */
        if (write(fd, "NO\n", 3) < 0) {}
        close(fd);
        return;
    }
    *who++ = 0;
    char *prompt = strchr(who, '\t');
    if (prompt) *prompt++ = 0;
    int pid = atoi(req);
    int again = pid && pid == ask.last_pid && now_s - ask.last_at < 120;
    if (ask.cached && in_job && !again) { /* a later sudo in the same install: answer with the password already typed */
        char buf[700];
        int n = snprintf(buf, sizeof buf, "OK %s\n", ask.cache);
        if (n > 0 && write(fd, buf, (size_t)n) < 0) {}
        explicit_bzero(buf, sizeof buf);
        close(fd);
        ask.last_pid = pid;
        ask.last_at = now_s;
        return;
    }
    if (again && ask.cached) { /* it was refused: the kept password is wrong */
        explicit_bzero(ask.cache, sizeof ask.cache);
        ask.cached = 0;
    }
    ask.last_pid = pid;
    ask.last_at = now_s;
    ask.fd = fd;
    if (in_job) {
        ask_saved = dlg;
        ask.has_saved = 1;
        memset(&dlg, 0, sizeof dlg);
    }
    if (guide.open) guide_close(1);
    if (comp_mode == MODE_APP) { /* the app is in front: come over it for the password */
        ask.over_app = 1;
        comp_set_mode(MODE_OVERLAY);
    }
    char title[200];
    if (again) snprintf(title, sizeof title, "Wrong password. Try again for %s (sudo)", *who ? who : "this app");
    else snprintf(title, sizeof title, "Password for %s (sudo)", *who ? who : "administrator access");
    osk_open_for(A_SUDO_PASS, title, "", "");
    snd(SND_GUIDE_OPEN);
}


/* ---------- "yad", for Pi-Apps ---------------------------------------------
 * Pi-Apps' installers ask their questions with yad windows, and a window like that can't be reached with the
 * gamepad. Inside a Pi-Apps job "yad" is this program (a link to it, see piapps_shim), which asks the running
 * shell to show the same question in Pixi's own dialog and prints the answer the way yad would. */

static void yad_plain(char *s)
{ /* the text of a label: no markup, no tabs or line breaks */
    char *o = s;
    for (char *c = s; *c;) {
        if (*c == '<') {
            while (*c && *c != '>') c++;
            if (*c) c++;
        } else if (!strncmp(c, "&amp;", 5)) *o++ = '&', c += 5;
        else if (!strncmp(c, "&lt;", 4)) *o++ = '<', c += 4;
        else if (!strncmp(c, "&gt;", 4)) *o++ = '>', c += 4;
        else if (*c == '\t') *o++ = ' ', c++;
        else if (*c == '\n') *o++ = '\x1e', c++;
        else *o++ = *c++;
    }
    *o = 0;
}

static int yad_client(int argc, char **argv)
{
    static const char *with_value[] = { "class", "name", "title", "text", "width", "height", "window-icon", "separator", "column", "print-column",
                                        "button", "borders", "timeout", "image", "geometry", "text-align", "icon", "field", "posx", "posy",
                                        "css", "image-path", "icon-theme", "hide-column", "search-column", "expand-column", "tooltip-column",
                                        "wrap-width", "wrap-cols", "ellipsize", "listen", "filename", "parent-win", "timeout-indicator", NULL };
    char title[200] = "", text[600] = "", sep[16] = "|";
    int list = 0, radio = 0, progress = 0, other = 0, textinfo = 0, printcol = 0, ncols = 0, chkcol = -1, nbtn = 0;
    int colkind[16] = { 0 }; /* 1 checkbox, 2 image */
    char btn[6][64];
    int btncode[6];
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (strncmp(a, "--", 2)) continue;
        char *name = a + 2, *val = strchr(name, '=');
        char nm[48];
        size_t nl = val ? (size_t)(val - name) : strlen(name);
        if (nl >= sizeof nm) continue;
        memcpy(nm, name, nl);
        nm[nl] = 0;
        if (val) val++;
        else {
            for (int k = 0; with_value[k]; k++)
                if (!strcmp(nm, with_value[k]) && i + 1 < argc) {
                    val = argv[++i];
                    break;
                }
        }
        if (!strcmp(nm, "title") && val) sncpy(title, sizeof title, val);
        else if (!strcmp(nm, "text") && val) sncpy(text, sizeof text, val);
        else if (!strcmp(nm, "separator") && val) sncpy(sep, sizeof sep, val);
        else if (!strcmp(nm, "print-column") && val) printcol = atoi(val);
        else if (!strcmp(nm, "list")) list = 1;
        else if (!strcmp(nm, "checklist")) chkcol = 0;
        else if (!strcmp(nm, "radiolist")) radio = 1, chkcol = 0;
        else if (!strcmp(nm, "text-info")) textinfo = 1;
        else if (!strcmp(nm, "progress") || !strcmp(nm, "notification")) progress = 1;
        else if (!strcmp(nm, "entry") || !strcmp(nm, "form") || !strcmp(nm, "file") || !strcmp(nm, "color") || !strcmp(nm, "scale") ||
                 !strcmp(nm, "calendar") || !strcmp(nm, "font") || !strcmp(nm, "dnd") || !strcmp(nm, "icons") || !strcmp(nm, "paned") ||
                 !strcmp(nm, "print") || !strcmp(nm, "multi-progress")) other = 1;
        else if (!strcmp(nm, "column") && val && ncols < 16) {
            const char *colon = strrchr(val, ':');
            if (colon && !strcmp(colon + 1, "CHK")) colkind[ncols] = 1;
            else if (colon && !strcmp(colon + 1, "IMG")) colkind[ncols] = 2;
            ncols++;
        } else if (!strcmp(nm, "button") && val && nbtn < 6) {
            const char *colon = strrchr(val, ':');
            size_t ll = colon ? (size_t)(colon - val) : strlen(val);
            if (ll >= sizeof btn[0]) ll = sizeof btn[0] - 1;
            memcpy(btn[nbtn], val, ll);
            btn[nbtn][ll] = 0;
            btncode[nbtn] = colon ? atoi(colon + 1) : nbtn;
            nbtn++;
        }
    }
    if (progress) { /* nothing to show: Pixi's own progress dialog does that */
        char junk[512];
        while (!isatty(0) && read(0, junk, sizeof junk) > 0) {}
        return 0;
    }
    if (other) return 1;
    for (int k = 0; k < ncols; k++)
        if (colkind[k] == 1 && chkcol < 0) chkcol = k;
    char input[16384] = "";
    size_t got = 0;
    if (!isatty(0)) {
        for (;;) {
            ssize_t r = read(0, input + got, sizeof input - 1 - got);
            if (r <= 0) break;
            got += (size_t)r;
            if (got + 1 >= sizeof input) break;
        }
        input[got] = 0;
    }
    char sock[200];
    askpass_paths(sock, sizeof sock, NULL, 0);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) return 1;
    yad_plain(title);
    yad_plain(text);
    char *req = malloc(24000);
    if (!req) return 1;
    size_t o = (size_t)snprintf(req, 24000, "YAD\nKIND\t%s\nRADIO\t%d\nTITLE\t%s\n", list ? "list" : "msg", radio, title);
    char *rows[YD_MAX][16];
    int nrows = 0;
    char *fields[400];
    int nf = 0;
    if (list && ncols > 0) {
        for (char *l = strtok(input, "\n"); l && nf < 400; l = strtok(NULL, "\n")) fields[nf++] = l;
        if (textinfo) nf = 0;
        for (int r = 0; r + ncols <= nf && nrows < YD_MAX - 4; r += ncols) {
            for (int k = 0; k < ncols; k++) rows[nrows][k] = fields[r + k];
            nrows++;
        }
        for (int r = 0; r < nrows; r++) {
            char lab[200] = "";
            for (int k = 0; k < ncols; k++)
                if (colkind[k] == 0) {
                    sncpy(lab, sizeof lab, rows[r][k]);
                    break;
                }
            yad_plain(lab);
            int on = chkcol >= 0 && !strcmp(rows[r][chkcol], "TRUE");
            o += (size_t)snprintf(req + o, 24000 - o, "ROW\t%d\t%.110s\n", on, lab);
        }
    } else if (textinfo) { /* the first few lines of what it shows */
        char shown[600] = "";
        int lines = 0;
        for (char *l = strtok(input, "\n"); l && lines < 6; l = strtok(NULL, "\n"), lines++) {
            char one[200];
            sncpy(one, sizeof one, l);
            yad_plain(one);
            if (strlen(shown) + strlen(one) + 3 < sizeof shown) snprintf(shown + strlen(shown), sizeof shown - strlen(shown), "%s%s", lines ? " " : "", one);
        }
        if (shown[0]) sncpy(text, sizeof text, shown);
    }
    o += (size_t)snprintf(req + o, 24000 - o, "TEXT\t%.590s\n", text);
    if (nbtn == 0) snprintf(btn[0], sizeof btn[0], "OK"), btncode[0] = 0, nbtn = 1;
    for (int k = 0; k < nbtn; k++) {
        char lab[64];
        const char *b = btn[k];
        if (!strncmp(b, "gtk-", 4)) b += 4;
        snprintf(lab, sizeof lab, "%s", b);
        if (lab[0] >= 'a' && lab[0] <= 'z') lab[0] = (char)(lab[0] - 32);
        yad_plain(lab);
        o += (size_t)snprintf(req + o, 24000 - o, "BTN\t%s\t%d\n", lab, btncode[k]);
    }
    o += (size_t)snprintf(req + o, 24000 - o, "END\n");
    if (write(fd, req, o) < 0) return 1;
    free(req);
    char reply[800];
    size_t rg = 0;
    for (;;) {
        ssize_t r = read(fd, reply + rg, sizeof reply - 1 - rg);
        if (r <= 0) break;
        rg += (size_t)r;
        if (rg + 1 >= sizeof reply) break;
    }
    close(fd);
    reply[rg] = 0;
    if (strncmp(reply, "OK ", 3)) return 1;
    int code = atoi(reply + 3);
    if (code != 0 || !list) return code;
    const char *eol = !strcmp(sep, "\\n") ? "\n" : !strcmp(sep, "\\t") ? "\t" : sep;
    for (char *l = strstr(reply, "SEL "); l; l = strstr(l + 4, "SEL ")) {
        int idx = atoi(l + 4);
        if (idx < 0 || idx >= nrows) continue;
        if (printcol > 0 && printcol <= ncols) printf("%s%s", rows[idx][printcol - 1], eol);
        else
            for (int k = 0; k < ncols; k++) printf("%s%s", rows[idx][k], eol);
    }
    return 0;
}

/* "pixi-shell --askpass [prompt]": run by sudo. Prints the password, or fails if there's none. */
static int askpass_client(const char *prompt)
{
    char sock[200];
    askpass_paths(sock, sizeof sock, NULL, 0);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    sncpy(a.sun_path, sizeof a.sun_path, sock);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "pixi-askpass: Pixi isn't running, so there's nowhere to ask for the password\n");
        return 1;
    }
    /* who's asking: the command sudo was given, from its command line */
    pid_t pp = getppid();
    char who[128] = "", path[64], cmd[2048];
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)pp);
    FILE *f = fopen(path, "rb");
    size_t n = f ? fread(cmd, 1, sizeof cmd - 1, f) : 0;
    if (f) fclose(f);
    cmd[n] = 0;
    static const char *with_value[] = { "-u", "-g", "-C", "-D", "-h", "-p", "-r", "-t", "-U", "-T", NULL };
    int skip_next = 0, first = 1;
    for (size_t i = 0; i < n;) {
        const char *tok = cmd + i;
        i += strlen(tok) + 1;
        if (first) { first = 0; continue; } /* sudo itself */
        if (skip_next) { skip_next = 0; continue; }
        if (!strcmp(tok, "--")) continue;
        if (tok[0] == '-') {
            for (int k = 0; with_value[k]; k++)
                if (!strcmp(tok, with_value[k])) skip_next = 1;
            continue;
        }
        const char *b = strrchr(tok, '/');
        sncpy(who, sizeof who, b ? b + 1 : tok);
        break;
    }
    for (char *p = who; *p; p++)
        if (*p == '\t' || *p == '\n') *p = ' ';
    char req[400];
    int len = snprintf(req, sizeof req, "%d\t%s\t%s\n", (int)pp, who, prompt ? prompt : "");
    if (len <= 0 || write(fd, req, (size_t)len) < 0) return 1;
    char reply[600];
    size_t got = 0;
    for (;;) {
        ssize_t r = read(fd, reply + got, sizeof reply - 1 - got);
        if (r <= 0) break;
        got += (size_t)r;
        if (got + 1 >= sizeof reply) break;
    }
    close(fd);
    reply[got] = 0;
    if (strncmp(reply, "OK ", 3)) return 1; /* cancelled */
    fputs(reply + 3, stdout);
    explicit_bzero(reply, sizeof reply);
    return 0;
}

/* ---------- typing into an app with the on-screen keyboard -----------------
 * In Keyboard + Mouse mode, Triangle brings Pixi's keyboard over the app. What is typed there is sent to the
 * app, key by key, when Done is pressed, so a sign-in form can be filled in. */
static struct { int over, has_saved; } appkb;
static __typeof__(dlg) appkb_saved;

static int appkb_key(unsigned char c, unsigned *mods)
{
    static const char *rows[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
    static const unsigned char base[3] = { 16, 30, 44 };
    static const char *shifted = "!@#$%^&*()";
    *mods = 0;
    if (c >= 'a' && c <= 'z') {
        for (int r = 0; r < 3; r++)
            for (int i = 0; rows[r][i]; i++)
                if (rows[r][i] == (char)c) return base[r] + i;
    }
    if (c >= 'A' && c <= 'Z') {
        *mods = 1;
        return appkb_key((unsigned char)(c + 32), &(unsigned){ 0 });
    }
    if (c >= '1' && c <= '9') return 2 + (c - '1');
    if (c == '0') return 11;
    const char *sh = strchr(shifted, (char)c);
    if (sh && c) {
        *mods = 1;
        return sh - shifted + 2 > 10 ? 11 : (int)(sh - shifted) + 2;
    }
    static const struct { char plain, shift; unsigned char code; } punct[] = {
        { '-', '_', 12 }, { '=', '+', 13 }, { '[', '{', 26 }, { ']', '}', 27 }, { ';', ':', 39 }, { '\'', '"', 40 }, { '`', '~', 41 },
        { '\\', '|', 43 }, { ',', '<', 51 }, { '.', '>', 52 }, { '/', '?', 53 },
    };
    for (size_t i = 0; i < sizeof punct / sizeof *punct; i++) {
        if (c == (unsigned char)punct[i].plain) return punct[i].code;
        if (c == (unsigned char)punct[i].shift) {
            *mods = 1;
            return punct[i].code;
        }
    }
    if (c == ' ') return 57;
    if (c == '\t') return 15;
    if (c == '\n') return 28;
    return 0;
}

static void appkb_send(const char *text)
{
    if (!pixi || pixi_ver < 2) return;
    for (const unsigned char *c = (const unsigned char *)text; *c; c++) {
        unsigned mods;
        int code = appkb_key(*c, &mods);
        if (!code) continue; /* only what a US keyboard has */
        pixi_shell_v1_key(pixi, (uint32_t)code, mods, 1);
        pixi_shell_v1_key(pixi, (uint32_t)code, mods, 0);
    }
    wl_display_flush(wl_dpy);
}

/* The keyboard over an app is a real one: each key goes down when it is pressed and up when it is let go,
 * so holding repeats and shortcuts work, and nothing is collected in a box first. */
static struct { int code, down; unsigned mods; } live;

static void live_up(void)
{
    if (live.down && pixi && pixi_ver >= 2) {
        pixi_shell_v1_key(pixi, (uint32_t)live.code, live.mods, 0);
        wl_display_flush(wl_dpy);
    }
    live.down = 0;
}

static void live_tap(int code, unsigned mods)
{
    if (!pixi || pixi_ver < 2) return;
    pixi_shell_v1_key(pixi, (uint32_t)code, mods, 1);
    pixi_shell_v1_key(pixi, (uint32_t)code, mods, 0);
    wl_display_flush(wl_dpy);
}

static void live_press(const PKey *k, const PKOut *o)
{
    int code = 0;
    unsigned mods = 0;
    if (k->type == KY_CHAR && o->fkey) code = o->fkey <= 10 ? 58 + o->fkey : o->fkey == 11 ? 87 : 88;
    else if (k->type == KY_CHAR) {
        unsigned unused;
        code = appkb_key((unsigned char)k->base[0], &unused);
        int letter = !k->shifted;
        int shift = letter ? ((o->shift != 0) != (pk.caps != 0)) : o->shift != 0;
        if (shift) mods |= 1;
    } else {
        switch (k->type) {
        case KY_TAB: code = 15; break;
        case KY_ENTER: code = 28; break;
        case KY_SPACE: code = 57; break;
        case KY_BKSP: code = 14; break;
        case KY_ESC: code = 1; break;
        case KY_UP: code = 103; break;
        case KY_DOWN: code = 108; break;
        case KY_LEFT: code = 105; break;
        case KY_RIGHT: code = 106; break;
        case KY_DEL: code = o->shift ? 110 : 111; break;
        case KY_PGUP: code = o->shift ? 109 : 104; break;
        case KY_HOME: code = o->shift ? 107 : 102; break;
        case KY_SUPER: code = 125; break;
        default: break;
        }
        if (k->type != KY_DEL && k->type != KY_PGUP && k->type != KY_HOME && o->shift) mods |= 1;
    }
    if (!code) return;
    if (o->ctrl) mods |= 4;
    if (o->alt) mods |= 8;
    if (!pixi || pixi_ver < 2) return;
    live.code = code;
    live.mods = mods;
    live.down = 1;
    pixi_shell_v1_key(pixi, (uint32_t)code, mods, 1);
    wl_display_flush(wl_dpy);
}

static void live_action(int a)
{
    switch (a) {
    case ACT_UP: case ACT_DOWN: case ACT_LEFT: case ACT_RIGHT: pk_nav(a, &dlg.r, &dlg.c); break;
    case ACT_OK:
        if (!live.down) { /* held keys are one press, the app repeats them itself */
            const PKey *k = pk_at(dlg.r, dlg.c);
            PKOut o;
            if (k && pk_press(k, &o)) {
                live_press(k, &o);
                snd(SND_KEY);
            }
        }
        break;
    case ACT_MENU:                                                     /* Triangle: Backspace; R + Triangle: put the keyboard away */
        if (tri_with_r) {
            tri_with_r = 0;
            live_up();
            SDL_StopTextInput();
            dlg.kind = D_NONE;
            snd(SND_BACK);
            break;
        }
        live_tap(14, 0);
        snd(SND_KEY);
        break;
    case ACT_SQUARE: case ACT_PGUP: live_tap(57, 0); snd(SND_KEY); break; /* Square: Space */
    case ACT_START: live_tap(28, 0); snd(SND_KEY); break;              /* Start: Enter */
    case ACT_BACK:                                                     /* B: put the keyboard away */
        live_up();
        SDL_StopTextInput();
        dlg.kind = D_NONE;
        snd(SND_BACK);
        break;
    default: break;
    }
}

static void appkb_open(void)
{
    if (dlg.kind == D_OSK) return; /* a keyboard is already up (a password box, say) */
    if (appkb.over) return;
    appkb.has_saved = 0;
    if (dlg.kind != D_NONE) { /* a page from before (a finished install, say) comes back afterwards */
        appkb_saved = dlg;
        appkb.has_saved = 1;
    }
    pm_release_all_fwd();
    comp_set_mode(MODE_OVERLAY);
    appkb.over = 1;
    scene_transparent = 1;
    osk_open_for(A_APP_TYPE, "Keyboard", "", "");
    SDL_StopTextInput(); /* it is not typing into a box */
    snd(SND_GUIDE_OPEN);
}

/* once a frame: the keyboard was closed without typing: back to the app */
static void appkb_tick(void)
{
    if (appkb.over && live.down && !(dlg.kind == D_OSK && dlg.action == A_APP_TYPE)) live_up();
    if (appkb.over && !(dlg.kind == D_OSK && dlg.action == A_APP_TYPE)) {
        appkb.over = 0;
        scene_transparent = 0;
        comp_set_mode(MODE_APP);
        if (appkb.has_saved && dlg.kind == D_NONE) dlg = appkb_saved;
        appkb.has_saved = 0;
    }
}

static void osk_done(void)
{
    if (dlg.action == A_APP_TYPE) {
        char t[sizeof dlg.buf];
        sncpy(t, sizeof t, dlg.buf);
        SDL_StopTextInput();
        memset(&dlg, 0, sizeof dlg);
        appkb.over = 0;
        scene_transparent = 0;
        comp_set_mode(MODE_APP);
        if (appkb.has_saved) dlg = appkb_saved;
        appkb.has_saved = 0;
        appkb_send(t);
        snd(SND_SELECT);
        return;
    }
    if (dlg.action == A_SUDO_PASS) {
        char pw[sizeof dlg.buf];
        sncpy(pw, sizeof pw, dlg.buf);
        SDL_StopTextInput();
        memset(&dlg, 0, sizeof dlg);
        if (ask.has_saved && pw[0]) { /* typed during an install: keep it until the install ends */
            sncpy(ask.cache, sizeof ask.cache, pw);
            ask.cached = 1;
        }
        askpass_answer(pw);
        explicit_bzero(pw, sizeof pw);
        snd(SND_SELECT);
        return;
    }
    if (dlg.action == A_STORE_SEARCH) {
        char q[96];
        sncpy(q, sizeof q, dlg.buf);
        SDL_StopTextInput();
        memset(&dlg, 0, sizeof dlg);
        store_search_done(q);
        return;
    }
    if (dlg.action == A_NET_SSID) { /* Enter Manually: the name, then its password */
        char ssid[128], t[260];
        sncpy(ssid, sizeof ssid, trim(dlg.buf));
        SDL_StopTextInput();
        memset(&dlg, 0, sizeof dlg);
        if (!ssid[0]) return;
        snprintf(t, sizeof t, "Password for %s (leave it empty if there's none)", ssid);
        osk_open_for(A_NET_HIDPW, t, ssid, "");
        return;
    }
    if (dlg.action == A_NET_HIDPW) {
        char ssid[256], pw[128], title[300], err[600];
        sncpy(ssid, sizeof ssid, dlg.ctx);
        sncpy(pw, sizeof pw, dlg.buf);
        SDL_StopTextInput();
        memset(&dlg, 0, sizeof dlg);
        if (pw[0] && strlen(pw) < 8) {
            show_message("Password too short", "Wi-Fi passwords are at least 8 characters.");
            snd(SND_ERROR);
            return;
        }
        snprintf(title, sizeof title, "Connecting to %s", ssid);
        show_busy(title, "This can take up to 30 seconds.");
        int rc = wifi_connect_hidden(ssid, pw, err, sizeof err);
        explicit_bzero(pw, sizeof pw);
        net_changed(rc == 0);
        if (rc == 0) {
            snprintf(title, sizeof title, "Connected to %s", ssid);
            show_message(title, "");
            snd(SND_SELECT);
        } else {
            show_message("Couldn't connect", err);
            snd(SND_ERROR);
        }
        return;
    }
    if (dlg.action == A_WIFI_EDIT) {
        char ssid[256], pw[128], err[300];
        sncpy(ssid, sizeof ssid, dlg.ctx);
        sncpy(pw, sizeof pw, dlg.buf);
        SDL_StopTextInput();
        memset(&dlg, 0, sizeof dlg);
        if (strlen(pw) < 8) {
            show_message("Password too short", "Wi-Fi passwords are at least 8 characters.");
            snd(SND_ERROR);
            return;
        }
        if (wifi_set_password(ssid, pw, err, sizeof err) != 0) {
            show_message("Couldn't change the password", err);
            snd(SND_ERROR);
            return;
        }
        wifi_join(ssid, NULL, 1);
        return;
    }
    if (dlg.action == A_BR_ADDR || dlg.action == A_BR_FIELD) {
        int addr = dlg.action == A_BR_ADDR, enter = osk_start_pressed;
        char txt[640];
        sncpy(txt, sizeof txt, dlg.buf);
        SDL_StopTextInput();
        memset(&dlg, 0, sizeof dlg);
        if (addr) browser_address_done(txt);
        else browser_field_done(txt, enter);
        return;
    }
    if (dlg.action == A_FILE_RENAME || dlg.action == A_FILE_MKDIR) {
        int rename_it = dlg.action == A_FILE_RENAME;
        char ctx[256], name[128];
        sncpy(ctx, sizeof ctx, dlg.ctx);
        sncpy(name, sizeof name, dlg.buf);
        SDL_StopTextInput();
        memset(&dlg, 0, sizeof dlg);
        if (rename_it) file_rename_done(ctx, name);
        else file_mkdir_done(ctx, name);
        return;
    }
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


/* ======================================================================== */
/* The side menu: a list on the right, like the PSP's and like a right-click */
/* ======================================================================== */

enum { SK_ACCENT = 1, SK_FILES, SK_ITEM, SK_POWER, SK_CHOICE };
enum { SMA_TOGGLE, SMA_ACTION, SMA_WALLPAPER, SMA_WALLPICK, SMA_NET_CONNECT, SMA_NET_INFO, SMA_APPSET, SMA_OPEN, SMA_COPY, SMA_CUT, SMA_PASTE, SMA_RENAME, SMA_DUP, SMA_MKDIR, SMA_DELETE, SMA_THEME, SMA_DETAILS, SMA_UNINSTALL, SMA_LAUNCH, SMA_POWER, SMA_CHOICE, SMA_WIFI_EDIT, SMA_WIFI_FORGET, SMA_APPMODE };

typedef struct {
    char label[72];
    const char *glyph;
    int act, arg, enabled, divider, check;
    int toggle; /* a switch instead of a tick: 0 none, 1 off, 2 on */
    int box;    /* a check box instead of a radio button */
    unsigned swatch;
} SideItem;

static struct {
    int open, kind, n, sel;
    float anim, spos;
    int setting;
    char title[96], path[512], dir[512], old_theme[24], pkg[96], detail[600], appid[160];
    SideItem it[32];
} side;

static struct {
    char path[512];
    int cut;
} clip;

static int side_is_open(void) { return side.open; }

static SideItem *side_add(const char *label, const char *glyph_name, int act, int enabled)
{
    SideItem *s = &side.it[side.n++];
    memset(s, 0, sizeof *s);
    sncpy(s->label, sizeof s->label, label);
    s->glyph = glyph_name;
    s->act = act;
    s->enabled = enabled;
    return s;
}

static void side_divider(void) { side_add("", NULL, 0, 0)->divider = 1; }

/* ---------- light or dark for the Linux apps ---------- */

/* Sets gtk-application-prefer-dark-theme in a GTK settings.ini, keeping everything else in the file. */
static void gtk_ini_set(const char *sub, int dark)
{
    char dir[1100], path[1200];
    snprintf(dir, sizeof dir, "%s/.config/%s", home_dir(), sub);
    mkdir_p(dir);
    snprintf(path, sizeof path, "%s/settings.ini", dir);
    char *old = calloc(1, 65536);
    if (!old) return;
    FILE *f = fopen(path, "r");
    if (f) {
        size_t n = fread(old, 1, 65535, f);
        old[n] = 0;
        fclose(f);
    }
    char *out = calloc(1, 65536 + 128);
    if (out) {
        int in_settings = 0, done = 0;
        char key[96];
        snprintf(key, sizeof key, "gtk-application-prefer-dark-theme=%d\n", dark);
        char *save = NULL;
        for (char *ln = strtok_r(old, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
            if (!strncmp(ln, "gtk-application-prefer-dark-theme", 33)) continue;
            strcat(out, ln);
            strcat(out, "\n");
            if (!done && !strcmp(ln, "[Settings]")) {
                strcat(out, key);
                done = 1;
            }
            (void)in_settings;
        }
        if (!done) {
            strcat(out, "[Settings]\n");
            strcat(out, key);
        }
        FILE *w = fopen(path, "w");
        if (w) {
            fputs(out, w);
            fclose(w);
        }
        free(out);
    }
    free(old);
}

/* Applies Light or Dark Mode for the Linux apps: the GTK settings files and the desktop's color scheme setting
 * (which GTK 4, Chromium and the portal read). Apps started afterwards also get GTK_THEME (see launch_item). */
static void app_scheme_apply(void)
{
    if (!cfg.app_scheme) return;
    int dark = cfg.app_scheme == 2;
    gtk_ini_set("gtk-3.0", dark);
    gtk_ini_set("gtk-4.0", dark);
    if (system(dark ? "gsettings set org.gnome.desktop.interface color-scheme prefer-dark >/dev/null 2>&1 &"
                    : "gsettings set org.gnome.desktop.interface color-scheme prefer-light >/dev/null 2>&1 &") != 0) {}
}

static void side_reset(int kind, const char *title)
{
    memset(&side.it, 0, sizeof side.it);
    side.open = 1;
    side.kind = kind;
    side.n = 0;
    side.sel = 0;
    side.spos = 0;
    side.appid[0] = 0;
    sncpy(side.title, sizeof side.title, title);
    snd(SND_SELECT);
}

static void side_open_accent(void)
{
    side_reset(SK_ACCENT, "Layout");
    sncpy(side.old_theme, sizeof side.old_theme, cfg.theme);
    side_add("Choose Wallpaper ...", "setting-wallpaper", SMA_WALLPICK, 1)->arg = -1;
    side_divider();
    for (int i = 0; i < NTHEMES; i++) {
        SideItem *s = side_add(THEMES[i].label, NULL, SMA_THEME, 1);
        s->arg = i;
        s->swatch = THEMES[i].accent;
        if (!strcasecmp(THEMES[i].name, cfg.theme)) side.sel = side.n - 1;
    }
    side_divider();
    SideItem *lm = side_add("Light Mode for Apps", "setting-appearance", SMA_APPMODE, 1);
    lm->arg = 1;
    lm->check = cfg.app_scheme == 1;
    SideItem *dm = side_add("Dark Mode for Apps", "setting-appearance", SMA_APPMODE, 1);
    dm->arg = 2;
    dm->check = cfg.app_scheme == 2;
    side.spos = (float)side.sel;
}

/* a short list of the choices for one setting, with the current one ticked */
static void side_open_choice(int setting)
{
    static const char *titles[] = { 0 };
    (void)titles;
    const char *title = setting == A_CLOCK ? "Information Bar" : setting == A_DATEFMT ? "Date format" : setting == A_BATPCT ? "Battery percentage"
                        : setting == A_SOUNDS ? "Menu sounds" : setting == A_VIDEOOUT ? "Video out" : "Confirm button";
    side_reset(SK_CHOICE, title);
    side.setting = setting;
    int cur = 0;
    if (setting == A_CLOCK) { /* the battery switch, a line, the clock, a line, then the date (choices 10 and up) */
        SideItem *bt = side_add("Battery percentage", NULL, SMA_TOGGLE, 1);
        bt->arg = A_BATPCT;
        bt->toggle = cfg.battery_pct ? 2 : 1;
        side_divider();
        side_add("12 Hour", NULL, SMA_CHOICE, 1)->arg = 0;
        side_add("24 Hour", NULL, SMA_CHOICE, 1)->arg = 1;
        side_divider();
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        for (int i = 0; i < 4; i++) {
            char d[48];
            clock_date_text(d, sizeof d, &tm, i);
            side_add(d, NULL, SMA_CHOICE, 1)->arg = 10 + i;
        }
        side.it[cfg.clock24 ? 3 : 2].check = 1;
        side.it[5 + cfg.date_fmt].check = 1;
        side.sel = cfg.clock24 ? 3 : 2;
        side.spos = (float)side.sel;
        return;
    } else if (setting == A_DATEFMT) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        for (int i = 0; i < 4; i++) {
            char d[48];
            clock_date_text(d, sizeof d, &tm, i);
            side_add(d, NULL, SMA_CHOICE, 1)->arg = i;
        }
        cur = cfg.date_fmt;
    } else if (setting == A_BATPCT) {
        side_add("Shown", NULL, SMA_CHOICE, 1)->arg = 1;
        side_add("Hidden", NULL, SMA_CHOICE, 1)->arg = 0;
        cur = cfg.battery_pct ? 0 : 1;
    } else if (setting == A_SOUNDS) {
        side_add("On", NULL, SMA_CHOICE, 1)->arg = 1;
        side_add("Off", NULL, SMA_CHOICE, 1)->arg = 0;
        cur = cfg.sounds ? 0 : 1;
    } else if (setting == A_VIDEOOUT) {
        side_add("None", NULL, SMA_CHOICE, 1)->arg = 0;
        side_add("USB (UVC webcam)", NULL, SMA_CHOICE, 1)->arg = 1;
        cur = cfg.video_out ? 1 : 0;
    } else {
        side_add("Bottom button", "pad-south", SMA_CHOICE, 1)->arg = 0;
        side_add("Right button", "pad-east", SMA_CHOICE, 1)->arg = 1;
        cur = cfg.confirm_east;
    }
    side.sel = cur;
    side.spos = (float)cur;
    side.it[cur].check = 1;
}

static void wallpaper_set(const char *path)
{
    sncpy(cfg.wallpaper, sizeof cfg.wallpaper, path ? path : "");
    state_save();
    if (stack[depth]) menu_rebuild(stack[depth]);
    snd(SND_SELECT);
}

static void side_open_updates(void)
{
    side_reset(SK_POWER, "Update");
    side_add("Update OS", "setting-update-system", SMA_ACTION, 1)->arg = A_UPDATE;
    side_add("Update Pixi", "setting-update-apps", SMA_ACTION, 1)->arg = A_UPDATE_PIXI;
}

static void side_open_servers(void)
{
    side_reset(SK_POWER, "Servers");
    side_add(ftp_running() ? "FTP Server (on)" : "FTP Server", "setting-network", SMA_ACTION, 1)->arg = A_FTP;
    side_add(web_ui_running() ? "Web Server (on)" : "Web Server", "category-internet", SMA_ACTION, 1)->arg = A_WEB;
    side_divider();
    side_add("Edit Web Config", "rename", SMA_ACTION, 1)->arg = A_WEB_EDIT;
    side_add("Reload Web Server", "refresh", SMA_ACTION, web_ui_running())->arg = A_WEB_RELOAD;
    side_add("Restore Default Config", "setting-update-system", SMA_ACTION, 1)->arg = A_WEB_RESET;
    side_divider();
    SideItem *sh = side_add("SSH", NULL, SMA_TOGGLE, 1);
    sh->arg = A_SSH;
    sh->toggle = ssh_active() ? 2 : 1;
}

static void side_open_store(void)
{
    side_reset(SK_POWER, "App Store");
    side_add("Refresh Apps & Games", "refresh", SMA_ACTION, 1)->arg = A_RESCAN;
    side_add("Apps List", "category-software", SMA_ACTION, 1)->arg = A_APPLIST;
}

/* Turn SSH on or off; returns whether it worked (and says why if not) */
static int ssh_set(int on)
{
    if (access("/usr/sbin/sshd", X_OK) != 0) {
        show_message("SSH isn't installed", "Install the openssh-server package first (in the Terminal: sudo apt install openssh-server), then try again.");
        snd(SND_ERROR);
        return 0;
    }
    int rc = system(on ? "sudo -n /usr/bin/systemctl enable --now ssh >/dev/null 2>&1" : "sudo -n /usr/bin/systemctl disable --now ssh >/dev/null 2>&1");
    if (rc != 0) {
        show_message("Couldn't change SSH", "This account isn't allowed to. Run install.sh again to allow it.");
        snd(SND_ERROR);
        return 0;
    }
    sysinfo_refresh();
    return 1;
}

static void side_open_wifi(Item *it)
{
    side_reset(SK_ITEM, it->title);
    sncpy(side.path, sizeof side.path, it->data ? it->data : "");
    sncpy(side.detail, sizeof side.detail, it->detail ? it->detail : "");
    int active = (it->flags & IF_ACTIVE) != 0, saved = (it->flags & IF_SAVED) != 0;
    side_add(active ? "Disconnect" : "Connect", active ? "setting-disconnect" : "setting-connect", SMA_OPEN, 1);
    if (saved && (it->flags & IF_SECURED)) side_add("Change password", "rename", SMA_WIFI_EDIT, 1);
    if (active) {
        char extra[400];
        wifi_info(it->data, extra, sizeof extra);
        size_t n = strlen(side.detail);
        snprintf(side.detail + n, sizeof side.detail - n, "\nStatus: connected%s", extra);
    }
    side_add("Info", "setting-info", SMA_DETAILS, 1);
    if (saved) {
        side_divider();
        side_add("Forget", "trash-empty", SMA_WIFI_FORGET, 1);
    }
}

static void side_open_power(void)
{
    side_reset(SK_POWER, "Power");
    side_add("Log out", "category-system", SMA_POWER, 1)->arg = A_LOGOUT;
    side_add("Restart", "refresh", SMA_POWER, 1)->arg = A_REBOOT;
    side_add("Shut down", "shutdown", SMA_POWER, 1)->arg = A_SHUTDOWN;
}

static void side_open_files(Menu *m, Item *it)
{
    side_reset(SK_FILES, it ? it->title : "This folder");
    sncpy(side.dir, sizeof side.dir, m->data ? m->data : home_dir());
    sncpy(side.path, sizeof side.path, it && it->data ? it->data : "");
    if (it) {
        side_add("Open", "open-with", SMA_OPEN, 1);
        side_add("Copy", "copy", SMA_COPY, 1);
        side_add("Cut", "move", SMA_CUT, 1);
    }
    side_add("Paste", "paste", SMA_PASTE, clip.path[0] != 0);
    if (it) {
        side_add("Rename", "rename", SMA_RENAME, 1);
        side_add("Duplicate", "copy", SMA_DUP, 1);
    }
    side_add("New folder", "new-folder", SMA_MKDIR, 1);
    if (it && it->detail) {
        sncpy(side.detail, sizeof side.detail, it->detail);
        side_add("Details", "setting-info", SMA_DETAILS, 1);
    }
    if (it) {
        side_divider();
        side_add("Delete", "trash-empty", SMA_DELETE, 1);
    }
}

/* The package an installed app came from, found from its launcher file. */
static int app_package(const char *id, char *pkg, size_t cap)
{
    if (!id || !*id || strchr(id, '/') || !strncmp(id, "custom:", 7)) return 0;
    static const char *dirs[] = { "/usr/share/applications", "/usr/local/share/applications", "/var/lib/flatpak/exports/share/applications" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        char path[400];
        snprintf(path, sizeof path, "%s/%s", dirs[i], id);
        if (access(path, R_OK) != 0) continue;
        if (i != 0) return 0; /* not from apt */
        char *a[] = { "dpkg", "-S", path, NULL };
        char out[400] = "";
        if (run_capture(a, out, sizeof out, 5000) != 0) return 0;
        char *colon = strchr(out, ':');
        if (!colon) return 0;
        *colon = 0;
        char *comma = strchr(out, ',');
        if (comma) *comma = 0;
        /* the package may carry an architecture, as in "name:arm64" */
        sncpy(pkg, cap, out);
        for (char *p = pkg; *p; p++)
            if (!isalnum((unsigned char)*p) && *p != '-' && *p != '.' && *p != '+') return 0;
        return pkg[0] != 0;
    }
    return 0;
}

static void side_open_item(Item *it)
{
    side_reset(SK_ITEM, it->title);
    side.pkg[0] = 0;
    sncpy(side.path, sizeof side.path, it->data && (it->flags & (IF_MEDIA | IF_ROM)) ? it->data : "");
    sncpy(side.detail, sizeof side.detail, it->detail ? it->detail : "");
    if (it->flags & IF_APP) {
        side_add("Open", "launch", SMA_LAUNCH, 1);
        if (it->data) {
            sncpy(side.appid, sizeof side.appid, it->data);
            side_add("App Settings", "setting-input", SMA_APPSET, 1);
            SideItem *rb = side_add("Run in Background", NULL, SMA_TOGGLE, 1);
            rb->arg = A_APP_BG;
            rb->toggle = appset_bg(it->data) ? 2 : 1;
            SideItem *hb = side_add("Hide Item", NULL, SMA_TOGGLE, 1);
            hb->arg = A_APP_HIDE;
            hb->box = 1;
            hb->check = appset_hidden(it->data);
        }
        if (it->detail) side_add("Details", "setting-info", SMA_DETAILS, 1);
        if (app_package(it->data, side.pkg, sizeof side.pkg)) {
            side_divider();
            side_add("Uninstall", "uninstall", SMA_UNINSTALL, 1);
        }
    } else if (it->flags & IF_ROM) {
        side_add("Play", "launch", SMA_LAUNCH, it->kind == K_LAUNCH);
        side_add("Details", "setting-info", SMA_DETAILS, 1);
    } else if (it->flags & IF_MEDIA) {
        side_add(it->action == A_VIEW ? "View" : "Play", "open-with", SMA_OPEN, 1);
        if (it->action == A_VIEW) side_add("Set as wallpaper", "setting-wallpaper", SMA_WALLPAPER, 1);
        side_add("Rename", "rename", SMA_RENAME, 1);
        side_add("Details", "setting-info", SMA_DETAILS, 1);
        side_divider();
        side_add("Delete", "trash-empty", SMA_DELETE, 1);
    }
}

static void side_close(void) { side.open = 0; }

/* name -> "name copy", "name copy 2", ... for a name that's taken */
static void file_unique(const char *dir, const char *name, char *out, size_t cap)
{
    struct stat st;
    snprintf(out, cap, "%s/%s", dir, name);
    if (lstat(out, &st) != 0) return;
    char stem[300], ext[64] = "";
    sncpy(stem, sizeof stem, name);
    char *dot = strrchr(stem, '.');
    if (dot && dot != stem && strlen(dot) < sizeof ext) {
        sncpy(ext, sizeof ext, dot);
        *dot = 0;
    }
    for (int n = 1; n < 1000; n++) {
        if (n == 1) snprintf(out, cap, "%s/%s copy%s", dir, stem, ext);
        else snprintf(out, cap, "%s/%s copy %d%s", dir, stem, n, ext);
        if (lstat(out, &st) != 0) return;
    }
}

static const char *path_base(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static void file_copy_to(const char *src, const char *dir, int move, int same_ok)
{
    struct stat st;
    if (lstat(src, &st) != 0) {
        show_message("Couldn't continue", "The original isn't there anymore.");
        snd(SND_ERROR);
        return;
    }
    size_t sl = strlen(src);
    if (S_ISDIR(st.st_mode) && !strncmp(dir, src, sl) && (dir[sl] == 0 || dir[sl] == '/')) {
        show_message("Can't paste here", "A folder can't be placed inside itself.");
        snd(SND_ERROR);
        return;
    }
    char parent[512];
    sncpy(parent, sizeof parent, src);
    char *sl2 = strrchr(parent, '/');
    if (sl2) *sl2 = sl2 == parent ? 0 : 0;
    if (move && !strcmp(parent[0] ? parent : "/", dir)) return; /* already here */
    (void)same_ok;
    char dest[800];
    file_unique(dir, path_base(src), dest, sizeof dest);
    char *a[6];
    int n = 0;
    a[n++] = move ? "mv" : "cp";
    if (!move) a[n++] = "-a";
    a[n++] = "--";
    a[n++] = (char *)src;
    a[n++] = dest;
    a[n] = NULL;
    show_busy(move ? "Moving" : "Copying", path_base(src));
    char out[400] = "";
    int rc = run_capture(a, out, sizeof out, 600000);
    if (dlg.kind == D_BUSY) dlg.kind = D_NONE;
    if (rc != 0) {
        show_message("Couldn't finish", out[0] ? trim(out) : "Something went wrong. Is there enough space?");
        snd(SND_ERROR);
        return;
    }
    if (move) clip.path[0] = 0;
    menu_rebuild(stack[depth]);
    snd(SND_SELECT);
}

static void file_delete_now(const char *path)
{
    size_t l = strlen(path);
    if (path[0] != '/' || l < 4 || !strcmp(path, home_dir()) || !strcmp(path_base(path), "..") || !strcmp(path_base(path), ".")) {
        show_message("Couldn't delete", "That can't be deleted from here.");
        snd(SND_ERROR);
        return;
    }
    struct stat st;
    int is_dir = lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
    char out[300] = "";
    int rc;
    if (is_dir) {
        show_busy("Deleting", path_base(path));
        char *a[] = { "rm", "-rf", "--", (char *)path, NULL };
        rc = run_capture(a, out, sizeof out, 600000);
        if (dlg.kind == D_BUSY) dlg.kind = D_NONE;
    } else {
        rc = unlink(path);
        if (rc != 0) sncpy(out, sizeof out, strerror(errno));
    }
    if (rc != 0) {
        show_message("Couldn't delete", out[0] ? trim(out) : "Something went wrong.");
        snd(SND_ERROR);
        return;
    }
    if (!strcmp(clip.path, path)) clip.path[0] = 0;
    menu_rebuild(stack[depth]);
    Menu *m = stack[depth];
    if (m->sel >= m->n) m->sel = m->n ? m->n - 1 : 0;
    snd(SND_SELECT);
}

static void osk_open_for(int action, const char *title, const char *ctx, const char *initial)
{
    memset(&dlg, 0, sizeof dlg);
    dlg.kind = D_OSK;
    dlg.action = action;
    sncpy(dlg.title, sizeof dlg.title, title);
    sncpy(dlg.ctx, sizeof dlg.ctx, ctx);
    sncpy(dlg.buf, sizeof dlg.buf, initial);
    osk_begin();
    SDL_StartTextInput();
}

static int name_ok(const char *n)
{
    return n[0] && !strchr(n, '/') && strcmp(n, ".") && strcmp(n, "..");
}

static void file_rename_done(const char *old, const char *name)
{
    if (!name_ok(name)) {
        show_message("That name won't work", "Names can't be empty or contain a slash.");
        snd(SND_ERROR);
        return;
    }
    char dir[512], dest[800];
    sncpy(dir, sizeof dir, old);
    char *s = strrchr(dir, '/');
    if (s) *s = 0;
    snprintf(dest, sizeof dest, "%s/%s", dir[0] ? dir : "", name);
    struct stat st;
    if (strcmp(old, dest) != 0 && lstat(dest, &st) == 0) {
        show_message("That name is taken", "Something in this folder already has that name.");
        snd(SND_ERROR);
        return;
    }
    if (rename(old, dest) != 0) {
        show_message("Couldn't rename", strerror(errno));
        snd(SND_ERROR);
        return;
    }
    if (!strcmp(clip.path, old)) sncpy(clip.path, sizeof clip.path, dest);
    menu_rebuild(stack[depth]);
    snd(SND_SELECT);
}

static void file_mkdir_done(const char *dir, const char *name)
{
    if (!name_ok(name)) {
        show_message("That name won't work", "Names can't be empty or contain a slash.");
        snd(SND_ERROR);
        return;
    }
    char dest[800];
    snprintf(dest, sizeof dest, "%s/%s", dir, name);
    if (mkdir(dest, 0755) != 0) {
        show_message("Couldn't create the folder", errno == EEXIST ? "Something already has that name." : strerror(errno));
        snd(SND_ERROR);
        return;
    }
    menu_rebuild(stack[depth]);
    snd(SND_SELECT);
}

static void side_perform(const SideItem *s)
{
    char path[512], dir[512], title[96];
    sncpy(path, sizeof path, side.path);
    sncpy(dir, sizeof dir, side.dir);
    sncpy(title, sizeof title, side.title);
    int act = s->act, arg = s->arg, setting = side.setting;
    char appid[160];
    sncpy(appid, sizeof appid, side.appid);
    char pkg[96], detail[600];
    sncpy(pkg, sizeof pkg, side.pkg);
    sncpy(detail, sizeof detail, side.detail);
    side_close();
    switch (act) {
    case SMA_WIFI_EDIT: {
        char t[200];
        snprintf(t, sizeof t, "New password for %s", path);
        osk_open_for(A_WIFI_EDIT, t, path, "");
        break;
    }
    case SMA_WIFI_FORGET: {
        char q[300];
        snprintf(q, sizeof q, "Forget %s?", path);
        show_confirm(q, "Its saved password is removed. You'll need the password to join again.", A_WIFI_FORGET, path);
        dlg.choice = 1;
        snd(SND_SELECT);
        break;
    }
    case SMA_ACTION: { /* run a Settings action, as if its own row had been picked */
        Item tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.action = arg;
        do_action(&tmp);
        break;
    }
    case SMA_WALLPAPER: wallpaper_set(path); break;
    case SMA_NET_CONNECT: net_connect_saved(path, arg); break;
    case SMA_NET_INFO: net_show_info(path); break;
    case SMA_CHOICE:
        if (setting == A_CLOCK && arg >= 10) cfg.date_fmt = arg - 10;
        else if (setting == A_CLOCK) cfg.clock24 = arg;
        else if (setting == A_DATEFMT) cfg.date_fmt = arg;
        else if (setting == A_BATPCT) cfg.battery_pct = arg;
        else if (setting == A_SOUNDS) cfg.sounds = arg;
        else if (setting == A_VIDEOOUT) {
            char vm[400];
            int rc = videoout_apply(arg, vm, sizeof vm);
            /* a failure leaves it off, so it isn't retried at every start; "restart first" keeps the choice */
            cfg.video_out = (rc == 0 || rc == 3) ? arg : 0;
            if (arg && rc == 0) show_message("Video out is on", "Plug the USB data port into a PC and open the PSPi as a camera (OBS, VLC or a browser).");
            else if (arg || rc == 3) show_message(rc == 3 ? "Restart needed" : "Video out", vm);
        } else cfg.confirm_east = arg;
        state_save();
        menu_rebuild(stack[depth]);
        snd(SND_SELECT);
        break;
    case SMA_POWER:
        if (arg == A_LOGOUT) show_confirm("Log out of Pixi?", "Running applications will be closed.", A_LOGOUT, "");
        else if (arg == A_REBOOT) show_confirm("Restart now?", "", A_REBOOT, "");
        else show_confirm("Shut down now?", "", A_SHUTDOWN, "");
        snd(SND_SELECT);
        break;
    case SMA_OPEN:
    case SMA_LAUNCH: handle_action(ACT_OK); break;
    case SMA_DETAILS: show_message(title, detail); snd(SND_SELECT); break;
    case SMA_APPSET: aps_open(appid, title); break;
    case SMA_UNINSTALL: {
        char q[200];
        snprintf(q, sizeof q, "Uninstall %s?", title);
        show_confirm(q, "The app and its files will be removed. Your own files stay.", A_STORE_REMOVE, pkg);
        dlg.choice = 1;
        snd(SND_SELECT);
        break;
    }
    case SMA_COPY:
    case SMA_CUT:
        sncpy(clip.path, sizeof clip.path, path);
        clip.cut = act == SMA_CUT;
        snd(SND_SELECT);
        break;
    case SMA_PASTE:
        if (clip.path[0]) file_copy_to(clip.path, dir, clip.cut, 0);
        break;
    case SMA_DUP: {
        char parent[512];
        sncpy(parent, sizeof parent, path);
        char *sl = strrchr(parent, '/');
        if (sl) *sl = 0;
        file_copy_to(path, parent, 0, 1);
        break;
    }
    case SMA_RENAME: osk_open_for(A_FILE_RENAME, "Rename", path, path_base(path)); break;
    case SMA_MKDIR: osk_open_for(A_FILE_MKDIR, "Name the new folder", dir, "New folder"); break;
    case SMA_DELETE: {
        char q[300];
        struct stat st;
        int is_dir = lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
        snprintf(q, sizeof q, "Delete \"%s\"?", title);
        show_confirm(q, is_dir ? "This folder and everything inside it will be deleted. This can't be undone." : "This can't be undone.", A_DELETE, path);
        dlg.choice = 1; /* No is the safe default */
        snd(SND_SELECT);
        break;
    }
    }
}

static void side_preview(void)
{
    if (side.kind != SK_ACCENT || side.sel < 0 || side.sel >= side.n) return;
    const SideItem *s = &side.it[side.sel];
    /* a color previews as you pass it; anything else shows the color you had */
    sncpy(cfg.theme, sizeof cfg.theme, s->act == SMA_THEME ? THEMES[s->arg].name : side.old_theme);
    theme_apply();
}

static void side_action(int a)
{
    int step = 0;
    switch (a) {
    case ACT_UP: step = -1; break;
    case ACT_DOWN: step = 1; break;
    case ACT_PGUP: step = -5; break;
    case ACT_PGDN: step = 5; break;
    case ACT_BACK:
    case ACT_MENU:
        if (side.kind == SK_ACCENT) {
            sncpy(cfg.theme, sizeof cfg.theme, side.old_theme);
            theme_apply();
        }
        side_close();
        snd(SND_BACK);
        return;
    case ACT_OK:
    case ACT_START: {
        if (side.sel < 0 || side.sel >= side.n) return;
        const SideItem *s = &side.it[side.sel];
        if (!s->enabled) {
            snd(SND_ERROR);
            return;
        }
        if (s->act == SMA_TOGGLE) { /* a switch flips where it is; the list stays open */
            SideItem *t = &side.it[side.sel];
            if (t->arg == A_SSH) { /* the system's own service: the switch only moves if it really changed */
                if (ssh_set(t->toggle != 2)) t->toggle = t->toggle == 2 ? 1 : 2;
                return;
            }
            if (t->arg == A_APP_HIDE) { /* gone from the home screen now; the Apps List brings it back */
                appset_set_hidden(side.appid, 1);
                appsets_save();
                side_close();
                homescreen_refresh();
                snd(SND_STEP);
                return;
            }
            t->toggle = t->toggle == 2 ? 1 : 2;
            if (t->arg == A_APP_BG) { /* kept for the app; the pausing follows on its own */
                appset_set_bg(side.appid, t->toggle == 2);
                snd(SND_STEP);
                return;
            }
            if (t->arg == A_BATPCT) cfg.battery_pct = t->toggle == 2;
            state_save();
            if (stack[depth]) menu_rebuild(stack[depth]);
            snd(SND_STEP);
        } else if (side.kind == SK_ACCENT && s->act == SMA_WALLPICK) { /* the wallpaper picker, like Pictures */
            sncpy(cfg.theme, sizeof cfg.theme, side.old_theme);
            theme_apply();
            side_close();
            Item tmp;
            memset(&tmp, 0, sizeof tmp);
            tmp.title = "Wallpaper";
            tmp.build = build_wallpaper;
            push_menu(&tmp);
        } else if (side.kind == SK_ACCENT && s->act == SMA_APPMODE) { /* light or dark for the Linux apps */
            sncpy(cfg.theme, sizeof cfg.theme, side.old_theme);
            theme_apply();
            cfg.app_scheme = s->arg;
            app_scheme_apply();
            state_save();
            side_close();
            snd(SND_SELECT);
        } else if (side.kind == SK_ACCENT) {
            state_save();
            side_close();
            menu_rebuild(stack[depth]);
            snd(SND_SELECT);
        } else {
            side_perform(s);
        }
        return;
    }
    default: return;
    }
    int dir = step < 0 ? -1 : 1, n = step < 0 ? -step : step;
    int i = side.sel;
    while (n-- > 0) {
        int j = i + dir;
        while (j >= 0 && j < side.n && (side.it[j].divider || !side.it[j].enabled)) j += dir; /* nothing to do there */
        if (j < 0 || j >= side.n) break;
        i = j;
    }
    if (i == side.sel && (step == 1 || step == -1)) { /* past the end: wrap around to the other end */
        int j = step > 0 ? 0 : side.n - 1;
        while (j >= 0 && j < side.n && (side.it[j].divider || !side.it[j].enabled)) j -= step > 0 ? -1 : 1;
        if (j >= 0 && j < side.n) i = j;
    }
    if (i != side.sel) {
        side.sel = i;
        snd(SND_STEP);
        side_preview();
    }
}

static void draw_side(void)
{
    float target = side.open ? 1.0f : 0.0f;
    side.anim += (target - side.anim) * 0.3f;
    if (!side.open && side.anim < 0.02f) {
        side.anim = 0;
        return;
    }
    float a = side.anim;
    float pw = fmaxf(W * 0.34f, 320 * S);
    float x = W - pw * a;
    pane(x, 0, pw + 40 * S, (float)H, 0, CIRCLE, (Col){ 0.03f, 0.02f, 0.06f, 0.82f }, 0, 0, 0, 1, 0); /* so the icons behind don't show through the text */
    pane(x, 0, pw + 40 * S, (float)H, 0, SQUIRCLE, with_alpha(th.accent_soft, 0.30f), 0, FROST_PANEL, GLOSS_QUIET, 1, 1);
    pane(x, 0, 2 * S, (float)H, 0, SQUIRCLE, with_alpha(th.text, 0.35f), 0, 0, 0, 1, 0);
    text(side.title, x + 34 * S, 26 * S, 26 * S, 1, th.text, 1, AL_LEFT, (int)(pw - 60 * S));
    float top = 92 * S, rh = (side.kind == SK_ACCENT ? 56 : 60) * S, bottom = (float)H - 80 * S;
    int visible = (int)((bottom - top) / rh);
    float want = (float)side.sel - (float)visible / 2 + 0.5f;
    float maxs = (float)(side.n - visible);
    if (want > maxs) want = maxs;
    if (want < 0) want = 0;
    side.spos += (want - side.spos) * 0.3f;
    for (int i = 0; i < side.n; i++) {
        float y = top + ((float)i - side.spos) * rh;
        if (y < top - rh * 0.5f || y + rh > bottom + rh * 0.5f) continue;
        float fa = clampf(fminf((y - top + rh * 0.5f) / rh, (bottom - y) / rh), 0, 1);
        const SideItem *s = &side.it[i];
        if (s->divider) {
            pane(x + 30 * S, y + rh / 2, pw - 60 * S, 1.5f * S, 0.75f * S, CIRCLE, with_alpha(th.text, 0.35f), 0, 0, 0, fa, 0);
            continue;
        }
        int sel = i == side.sel;
        float ia = fa * (s->enabled ? 1.0f : 0.4f);
        if (sel)
            pane(x + 16 * S, y + 4 * S, pw - 32 * S, rh - 8 * S, (rh - 8 * S) / 2, SQUIRCLE, with_alpha(th.accent, 0.62f), DEPTH_CONTROL * S,
                 FROST_CONTROL, GLOSS_FULL, fa, 0);
        float ix = x + 52 * S;
        if (s->swatch) {
            float sw = 32 * S;
            pane(ix - sw / 2, y + rh / 2 - sw / 2, sw, sw, 7 * S, SQUIRCLE, hexcol(s->swatch, 1), 0, 0, GLOSS_QUIET, ia, 0);
        } else if (s->glyph) {
            glyph(s->glyph, ix, y + rh / 2, 32 * S, th.text, ia);
        }
        float lx = s->glyph || s->swatch ? 96 : 40;
        text(s->label, x + lx * S, y + rh / 2 - 12 * S, 24 * S, sel, th.text, ia, AL_LEFT, (int)(pw - (lx + 34) * S));
        if (s->toggle) { /* a switch: a hollow pill with the knob left when off, the pill lit with the knob right when on */
            glyph(s->toggle == 2 ? "toggle-on" : "toggle-off", x + pw - 62 * S, y + rh / 2, 64 * S, th.text, ia);
        } else if (s->box) {
            mark_draw(2, s->check, x + pw - 44 * S, y + rh / 2, 30 * S, th.text, ia);
        } else if (s->act == SMA_THEME || s->act == SMA_CHOICE || s->act == SMA_APPMODE) {
            int on = s->act == SMA_THEME ? side.kind == SK_ACCENT && !strcasecmp(side.old_theme, THEMES[s->arg].name) : s->check;
            mark_draw(1, on, x + pw - 44 * S, y + rh / 2, 30 * S, th.text, ia);
        } else if (s->check)
            glyph("chosen", x + pw - 44 * S, y + rh / 2, 26 * S, th.text, ia);
    }
}

/* ======================================================================== */
/* Network Settings: the PSP's connection screens, in Pixi's look           */
/* ======================================================================== */

/* A list of saved connections with [New Connection] at the top; a new one is found by scanning (a
 * table of access points) or typed in by name; a saved one opens a page of its details. Connect,
 * Disconnect, Change password and Delete are in the Options menu, as everywhere else. */

enum { NV_LIST, NV_NEW, NV_SCAN, NV_INFO, NV_COUNT };

static struct {
    int open, view, sel[NV_COUNT], top[NV_COUNT];
    int radio;
    WifiConn conns[32];
    int nconn;
    WifiNet nets[48];
    int nnets;
    WifiDetails info;
} netui;

static int net_is_open(void) { return netui.open; }

/* rows on the list: Wi-Fi, [New Connection], then the saved ones */
static int net_rows(void)
{
    switch (netui.view) {
    case NV_LIST: return netui.radio > 0 ? 2 + netui.nconn : 1;
    case NV_NEW: return 2;
    case NV_SCAN: return netui.nnets;
    default: return 0;
    }
}

static void net_load_list(void)
{
    netui.radio = wifi_radio();
    netui.nconn = netui.radio > 0 ? wifi_saved(netui.conns, 32) : 0;
    if (netui.sel[NV_LIST] >= net_rows()) netui.sel[NV_LIST] = net_rows() - 1;
    if (netui.sel[NV_LIST] < 0) netui.sel[NV_LIST] = 0;
}

static void net_open(void)
{
    if (!wifi_available()) {
        show_message("Network", "NetworkManager isn't installed. Install the network-manager package (the installer adds it).");
        snd(SND_ERROR);
        return;
    }
    memset(&netui, 0, sizeof netui);
    netui.open = 1;
    netui.view = NV_LIST;
    net_load_list();
    netui.sel[NV_LIST] = netui.radio > 0 ? 1 : 0; /* [New Connection] */
    snd(SND_SELECT);
}

static void net_scan(void)
{
    show_busy("Network Settings", "Scanning for access points\xE2\x80\xA6");
    netui.nnets = wifi_scan(netui.nets, 48, 1);
    netui.sel[NV_SCAN] = netui.top[NV_SCAN] = 0;
    if (dlg.kind == D_BUSY) dlg.kind = D_NONE;
}

static void net_show_info(const char *name)
{
    wifi_details(name, &netui.info);
    netui.view = NV_INFO;
    snd(SND_SELECT);
}

/* after connecting, disconnecting or deleting: fresh lists, and back to them once connected */
static void net_changed(int connected)
{
    if (!netui.open) return;
    char keep[128] = "";
    if (netui.view == NV_INFO) sncpy(keep, sizeof keep, netui.info.name);
    net_load_list();
    if (connected && netui.view != NV_LIST) {
        netui.view = NV_LIST;
        if (netui.nconn > 0) netui.sel[NV_LIST] = 2; /* the connected one is listed first */
    } else if (netui.view == NV_INFO) {
        int still = 0;
        for (int i = 0; i < netui.nconn; i++)
            if (!strcmp(netui.conns[i].name, keep)) still = 1;
        if (still) wifi_details(keep, &netui.info);
        else netui.view = NV_LIST;
    }
}

/* the Options menu for a saved connection */
static void net_options(const char *name)
{
    WifiDetails d;
    wifi_details(name, &d);
    side_reset(SK_ITEM, name);
    sncpy(side.path, sizeof side.path, name);
    side_add(d.active ? "Disconnect" : "Connect", d.active ? "setting-disconnect" : "setting-connect", SMA_NET_CONNECT, 1)->arg = d.active;
    if (strcmp(d.security, "None")) side_add("Change password", "rename", SMA_WIFI_EDIT, 1);
    if (netui.view != NV_INFO) side_add("Details", "setting-info", SMA_NET_INFO, 1);
    side_divider();
    side_add("Delete", "trash-empty", SMA_WIFI_FORGET, 1);
}

static void net_connect_saved(const char *name, int active)
{
    if (active) {
        char q[200];
        snprintf(q, sizeof q, "Disconnect from %s?", name);
        show_confirm(q, "", A_WIFI_NET, name);
        snd(SND_SELECT);
    } else wifi_join(name, NULL, 1);
}

static void net_pick_ap(const WifiNet *w)
{
    if (w->active) {
        show_message(w->ssid, "This device is already connected to it.");
        return;
    }
    if (w->saved) wifi_join(w->ssid, NULL, 1);
    else if (w->secured) { /* the password, then connect (osk_done's last case) */
        char t[200];
        snprintf(t, sizeof t, "Password for %s", w->ssid);
        osk_open_for(A_NONE, t, w->ssid, "");
    } else wifi_join(w->ssid, NULL, 0);
}

static void net_action(int a)
{
    int rows = net_rows(), *sel = &netui.sel[netui.view];
    switch (a) {
    case ACT_UP:
        if (rows > 0) *sel = (*sel + rows - 1) % rows, snd(SND_STEP);
        break;
    case ACT_DOWN:
        if (rows > 0) *sel = (*sel + 1) % rows, snd(SND_STEP);
        break;
    case ACT_BACK:
        snd(SND_BACK);
        if (netui.view == NV_LIST) {
            netui.open = 0;
            if (stack[depth]) menu_rebuild(stack[depth]); /* Settings shows the network it's on */
        } else if (netui.view == NV_SCAN) netui.view = NV_NEW;
        else netui.view = NV_LIST;
        break;
    case ACT_LEFT:
    case ACT_RIGHT:
        if (netui.view != NV_LIST || *sel != 0) break;
        __attribute__((fallthrough)); /* left and right flip Wi-Fi on and off */
    case ACT_OK:
        if (netui.view == NV_LIST) {
            if (*sel == 0) {
                int on = netui.radio > 0;
                show_busy("Wi-Fi", on ? "Turning Wi-Fi off\xE2\x80\xA6" : "Turning Wi-Fi on\xE2\x80\xA6");
                wifi_set_radio(!on);
                if (!on) SDL_Delay(2500);
                net_load_list();
                dlg.kind = D_NONE;
                snd(SND_SELECT);
            } else if (*sel == 1) {
                netui.view = NV_NEW;
                snd(SND_SELECT);
            } else net_show_info(netui.conns[*sel - 2].name);
        } else if (netui.view == NV_NEW) {
            if (*sel == 0) {
                net_scan();
                netui.view = NV_SCAN;
                snd(SND_SELECT);
            } else osk_open_for(A_NET_SSID, "Network name (SSID)", "", "");
        } else if (netui.view == NV_SCAN) {
            if (*sel < netui.nnets) net_pick_ap(&netui.nets[*sel]);
        }
        break;
    case ACT_MENU:
        if (netui.view == NV_LIST && *sel >= 2) net_options(netui.conns[*sel - 2].name);
        else if (netui.view == NV_INFO) net_options(netui.info.name);
        break;
    case ACT_START:
        if (netui.view == NV_SCAN) net_scan(), snd(SND_SELECT);
        break;
    }
}

/* one row: a pill like App Settings', with text on the left and maybe a value on the right */
static void net_row(float x, float y, float w, float h, int sel, const char *left, const char *right, const char *glyph_name)
{
    pane(x, y, w, h, h / 2, SQUIRCLE, sel ? with_alpha(th.accent, 0.62f) : with_alpha(th.glass_raised, 0.09f), DEPTH_CONTROL * S, FROST_CONTROL,
         sel ? GLOSS_FULL : GLOSS_QUIET, 1, 0);
    float lx = x + 26 * S;
    if (glyph_name) {
        glyph(glyph_name, lx + 14 * S, y + h / 2, 28 * S, th.text, 1);
        lx += 44 * S;
    }
    float rw = right ? (float)text_width(right, 25 * S, 1) + 24 * S : 0;
    text(left, lx, y + h / 2 - 14 * S, 27 * S, sel, th.text, 1, AL_LEFT, (int)(x + w - lx - rw - 26 * S));
    if (right) text(right, x + w - 26 * S, y + h / 2 - 13 * S, 25 * S, 1, th.text, 1, AL_RIGHT, (int)(w * 0.45f));
}

/* how many rows fit from y0, and the first to show so the selected one is in view */
static int net_scroll(float y0, float step, int rows)
{
    int fit = (int)((H - 112 * S - y0) / step);
    if (fit < 1) fit = 1;
    int *top = &netui.top[netui.view], sel = netui.sel[netui.view];
    if (sel < *top) *top = sel;
    if (sel >= *top + fit) *top = sel - fit + 1;
    if (*top > rows - fit) *top = rows - fit > 0 ? rows - fit : 0;
    return fit;
}

static void net_scrollbar(float x, float y0, float step, int fit, int rows)
{
    if (rows <= fit) return;
    float hh = step * fit - 8 * S, bh = fmaxf(hh * fit / rows, 20 * S), by = y0 + (hh - bh) * netui.top[netui.view] / (float)(rows - fit);
    pane(x, y0, 4 * S, hh, 2 * S, CIRCLE, with_alpha(th.text, 0.18f), 0, 0, 0, 1, 0);
    pane(x, by, 4 * S, bh, 2 * S, CIRCLE, with_alpha(th.accent_soft, 0.95f), 0, 0, 0, 1, 0);
}

static void draw_net(void)
{
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.28f }, 0, 0, 0, 1, 0);
    draw_page_header("setting-wifi", "Network Settings");
    float cx = W / 2.0f, rw = W * 0.66f, rx = cx - rw / 2, rh = 62 * S, step = rh + 10 * S;
    const char *about = NULL;
    int sel = netui.sel[netui.view];
    Hint h[3];
    int nh = 0;
    h[nh++] = (Hint){ pad_ok(), "Enter" };

    if (netui.view == NV_LIST) {
        text("Select a connection to edit.", cx, 98 * S, 30 * S, 0, th.text, 1, AL_CENTER, (int)(W * 0.9f));
        int rows = net_rows();
        float y0 = 160 * S;
        int fit = net_scroll(y0, step, rows), top = netui.top[NV_LIST];
        for (int i = top; i < rows && i < top + fit; i++) {
            float y = y0 + (i - top) * step;
            if (i == 0) {
                net_row(rx, y, rw, rh, sel == 0, "Wi-Fi", netui.radio > 0 ? "On" : "Off", "setting-wifi");
            } else if (i == 1) {
                net_row(rx, y, rw, rh, sel == 1, "[New Connection]", NULL, "add");
            } else {
                const WifiConn *c = &netui.conns[i - 2];
                net_row(rx, y, rw, rh, sel == i, c->name, c->active ? "Connected" : NULL, c->active ? "chosen" : "signal-fair");
            }
        }
        net_scrollbar(rx - 18 * S, y0, step, fit, rows);
        if (netui.radio <= 0)
            text("Turn Wi-Fi on to see and add connections.", cx, y0 + step + 14 * S, 26 * S, 0, th.text_soft, 1, AL_CENTER, (int)(W * 0.9f));
        else if (netui.nconn == 0 && rows <= fit)
            text("No saved connections yet.", cx, y0 + 2 * step + 14 * S, 26 * S, 0, th.text_soft, 1, AL_CENTER, (int)(W * 0.9f));
        about = sel == 0 ? "Turns the wireless radio on or off. Left and right change it too."
                : sel == 1 ? "Sets up a connection to a wireless network."
                           : "Shows the connection's details. Options: connect, change the password or delete it.";
        if (sel >= 2) h[nh++] = (Hint){ "pad-north", "Options" };
    } else if (netui.view == NV_NEW) {
        text("WLAN Settings", cx, 98 * S, 34 * S, 1, th.text, 1, AL_CENTER, 0);
        float y0 = 172 * S;
        static const char *names[2] = { "Scan", "Enter Manually" };
        for (int i = 0; i < 2; i++) {
            float y = y0 + i * step;
            net_row(rx, y, rw, rh, sel == i, names[i], NULL, i == 0 ? "search" : "rename");
            glyph("arrow-right", rx + rw - 34 * S, y + rh / 2, 26 * S, th.text, sel == i ? 1 : 0.5f);
        }
        about = sel == 0 ? "Pixi will scan for access points nearby. Select one to connect to it."
                         : "Type the name (SSID) of a network that doesn't show up in a scan.";
    } else if (netui.view == NV_SCAN) {
        text("Select the access point to be used.", cx, 98 * S, 30 * S, 0, th.text, 1, AL_CENTER, (int)(W * 0.9f));
        float tw = W * 0.82f, tx = cx - tw / 2, y0 = 196 * S;
        float c_sec = tx + tw * 0.58f, c_sig = tx + tw - 26 * S;
        text("SSID", tx + 26 * S, 148 * S, 23 * S, 1, th.text_soft, 1, AL_LEFT, 0);
        text("Security", c_sec, 148 * S, 23 * S, 1, th.text_soft, 1, AL_LEFT, 0);
        text("Signal Strength", c_sig, 148 * S, 23 * S, 1, th.text_soft, 1, AL_RIGHT, 0);
        pane(tx, 182 * S, tw, 1.5f * S, 0.75f * S, CIRCLE, with_alpha(th.text, 0.30f), 0, 0, 0, 1, 0);
        int rows = netui.nnets, fit = net_scroll(y0, step, rows), top = netui.top[NV_SCAN];
        for (int i = top; i < rows && i < top + fit; i++) {
            const WifiNet *w = &netui.nets[i];
            float y = y0 + (i - top) * step;
            int s = sel == i;
            pane(tx, y, tw, rh, rh / 2, SQUIRCLE, s ? with_alpha(th.accent, 0.62f) : with_alpha(th.glass_raised, 0.09f), DEPTH_CONTROL * S, FROST_CONTROL,
                 s ? GLOSS_FULL : GLOSS_QUIET, 1, 0);
            float lx = tx + 26 * S;
            if (w->active) glyph("chosen", lx + 12 * S, y + rh / 2, 26 * S, th.text, 1), lx += 36 * S;
            text(w->ssid, lx, y + rh / 2 - 14 * S, 27 * S, s, th.text, 1, AL_LEFT, (int)(c_sec - lx - 16 * S));
            text(w->secured ? w->security : "None", c_sec, y + rh / 2 - 13 * S, 25 * S, 0, th.text, 1, AL_LEFT, (int)(c_sig - c_sec - 130 * S));
            char sig[16];
            snprintf(sig, sizeof sig, "%d%%", w->signal);
            text(sig, c_sig, y + rh / 2 - 13 * S, 25 * S, 1, th.text, 1, AL_RIGHT, 0);
        }
        net_scrollbar(tx - 18 * S, y0, step, fit, rows);
        if (!rows) text("No access points found.", cx, y0 + 20 * S, 28 * S, 0, th.text_soft, 1, AL_CENTER, 0);
        about = rows ? (netui.nets[sel].saved ? "Saved: connects with its stored password." : netui.nets[sel].secured ? "You'll be asked for its password." : "An open network: no password.")
                     : "Move closer to the access point, or press Start to scan again.";
        h[nh++] = (Hint){ "pad-start", "Scan again" };
    } else { /* NV_INFO: a page of label / value lines, as on the PSP */
        const WifiDetails *d = &netui.info;
        char sig[16] = "-", ch[16] = "-";
        if (d->signal >= 0) snprintf(sig, sizeof sig, "%d%%", d->signal);
        if (d->channel > 0) snprintf(ch, sizeof ch, "%d", d->channel);
        const char *labels[] = { "Connection Name", "Status", "Signal Strength", "SSID", "BSSID", "Channel", "Security", "IP Address", "Gateway", "DNS" };
        const char *vals[] = { d->name, d->active ? "Connected" : "Not connected", sig, d->ssid[0] ? d->ssid : "-", d->bssid[0] ? d->bssid : "-", ch, d->security,
                               d->ip[0] ? d->ip : "-", d->gateway[0] ? d->gateway : "-", d->dns[0] ? d->dns : "-" };
        float y0 = 104 * S, ls = 46 * S, mid = W * 0.47f;
        for (int i = 0; i < 10; i++) {
            text(labels[i], mid - 16 * S, y0 + i * ls, 27 * S, 0, th.text_soft, 1, AL_RIGHT, (int)(mid * 0.9f));
            text(vals[i], mid + 16 * S, y0 + i * ls, 27 * S, 1, th.text, 1, AL_LEFT, (int)(W - mid - 50 * S));
        }
        nh = 0;
        h[nh++] = (Hint){ "pad-north", "Options" };
    }
    if (about && !side.open) {
        char wl[2][256];
        int n = wrap_text(about, 23 * S, (int)(W * 0.86f), wl, 2);
        for (int i = 0; i < n; i++) text(wl[i], cx, (float)H - 104 * S + i * 30 * S, 23 * S, 0, th.text_soft, 0.9f, AL_CENTER, 0);
    }
    if (side.open) {
        draw_side();
        Hint sh[2] = { { pad_ok(), "Select" }, { pad_back(), "Back" } };
        draw_hints(sh, 2);
    } else {
        h[nh++] = (Hint){ pad_back(), "Back" };
        draw_hints(h, nh);
    }
}

/* ======================================================================== */
/* System information, as a page like File Transfer                         */
/* ======================================================================== */

static struct {
    int open;
    char text[1024];
    float next;
} sysui;

static int sysinfo_is_open(void) { return sysui.open; }

static void sysinfo_open(void)
{
    sysinfo_text(sysui.text, sizeof sysui.text);
    sysui.next = now_s + 2.0f;
    sysui.open = 1;
    snd(SND_SELECT);
}

static void sysinfo_refresh(void) { sysinfo_text(sysui.text, sizeof sysui.text); }

static void draw_sysinfo(void)
{
    if (now_s >= sysui.next) { /* temperature and memory move, so keep them fresh */
        sysinfo_text(sysui.text, sizeof sysui.text);
        sysui.next = now_s + 2.0f;
    }
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.28f }, 0, 0, 0, 1, 0);
    draw_page_header("setting-info", "System Information");
    int n = 0;
    for (const char *p = sysui.text; *p; p++)
        if (*p == '\n') n++;
    n++;
    float step = fminf(52 * S, ((float)H - 190 * S) / (float)n), y = 104 * S, cx = W / 2.0f;
    const char *p = sysui.text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char row[300];
        snprintf(row, sizeof row, "%.*s", (int)(len < sizeof row ? len : sizeof row - 1), p);
        char *sep = strstr(row, ": ");
        if (sep) {
            *sep = 0;
            text(row, cx - 16 * S, y, 24 * S, 0, th.text_soft, 1, AL_RIGHT, (int)(W * 0.44f));
            text(sep + 2, cx + 16 * S, y, 24 * S, 1, th.text, 1, AL_LEFT, (int)(W * 0.46f));
        } else {
            text(row, cx, y, 24 * S, 0, th.text, 1, AL_CENTER, (int)(W * 0.9f));
        }
        y += step;
        p += len;
        if (*p == '\n') p++;
    }
    Hint h[1] = { { pad_back(), "Back" } };
    draw_hints(h, 1);
}

static void sysinfo_action(int a)
{
    if (a == ACT_BACK || a == ACT_OK || a == ACT_START) {
        sysui.open = 0;
        snd(SND_BACK);
    }
}

/* ======================================================================== */
/* Gamepad tester                                                           */
/* ======================================================================== */

enum {
    GB_UP, GB_DOWN, GB_LEFT, GB_RIGHT, GB_TRIANGLE, GB_CIRCLE, GB_CROSS, GB_SQUARE,
    GB_L1, GB_L2, GB_R1, GB_R2, GB_SELECT, GB_START, GB_HOME, GB_VOLDN, GB_VOLUP, GB_MUTE, GB_DISPLAY, GB_COUNT
};

/* What the controller is doing, whatever the shell makes of it. SDL and the raw input devices
   each fill in what they can see, and a button is down if either says so. */
static struct {
    int open;
    unsigned char sdl[GB_COUNT], ev[GB_COUNT], shm[GB_COUNT];
    char last[64];
    float back_since;
} gpt;

static int stick_raw[4];

static int gptest_is_open(void) { return gpt.open; }

static int gp_down(int b) { return gpt.sdl[b] || gpt.ev[b] || gpt.shm[b]; }

static void gp_sdl_set(int b, int down)
{
    if (b >= 0 && b < GB_COUNT) gpt.sdl[b] = (unsigned char)down;
}

/* Key codes some of the PSPi's buttons may come as (the exact ones depend on the controller's driver) */
#ifndef KEY_DISPLAYTOGGLE
#define KEY_DISPLAYTOGGLE 431
#endif
#ifndef KEY_BRIGHTNESS_CYCLE
#define KEY_BRIGHTNESS_CYCLE 243
#endif
#ifndef KEY_SWITCHVIDEOMODE
#define KEY_SWITCHVIDEOMODE 227
#endif

static int ev_gb(int code)
{
    switch (code) {
    case BTN_TL2: return GB_L2;
    case BTN_TR2: return GB_R2;
    case BTN_SELECT: return GB_SELECT;
    case BTN_START: return GB_START;
    case BTN_MODE: case KEY_HOMEPAGE: return GB_HOME;
    case KEY_VOLUMEUP: return GB_VOLUP;
    case KEY_VOLUMEDOWN: return GB_VOLDN;
    case KEY_MUTE: return GB_MUTE;
    case KEY_BRIGHTNESSDOWN: case KEY_BRIGHTNESSUP: case KEY_BRIGHTNESS_CYCLE: case KEY_DISPLAYTOGGLE: case KEY_SWITCHVIDEOMODE: return GB_DISPLAY;
    default: return -1;
    }
}

/* the raw input devices: what SDL doesn't report. Every press is also written on the tester's screen
 * with the device's name and the key's number, so a button that doesn't light can be identified. */
static void gp_evdev_key(const char *dev, int code, int value)
{
    int b = ev_gb(code);
    if (b >= 0) gpt.ev[b] = value != 0;
    if (value == 1) snprintf(gpt.last, sizeof gpt.last, "%s: key %d%s", dev, code, b >= 0 ? "" : " (not used)");
}

static void gp_sdl_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP: {
        int d = e->type == SDL_CONTROLLERBUTTONDOWN, b = -1;
        switch (e->cbutton.button) {
        case SDL_CONTROLLER_BUTTON_A: b = GB_CROSS; break;
        case SDL_CONTROLLER_BUTTON_B: b = GB_CIRCLE; break;
        case SDL_CONTROLLER_BUTTON_X: b = GB_SQUARE; break;
        case SDL_CONTROLLER_BUTTON_Y: b = GB_TRIANGLE; break;
        case SDL_CONTROLLER_BUTTON_BACK: b = GB_SELECT; break;
        case SDL_CONTROLLER_BUTTON_START: b = GB_START; break;
        case SDL_CONTROLLER_BUTTON_GUIDE: b = GB_HOME; break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: b = GB_L1; break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: b = GB_R1; break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: b = GB_UP; break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: b = GB_DOWN; break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: b = GB_LEFT; break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: b = GB_RIGHT; break;
        default: break;
        }
        if (b >= 0) gp_sdl_set(b, d);
        if (d) snprintf(gpt.last, sizeof gpt.last, "Controller button %d%s", e->cbutton.button, b >= 0 ? "" : " (not used)");
        break;
    }
    case SDL_CONTROLLERAXISMOTION:
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) gp_sdl_set(GB_L2, e->caxis.value > 8000);
        if (e->caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) gp_sdl_set(GB_R2, e->caxis.value > 8000);
        break;
    case SDL_JOYBUTTONDOWN:
    case SDL_JOYBUTTONUP: {
        int is_gc = SDL_GameControllerFromInstanceID(e->jbutton.which) != NULL;
        int d = e->type == SDL_JOYBUTTONDOWN, k = e->jbutton.button, b = -1;
        if (d) snprintf(gpt.last, sizeof gpt.last, "Joystick button %d", k);
        if (is_gc) break; /* the controller events above cover it */
        if (k == cfg.joy_ok) b = GB_CROSS;
        else if (k == cfg.joy_back) b = GB_CIRCLE;
        else if (k == cfg.joy_square) b = GB_SQUARE;
        else if (k == cfg.joy_menu) b = GB_TRIANGLE;
        else if (k == cfg.joy_pgup) b = GB_L1;
        else if (k == cfg.joy_pgdn) b = GB_R1;
        else if (k == cfg.joy_select) b = GB_SELECT;
        else if (k == cfg.joy_start) b = GB_START;
        else if (k == cfg.joy_guide) b = GB_HOME;
        if (b >= 0) gp_sdl_set(b, d);
        break;
    }
    case SDL_JOYHATMOTION:
        if (SDL_GameControllerFromInstanceID(e->jhat.which)) break;
        gp_sdl_set(GB_UP, (e->jhat.value & SDL_HAT_UP) != 0);
        gp_sdl_set(GB_DOWN, (e->jhat.value & SDL_HAT_DOWN) != 0);
        gp_sdl_set(GB_LEFT, (e->jhat.value & SDL_HAT_LEFT) != 0);
        gp_sdl_set(GB_RIGHT, (e->jhat.value & SDL_HAT_RIGHT) != 0);
        break;
    default: break;
    }
}

static void gptest_open(void)
{
    memset(&gpt, 0, sizeof gpt);
    gpt.open = 1;
    snd(SND_SELECT);
}

/* Everything is the tester's own while it's open; holding Back is the way out. */
static void gptest_action(int a) { (void)a; }

static void gp_pill(float x, float y, float w, float h, int down, const char *label, float g, float ox)
{
    float r = h * 0.5f * g;
    pane(ox + x * g, y * g, w * g, h * g, r, CIRCLE, with_alpha(th.text, down ? 0.92f : 0.10f), 0, 0, 0, 1, 0);
    outline(ox + x * g, y * g, w * g, h * g, r, 1.6f * g, with_alpha(th.text, down ? 1.0f : 0.5f), 1);
    Col c = down ? (Col){ 0.12f, 0.06f, 0.28f, 1 } : th.text_soft;
    text(label, ox + (x + w / 2) * g, (y + h / 2 - 8.5f) * g, 15 * g, 1, c, 1, AL_CENTER, 0);
}

static void gp_dot(float cx, float cy, float r, Col c, float g, float ox)
{
    pane(ox + (cx - r) * g, (cy - r) * g, 2 * r * g, 2 * r * g, r * g, CIRCLE, c, 0, 0, 0, 1, 0);
}

static void gp_ring(float cx, float cy, float r, float t, Col c, float g, float ox)
{
    outline(ox + (cx - r) * g, (cy - r) * g, 2 * r * g, 2 * r * g, r * g, t * g, c, 1);
}

/* a thick line, laid down as a row of dots (there's no line primitive) */
static void gp_seg(float x0, float y0, float x1, float y1, float t, Col c, float g, float ox)
{
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    int n = (int)(len / (t * 0.4f)) + 1;
    for (int i = 0; i <= n; i++) {
        float f = (float)i / (float)n;
        gp_dot(x0 + dx * f, y0 + dy * f, t * 0.5f, c, g, ox);
    }
}

static void gp_face(float cx, float cy, int which, int down, float w, float h, float g, float ox)
{
    static const Col fills[4] = { { 0.20f, 0.80f, 0.40f, 1 }, { 1.0f, 0.23f, 0.36f, 1 }, { 0.20f, 0.60f, 1.0f, 1 }, { 1.0f, 0.43f, 0.78f, 1 } };
    int idx = which == GB_TRIANGLE ? 0 : which == GB_CIRCLE ? 1 : which == GB_CROSS ? 2 : 3;
    float r = fminf(w, h) / 2;
    Col line = down ? (Col){ 1, 1, 1, 1 } : with_alpha(th.text, 0.6f);
    pane(ox + (cx - w / 2) * g, (cy - h / 2) * g, w * g, h * g, r * g, CIRCLE, down ? fills[idx] : with_alpha(th.text, 0.08f), 0, 0, 0, 1, 0);
    outline(ox + (cx - w / 2) * g, (cy - h / 2) * g, w * g, h * g, r * g, 2 * g,
            down ? (Col){ 1, 1, 1, 1 } : (Col){ th.text.r * 0.8f, th.text.g * 0.8f, th.text.b * 0.8f, 1 }, 1);
    float s = 10;
    if (which == GB_CIRCLE) {
        gp_ring(cx, cy, s, 2.6f, line, g, ox);
    } else if (which == GB_SQUARE) {
        outline(ox + (cx - s) * g, (cy - s) * g, 2 * s * g, 2 * s * g, 1.5f * g, 2.6f * g, line, 1);
    } else if (which == GB_CROSS) {
        gp_seg(cx - s, cy - s, cx + s, cy + s, 3, line, g, ox);
        gp_seg(cx - s, cy + s, cx + s, cy - s, 3, line, g, ox);
    } else {
        gp_seg(cx, cy - s - 1, cx - s, cy + s - 2, 2.6f, line, g, ox);
        gp_seg(cx, cy - s - 1, cx + s, cy + s - 2, 2.6f, line, g, ox);
        gp_seg(cx - s, cy + s - 2, cx + s, cy + s - 2, 2.6f, line, g, ox);
    }
}

static Col gp_lerp(Col a, Col b, float t)
{
    return (Col){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1 };
}

/* the analog nub: a ring with a dot that follows the stick */
static void gp_stick(float cx, float cy, int ax, int ay, float g, float ox, const char *tag)
{
    const float R = 22, maxd = 14;
    Col edge = (Col){ th.text.r * 0.8f, th.text.g * 0.8f, th.text.b * 0.8f, 1 };
    gp_dot(cx, cy, R, with_alpha(th.text, 0.06f), g, ox);
    gp_ring(cx, cy, R, 2.2f, edge, g, ox);
    gp_ring(cx, cy, 5, 1.2f, with_alpha(th.text, 0.3f), g, ox);
    float vx = stick_raw[ax] / 32767.0f, vy = stick_raw[ay] / 32767.0f;
    float m = sqrtf(vx * vx + vy * vy);
    if (m > 1) vx /= m, vy /= m, m = 1;
    Col col = m < 0.16f ? (Col){ 1, 1, 1, 1 }
              : m < 0.66f ? gp_lerp((Col){ 0.2f, 0.8f, 0.4f, 1 }, (Col){ 1, 0.8f, 0, 1 }, (m - 0.16f) / 0.5f)
                          : gp_lerp((Col){ 1, 0.8f, 0, 1 }, (Col){ 1, 0.29f, 0.29f, 1 }, (m - 0.66f) / 0.34f);
    if (m > 0.16f) gp_ring(cx + vx * maxd, cy + vy * maxd, 8, 1.8f, col, g, ox);
    gp_dot(cx + vx * maxd, cy + vy * maxd, 5, col, g, ox);
    text(tag, ox + cx * g, (cy + R + 4) * g, 11 * g, 1, th.text_soft, 1, AL_CENTER, 0);
}

/* The console is drawn from a PSP-1000 line drawing (a 750x750 SVG): its coordinates are kept as they were
   and mapped onto the page here. */
#define GPK 1.24f
#define GPY0 108.0f
static float gp_X(float x) { return 400 + (x - 375) * GPK; }
static float gp_Y(float y) { return GPY0 + (y - 244.5f) * GPK; }

typedef float GpPt[2];

/* fill a polygon with horizontal strips */
static void gp_polyfill(const GpPt *p, int n, Col c, float step, float g, float ox)
{
    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < n; i++) {
        float y = gp_Y(p[i][1]);
        if (y < ymin) ymin = y;
        if (y > ymax) ymax = y;
    }
    for (float y = floorf(ymin); y < ymax; y += step) {
        float yy = y + step * 0.5f, xs[32];
        int k = 0;
        for (int i = 0; i < n && k < 32; i++) {
            const float *a = p[i], *b = p[(i + 1) % n];
            float ay = gp_Y(a[1]), by = gp_Y(b[1]);
            if ((ay <= yy && by > yy) || (by <= yy && ay > yy)) {
                float ax = gp_X(a[0]), bx = gp_X(b[0]);
                xs[k++] = ax + (yy - ay) * (bx - ax) / (by - ay);
            }
        }
        for (int i = 0; i < k; i++)
            for (int j = i + 1; j < k; j++)
                if (xs[j] < xs[i]) { float t = xs[i]; xs[i] = xs[j]; xs[j] = t; }
        for (int i = 0; i + 1 < k; i += 2) pane(ox + xs[i] * g, y * g, (xs[i + 1] - xs[i]) * g, step * g, 0, CIRCLE, c, 0, 0, 0, 1, 0);
    }
}

/* an outline along the points: straight runs along an axis are one strip, the rest is dots */
static void gp_polyline(const GpPt *p, int n, int closed, float t, Col c, float g, float ox)
{
    float carry = 0;
    int last = closed ? n : n - 1;
    for (int i = 0; i < last; i++) {
        float x0 = gp_X(p[i][0]), y0 = gp_Y(p[i][1]), x1 = gp_X(p[(i + 1) % n][0]), y1 = gp_Y(p[(i + 1) % n][1]);
        float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
        if (len < 0.01f) continue;
        if (fabsf(dy) < 0.02f && len > 6) {
            pane(ox + (fminf(x0, x1) - t / 2) * g, (y0 - t / 2) * g, (fabsf(dx) + t) * g, t * g, t / 2 * g, CIRCLE, c, 0, 0, 0, 1, 0);
            continue;
        }
        if (fabsf(dx) < 0.02f && len > 6) {
            pane(ox + (x0 - t / 2) * g, (fminf(y0, y1) - t / 2) * g, t * g, (fabsf(dy) + t) * g, t / 2 * g, CIRCLE, c, 0, 0, 0, 1, 0);
            continue;
        }
        float sp = fmaxf(t * 0.5f, 0.9f);
        for (float d = carry; d < len; d += sp) {
            gp_dot(x0 + dx * d / len, y0 + dy * d / len, t / 2, c, g, ox);
            carry = d + sp - len;
        }
        if (len <= carry) carry -= len;
    }
}

static const GpPt gp_outer[] = {
    { 143.41f, 244.47f }, { 606.59f, 244.47f }, { 609.46f, 244.59f }, { 612.61f, 245.05f }, { 616.06f, 245.98f }, { 619.04f, 247.17f },
    { 622.1f, 248.78f }, { 625.56f, 251.04f }, { 629.17f, 253.9f }, { 633.61f, 258.05f }, { 637.64f, 262.36f }, { 641.91f, 267.44f },
    { 646.81f, 273.69f }, { 649.57f, 277.49f }, { 652.23f, 281.5f }, { 654.77f, 285.7f }, { 657.19f, 290.08f }, { 659.49f, 294.62f },
    { 661.65f, 299.31f }, { 663.67f, 304.14f }, { 665.55f, 309.08f }, { 667.28f, 314.12f }, { 668.86f, 319.25f }, { 670.29f, 324.45f },
    { 671.57f, 329.69f }, { 672.69f, 334.97f }, { 673.67f, 340.27f }, { 674.5f, 345.58f }, { 675.56f, 354.38f }, { 676.33f, 364.81f },
    { 676.58f, 375 }, { 676.13f, 388.65f }, { 675.18f, 399.14f }, { 674.23f, 406.2f }, { 673.35f, 411.51f }, { 672.32f, 416.81f },
    { 671.13f, 422.09f }, { 669.8f, 427.34f }, { 668.3f, 432.53f }, { 666.66f, 437.65f }, { 664.86f, 442.68f }, { 662.92f, 447.61f },
    { 660.83f, 452.42f }, { 658.6f, 457.1f }, { 656.23f, 461.62f }, { 653.74f, 465.98f }, { 651.12f, 470.16f }, { 648.39f, 474.16f },
    { 645.55f, 477.95f }, { 644.5f, 479.27f }, { 643.7f, 480.16f }, { 642.67f, 481.04f }, { 641.38f, 481.84f }, { 639.85f, 482.46f },
    { 638.39f, 482.78f }, { 637.06f, 482.88f }, { 594.6f, 482.88f }, { 592.54f, 483.1f }, { 590.43f, 483.84f }, { 588.48f, 485.11f },
    { 587.89f, 485.65f }, { 570.79f, 502.76f }, { 569.27f, 503.99f }, { 567.49f, 504.9f }, { 565.46f, 505.43f }, { 564.09f, 505.53f },
    { 143.41f, 505.53f }, { 140.8f, 505.43f }, { 137.66f, 505.0f }, { 134.21f, 504.11f }, { 130.96f, 502.83f }, { 127.9f, 501.22f },
    { 124.44f, 498.96f }, { 120.83f, 496.1f }, { 117.05f, 492.6f }, { 113.73f, 489.16f }, { 110.97f, 486.03f }, { 108.09f, 482.56f },
    { 106.6f, 480.7f }, { 103.19f, 476.32f }, { 100.43f, 472.51f }, { 97.77f, 468.5f }, { 95.23f, 464.3f }, { 92.81f, 459.92f },
    { 90.51f, 455.38f }, { 88.35f, 450.69f }, { 86.33f, 445.87f }, { 84.45f, 440.92f }, { 82.72f, 435.88f }, { 81.14f, 430.75f },
    { 79.71f, 425.55f }, { 78.43f, 420.31f }, { 77.31f, 415.03f }, { 76.33f, 409.73f }, { 75.5f, 404.43f }, { 74.81f, 399.14f },
    { 74.12f, 392.13f }, { 73.67f, 385.19f }, { 73.45f, 378.37f }, { 73.43f, 375 }, { 73.67f, 364.81f }, { 74.12f, 357.88f },
    { 74.81f, 350.87f }, { 75.5f, 345.58f }, { 76.33f, 340.27f }, { 77.31f, 334.97f }, { 78.43f, 329.69f }, { 79.71f, 324.45f },
    { 81.14f, 319.25f }, { 82.72f, 314.12f }, { 84.45f, 309.08f }, { 86.33f, 304.14f }, { 88.35f, 299.31f }, { 90.51f, 294.62f },
    { 92.81f, 290.08f }, { 95.23f, 285.7f }, { 97.77f, 281.5f }, { 100.43f, 277.49f }, { 103.19f, 273.69f }, { 108.09f, 267.44f },
    { 112.36f, 262.36f }, { 116.4f, 258.05f }, { 120.83f, 253.9f }, { 125.03f, 250.62f }, { 128.74f, 248.3f }, { 132.87f, 246.37f },
    { 137.13f, 245.11f }, { 141.32f, 244.53f },
};

/* the inner rim, with the two notches at the bottom corners */
static const GpPt gp_rim[] = {
    { 111.6f, 272.45f }, { 113.4f, 270.17f }, { 116.3f, 268.0f }, { 120.6f, 266.77f }, { 156.4f, 266.77f }, { 160.0f, 266.0f },
    { 163.2f, 263.8f }, { 172.75f, 253.34f }, { 175.5f, 251.4f }, { 179.5f, 250.35f }, { 570.5f, 250.35f }, { 574.5f, 251.4f },
    { 577.25f, 253.34f }, { 586.8f, 263.8f }, { 590.0f, 266.0f }, { 593.6f, 266.77f }, { 629.4f, 266.77f }, { 633.0f, 267.5f },
    { 636.6f, 270.17f }, { 638.4f, 272.45f }, { 642.1f, 277.24f }, { 648.2f, 286.1f }, { 652.1f, 293.0f }, { 657.1f, 303.4f },
    { 660.7f, 311.2f }, { 664.7f, 322.4f }, { 668.7f, 346.4f }, { 670.0f, 358.3f }, { 670.7f, 375 }, { 669.4f, 398.5f },
    { 666.0f, 417.3f }, { 661.2f, 435.6f }, { 655.0f, 450.2f }, { 647.4f, 464.2f }, { 640.9f, 474.3f }, { 639.96f, 475.5f },
    { 638.3f, 476.6f }, { 637.06f, 476.99f }, { 594.6f, 476.99f }, { 590.0f, 477.8f }, { 586.0f, 479.0f }, { 583.7f, 481.5f },
    { 566.6f, 498.6f }, { 564.1f, 499.65f }, { 187.7f, 499.65f }, { 184.0f, 499.2f }, { 180.0f, 497.5f }, { 178.9f, 496.06f },
    { 167.9f, 485.5f }, { 163.0f, 483.1f }, { 159.2f, 482.04f }, { 120.2f, 482.04f }, { 117.0f, 481.4f }, { 114.0f, 480.0f },
    { 112.45f, 478.6f }, { 108.9f, 474.0f }, { 103.1f, 465.9f }, { 97.8f, 456.8f }, { 91.8f, 443.7f }, { 86.3f, 429.3f },
    { 82.1f, 410.4f }, { 79.8f, 389.1f }, { 79.3f, 375 }, { 79.8f, 360.9f }, { 81.3f, 346.6f }, { 83.4f, 334.3f },
    { 87.3f, 319.0f }, { 91.2f, 306.3f }, { 96.4f, 296.0f }, { 103.9f, 283.0f },
};

/* the left arm of the d-pad (an arrow pointing at the middle); the others are this, turned */
static const GpPt gp_arm[] = {
    { 104.7f, 348.2f }, { 121.5f, 348.2f }, { 135.95f, 363.1f }, { 121.5f, 378.0f }, { 104.7f, 378.0f }, { 100.2f, 372.5f },
    { 99.65f, 363.1f }, { 100.2f, 353.7f },
};

/* a bowl-shaped button (Home, Select, Start), flat on top */
static void gp_bowl(float cx, int down, float g, float ox)
{
    GpPt p[8] = { { cx - 13.5f, 479 }, { cx + 13.5f, 479 }, { cx + 13.0f, 484 }, { cx + 9.5f, 489 }, { cx, 491.6f }, { cx - 9.5f, 489 }, { cx - 13.0f, 484 }, { cx - 13.5f, 479 } };
    gp_polyfill(p, 8, with_alpha(th.text, down ? 0.92f : 0.10f), 1, g, ox);
    gp_polyline(p, 8, 1, 2, (Col){ th.text.r * 0.8f, th.text.g * 0.8f, th.text.b * 0.8f, 1 }, g, ox);
}

static void gp_oval(float cx, int down, float g, float ox)
{
    float x = gp_X(cx) - 8.9f * GPK, y = gp_Y(478.4f), w = 17.8f * GPK, h = 13.2f * GPK;
    pane(ox + x * g, y * g, w * g, h * g, h / 2 * g, CIRCLE, with_alpha(th.text, down ? 0.92f : 0.10f), 0, 0, 0, 1, 0);
    outline(ox + x * g, y * g, w * g, h * g, h / 2 * g, 2 * g, (Col){ th.text.r * 0.8f, th.text.g * 0.8f, th.text.b * 0.8f, 1 }, 1);
}

static int ev_n, ev_denied; /* the raw input devices being read, and the ones that couldn't be */

static void draw_gptest(void)
{
    float g = fminf(W / 800.0f, H / 480.0f), ox = (W - 800 * g) / 2;
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.28f }, 0, 0, 0, 1, 0);
    draw_page_header("category-games", "Gamepad Tester");

    /* the PSP, from the line drawing */
    Col edge = (Col){ th.text.r * 0.8f, th.text.g * 0.8f, th.text.b * 0.8f, 1 };
    gp_polyfill(gp_outer, (int)(sizeof gp_outer / sizeof *gp_outer), with_alpha(th.text, 0.05f), 2, g, ox);
    gp_polyline(gp_outer, (int)(sizeof gp_outer / sizeof *gp_outer), 1, 2.8f, edge, g, ox);
    gp_polyline(gp_rim, (int)(sizeof gp_rim / sizeof *gp_rim), 1, 1.6f, with_alpha(edge, 0.55f), g, ox);

    /* the screen, in its double frame */
    {
        float x = gp_X(206.02f), y = gp_Y(270.28f), w = 337.96f * GPK, h = 191.68f * GPK;
        pane(ox + x * g, y * g, w * g, h * g, 4 * g, CIRCLE, (Col){ 0, 0, 0, 0.38f }, 0, 0, 0, 1, 0);
        outline(ox + x * g, y * g, w * g, h * g, 4 * g, 2.4f * g, edge, 1);
        outline(ox + gp_X(210.84f) * g, gp_Y(275.1f) * g, 328.3f * GPK * g, 182.0f * GPK * g, 3 * g, 1.6f * g, with_alpha(edge, 0.6f), 1);
    }

    gp_pill(62, 52, 130, 21, gp_down(GB_L1), "L1", g, ox);
    gp_pill(62, 77, 130, 21, gp_down(GB_L2), "L2", g, ox);
    gp_pill(608, 52, 130, 21, gp_down(GB_R1), "R1", g, ox);
    gp_pill(608, 77, 130, 21, gp_down(GB_R2), "R2", g, ox);

    /* d-pad: a round bezel with four arrows */
    {
        const float cx0 = 145.03f, cy0 = 363.1f;
        gp_ring(gp_X(143.8f), gp_Y(364.0f), 45.5f * GPK, 2, edge, g, ox);
        static const int dirs[4] = { GB_LEFT, GB_UP, GB_RIGHT, GB_DOWN };
        for (int d = 0; d < 4; d++) {
            GpPt p[8];
            for (int i = 0; i < 8; i++) {
                float rx = gp_arm[i][0] - cx0, ry = gp_arm[i][1] - cy0, x, y;
                switch (d) {
                case 0: x = rx; y = ry; break;
                case 1: x = ry; y = rx; break;
                case 2: x = -rx; y = ry; break;
                default: x = ry; y = -rx; break;
                }
                p[i][0] = cx0 + x;
                p[i][1] = cy0 + y;
            }
            gp_polyfill(p, 8, with_alpha(th.text, gp_down(dirs[d]) ? 0.92f : 0.10f), 1, g, ox);
            gp_polyline(p, 8, 1, 2, edge, g, ox);
        }
    }

    /* the face buttons: ovals, longer along the line to the middle */
    {
        const float fx = gp_X(607.5f), fy = gp_Y(361.8f), off = 39 * GPK, tall = 36 * GPK, shortw = 31 * GPK;
        gp_face(fx, fy - off, GB_TRIANGLE, gp_down(GB_TRIANGLE), shortw, tall, g, ox);
        gp_face(fx - off, fy, GB_SQUARE, gp_down(GB_SQUARE), tall, shortw, g, ox);
        gp_face(fx + off, fy, GB_CIRCLE, gp_down(GB_CIRCLE), tall, shortw, g, ox);
        gp_face(fx, fy + off, GB_CROSS, gp_down(GB_CROSS), shortw, tall, g, ox);
    }

    gp_stick(gp_X(147.4f), gp_Y(443.8f), 0, 1, g, ox, "L STICK");
    gp_stick(800 - gp_X(147.4f), gp_Y(443.8f), 2, 3, g, ox, "R STICK");

    /* the lights, and the row of small buttons along the bottom */
    {
        Col soft = with_alpha(edge, 0.9f);
        for (int side = 0; side < 2; side++) {
            float x = gp_X(side ? 572.0f : 179.5f) - 6.65f * GPK, y = gp_Y(460.55f);
            outline(ox + x * g, y * g, 13.3f * GPK * g, 9.9f * GPK * g, 4.95f * GPK * g, 1.8f * g, soft, 1);
        }
        gp_bowl(206.8f, gp_down(GB_HOME), g, ox);
        gp_oval(253.1f, gp_down(GB_VOLDN), g, ox);
        gp_oval(291.9f, gp_down(GB_VOLUP), g, ox);
        gp_oval(441.2f, gp_down(GB_DISPLAY), g, ox);
        gp_oval(466.9f, gp_down(GB_MUTE), g, ox);
        gp_bowl(501.8f, gp_down(GB_SELECT), g, ox);
        gp_bowl(540.8f, gp_down(GB_START), g, ox);
        /* DISPLAY and MUTE are close together, so DISPLAY's label ends at its button and reaches left */
        const struct { float x; const char *t; int align; } lab[] = { { 206.8f, "HOME", AL_CENTER }, { 253.1f, "VOL-", AL_CENTER }, { 291.9f, "VOL+", AL_CENTER },
                                                                     { 441.2f, "DISPLAY", AL_RIGHT }, { 466.9f, "MUTE", AL_CENTER },
                                                                     { 501.8f, "SELECT", AL_CENTER }, { 540.8f, "START", AL_CENTER } };
        for (size_t i = 0; i < sizeof lab / sizeof *lab; i++) {
            float lx = ox + gp_X(lab[i].x) * g + (lab[i].align == AL_RIGHT ? 6 * g : 0);
            text(lab[i].t, lx, (gp_Y(478.4f) - 14) * g, 10 * g, 1, th.text_soft, 1, lab[i].align, 0);
        }
    }

    /* the screen: the stick readouts, and the way out */
    char buf[96];
    float sx = ox + 400 * g;
    snprintf(buf, sizeof buf, "Left   X %3d   Y %3d", (stick_raw[0] + 32768) >> 8, (stick_raw[1] + 32768) >> 8);
    text(buf, sx, 172 * g, 18 * g, 0, th.text, 1, AL_CENTER, 0);
    snprintf(buf, sizeof buf, "Right   X %3d   Y %3d", (stick_raw[2] + 32768) >> 8, (stick_raw[3] + 32768) >> 8);
    text(buf, sx, 204 * g, 18 * g, 0, th.text, 1, AL_CENTER, 0);
    text(gpt.last[0] ? gpt.last : "Press any button", sx, 244 * g, 16 * g, 0, th.text, 1, AL_CENTER, (int)(380 * g));
    if (ev_n == 0) snprintf(buf, sizeof buf, ev_denied ? "Can't read the input devices (the input group)" : "No controller input device found");
    else snprintf(buf, sizeof buf, "Reading %d input device%s", ev_n, ev_n == 1 ? "" : "s");
    text(buf, sx, 268 * g, 13 * g, 0, th.text_soft, 1, AL_CENTER, (int)(380 * g));

    float held_for = gpt.back_since > 0 ? SDL_GetTicks() / 1000.0f - gpt.back_since : 0; /* gptest_tick() does the timing */
    if (held_for < 0) held_for = 0;
    float bw = 240 * g;
    pane(sx - bw / 2, 312 * g, bw, 4 * g, 2 * g, CIRCLE, with_alpha(th.text, 0.25f), 0, 0, 0, 1, 0);
    if (held_for > 0) pane(sx - bw / 2, 312 * g, fmaxf(bw * held_for / 1.2f, 4 * g), 4 * g, 2 * g, CIRCLE, with_alpha(th.text, 0.95f), 0, 0, 0, 1, 0);
    text("Hold Back (or Select + Start) to exit", sx, 326 * g, 15 * g, 0, th.text_soft, 1, AL_CENTER, (int)(380 * g));

    Hint h[1] = { { pad_back(), "Hold to exit" } };
    draw_hints(h, 1);
}

/* ======================================================================== */
/* File transfer: the PSP's USB Mode, over the network                       */
/* ======================================================================== */

/* It runs as an app of its own: Back goes to the home screen and leaves it serving, it has a card in
 * the Guide, and Start (or Close in the Guide) stops it. */
static struct {
    int open, running;
} ftpui;

static int ftp_ui_is_open(void) { return ftpui.open; }
static int ftp_running(void) { return ftpui.running; }

static void ftp_ui_open(void)
{
    if (ftpui.running) {
        app_hide_all();
        ftpui.open = 1;
        snd(SND_SELECT);
        return;
    }
    /* make sure the media folders are there to copy into; the shell looks for media in these */
    static const char *want[] = { "Pictures", "Music", "Videos", "Games", "Documents", "Downloads", "www" };
    games_folders_ensure(); /* Games gets a folder per system to copy into */
    for (size_t i = 0; i < sizeof want / sizeof *want; i++) {
        char d[1100];
        snprintf(d, sizeof d, "%s/%s", home_dir(), want[i]);
        mkdir(d, 0755);
    }
    ftp_set_visible(want, (int)(sizeof want / sizeof *want)); /* nothing else of the home folder is shown */
    char err[200] = "";
    if (ftp_start(home_dir(), cfg.ftp_port, err, sizeof err) != 0) {
        show_message("Couldn't start file transfer", err);
        snd(SND_ERROR);
        return;
    }
    ftpui.running = 1;
    app_hide_all();
    ftpui.open = 1;
    snd(SND_SELECT);
}

static void ftp_ui_stop(void)
{
    if (ftpui.running) ftp_stop();
    ftpui.running = ftpui.open = 0;
    if (stack[depth]) menu_rebuild(stack[depth]); /* Settings shows whether it's on */
}

static void ftp_ui_action(int a)
{
    if (a == ACT_BACK) { /* to the home screen; it keeps serving */
        ftpui.open = 0;
        if (stack[depth]) menu_rebuild(stack[depth]);
        snd(SND_BACK);
    } else if (a == ACT_START) {
        ftp_ui_stop();
        snd(SND_BACK);
    }
}

static void draw_ftp(void)
{
    FtpStatus st;
    ftp_status(&st);
    char ip[64];
    ftp_address(ip, sizeof ip);
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.28f }, 0, 0, 0, 1, 0);
    draw_page_header("setting-network", "FTP Server");
    /* the mode, with a line that moves while it's waiting */
    float cx = W / 2.0f, cy = H * 0.34f;
    text("FTP Mode", cx, cy - 22 * S, 42 * S, 1, th.text, 1, AL_CENTER, 0);
    float lw = W * 0.46f, ly = cy + 54 * S;
    pane(cx - lw / 2, ly, lw, 3 * S, 1.5f * S, CIRCLE, with_alpha(th.text, 0.25f), 0, 0, 0, 1, 0);
    float seg = lw * 0.28f, p = fmodf(now_s * 0.5f, 1.0f);
    float sx = cx - lw / 2 + (lw - seg) * (0.5f - 0.5f * cosf(p * 6.2832f));
    pane(sx, ly - 0.5f * S, seg, 4 * S, 2 * S, CIRCLE, with_alpha(th.text, 0.9f), 0, 0, 0, 1, 0);
    /* how to connect */
    float ty = ly + 44 * S, step = 36 * S;
    char line[300];
    if (ip[0]) snprintf(line, sizeof line, "Address:  ftp://%s:%d", ip, st.port);
    else snprintf(line, sizeof line, "Not connected to a network. Connect to Wi-Fi first.");
    text(line, cx, ty, 26 * S, 1, th.text, 1, AL_CENTER, (int)(W * 0.9f));
    snprintf(line, sizeof line, "User name:  %s", st.user);
    text(line, cx, ty + step, 24 * S, 0, th.text, 1, AL_CENTER, (int)(W * 0.9f));
    text("Password:  the one you sign in to this device with", cx, ty + step * 2, 24 * S, 0, th.text, 1, AL_CENTER, (int)(W * 0.9f));
    text("Folders:  Pictures, Music, Videos, Games, Documents, Downloads, www", cx, ty + step * 2.8f, 21 * S, 0, th.text_soft, 1, AL_CENTER, (int)(W * 0.9f));
    char sent[32], got[32];
    human_size(sent, sizeof sent, (double)st.sent);
    human_size(got, sizeof got, (double)st.received);
    snprintf(line, sizeof line, "%d connected  \xC2\xB7  sent %s  \xC2\xB7  received %s", st.loggedin, sent, got);
    text(line, cx, ty + step * 3.7f, 21 * S, 0, th.text_soft, 1, AL_CENTER, (int)(W * 0.9f));
    if (st.last[0]) text(st.last, cx, ty + step * 4.4f, 21 * S, 0, th.text_soft, 1, AL_CENTER, (int)(W * 0.9f));
    text("Passwords aren't encrypted over FTP. Use it on a network you trust.", cx, (float)H - 96 * S, 19 * S, 0, th.text_soft, 0.8f, AL_CENTER, (int)(W * 0.9f));
    Hint h[2] = { { "pad-start", "Stop server" }, { pad_back(), "Home screen" } };
    draw_hints(h, 2);
}

/* ======================================================================== */
/* Text editor: a monospaced text box with Save and Cancel                   */
/* ======================================================================== */

/* A small editor for one text file, drawn like the server pages. A real keyboard types into it; with a
 * gamepad the PC keyboard is on screen below. Ctrl+S saves and Ctrl+X closes (Start saves too). */

#define ED_MAX (256 * 1024)

static struct {
    int open, dirty, r, c, is_web;
    char *buf;
    size_t len, cap, cur;
    int top, left, want; /* first line and column on screen; the column up and down try to keep */
    char path[600], title[96];
    int fpx;
    float cw, chh, kbh;
} ed;

static void web_saved(char *title, size_t tc, char *body, size_t bc);

static int ed_is_open(void) { return ed.open; }

static void ed_layout(void)
{
    ed.fpx = (int)lroundf(H * 0.03f);
    if (ed.fpx < 12) ed.fpx = 12;
    TTF_Font *f = font_get(ed.fpx, 2, 0);
    int w = 0;
    TTF_SizeUTF8(f, "MMMMMMMMMMMMMMMMMMMM", &w, NULL);
    ed.cw = w > 0 ? w / 20.0f : ed.fpx * 0.6f;
    ed.chh = (float)TTF_FontLineSkip(f);
    if (ed.chh < ed.fpx) ed.chh = (float)ed.fpx;
    ed.kbh = floorf(H * 0.34f);
}

/* where the text box and the buttons go */
static void ed_geometry(float *bx, float *by, float *bw, float *bh, float *ty, float *th_)
{
    float btn_h = 30 * S;
    *ty = (float)H - ed.kbh - btn_h - 8 * S; /* the buttons' top */
    *bx = 18 * S;
    *by = 88 * S;
    *bw = (float)W - 36 * S;
    *bh = *ty - 8 * S - *by;
    *th_ = btn_h;
}

static int ed_rows(void)
{
    float bx, by, bw, bh, ty, tb;
    ed_geometry(&bx, &by, &bw, &bh, &ty, &tb);
    int n = (int)floorf((bh - 16 * S) / ed.chh);
    return n < 1 ? 1 : n;
}

static int ed_cols(void)
{
    float bx, by, bw, bh, ty, tb;
    ed_geometry(&bx, &by, &bw, &bh, &ty, &tb);
    int n = (int)floorf((bw - 28 * S) / ed.cw);
    return n < 4 ? 4 : n;
}

static size_t ed_prev(size_t i)
{
    if (!i) return 0;
    i--;
    while (i > 0 && ((unsigned char)ed.buf[i] & 0xC0) == 0x80) i--;
    return i;
}

static size_t ed_next(size_t i)
{
    if (i >= ed.len) return ed.len;
    i++;
    while (i < ed.len && ((unsigned char)ed.buf[i] & 0xC0) == 0x80) i++;
    return i;
}

static size_t ed_line_start(size_t i)
{
    while (i > 0 && ed.buf[i - 1] != '\n') i--;
    return i;
}

static size_t ed_line_end(size_t i)
{
    while (i < ed.len && ed.buf[i] != '\n') i++;
    return i;
}

/* the column a byte offset sits at, with tabs going to the next multiple of 4 */
static int ed_col_of(size_t ls, size_t off)
{
    int col = 0;
    for (size_t i = ls; i < off; i = ed_next(i)) col += ed.buf[i] == '\t' ? 4 - col % 4 : 1;
    return col;
}

static size_t ed_off_of(size_t ls, size_t le, int col)
{
    int c = 0;
    size_t i = ls;
    while (i < le && c < col) {
        c += ed.buf[i] == '\t' ? 4 - c % 4 : 1;
        i = ed_next(i);
    }
    return i;
}

static int ed_line_of(size_t off)
{
    int n = 0;
    for (size_t i = 0; i < off && i < ed.len; i++)
        if (ed.buf[i] == '\n') n++;
    return n;
}

static int ed_line_count(void)
{
    int n = 1;
    for (size_t i = 0; i < ed.len; i++)
        if (ed.buf[i] == '\n') n++;
    return n;
}

static size_t ed_start_of_line(int line)
{
    size_t i = 0;
    while (line > 0 && i < ed.len) {
        if (ed.buf[i] == '\n') line--;
        i++;
    }
    return i;
}

/* scroll so the caret is on screen */
static void ed_reveal(void)
{
    int rows = ed_rows(), cols = ed_cols();
    int line = ed_line_of(ed.cur), col = ed_col_of(ed_line_start(ed.cur), ed.cur);
    if (line < ed.top) ed.top = line;
    if (line >= ed.top + rows) ed.top = line - rows + 1;
    if (col < ed.left) ed.left = col;
    if (col >= ed.left + cols - 1) ed.left = col - cols + 2;
    if (ed.left < 0) ed.left = 0;
    int max_top = ed_line_count() - 1;
    if (ed.top > max_top) ed.top = max_top;
    if (ed.top < 0) ed.top = 0;
}

static void ed_remember(void) { ed.want = ed_col_of(ed_line_start(ed.cur), ed.cur); }

static void ed_free(void)
{
    free(ed.buf);
    memset(&ed, 0, sizeof ed);
}

/* another app came to the front: the editor goes (its text wasn't saved) */
static void ed_abandon(void)
{
    if (!ed.open) return;
    SDL_StopTextInput();
    ed_free();
}

static void ed_close(void)
{
    if (ed.open) SDL_StopTextInput();
    ed_free();
    snd(SND_BACK);
}

static int ed_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    size_t n = 0;
    free(ed.buf);
    ed.cap = 4096;
    ed.buf = malloc(ed.cap);
    ed.len = 0;
    if (!ed.buf) return -1;
    if (f) {
        for (;;) {
            if (ed.len + 4096 + 1 > ed.cap) {
                if (ed.cap >= ED_MAX + 8192) break;
                size_t nc = ed.cap * 2;
                char *nb = realloc(ed.buf, nc);
                if (!nb) break;
                ed.buf = nb;
                ed.cap = nc;
            }
            n = fread(ed.buf + ed.len, 1, 4096, f);
            if (n == 0) break;
            ed.len += n;
        }
        fclose(f);
    }
    if (ed.len > ED_MAX) return -2; /* too big: saving would cut it short */
    /* Windows line endings become plain ones */
    size_t o = 0;
    for (size_t i = 0; i < ed.len; i++)
        if (!(ed.buf[i] == '\r' && i + 1 < ed.len && ed.buf[i + 1] == '\n')) ed.buf[o++] = ed.buf[i];
    ed.len = o;
    ed.buf[ed.len] = 0;
    return 0;
}

static void ed_open(const char *path, const char *title, int is_web)
{
    ed_free();
    ed_layout();
    sncpy(ed.path, sizeof ed.path, path);
    sncpy(ed.title, sizeof ed.title, title);
    ed.is_web = is_web;
    int lr = ed_load(path);
    if (lr != 0) {
        ed_free();
        show_message("Couldn't open the file", lr == -2 ? "The file is too big to edit here." : "There wasn't enough memory.");
        snd(SND_ERROR);
        return;
    }
    ed.cur = 0;
    ed.top = ed.left = 0;
    ed.want = 0;
    ed.dirty = 0;
    pk_reset();
    ed.r = 3;
    ed.c = 3; /* the space bar */
    ed.open = 1;
    SDL_StartTextInput();
    side_close();
    snd(SND_SELECT);
}

static void ed_insert(const char *s, size_t n)
{
    if (!n) return;
    if (ed.len + n > ED_MAX) {
        snd(SND_ERROR);
        return;
    }
    if (ed.len + n + 1 > ed.cap) {
        size_t nc = ed.cap;
        while (ed.len + n + 1 > nc) nc *= 2;
        char *nb = realloc(ed.buf, nc);
        if (!nb) return;
        ed.buf = nb;
        ed.cap = nc;
    }
    memmove(ed.buf + ed.cur + n, ed.buf + ed.cur, ed.len - ed.cur);
    memcpy(ed.buf + ed.cur, s, n);
    ed.len += n;
    ed.cur += n;
    ed.buf[ed.len] = 0;
    ed.dirty = 1;
    ed_remember();
    ed_reveal();
}

static void ed_type(const char *s)
{
    char clean[64];
    size_t o = 0;
    for (; *s && o + 1 < sizeof clean; s++)
        if ((unsigned char)*s >= 32 && *s != 127) clean[o++] = *s;
    if (o) {
        ed_insert(clean, o);
        snd(SND_KEY);
    }
}

static void ed_enter(void)
{
    size_t ls = ed_line_start(ed.cur), i = ls;
    while (i < ed.cur && (ed.buf[i] == ' ' || ed.buf[i] == '\t')) i++;
    char ins[256];
    size_t n = i - ls;
    if (n > sizeof ins - 2) n = sizeof ins - 2;
    ins[0] = '\n';
    memcpy(ins + 1, ed.buf + ls, n); /* the new line starts as indented as this one */
    ed_insert(ins, n + 1);
    snd(SND_KEY);
}

static void ed_backspace(void)
{
    if (!ed.cur) return;
    size_t p = ed_prev(ed.cur);
    memmove(ed.buf + p, ed.buf + ed.cur, ed.len - ed.cur);
    ed.len -= ed.cur - p;
    ed.cur = p;
    ed.buf[ed.len] = 0;
    ed.dirty = 1;
    ed_remember();
    ed_reveal();
    snd(SND_KEY);
}

static void ed_delete(void)
{
    if (ed.cur >= ed.len) return;
    size_t n = ed_next(ed.cur);
    memmove(ed.buf + ed.cur, ed.buf + n, ed.len - n);
    ed.len -= n - ed.cur;
    ed.buf[ed.len] = 0;
    ed.dirty = 1;
    ed_remember();
    ed_reveal();
    snd(SND_KEY);
}

static void ed_left(void)
{
    ed.cur = ed_prev(ed.cur);
    ed_remember();
    ed_reveal();
    snd(SND_STEP);
}

static void ed_right(void)
{
    ed.cur = ed_next(ed.cur);
    ed_remember();
    ed_reveal();
    snd(SND_STEP);
}

/* up (negative) or down by lines, keeping the wanted column */
static void ed_vertical(int lines)
{
    size_t ls = ed_line_start(ed.cur);
    for (; lines < 0; lines++) {
        if (ls == 0) {
            ed.cur = 0;
            break;
        }
        ls = ed_line_start(ls - 1);
        ed.cur = ed_off_of(ls, ed_line_end(ls), ed.want);
    }
    for (; lines > 0; lines--) {
        size_t le = ed_line_end(ls);
        if (le >= ed.len) {
            ed.cur = ed.len;
            break;
        }
        ls = le + 1;
        ed.cur = ed_off_of(ls, ed_line_end(ls), ed.want);
    }
    if (ed.cur > ed.len) ed.cur = ed.len;
    ed_reveal();
    snd(SND_STEP);
}

static void ed_home(void)
{
    ed.cur = ed_line_start(ed.cur);
    ed_remember();
    ed_reveal();
    snd(SND_STEP);
}

static void ed_end(void)
{
    ed.cur = ed_line_end(ed.cur);
    ed_remember();
    ed_reveal();
    snd(SND_STEP);
}

static void ed_save(void)
{
    char tmp[640];
    snprintf(tmp, sizeof tmp, "%s.pixi-save", ed.path);
    FILE *f = fopen(tmp, "wb");
    int ok = f != NULL;
    if (f) {
        ok = fwrite(ed.buf, 1, ed.len, f) == ed.len;
        if (fflush(f) != 0) ok = 0;
        if (fclose(f) != 0) ok = 0;
        if (ok && rename(tmp, ed.path) != 0) ok = 0;
        if (!ok) unlink(tmp);
    }
    if (!ok) {
        show_message("Couldn't save", "The file couldn't be written. Check that there's room on the card.");
        snd(SND_ERROR);
        return;
    }
    ed.dirty = 0;
    char title[96] = "Saved", body[700] = "The file was saved.";
    if (ed.is_web) web_saved(title, sizeof title, body, sizeof body);
    show_message(title, body);
    snd(SND_SELECT);
}

static void ed_ask_close(void)
{
    if (!ed.dirty) {
        ed_close();
        return;
    }
    show_confirm("Close without saving?", "Your changes to this file will be lost.", A_ED_DISCARD, "");
    dlg.choice = 1; /* No is the safe default */
}

static void ed_page(int dir)
{
    int n = ed_rows() - 1;
    ed_vertical(dir * (n > 0 ? n : 1));
}

/* one key, from either keyboard */
static void ed_press(int type, const char *text, int ctrl, int shift)
{
    switch (type) {
    case KY_CHAR:
    case KY_SPACE:
        if (ctrl) {
            if (text[0] == 's' || text[0] == 'S') ed_save();
            else if (text[0] == 'x' || text[0] == 'X') ed_ask_close();
            else snd(SND_ERROR);
        } else ed_type(text);
        break;
    case KY_ENTER: ed_enter(); break;
    case KY_BKSP: ed_backspace(); break;
    case KY_DEL:
        if (shift) snd(SND_ERROR); /* Insert: there's only one way of typing here */
        else ed_delete();
        break;
    case KY_TAB:
        if (shift) snd(SND_ERROR);
        else {
            ed_insert("    ", 4);
            snd(SND_KEY);
        }
        break;
    case KY_UP: ed_vertical(-1); break;
    case KY_DOWN: ed_vertical(1); break;
    case KY_LEFT: ed_left(); break;
    case KY_RIGHT: ed_right(); break;
    case KY_HOME:
        if (shift) ed_end();
        else ed_home();
        break;
    case KY_PGUP: ed_page(shift ? 1 : -1); break;
    case KY_SUPER: handle_action(ACT_GUIDE); break;
    default: break; /* Esc and the rest do nothing here */
    }
}

static void ed_press_key(const PKey *k)
{
    PKOut o;
    if (!k || !pk_press(k, &o)) return;
    if (o.type == KY_CHAR && o.fkey) {
        snd(SND_ERROR); /* no F keys in an editor */
        return;
    }
    ed_press(o.type, o.text, o.ctrl, o.shift);
}

static void ed_action(int a)
{
    switch (a) {
    case ACT_UP:
    case ACT_DOWN:
    case ACT_LEFT:
    case ACT_RIGHT: pk_nav(a, &ed.r, &ed.c); break;
    case ACT_OK: ed_press_key(pk_at(ed.r, ed.c)); break;
    case ACT_BACK:
    case ACT_MENU: ed_backspace(); break;
    case ACT_PGUP: ed_type(" "); break; /* Square */
    case ACT_START: ed_save(); break;
    }
}

/* while a button is held: the keys that repeat */
static void ed_repeat_key(int a)
{
    if (a == ACT_BACK) ed_backspace();
    else if (a == ACT_OK) ed_press_key(pk_at(ed.r, ed.c));
}

static int ed_active(void) { return ed.open && !hidden && !guide.open && dlg.kind == D_NONE; }

/* a real keyboard. Returns 1 when it used the key. */
static int ed_sdl_key(const SDL_KeyboardEvent *k)
{
    if (!ed.open || guide.open || dlg.kind != D_NONE || hidden) return 0;
    SDL_Keycode sym = k->keysym.sym;
    if (sym == SDLK_F1) return 0; /* the guide */
    int shift = (k->keysym.mod & KMOD_SHIFT) != 0, ctrl = (k->keysym.mod & KMOD_CTRL) != 0;
    if (ctrl && !k->repeat) {
        if (sym == SDLK_s) {
            ed_save();
            return 1;
        }
        if (sym == SDLK_x) {
            ed_ask_close();
            return 1;
        }
    }
    switch (sym) {
    case SDLK_RETURN: case SDLK_KP_ENTER: ed_enter(); break;
    case SDLK_BACKSPACE: ed_backspace(); break;
    case SDLK_DELETE: ed_delete(); break;
    case SDLK_TAB: ed_press(KY_TAB, "", 0, shift); break;
    case SDLK_UP: ed_vertical(-1); break;
    case SDLK_DOWN: ed_vertical(1); break;
    case SDLK_LEFT: ed_left(); break;
    case SDLK_RIGHT: ed_right(); break;
    case SDLK_HOME:
        if (ctrl) {
            ed.cur = 0;
            ed_remember();
            ed_reveal();
        } else ed_home();
        break;
    case SDLK_END:
        if (ctrl) {
            ed.cur = ed.len;
            ed_remember();
            ed_reveal();
        } else ed_end();
        break;
    case SDLK_PAGEUP: ed_page(-1); break;
    case SDLK_PAGEDOWN: ed_page(1); break;
    default: break;
    }
    return 1; /* printable keys arrive as text; nothing else should reach the menus */
}

static void ed_sdl_text(const char *s)
{
    if (!ed_active()) return;
    if (SDL_GetModState() & KMOD_CTRL) return;
    ed_type(s);
}

/* ---------- drawing ----------------------------------------------------- */

static void draw_editor(void)
{
    ed_layout();
    ed_reveal();
    float bx, by, bw, bh, ty, btn_h;
    ed_geometry(&bx, &by, &bw, &bh, &ty, &btn_h);
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.28f }, 0, 0, 0, 1, 0);
    draw_page_header("file-page", ed.title);

    /* the text box */
    pane(bx, by, bw, bh, 14 * S, SQUIRCLE, (Col){ 0, 0, 0, 0.38f }, DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_QUIET, 1, 0);
    int rows = ed_rows(), cols = ed_cols();
    float tx = bx + 14 * S, tyy = by + 8 * S, fs = (float)ed.fpx, lift = fs * 0.18f;
    int cur_line = ed_line_of(ed.cur), cur_col = ed_col_of(ed_line_start(ed.cur), ed.cur);
    size_t ls = ed_start_of_line(ed.top);
    for (int r = 0; r < rows; r++) {
        if (ls > ed.len) break;
        size_t le = ed_line_end(ls);
        float ry = tyy + r * ed.chh;
        /* the line from the left edge of the view, with tabs spread out */
        char vis[1024];
        size_t o = 0;
        int col = 0;
        for (size_t i = ls; i < le && col < ed.left + cols && o + 8 < sizeof vis; i = ed_next(i)) {
            if (ed.buf[i] == '\t') {
                int w = 4 - col % 4;
                for (int t = 0; t < w; t++, col++)
                    if (col >= ed.left && o + 8 < sizeof vis) vis[o++] = ' ';
            } else {
                size_t n = ed_next(i) - i;
                if (col >= ed.left && o + n + 8 < sizeof vis) {
                    memcpy(vis + o, ed.buf + i, n);
                    o += n;
                }
                col++;
            }
        }
        while (o > 0 && vis[o - 1] == ' ') o--; /* trailing spaces have nothing to draw */
        vis[o] = 0;
        if (ed.top + r == cur_line) { /* the caret goes under the text */
            float px = floorf(tx + (cur_col - ed.left) * ed.cw);
            float pulse = 0.78f + 0.22f * sinf(now_s * 5.0f);
            pane(px, floorf(ry), ceilf(ed.cw), ceilf(ed.chh), 0, CIRCLE, with_alpha(th.accent, pulse), 0, 0, 0, 1, 0);
        }
        if (o) text(vis, tx, ry + lift, fs, 2, th.text, 1, AL_LEFT, 0);
        if (le >= ed.len) break;
        ls = le + 1;
    }
    int total = ed_line_count();
    if (total > rows) { /* a thin scroll bar */
        float th_ = bh - 20 * S, hh = fmaxf(24 * S, th_ * (float)rows / (float)total);
        float yy = by + 10 * S + (th_ - hh) * (float)ed.top / (float)(total - rows > 0 ? total - rows : 1);
        pane(bx + bw - 9 * S, by + 10 * S, 3 * S, th_, 1.5f * S, CIRCLE, with_alpha(th.text, 0.12f), 0, 0, 0, 1, 0);
        pane(bx + bw - 9 * S, yy, 3 * S, hh, 1.5f * S, CIRCLE, with_alpha(th.text, 0.5f), 0, 0, 0, 1, 0);
    }

    /* what the keys do, and where the caret is */
    text("Ctrl+S  Save      Ctrl+X  Close", bx + 4 * S, ty + btn_h / 2 - 10 * S, 19 * S, 0, th.text, 0.9f, AL_LEFT, (int)(bw * 0.5f));
    char st[160];
    snprintf(st, sizeof st, "%sLine %d, column %d", ed.dirty ? "Not saved     " : "", cur_line + 1, cur_col + 1);
    text(st, bx + bw - 4 * S, ty + btn_h / 2 - 10 * S, 19 * S, 0, th.text_soft, 1, AL_RIGHT, (int)(bw * 0.4f));

    /* the keyboard */
    float y0 = (float)H - ed.kbh, pad = 6 * S;
    pane(0, y0, (float)W, ed.kbh + 40 * S, 0, SQUIRCLE, with_alpha(th.glass, 0.92f), DEPTH_PANEL * S, FROST_PANEL, GLOSS_QUIET, 1, 1);
    pane(0, y0, (float)W, 2 * S, 0, CIRCLE, with_alpha(th.text, 0.30f), 0, 0, 0, 1, 0);
    pk_draw(pad, y0 + pad, W - 2 * pad, ed.kbh - 2 * pad, ed.r, ed.c, 1, 0);
}

/* ======================================================================== */
/* Web server: nginx, with PHP, serving the www folder                       */
/* ======================================================================== */

/* An app of its own, like file transfer: Back goes to the home screen and leaves it serving, it has a
 * card in the Guide, and Start (or Close in the Guide) stops it. Triangle edits the settings file. */
static struct {
    int open, running;
} webui;

static int web_ui_is_open(void) { return webui.open; }
static int web_ui_running(void) { return webui.running && web_running(); }

static void web_ui_open(void)
{
    if (web_ui_running()) {
        app_hide_all();
        webui.open = 1;
        snd(SND_SELECT);
        return;
    }
    webui.running = 0;
    if (!web_installed()) {
        show_confirm("The web server needs nginx", "It isn't installed yet. Install it now, with PHP?", A_WEB_INSTALL, "");
        return;
    }
    char err[300] = "";
    if (web_start(home_dir(), cfg.web_port, err, sizeof err) != 0) {
        show_message("Couldn't start the web server", err);
        snd(SND_ERROR);
        return;
    }
    webui.running = 1;
    app_hide_all();
    webui.open = 1;
    if (stack[depth]) menu_rebuild(stack[depth]);
    snd(SND_SELECT);
}

static void web_ui_stop(void)
{
    if (ed_is_open()) ed_close();
    if (webui.running) web_stop();
    webui.running = webui.open = 0;
    if (stack[depth]) menu_rebuild(stack[depth]);
}

/* the settings file, in Pixi's own editor; saving checks the file and loads it if the server is up */
static void web_edit_config(void)
{
    if (web_config_ensure(home_dir(), cfg.web_port) != 0) {
        show_message("Couldn't open the settings", "The web server's settings file couldn't be made.");
        snd(SND_ERROR);
        return;
    }
    ed_open(web_config_path(), "Web server settings", 1);
}

/* what the Saved dialog says once the settings file is written */
static void web_saved(char *title, size_t tc, char *body, size_t bc)
{
    char out[600];
    if (web_config_test(out, sizeof out) != 0) {
        sncpy(title, tc, "Saved, but there's a mistake");
        snprintf(body, bc, "The web server can't use this file yet, so it keeps the old settings. %s", out);
    } else if (web_ui_running()) {
        web_reload();
        sncpy(title, tc, "Saved");
        sncpy(body, bc, "The settings were saved and the web server loaded them.");
    } else {
        sncpy(title, tc, "Saved");
        sncpy(body, bc, "The settings were saved. The web server reads them when it starts.");
    }
}

static void web_reload_now(void)
{
    char out[600];
    if (!web_ui_running()) {
        show_message("The web server is off", "Start it first. It reads its settings file when it starts.");
        return;
    }
    if (web_config_test(out, sizeof out) != 0) {
        show_message("There's a mistake in the settings file", out);
        snd(SND_ERROR);
        return;
    }
    web_reload();
    show_message("Web server", "The settings were loaded.");
    snd(SND_SELECT);
}

static void web_reset_now(void)
{
    if (web_config_reset(home_dir(), cfg.web_port) != 0) {
        show_message("Couldn't restore the settings", "The settings file couldn't be written.");
        snd(SND_ERROR);
        return;
    }
    if (web_ui_running()) web_reload();
    snd(SND_SELECT);
}

static void web_tick(void)
{
    if (webui.running && !web_running()) { /* it stopped by itself */
        webui.running = webui.open = 0;
        if (stack[depth]) menu_rebuild(stack[depth]);
        show_message("The web server stopped", "Check ~/.cache/pixi/nginx/error.log for why.");
    }
}

static void web_ui_action(int a)
{
    if (a == ACT_BACK) { /* to the home screen; it keeps serving */
        webui.open = 0;
        if (stack[depth]) menu_rebuild(stack[depth]);
        snd(SND_BACK);
    } else if (a == ACT_START) {
        web_ui_stop();
        snd(SND_BACK);
    } else if (a == ACT_MENU) {
        web_edit_config();
    } else if (a == ACT_SQUARE) {
        web_reload_now();
    }
}

/* a QR code for a web address, in white with a quiet border so a phone can read it */
static void draw_qr(const char *url, float cx, float cy, float size)
{
    static char cached[200];
    static QRcode *code;
    if (strcmp(cached, url)) {
        if (code) QRcode_free(code);
        code = QRcode_encodeString(url, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
        sncpy(cached, sizeof cached, url);
    }
    if (!code) return;
    int n = code->width, border = 3;
    float m = size / (float)(n + 2 * border), x0 = cx - size / 2, y0 = cy - size / 2;
    pane(x0, y0, size, size, 12 * S, SQUIRCLE, (Col){ 1, 1, 1, 1 }, DEPTH_CONTROL * S, 0, 0, 1, 0);
    const Col ink = { 0.04f, 0.04f, 0.07f, 1 };
    for (int r = 0; r < n; r++) { /* runs of dark modules along a row are one rectangle */
        for (int c = 0; c < n;) {
            if (!(code->data[r * n + c] & 1)) {
                c++;
                continue;
            }
            int c2 = c;
            while (c2 < n && (code->data[r * n + c2] & 1)) c2++;
            pane(floorf(x0 + (border + c) * m), floorf(y0 + (border + r) * m), ceilf((c2 - c) * m), ceilf(m), 0, CIRCLE, ink, 0, 0, 0, 1, 0);
            c = c2;
        }
    }
}

static void draw_web(void)
{
    WebStatus st;
    web_status(&st);
    char ip[64];
    ftp_address(ip, sizeof ip);
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.28f }, 0, 0, 0, 1, 0);
    draw_page_header("category-internet", "Web Server");
    char url[160] = "";
    if (ip[0]) {
        if (st.port == 80) snprintf(url, sizeof url, "http://%s/", ip);
        else snprintf(url, sizeof url, "http://%s:%d/", ip, st.port);
    }
    int qr = url[0] != 0;                            /* the text moves left to make room for the code */
    float cx = qr ? W * 0.37f : W / 2.0f, cy = H * 0.34f;
    float tw_max = qr ? W * 0.68f : W * 0.9f;
    text("Web Server", cx, cy - 22 * S, 42 * S, 1, th.text, 1, AL_CENTER, 0);
    float lw = W * (qr ? 0.36f : 0.46f), ly = cy + 54 * S;
    pane(cx - lw / 2, ly, lw, 3 * S, 1.5f * S, CIRCLE, with_alpha(th.text, 0.25f), 0, 0, 0, 1, 0);
    float seg = lw * 0.28f, p = fmodf(now_s * 0.5f, 1.0f);
    float sx = cx - lw / 2 + (lw - seg) * (0.5f - 0.5f * cosf(p * 6.2832f));
    pane(sx, ly - 0.5f * S, seg, 4 * S, 2 * S, CIRCLE, with_alpha(th.text, 0.9f), 0, 0, 0, 1, 0);
    float ty = ly + 44 * S, step = 36 * S;
    char line[400];
    if (ip[0] && st.port == 80) snprintf(line, sizeof line, "Address:  http://%s", ip);
    else if (ip[0]) snprintf(line, sizeof line, "Address:  http://%s:%d", ip, st.port);
    else snprintf(line, sizeof line, "Not connected to a network. Connect to Wi-Fi first.");
    text(line, cx, ty, 26 * S, 1, th.text, 1, AL_CENTER, (int)tw_max);
    snprintf(line, sizeof line, "Folder:  ~/www");
    text(line, cx, ty + step, 24 * S, 0, th.text, 1, AL_CENTER, (int)tw_max);
    if (st.php) snprintf(line, sizeof line, "PHP %s is on. Pages ending in .php run.", st.php_version);
    else if (st.php_version[0]) snprintf(line, sizeof line, "PHP %s is installed but isn't running.", st.php_version);
    else snprintf(line, sizeof line, "PHP isn't installed. Install php-fpm to run .php pages.");
    text(line, cx, ty + step * 2, 24 * S, 0, th.text, 1, AL_CENTER, (int)tw_max);
    text("Settings:  ~/.config/pixi/nginx/nginx.conf", cx, ty + step * 2.8f, 21 * S, 0, th.text_soft, 1, AL_CENTER, (int)tw_max);
    if (st.last[0]) text(st.last, cx, ty + step * 3.7f, 18 * S, 0, th.text_soft, 1, AL_CENTER, (int)tw_max);
    if (qr) {
        float qs = fminf(H * 0.40f, W * 0.24f);
        float qx = W * 0.795f, qy = cy + 30 * S + qs / 2;
        draw_qr(url, qx, qy, qs);
        text("Scan to open the site", qx, qy + qs / 2 + 10 * S, 19 * S, 0, th.text, 0.9f, AL_CENTER, (int)(W * 0.3f));
    }
    text("Anyone on your network can open this page. Don't put private files in www.", cx, (float)H - 96 * S, 19 * S, 0, th.text_soft, 0.8f, AL_CENTER, (int)(W * 0.9f));
    Hint h[4] = { { "pad-start", "Stop server" }, { "pad-north", "Edit settings" }, { "pad-west", "Reload" }, { pad_back(), "Home screen" } };
    draw_hints(h, 4);
}

/* ======================================================================== */
/* App Settings: a page like File Transfer, from an app's Options           */
/* ======================================================================== */

enum { APS_INPUT, APS_SUDO, APS_ROWS };

static struct {
    int open, sel;
    char id[160], name[160];
} aps;

static int aps_is_open(void) { return aps.open; }

static void aps_open(const char *id, const char *name)
{
    memset(&aps, 0, sizeof aps);
    sncpy(aps.id, sizeof aps.id, id);
    sncpy(aps.name, sizeof aps.name, name);
    aps.open = 1;
    snd(SND_SELECT);
}

static void aps_change(void)
{
    if (aps.sel == APS_INPUT) {
        appset_set_input(aps.id, appset_input(aps.id) == PAD_MOUSE ? PAD_GAMEPAD : PAD_MOUSE);
        snd(SND_STEP);
    } else if (aps.sel == APS_SUDO) {
        appset_set_sudo(aps.id, !appset_sudo(aps.id));
        snd(SND_STEP);
    }
}

static void aps_action(int a)
{
    switch (a) {
    case ACT_BACK:
    case ACT_START:
        aps.open = 0;
        snd(SND_BACK);
        break;
    case ACT_UP:
        if (aps.sel > 0) aps.sel--, snd(SND_STEP);
        break;
    case ACT_DOWN:
        if (aps.sel < APS_ROWS - 1) aps.sel++, snd(SND_STEP);
        break;
    case ACT_LEFT:
    case ACT_RIGHT:
    case ACT_OK: aps_change(); break;
    }
}

static void draw_appsettings(void)
{
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.28f }, 0, 0, 0, 1, 0);
    draw_page_header("setting-input", "App Settings");
    float cx = W / 2.0f;
    text(aps.name, cx, 98 * S, 34 * S, 1, th.text, 1, AL_CENTER, (int)(W * 0.8f));
    float lw = W * 0.46f;
    pane(cx - lw / 2, 150 * S, lw, 3 * S, 1.5f * S, CIRCLE, with_alpha(th.text, 0.25f), 0, 0, 0, 1, 0);

    /* the settings, one to a row: its name, and its value between arrows */
    float rw = W * 0.64f, rh = 58 * S, rx = cx - rw / 2, ry = 176 * S;
    for (int i = 0; i < APS_ROWS; i++) {
        int sel = aps.sel == i;
        float y = ry + (float)i * (rh + 10 * S);
        pane(rx, y, rw, rh, rh / 2, SQUIRCLE, sel ? with_alpha(th.accent, 0.62f) : with_alpha(th.glass_raised, 0.09f), DEPTH_CONTROL * S,
             FROST_CONTROL, sel ? GLOSS_FULL : GLOSS_QUIET, 1, 0);
        const char *name = i == APS_INPUT ? "Start in" : "Allow Auto sudo";
        const char *val = i == APS_INPUT ? (appset_input(aps.id) == PAD_MOUSE ? "Keyboard + Mouse" : "Gamepad") : (appset_sudo(aps.id) ? "On" : "Off");
        text(name, rx + 28 * S, y + rh / 2 - 13 * S, 25 * S, sel, th.text, 1, AL_LEFT, (int)(rw * 0.45f));
        float vx = rx + rw - 28 * S - 28 * S;
        glyph("arrow-right", vx + 14 * S, y + rh / 2, 26 * S, th.text, sel ? 1 : 0.5f);
        float tw = text(val, vx - 8 * S, y + rh / 2 - 13 * S, 25 * S, 1, th.text, 1, AL_RIGHT, (int)(rw * 0.5f));
        glyph("arrow-left", vx - 8 * S - tw - 22 * S, y + rh / 2, 26 * S, th.text, sel ? 1 : 0.5f);
    }

    /* what the choice means */
    float ty = ry + (float)APS_ROWS * (rh + 10 * S) + 24 * S, step = 31 * S;
    if (aps.sel == APS_INPUT) {
        if (appset_input(aps.id) == PAD_MOUSE) {
            const char *lines[4] = { "The gamepad works as a mouse and keyboard.",
                                     "Left stick moves the pointer and the right stick scrolls.",
                                     "Confirm clicks and Back right-clicks. The D-pad sends the arrow keys.",
                                     "Start is Enter, Select is Tab and the square button is Esc." };
            for (int i = 0; i < 4; i++) text(lines[i], cx, ty + (float)i * step, 21 * S, i == 0, th.text, 1, AL_CENTER, (int)(W * 0.92f));
        } else {
            text("The app gets the gamepad as it is.", cx, ty, 21 * S, 1, th.text, 1, AL_CENTER, (int)(W * 0.92f));
            text("Use this for games and apps made to work with a controller.", cx, ty + step, 21 * S, 0, th.text, 1, AL_CENTER, (int)(W * 0.92f));
        }
        text("It can also be changed while the app runs, with the third button in the Guide.", cx, (float)H - 96 * S, 19 * S, 0, th.text_soft, 0.8f,
             AL_CENTER, (int)(W * 0.92f));
    } else if (appset_sudo(aps.id)) {
        text("The app starts as administrator (root), through sudo.", cx, ty, 21 * S, 1, th.text, 1, AL_CENTER, (int)(W * 0.92f));
        text("No need to type sudo. If a password is needed, Pixi asks for it on screen.", cx, ty + step, 21 * S, 0, th.text, 1, AL_CENTER, (int)(W * 0.92f));
        text("Only turn this on for apps you trust that need it, like disk tools.", cx, ty + step * 2, 21 * S, 0, th.text, 1, AL_CENTER, (int)(W * 0.92f));
    } else {
        text("The app starts as you, like any other.", cx, ty, 21 * S, 1, th.text, 1, AL_CENTER, (int)(W * 0.92f));
        text("If it asks for administrator access itself, Pixi still shows a password box.", cx, ty + step, 21 * S, 0, th.text, 1, AL_CENTER, (int)(W * 0.92f));
    }
    Hint h[2] = { { pad_ok(), "Change" }, { pad_back(), "Back" } };
    draw_hints(h, 2);
}


/* ======================================================================== */
/* Apps List: every app in a tree, with a check box to hide it from the home screen */
/* ======================================================================== */

typedef struct {
    int folder, depth;
    char label[100], id[200];
} AlRow;

static struct {
    int open, sel, top, n, cap;
    AlRow *r;
} al;

static int alist_is_open(void) { return al.open; }

static int al_strcmp(const void *a, const void *b) { return natural_cmp(*(char *const *)a, *(char *const *)b); }

static AlRow *al_add(int folder, int depth_, const char *label, const char *id)
{
    if (al.n >= al.cap) {
        al.cap = al.cap ? al.cap * 2 : 128;
        al.r = realloc(al.r, sizeof *al.r * (size_t)al.cap);
    }
    AlRow *r = &al.r[al.n++];
    memset(r, 0, sizeof *r);
    r->folder = folder;
    r->depth = depth_;
    sncpy(r->label, sizeof r->label, label);
    if (id) sncpy(r->id, sizeof r->id, id);
    return r;
}

/* the rows below a folder, down to the next row that is as shallow as it is */
static int al_end(int i)
{
    int e = i + 1;
    while (e < al.n && al.r[e].depth > al.r[i].depth) e++;
    return e;
}

/* a folder is ticked when everything in it is hidden */
static int al_ticked(int i)
{
    if (!al.r[i].folder) return appset_hidden(al.r[i].id);
    int any = 0;
    for (int k = i + 1; k < al_end(i); k++)
        if (!al.r[k].folder) {
            any = 1;
            if (!appset_hidden(al.r[k].id)) return 0;
        }
    return any;
}

static void alist_open(void)
{
    free(al.r);
    memset(&al, 0, sizeof al);
    al_add(0, 0, "Files", "pixi:files");
    al_add(1, 0, "Apps", NULL);
    al_add(0, 1, "Web Browser", "pixi:browser");
    al_add(0, 1, "Terminal", "pixi:terminal");
    for (int c = CAT_INTERNET; c < CAT_COUNT; c++) {
        int have = 0;
        for (int i = 0; i < apps.n; i++) have += apps.v[i].category == c;
        if (!have) continue;
        al_add(1, 1, cat_names[c], NULL);
        for (int i = 0; i < apps.n; i++)
            if (apps.v[i].category == c) al_add(0, 2, apps.v[i].name, apps.v[i].id);
    }
    al_add(1, 0, "Games", NULL);
    for (int i = 0; i < apps.n; i++)
        if (apps.v[i].category == CAT_GAMES) al_add(0, 1, apps.v[i].name, apps.v[i].id);
    char *root = expand_home(cfg.pc_root);
    DIR *dp = opendir(root);
    if (dp) {
        StrList dirs = { 0 };
        struct dirent *de;
        while ((de = readdir(dp)))
            if (de->d_name[0] != '.') {
                char *pth = xasprintf("%s/%s", root, de->d_name);
                struct stat st;
                char exe[1100], icon[1100];
                if (stat(pth, &st) == 0 && S_ISDIR(st.st_mode) && pc_game_find(pth, exe, sizeof exe, icon, sizeof icon)) sl_add(&dirs, de->d_name);
                free(pth);
            }
        closedir(dp);
        if (dirs.n) {
            qsort(dirs.v, (size_t)dirs.n, sizeof *dirs.v, al_strcmp);
            al_add(1, 1, "PC", NULL);
            for (int i = 0; i < dirs.n; i++) {
                char title[200], id[220];
                sncpy(title, sizeof title, dirs.v[i]);
                for (char *q = title; *q; q++)
                    if (*q == '_') *q = ' ';
                snprintf(id, sizeof id, "pc:%s", dirs.v[i]);
                al_add(0, 2, title, id);
            }
        }
        sl_free(&dirs);
    }
    free(root);
    al.sel = 0;
    al.open = 1;
    snd(SND_SELECT);
}

static void alist_close(void)
{
    appsets_save();
    al.open = 0;
    free(al.r);
    al.r = NULL;
    al.n = al.cap = 0;
    homescreen_refresh();
    snd(SND_BACK);
}

static void alist_toggle(void)
{
    if (al.sel < 0 || al.sel >= al.n) return;
    int on = !al_ticked(al.sel);
    if (al.r[al.sel].folder) {
        for (int k = al.sel + 1; k < al_end(al.sel); k++)
            if (!al.r[k].folder) appset_set_hidden(al.r[k].id, on);
    } else {
        appset_set_hidden(al.r[al.sel].id, on);
    }
    appsets_save();
    snd(SND_STEP);
}

static void alist_action(int a)
{
    switch (a) {
    case ACT_BACK:
    case ACT_START: alist_close(); break;
    case ACT_UP:
        if (al.n > 1) al.sel = al.sel > 0 ? al.sel - 1 : al.n - 1, snd(SND_STEP);
        break;
    case ACT_DOWN:
        if (al.n > 1) al.sel = al.sel < al.n - 1 ? al.sel + 1 : 0, snd(SND_STEP);
        break;
    case ACT_PGUP: al.sel = al.sel > 5 ? al.sel - 6 : 0; snd(SND_STEP); break;
    case ACT_PGDN: al.sel = al.sel + 6 < al.n ? al.sel + 6 : al.n - 1; snd(SND_STEP); break;
    case ACT_OK:
    case ACT_LEFT:
    case ACT_RIGHT: alist_toggle(); break;
    }
}

static void draw_alist(void)
{
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.28f }, 0, 0, 0, 1, 0);
    draw_page_header("category-software", "Apps List");
    float cx = W / 2.0f;
    text("Tick an item to hide it from the home screen", cx, 90 * S, 22 * S, 0, th.text_soft, 1, AL_CENTER, (int)(W * 0.9f));
    float rw = W * 0.64f, rh = 46 * S, rx = cx - rw / 2, ry = 130 * S;
    int rows = (int)floorf(((float)H - ry - 110 * S) / rh);
    if (rows < 3) rows = 3;
    if (al.sel < al.top) al.top = al.sel;
    if (al.sel >= al.top + rows) al.top = al.sel - rows + 1;
    if (al.top > al.n - rows) al.top = al.n - rows;
    if (al.top < 0) al.top = 0;
    for (int k = 0; k < rows && al.top + k < al.n; k++) {
        int i = al.top + k;
        const AlRow *r = &al.r[i];
        float y = ry + (float)k * rh;
        int sel = i == al.sel;
        pane(rx, y, rw, rh - 6 * S, (rh - 6 * S) / 2, SQUIRCLE, sel ? with_alpha(th.accent, 0.62f) : with_alpha(th.glass_raised, 0.07f), DEPTH_CONTROL * S,
             FROST_CONTROL, sel ? GLOSS_FULL : GLOSS_QUIET, 1, 0);
        float x = rx + 26 * S + (float)r->depth * 34 * S;
        mark_draw(2, al_ticked(i), x + 14 * S, y + (rh - 6 * S) / 2, 28 * S, th.text, 1);
        x += 40 * S;
        if (r->folder) {
            glyph("file-folder", x + 14 * S, y + (rh - 6 * S) / 2, 28 * S, th.text, 1);
            x += 40 * S;
        }
        text(r->label, x, y + (rh - 6 * S) / 2 - 12 * S, 23 * S, sel || r->folder, th.text, 1, AL_LEFT, (int)(rx + rw - x - 20 * S));
    }
    if (al.n > rows) { /* where the window is in the list */
        float th_ = (float)rows * rh, bh = th_ * (float)rows / (float)al.n, by = ry + (th_ - bh) * (float)al.top / (float)(al.n - rows);
        pane(rx + rw + 10 * S, by, 4 * S, bh, 2 * S, CIRCLE, with_alpha(th.text, 0.4f), 0, 0, 0, 1, 0);
    }
    Hint h[2] = { { pad_ok(), "Hide / Show" }, { pad_back(), "Back" } };
    draw_hints(h, 2);
}

/* ======================================================================== */
/* The web browser: Chromium underneath, a PSP-style screen on top          */
/* ======================================================================== */

#define BM_MAX 64
enum { BV_MAIN, BV_BOOKMARKS, BV_HISTORY, BV_TEXT, BV_MODE };
enum { BI_ADDR = 1, BI_BACK, BI_FWD, BI_RELOAD, BI_HOME, BI_ADDBM, BI_BOOKMARKS, BI_HISTORY, BI_NEWTAB, BI_TEXT, BI_MODE, BI_FULL, BI_CLEAR,
       BI_HOMESCREEN, BI_QUIT, BI_GO, BI_SETTEXT, BI_SETMODE };

typedef struct {
    char label[100], sub[200], url[600];
    const char *glyph;
    int id, arg, enabled, divider, check, ck; /* ck: 1 radio button, 2 check box, 0 a badge on the one in force */
    int tb[4], ntb, tsel, urlbar; /* a row of small buttons, or the address field */
} BItem;

typedef struct {
    BItem it[BM_MAX];
    int n, sel;
    float spos, anim;
} BList;

static struct {
    int open, running, need_home, panel, view, hidden, zoom, mobile, probing, cursor_init;
    float cx, cy;
    BList tabs, menu;
    GLuint tex;
    int tw, th;
    unsigned seq;
    Uint32 probe_at, probe_until, last_move, last_wheel, last_tick;
    float last_sx, last_sy;
    char last_hist[600], status[100], home_url[700];
    float status_until;
    int osk_enter;
} bui;

static const int br_zoom_css[3] = { 700, 900, 1200 };
static const char *br_zoom_name[3] = { "Large", "Normal", "Small" };

static struct {
    char url[600], title[100];
} bms[BM_MAX];
static int nbm;

static void cfg_file(const char *name, char *out, size_t cap) { snprintf(out, cap, "%s/.config/pixi/%s", home_dir(), name); }

static void bm_save(void)
{
    char p[400];
    cfg_file("bookmarks.txt", p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < nbm; i++) fprintf(f, "%s\t%s\n", bms[i].url, bms[i].title);
    fclose(f);
}

static void bm_clean(char *s)
{
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

static void bm_load(void)
{
    char dir[400], p[400];
    snprintf(dir, sizeof dir, "%s/.config", home_dir());
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.config/pixi", home_dir());
    mkdir(dir, 0755);
    cfg_file("bookmarks.txt", p, sizeof p);
    nbm = 0;
    FILE *f = fopen(p, "r");
    if (!f) {
        static const char *seed[][2] = { { "https://duckduckgo.com", "DuckDuckGo" },
                                         { "https://en.wikipedia.org", "Wikipedia" },
                                         { "https://www.raspberrypi.com", "Raspberry Pi" },
                                         { "https://github.com", "GitHub" },
                                         { "https://news.ycombinator.com", "Hacker News" },
                                         { "https://www.youtube.com", "YouTube" } };
        for (size_t i = 0; i < sizeof seed / sizeof *seed; i++) {
            sncpy(bms[nbm].url, sizeof bms[nbm].url, seed[i][0]);
            sncpy(bms[nbm].title, sizeof bms[nbm].title, seed[i][1]);
            nbm++;
        }
        bm_save();
        return;
    }
    char line[800];
    while (nbm < BM_MAX && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab || tab == line) continue;
        *tab = 0;
        sncpy(bms[nbm].url, sizeof bms[nbm].url, line);
        sncpy(bms[nbm].title, sizeof bms[nbm].title, tab + 1);
        nbm++;
    }
    fclose(f);
}

static int bm_find(const char *url)
{
    for (int i = 0; i < nbm; i++)
        if (!strcmp(bms[i].url, url)) return i;
    return -1;
}

static void html_esc(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 8 < cap; in++) {
        const char *r = *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '&' ? "&amp;" : *in == '"' ? "&quot;" : NULL;
        if (r) o += (size_t)snprintf(out + o, cap - o, "%s", r);
        else out[o++] = *in;
    }
    out[o] = 0;
}

/* the start page: one big tile for each bookmark */
static void home_write(void)
{
    char p[400];
    cfg_file("home.html", p, sizeof p);
    snprintf(bui.home_url, sizeof bui.home_url, "file://%s", p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    fputs("<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>Start page</title><style>"
          "body{margin:0;min-height:100vh;font-family:sans-serif;color:#fff;background:linear-gradient(160deg,#16243a,#0b0f18)}"
          "h1{margin:0;padding:34px 40px 6px;font-size:34px;font-weight:600;letter-spacing:.5px}p{margin:0;padding:0 40px 18px;opacity:.6;font-size:18px}"
          "#g{display:grid;grid-template-columns:repeat(auto-fill,minmax(210px,1fr));gap:18px;padding:12px 40px 40px}"
          "a{display:block;text-decoration:none;color:#fff;border-radius:18px;padding:22px 18px;height:96px;box-sizing:border-box;position:relative;overflow:hidden}"
          "a b{display:block;font-size:42px;line-height:48px;opacity:.9}a span{position:absolute;left:18px;right:18px;bottom:14px;font-size:20px;white-space:nowrap;"
          "overflow:hidden;text-overflow:ellipsis}</style></head><body><h1>Pixi Browser</h1><p>Speed dial \xE2\x80\x94 add pages from the menu</p><div id=g>",
          f);
    for (int i = 0; i < nbm; i++) {
        unsigned h = 0;
        for (const char *c = bms[i].title; *c; c++) h = h * 31 + (unsigned char)*c;
        char u[1300], t[300], ini[8] = "";
        html_esc(bms[i].url, u, sizeof u);
        html_esc(bms[i].title, t, sizeof t);
        const unsigned char *c0 = (const unsigned char *)bms[i].title;
        size_t l = *c0 < 0x80 ? 1 : *c0 >= 0xF0 ? 4 : *c0 >= 0xE0 ? 3 : 2;
        if (strlen(bms[i].title) >= l) memcpy(ini, bms[i].title, l), ini[l] = 0;
        if (ini[0] >= 'a' && ini[0] <= 'z') ini[0] = (char)(ini[0] - 32);
        fprintf(f, "<a href=\"%s\" style=\"background:hsl(%u,45%%,36%%)\"><b>%s</b><span>%s</span></a>", u, h % 360, ini, t);
    }
    fputs("</div></body></html>", f);
    fclose(f);
}

static void hist_add(const char *url, const char *title)
{
    char p[400];
    cfg_file("history.txt", p, sizeof p);
    struct stat st;
    if (stat(p, &st) == 0 && st.st_size > 256 * 1024) unlink(p);
    FILE *f = fopen(p, "a");
    if (!f) return;
    char t[100];
    sncpy(t, sizeof t, title);
    bm_clean(t);
    fprintf(f, "%s\t%s\n", url, t);
    fclose(f);
}

static BItem *bl_add(BList *l, const char *label, const char *sub, const char *glyph_name, int id, int enabled)
{
    if (l->n >= BM_MAX) return &l->it[BM_MAX - 1];
    BItem *b = &l->it[l->n++];
    memset(b, 0, sizeof *b);
    sncpy(b->label, sizeof b->label, label);
    if (sub) sncpy(b->sub, sizeof b->sub, sub);
    b->glyph = glyph_name;
    b->id = id;
    b->enabled = enabled;
    return b;
}

static void bl_divider(BList *l) { bl_add(l, "", NULL, NULL, 0, 0)->divider = 1; }

static const char *br_show_url(const char *u, char *buf, size_t cap)
{
    if (!strncmp(u, "file://", 7) && strstr(u, "/pixi/home.html")) return "Start page";
    if (!u[0] || !strcmp(u, "about:blank")) return "New tab";
    if (!strncmp(u, "https://", 8)) u += 8;
    else if (!strncmp(u, "http://", 7)) u += 7;
    if (!strncmp(u, "www.", 4)) u += 4;
    sncpy(buf, cap, u);
    return buf;
}

static void menu_build(int view)
{
    BList *l = &bui.menu;
    BrState st;
    br_get(&st);
    int cur = st.active >= 0 && st.active < st.ntabs ? st.active : 0;
    memset(l->it, 0, sizeof l->it);
    l->n = 0;
    l->sel = 0;
    l->spos = 0;
    bui.view = view;
    if (view == BV_MAIN) {
        bl_add(l, "", NULL, "setting-address", BI_ADDR, 1)->urlbar = 1;
        BItem *tbi = bl_add(l, "", NULL, NULL, 0, 1);
        tbi->ntb = 4;
        tbi->tb[0] = BI_HOME, tbi->tb[1] = BI_BACK, tbi->tb[2] = BI_FWD, tbi->tb[3] = BI_RELOAD;
        tbi->tsel = 1;
        bl_divider(l);
        int has = st.ntabs && bm_find(st.tabs[cur].url) >= 0;
        bl_add(l, has ? "Remove bookmark" : "Bookmark this page", NULL, "add", BI_ADDBM,
               st.ntabs && strncmp(st.tabs[cur].url, "about:", 6) != 0 && !strstr(st.tabs[cur].url, "/pixi/home.html"));
        bl_add(l, "Bookmarks", NULL, "setting-address", BI_BOOKMARKS, 1);
        bl_add(l, "History", NULL, "setting-schedule", BI_HISTORY, 1);
        bl_divider(l);
        bl_add(l, "New tab", NULL, "category-internet", BI_NEWTAB, st.ntabs < BR_MAX_TABS);
        bl_add(l, "Text size", br_zoom_name[bui.zoom], "setting-scale", BI_TEXT, 1);
        bl_add(l, "Display mode", bui.mobile ? "Mobile sites" : "Desktop sites", "setting-display", BI_MODE, 1);
        bl_divider(l);
        bl_add(l, "Clear cookies and cache", NULL, "search-clear", BI_CLEAR, 1);
        bl_add(l, "Back to home screen", NULL, "file-home", BI_HOMESCREEN, 1);
        bl_add(l, "Quit browser", NULL, "shutdown", BI_QUIT, 1);
    } else if (view == BV_BOOKMARKS) {
        for (int i = 0; i < nbm; i++) {
            char b[200];
            BItem *it = bl_add(l, bms[i].title, br_show_url(bms[i].url, b, sizeof b), NULL, BI_GO, 1);
            sncpy(it->url, sizeof it->url, bms[i].url);
            it->arg = i;
        }
        if (!nbm) bl_add(l, "No bookmarks yet", NULL, NULL, 0, 0);
    } else if (view == BV_HISTORY) {
        char p[400], line[800];
        cfg_file("history.txt", p, sizeof p);
        static char urls[60][600], titles[60][100];
        int n = 0, total = 0;
        FILE *f = fopen(p, "r");
        while (f && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *tab = strchr(line, '\t');
            if (!tab) continue;
            *tab = 0;
            sncpy(urls[total % 60], sizeof urls[0], line);
            sncpy(titles[total % 60], sizeof titles[0], tab + 1);
            total++;
        }
        if (f) fclose(f);
        n = total < 60 ? total : 60;
        for (int i = 0; i < n; i++) {
            int k = (total - 1 - i) % 60;
            char b[200];
            BItem *it = bl_add(l, titles[k][0] ? titles[k] : urls[k], br_show_url(urls[k], b, sizeof b), NULL, BI_GO, 1);
            sncpy(it->url, sizeof it->url, urls[k]);
        }
        if (!n) bl_add(l, "Nothing visited yet", NULL, NULL, 0, 0);
    } else if (view == BV_TEXT) {
        for (int i = 0; i < 3; i++) {
            BItem *it = bl_add(l, br_zoom_name[i], NULL, NULL, BI_SETTEXT, 1);
            it->arg = i;
            it->check = i == bui.zoom, it->ck = 1;
            if (i == bui.zoom) l->sel = i;
        }
    } else if (view == BV_MODE) {
        for (int i = 0; i < 2; i++) {
            BItem *it = bl_add(l, i ? "Mobile sites" : "Desktop sites", NULL, NULL, BI_SETMODE, 1);
            it->arg = i;
            it->check = i == bui.mobile, it->ck = 1;
            if (i == bui.mobile) l->sel = i;
        }
    }
    l->spos = (float)l->sel;
}

static void bui_status(const char *s)
{
    sncpy(bui.status, sizeof bui.status, s);
    bui.status_until = now_s + 2.2f;
}

static int browser_is_open(void) { return bui.open; }

static void bui_apply_view(void) { br_set_viewport(br_zoom_css[bui.zoom], bui.mobile); }

static void browser_open(void)
{
    if (!bui.running) {
        char path[400];
        if (br_find(cfg.browser_path, path, sizeof path) != 0) {
            show_confirm("The browser needs Chromium", "It isn't installed yet. Install it now? It's a big download.", A_BR_INSTALL, "");
            return;
        }
        char prof[400];
        cfg_file("browser", prof, sizeof prof);
        bm_load();
        home_write();
        if (br_start(path, prof, W, H) != 0) {
            show_message("Couldn't start the browser", "Chromium wouldn't start.");
            snd(SND_ERROR);
            return;
        }
        bui.running = 1;
        bui.need_home = 1;
        bui.zoom = 1;
        bui.mobile = 0;
        bui_apply_view();
    }
    if (!bui.open) bui_status("Square: tabs   Triangle: menu   Start: address");
    bui.status_until = now_s + 4.0f;
    bui.open = 1;
    bui.panel = 0;
    if (!bui.cursor_init) {
        bui.cx = W / 2.0f;
        bui.cy = H / 2.0f;
        bui.cursor_init = 1;
    }
    snd(SND_SELECT);
}

static void browser_quit(void)
{
    br_stop();
    bui.running = bui.open = 0;
    bui.panel = 0;
    bui.seq = 0;
    bui.tw = bui.th = 0;
    if (bui.tex) glDeleteTextures(1, &bui.tex), bui.tex = 0;
    bui.last_hist[0] = 0;
}

static float bui_scale_x(void) { return bui.tw > 0 ? (float)bui.tw / (float)W : 1.0f; }

static void bui_click(void)
{
    float k = bui_scale_x();
    br_click((int)(bui.cx * k), (int)(bui.cy * k));
    bui.probe_at = SDL_GetTicks() + 300;
}

static void bui_address(void)
{
    BrState st;
    br_get(&st);
    const char *u = st.ntabs && st.active < st.ntabs ? st.tabs[st.active].url : "";
    if (!strncmp(u, "file://", 7) || !strncmp(u, "about:", 6)) u = "";
    bui.panel = 0;
    osk_open_for(A_BR_ADDR, "Search or type an address", "", u);
}

static void tabs_open(void)
{
    BrState st;
    br_get(&st);
    bui.panel = 1;
    BList *l = &bui.tabs;
    l->sel = st.active;
    l->spos = (float)l->sel;
    snd(SND_SELECT);
}

static void tabs_rebuild(void)
{
    BList *l = &bui.tabs;
    BrState st;
    br_get(&st);
    int keep = l->sel;
    l->n = 0;
    for (int i = 0; i < st.ntabs; i++) {
        char b[200];
        BItem *it = bl_add(l, st.tabs[i].title[0] ? st.tabs[i].title : br_show_url(st.tabs[i].url, b, sizeof b), br_show_url(st.tabs[i].url, b, sizeof b), NULL, BI_GO, 1);
        it->arg = i;
        it->check = i == st.active;
    }
    bl_add(l, "New tab", NULL, "add", BI_NEWTAB, st.ntabs < BR_MAX_TABS);
    if (keep >= l->n) keep = l->n - 1;
    l->sel = keep < 0 ? 0 : keep;
}

static void bl_move(BList *l, int step)
{
    int dir = step < 0 ? -1 : 1, n = step < 0 ? -step : step, i = l->sel;
    while (n-- > 0) {
        int j = i + dir;
        while (j >= 0 && j < l->n && l->it[j].divider) j += dir;
        if (j < 0 || j >= l->n) break;
        i = j;
    }
    if (i != l->sel) l->sel = i, snd(SND_STEP);
}

/* keep the address and the arrow buttons in step with the page */
static void menu_refresh_main(void)
{
    if (bui.view != BV_MAIN || bui.menu.n < 2) return;
    BrState st;
    br_get(&st);
    int cur = st.active >= 0 && st.active < st.ntabs ? st.active : 0;
    char b[200];
    BItem *u = &bui.menu.it[0];
    sncpy(u->label, sizeof u->label, st.ntabs ? br_show_url(st.tabs[cur].url, b, sizeof b) : "");
    u->glyph = st.ntabs && !strncmp(st.tabs[cur].url, "https://", 8) ? "authenticate" : "setting-address";
}

static int tb_enabled(int id)
{
    BrState st;
    br_get(&st);
    return id == BI_BACK ? st.can_back : id == BI_FWD ? st.can_forward : 1;
}

static void menu_perform(const BItem *s)
{
    switch (s->id) {
    case BI_ADDR: bui_address(); return;
    case BI_BACK: br_back(); break;
    case BI_FWD: br_forward(); break;
    case BI_RELOAD: {
        BrState st;
        br_get(&st);
        if (st.ntabs && st.tabs[st.active].loading) br_stop_loading();
        else br_reload();
        break;
    }
    case BI_HOME: br_navigate(bui.home_url); break;
    case BI_ADDBM: {
        BrState st;
        br_get(&st);
        if (!st.ntabs) break;
        const char *u = st.tabs[st.active].url;
        int at = bm_find(u);
        if (at >= 0) {
            memmove(&bms[at], &bms[at + 1], sizeof bms[0] * (size_t)(nbm - at - 1));
            nbm--;
            bui_status("Bookmark removed");
        } else if (nbm < BM_MAX) {
            sncpy(bms[nbm].url, sizeof bms[nbm].url, u);
            char b[200];
            sncpy(bms[nbm].title, sizeof bms[nbm].title, st.tabs[st.active].title[0] ? st.tabs[st.active].title : br_show_url(u, b, sizeof b));
            bm_clean(bms[nbm].title);
            nbm++;
            bui_status("Bookmarked");
        } else {
            bui_status("Too many bookmarks");
        }
        bm_save();
        home_write();
        break;
    }
    case BI_BOOKMARKS: menu_build(BV_BOOKMARKS); snd(SND_SELECT); return;
    case BI_HISTORY: menu_build(BV_HISTORY); snd(SND_SELECT); return;
    case BI_TEXT: menu_build(BV_TEXT); snd(SND_SELECT); return;
    case BI_MODE: menu_build(BV_MODE); snd(SND_SELECT); return;
    case BI_GO: br_navigate(s->url); break;
    case BI_NEWTAB: br_new_tab(bui.home_url); break;
    case BI_SETTEXT:
        bui.zoom = s->arg;
        bui_apply_view();
        break;
    case BI_SETMODE:
        bui.mobile = s->arg;
        bui_apply_view();
        break;
    case BI_CLEAR:
        br_clear_data();
        bui_status("Cookies and cache cleared");
        break;
    case BI_HOMESCREEN: bui.open = 0; bui.panel = 0; snd(SND_BACK); return;
    case BI_QUIT: browser_quit(); snd(SND_BACK); return;
    }
    bui.panel = 0;
    snd(SND_SELECT);
}

static void browser_action(int a)
{
    BrState st;
    br_get(&st);
    if (bui.panel == 1) { /* the tabs, down the left side */
        BList *l = &bui.tabs;
        switch (a) {
        case ACT_UP: bl_move(l, -1); break;
        case ACT_DOWN: bl_move(l, 1); break;
        case ACT_PGUP: bl_move(l, -5); break;
        case ACT_PGDN: bl_move(l, 5); break;
        case ACT_BACK:
        case ACT_SQUARE: bui.panel = 0; snd(SND_BACK); break;
        case ACT_MENU:
            if (l->sel < st.ntabs) br_close_tab(l->sel), snd(SND_SELECT);
            break;
        case ACT_OK:
        case ACT_START:
            if (l->sel < st.ntabs) {
                br_switch_tab(l->sel);
                bui.panel = 0;
                snd(SND_SELECT);
            } else if (st.ntabs < BR_MAX_TABS) {
                br_new_tab(bui.home_url);
                bui.panel = 0;
                snd(SND_SELECT);
            }
            break;
        }
        return;
    }
    if (bui.panel == 2) { /* the menu, down the right side */
        BList *l = &bui.menu;
        switch (a) {
        case ACT_UP: bl_move(l, -1); break;
        case ACT_DOWN: bl_move(l, 1); break;
        case ACT_PGUP: bl_move(l, -5); break;
        case ACT_PGDN: bl_move(l, 5); break;
        case ACT_LEFT:
        case ACT_RIGHT:
            if (l->sel < l->n && l->it[l->sel].ntb) {
                BItem *b = &l->it[l->sel];
                int ns = b->tsel + (a == ACT_LEFT ? -1 : 1);
                if (ns >= 0 && ns < b->ntb) b->tsel = ns, snd(SND_STEP);
            }
            break;
        case ACT_BACK:
            if (bui.view != BV_MAIN) menu_build(BV_MAIN), snd(SND_BACK);
            else bui.panel = 0, snd(SND_BACK);
            break;
        case ACT_MENU: bui.panel = 0; snd(SND_BACK); break;
        case ACT_SQUARE:
            if (bui.view == BV_BOOKMARKS && l->sel < l->n && l->it[l->sel].id == BI_GO) {
                int at = l->it[l->sel].arg, keep = l->sel;
                memmove(&bms[at], &bms[at + 1], sizeof bms[0] * (size_t)(nbm - at - 1));
                nbm--;
                bm_save();
                home_write();
                menu_build(BV_BOOKMARKS);
                l->sel = keep < l->n ? keep : l->n - 1;
                if (l->sel < 0) l->sel = 0;
                l->spos = (float)l->sel;
                snd(SND_SELECT);
            }
            break;
        case ACT_OK:
        case ACT_START:
            if (l->sel < l->n) {
                BItem copy = l->it[l->sel];
                if (copy.ntb) {
                    copy.id = copy.tb[copy.tsel];
                    copy.enabled = tb_enabled(copy.id);
                }
                if (!copy.enabled) snd(SND_ERROR);
                else menu_perform(&copy);
            }
            break;
        }
        return;
    }
    switch (a) { /* looking at a page */
    case ACT_OK: bui_click(); break;
    case ACT_BACK:
        if (st.can_back) br_back();
        else bui.open = 0, snd(SND_BACK); /* nothing to go back to: leave the browser running and return home */
        break;
    case ACT_MENU: bui.panel = 2; menu_build(BV_MAIN); snd(SND_SELECT); break;
    case ACT_SQUARE: tabs_open(); break;
    case ACT_PGUP: br_back(); break;
    case ACT_PGDN: br_forward(); break;
    case ACT_START: bui_address(); break;
    case ACT_UP: br_wheel((int)(bui.cx * bui_scale_x()), (int)(bui.cy * bui_scale_x()), 0, -140); break;
    case ACT_DOWN: br_wheel((int)(bui.cx * bui_scale_x()), (int)(bui.cy * bui_scale_x()), 0, 140); break;
    case ACT_LEFT: br_wheel((int)(bui.cx * bui_scale_x()), (int)(bui.cy * bui_scale_x()), -140, 0); break;
    case ACT_RIGHT: br_wheel((int)(bui.cx * bui_scale_x()), (int)(bui.cy * bui_scale_x()), 140, 0); break;
    }
}

static void browser_address_done(const char *text)
{
    if (text[0]) br_navigate(text);
    bui.panel = 0;
}

static void browser_field_done(const char *text, int enter)
{
    br_set_field(text);
    if (enter) br_key("Enter");
}

static float stick_shaped(int raw)
{
    float v = (float)raw / 32767.0f, m = fabsf(v);
    if (m < 0.18f) return 0;
    m = (m - 0.18f) / 0.82f;
    return v < 0 ? -m : m;
}

static float stick_axis(int i) { return stick_shaped(stick_raw[i]); }

/* once a frame while the browser is running */
static void browser_tick(void)
{
    if (!bui.running) return;
    BrState st;
    br_get(&st);
    Uint32 now = SDL_GetTicks();
    float dt = bui.last_tick ? fminf((now - bui.last_tick) / 1000.0f, 0.1f) : 0.016f;
    bui.last_tick = now;
    if (st.state == 3) {
        show_message("The browser stopped", st.error[0] ? st.error : "Chromium closed unexpectedly.");
        snd(SND_ERROR);
        browser_quit();
        return;
    }
    if (st.state == 2 && bui.need_home && st.ntabs >= 1) {
        br_navigate(bui.home_url);
        bui.need_home = 0;
    }
    /* what to remember in the history */
    if (st.ntabs && st.active < st.ntabs && !st.tabs[st.active].loading) {
        const char *u = st.tabs[st.active].url;
        if (u[0] && strncmp(u, "about:", 6) && !strstr(u, "/pixi/home.html") && strcmp(u, bui.last_hist)) {
            sncpy(bui.last_hist, sizeof bui.last_hist, u);
            hist_add(u, st.tabs[st.active].title);
        }
    }
    if (!bui.open || dlg.kind != D_NONE) return;
    if (bui.probe_at && now >= bui.probe_at) { /* did the click land in a text box? */
        bui.probe_at = 0;
        br_probe_field();
        bui.probing = 1;
        bui.probe_until = now + 900;
    }
    if (bui.probing) {
        int pw;
        char txt[1024];
        if (br_field(&pw, txt, sizeof txt)) {
            bui.probing = 0;
            osk_open_for(A_BR_FIELD, pw ? "Enter password" : "Enter text", "", txt);
            return;
        }
        if (now > bui.probe_until) bui.probing = 0;
    }
    if (bui.panel) return;
    /* the left stick moves the pointer; the right stick scrolls */
    float lx = stick_axis(0), ly = stick_axis(1), rx = stick_axis(2), ry = stick_axis(3);
    if (lx || ly) {
        float sp = 1640.0f * S;
        bui.cx += lx * fabsf(lx) * sp * dt;
        bui.cy += ly * fabsf(ly) * sp * dt;
    }
    if (bui.cx < 0) bui.cx = 0;
    if (bui.cx > W - 1) bui.cx = (float)W - 1;
    if (bui.cy < 0) bui.cy = 0;
    if (bui.cy > H - 1) bui.cy = (float)H - 1;
    float k = bui_scale_x();
    if ((lx || ly) && now - bui.last_move > 30 && (fabsf(bui.cx - bui.last_sx) > 0.5f || fabsf(bui.cy - bui.last_sy) > 0.5f)) {
        br_mouse_move((int)(bui.cx * k), (int)(bui.cy * k));
        bui.last_sx = bui.cx, bui.last_sy = bui.cy, bui.last_move = now;
    }
    float sx = rx, sy = ry; /* pushing the pointer against the top or bottom edge scrolls too */
    if (!sy && ly > 0.3f && bui.cy >= H - 2) sy = ly;
    if (!sy && ly < -0.3f && bui.cy <= 1) sy = ly;
    if ((sx || sy) && now - bui.last_wheel > 30) {
        br_wheel((int)(bui.cx * k), (int)(bui.cy * k), (int)(sx * 90), (int)(sy * 90));
        bui.last_wheel = now;
    }
}

static void bui_upload(void)
{
    const unsigned char *px;
    int w, h;
    unsigned seq;
    if (!br_frame_lock(&px, &w, &h, &seq)) return;
    if (seq != bui.seq || !bui.tex) {
        if (!bui.tex) {
            glGenTextures(1, &bui.tex);
            glBindTexture(GL_TEXTURE_2D, bui.tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            bui.tw = bui.th = 0;
        }
        glBindTexture(GL_TEXTURE_2D, bui.tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (w != bui.tw || h != bui.th) glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        else glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        bui.tw = w, bui.th = h, bui.seq = seq;
    }
    br_frame_unlock();
}

static void draw_blist(BList *l, float x, float pw, const char *title, float a)
{
    float top = 92 * S, rh = 66 * S, bottom = (float)H - 80 * S;
    text(title, x + 34 * S, 26 * S, 26 * S, 1, th.text, a, AL_LEFT, (int)(pw - 60 * S));
    int visible = (int)((bottom - top) / rh);
    float want = (float)l->sel - (float)visible / 2 + 0.5f, maxs = (float)(l->n - visible);
    if (want > maxs) want = maxs;
    if (want < 0) want = 0;
    l->spos += (want - l->spos) * 0.3f;
    for (int i = 0; i < l->n; i++) {
        float y = top + ((float)i - l->spos) * rh;
        if (y < top - rh * 0.5f || y + rh > bottom + rh * 0.5f) continue;
        float fa = clampf(fminf((y - top + rh * 0.5f) / rh, (bottom - y) / rh), 0, 1) * a;
        const BItem *s = &l->it[i];
        if (s->divider) {
            pane(x + 30 * S, y + rh / 2, pw - 60 * S, 1.5f * S, 0.75f * S, CIRCLE, with_alpha(th.text, 0.35f), 0, 0, 0, fa, 0);
            continue;
        }
        float ia = fa * (s->enabled ? 1.0f : 0.4f);
        if (i == l->sel && !s->ntb)
            pane(x + 16 * S, y + 4 * S, pw - 32 * S, rh - 8 * S, (rh - 8 * S) / 2, SQUIRCLE, with_alpha(th.accent, 0.62f), DEPTH_CONTROL * S, FROST_CONTROL,
                 GLOSS_FULL, fa, 0);
        if (s->ntb) { /* a row of small buttons: home, back, forward, reload */
            static const char *tbg[] = { "file-home", "arrow-left", "arrow-right", "refresh" };
            float bw = (pw - 32 * S) / (float)s->ntb;
            for (int b = 0; b < s->ntb; b++) {
                int on = tb_enabled(s->tb[b]), pick = i == l->sel && b == s->tsel;
                BrState bs;
                br_get(&bs);
                float bx = x + 16 * S + b * bw;
                pane(bx + 3 * S, y + 4 * S, bw - 6 * S, rh - 8 * S, (rh - 8 * S) / 2, SQUIRCLE,
                     pick ? with_alpha(th.accent, 0.70f) : with_alpha(th.glass_raised, 0.14f), DEPTH_CONTROL * S, FROST_CONTROL, pick ? GLOSS_FULL : GLOSS_QUIET, fa, 0);
                const char *g = s->tb[b] == BI_RELOAD && bs.ntabs && bs.active < bs.ntabs && bs.tabs[bs.active].loading ? "search-clear" : tbg[b];
                glyph(g, bx + bw / 2, y + rh / 2, 32 * S, th.text, fa * (on ? 1.0f : 0.4f));
            }
            continue;
        }
        if (s->urlbar && i != l->sel)
            pane(x + 16 * S, y + 4 * S, pw - 32 * S, rh - 8 * S, (rh - 8 * S) / 2, SQUIRCLE, with_alpha(th.glass_raised, 0.14f), 0, 0, GLOSS_QUIET, fa, 0);
        float tx = x + 34 * S;
        if (s->glyph) {
            glyph(s->glyph, x + 52 * S, y + rh / 2, 32 * S, th.text, ia);
            tx = x + 96 * S;
        }
        int maxw = (int)(pw - (tx - x) - 60 * S);
        if (s->sub[0]) {
            text(s->label, tx, y + 9 * S, 22 * S, i == l->sel, th.text, ia, AL_LEFT, maxw);
            text(s->sub, tx, y + 37 * S, 17 * S, 0, th.text_soft, ia, AL_LEFT, maxw);
        } else {
            text(s->label, tx, y + rh / 2 - 12 * S, 24 * S, i == l->sel, th.text, ia, AL_LEFT, maxw);
        }
        mark_draw(s->ck, s->check, x + pw - 44 * S, y + rh / 2, s->ck ? 30 * S : 26 * S, th.text, ia);
    }
}

static GLuint ptr_tex;

/* the arrow is drawn once into a small picture */
static GLuint pointer_texture(void)
{
    enum { N = 128 };
    static const float px[4] = { 30.0f / 184, 168.0f / 184, 95.0f / 184, 58.0f / 184 };
    static const float py[4] = { 20.0f / 184, 82.0f / 184, 102.0f / 184, 170.0f / 184 };
    static unsigned char img[N * N * 4];
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float fx = (x + 0.5f) / N, fy = (y + 0.5f) / N;
            int in = 0;
            float d = 1e9f;
            for (int i = 0, j = 3; i < 4; j = i++) {
                if (((py[i] > fy) != (py[j] > fy)) && (fx < (px[j] - px[i]) * (fy - py[i]) / (py[j] - py[i]) + px[i])) in = !in;
                float ex = px[j] - px[i], ey = py[j] - py[i], t = ((fx - px[i]) * ex + (fy - py[i]) * ey) / (ex * ex + ey * ey);
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                float dx = fx - (px[i] + ex * t), dy = fy - (py[i] + ey * t), dd = sqrtf(dx * dx + dy * dy);
                if (dd < d) d = dd;
            }
            float px_ = d * N; /* distance in pixels */
            float black = in ? clampf(px_ - 3.0f, 0, 1) : 0;                    /* the dark inside, after a thin white rim */
            float cover = in ? 1.0f : clampf(7.0f - px_, 0, 1);                  /* the white outline outside the edge */
            unsigned char v = (unsigned char)(255 * (1 - black));
            unsigned char *o = img + (y * N + x) * 4;
            o[0] = o[1] = o[2] = v;
            o[3] = (unsigned char)(255 * cover);
        }
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static void draw_browser(void)
{
    bui_upload();
    BrState st;
    br_get(&st);
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0.07f, 0.07f, 0.08f, 1 }, 0, 0, 0, 1, 0);
    if (bui.tex && bui.tw) picture(bui.tex, 0, 0, (float)W, (float)H, (Col){ 1, 1, 1, 1 }, 1);
    int loading = st.ntabs && st.active < st.ntabs && st.tabs[st.active].loading;
    if (!bui.tex || st.state < 2) {
        text("Starting the browser\xE2\x80\xA6", W / 2.0f, H / 2.0f - 14 * S, 28 * S, 1, th.text, 0.9f, AL_CENTER, 0);
    }
    if (loading) { /* the pulsing mark at the bottom right, like the PSP's */
        float p = 0.5f + 0.5f * sinf(now_s * 5.0f);
        float r = (14 + 4 * p) * S;
        pane((float)W - 38 * S - r, (float)H - 34 * S - r, r * 2, r * 2, r, CIRCLE, with_alpha(th.accent, 0.5f + 0.4f * p), 0, 0, 0, 1, 0);
    }
    /* the pointer: a black arrow with a white outline, easy to see on any page */
    if (!bui.panel && dlg.kind == D_NONE) {
        if (!ptr_tex) ptr_tex = pointer_texture();
        float sz = 68 * S;
        picture(ptr_tex, bui.cx - 0.163f * sz, bui.cy - 0.109f * sz, sz, sz, (Col){ 1, 1, 1, 1 }, 1);
    }
    /* the tab list on the left, the menu on the right */
    float pw = fmaxf(W * 0.36f, 330 * S);
    bui.tabs.anim += ((bui.panel == 1 ? 1.0f : 0.0f) - bui.tabs.anim) * 0.3f;
    bui.menu.anim += ((bui.panel == 2 ? 1.0f : 0.0f) - bui.menu.anim) * 0.3f;
    if (bui.panel != 1 && bui.tabs.anim < 0.02f) bui.tabs.anim = 0;
    if (bui.panel != 2 && bui.menu.anim < 0.02f) bui.menu.anim = 0;
    if (bui.tabs.anim > 0) {
        if (bui.panel == 1) tabs_rebuild();
        float a = bui.tabs.anim, x = -40 * S - pw * (1 - a);
        pane(x, 0, pw + 40 * S, (float)H, 0, CIRCLE, (Col){ 0.03f, 0.03f, 0.05f, 0.94f }, 0, 0, 0, 1, 0);
        pane(x, 0, pw + 40 * S, (float)H, 0, SQUIRCLE, (Col){ 0.05f + th.accent.r * 0.16f, 0.05f + th.accent.g * 0.16f, 0.07f + th.accent.b * 0.16f, 0.97f }, 0, 0, GLOSS_QUIET, 1, 0);
        pane(x + pw + 38 * S, 0, 2 * S, (float)H, 0, SQUIRCLE, with_alpha(th.text, 0.35f), 0, 0, 0, 1, 0);
        draw_blist(&bui.tabs, x + 40 * S, pw, "Tabs", 1);
    }
    if (bui.menu.anim > 0) {
        menu_refresh_main();
        float a = bui.menu.anim, x = W - pw * a;
        pane(x, 0, pw + 40 * S, (float)H, 0, CIRCLE, (Col){ 0.03f, 0.03f, 0.05f, 0.94f }, 0, 0, 0, 1, 0);
        pane(x, 0, pw + 40 * S, (float)H, 0, SQUIRCLE, (Col){ 0.05f + th.accent.r * 0.16f, 0.05f + th.accent.g * 0.16f, 0.07f + th.accent.b * 0.16f, 0.97f }, 0, 0, GLOSS_QUIET, 1, 0);
        pane(x, 0, 2 * S, (float)H, 0, SQUIRCLE, with_alpha(th.text, 0.35f), 0, 0, 0, 1, 0);
        const char *title = bui.view == BV_BOOKMARKS ? "Bookmarks" : bui.view == BV_HISTORY ? "History" : bui.view == BV_TEXT ? "Text size" : bui.view == BV_MODE ? "Display mode" : "Menu";
        draw_blist(&bui.menu, x, pw, title, 1);
    }
    /* what's happened, briefly */
    if (bui.status[0] && now_s < bui.status_until) {
        float tw = fmaxf((float)text_width(bui.status, 22 * S, 1) + 60 * S, 200 * S);
        pane(W / 2.0f - tw / 2, (float)H - 130 * S, tw, 48 * S, 24 * S, SQUIRCLE, (Col){ 0, 0, 0, 0.78f }, 0, 0, 0, 1, 0);
        text(bui.status, W / 2.0f, (float)H - 130 * S + 12 * S, 22 * S, 1, (Col){ 1, 1, 1, 1 }, 1, AL_CENTER, 0);
    }
    if (dlg.kind == D_NONE) {
        if (bui.panel == 1) {
            Hint h[3] = { { pad_ok(), "Open" }, { "pad-north", "Close tab" }, { pad_back(), "Back" } };
            draw_hints_at(h, 3, 1);
        } else if (bui.panel == 2) {
            if (bui.view == BV_BOOKMARKS) {
                Hint h[3] = { { pad_ok(), "Open" }, { "pad-west", "Remove" }, { pad_back(), "Back" } };
                draw_hints(h, 3);
            } else {
                Hint h[2] = { { pad_ok(), "Select" }, { pad_back(), bui.view == BV_MAIN ? "Close" : "Back" } };
                draw_hints(h, 2);
            }
        }
    }
}

/* ======================================================================== */
/* The video player                                                         */
/* ======================================================================== */

enum { VV_MAIN, VV_AUDIO, VV_SUBS, VV_SPEED, VV_PICTURE, VV_SUBSIZE };
enum { VI_PLAY = 1, VI_PREV, VI_NEXT, VI_AUDIO, VI_SUBS, VI_SPEED, VI_PICTURE, VI_SUBSIZE, VI_LOOP, VI_INFO, VI_HOME, VI_QUIT, VI_SET };

static const double vp_speeds[6] = { 0.5, 0.75, 1.0, 1.25, 1.5, 2.0 };
static const char *vp_speed_names[6] = { "0.5\xC3\x97", "0.75\xC3\x97", "Normal", "1.25\xC3\x97", "1.5\xC3\x97", "2\xC3\x97" };
static const char *vp_picture_names[5] = { "Original", "16:9", "4:3", "Zoom to fill", "Stretch" };
static const double vp_sub_scales[4] = { 0.7, 1.0, 1.4, 1.8 };
static const char *vp_sub_names[4] = { "Small", "Normal", "Large", "Extra large" };

static struct {
    int open, running;
    char **list;
    int nlist, cur;
    char title[200];
    VdState st;
    GLuint fb, tex;
    int tex_w, tex_h, have_frame;
    int user_paused, applied;
    float ctl; /* how visible the controls are, 0 to 1 */
    float show_until, resume_flash, vol_until, status_until, seek_until;
    double seek_acc;
    int panel, view;
    BList menu;
    int speed, picture, subsize, loop, volume;
    int auto_next; /* the file now playing was started by the playlist moving on, not by a choice */
    char status[100];
} vp;

static int video_is_open(void) { return vp.open; }

static void vp_status(const char *s)
{
    sncpy(vp.status, sizeof vp.status, s);
    vp.status_until = now_s + 2.0f;
}

static void vp_show(void) { vp.show_until = now_s + 3.5f; }

static void vp_time(char *out, size_t cap, double s)
{
    long t = (long)((s < 0 ? 0 : s) + 0.5);
    if (t >= 3600) snprintf(out, cap, "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    else snprintf(out, cap, "%ld:%02ld", t / 60, t % 60);
}

/* "+10s", "-1 min", "+1m 30s" */
static void vp_skip_text(char *out, size_t cap, double a)
{
    const char *sign = a < 0 ? "-" : "+";
    long v = (long)(fabs(a) + 0.5);
    if (v < 60) snprintf(out, cap, "%s%lds", sign, v);
    else if (v % 60 == 0) snprintf(out, cap, "%s%ld min", sign, v / 60);
    else snprintf(out, cap, "%s%ldm %02lds", sign, v / 60, v % 60);
}

static const char *vp_lang(const char *code)
{
    static const struct {
        const char *c, *n;
    } t[] = {
        { "eng", "English" }, { "en", "English" }, { "jpn", "Japanese" }, { "ja", "Japanese" }, { "fre", "French" }, { "fra", "French" },
        { "fr", "French" }, { "ger", "German" }, { "deu", "German" }, { "de", "German" }, { "spa", "Spanish" }, { "es", "Spanish" },
        { "ita", "Italian" }, { "it", "Italian" }, { "por", "Portuguese" }, { "pt", "Portuguese" }, { "rus", "Russian" },
        { "ru", "Russian" }, { "chi", "Chinese" }, { "zho", "Chinese" }, { "zh", "Chinese" }, { "kor", "Korean" }, { "ko", "Korean" },
        { "ara", "Arabic" }, { "ar", "Arabic" }, { "hin", "Hindi" }, { "hi", "Hindi" }, { "dut", "Dutch" }, { "nld", "Dutch" },
        { "nl", "Dutch" }, { "pol", "Polish" }, { "pl", "Polish" }, { "swe", "Swedish" }, { "sv", "Swedish" }, { "tur", "Turkish" },
        { "tr", "Turkish" }, { "tha", "Thai" }, { "th", "Thai" }, { "vie", "Vietnamese" }, { "vi", "Vietnamese" }, { "ind", "Indonesian" },
        { "id", "Indonesian" }, { "heb", "Hebrew" }, { "he", "Hebrew" }, { "gre", "Greek" }, { "ell", "Greek" }, { "el", "Greek" },
        { "cze", "Czech" }, { "ces", "Czech" }, { "cs", "Czech" }, { "hun", "Hungarian" }, { "hu", "Hungarian" }, { "dan", "Danish" },
        { "da", "Danish" }, { "fin", "Finnish" }, { "fi", "Finnish" }, { "nor", "Norwegian" }, { "no", "Norwegian" }, { "ukr", "Ukrainian" },
        { "uk", "Ukrainian" }, { "rum", "Romanian" }, { "ron", "Romanian" }, { "ro", "Romanian" }, { "und", "Unknown" },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (!strcasecmp(code, t[i].c)) return t[i].n;
    return NULL;
}

/* "Japanese" and "Commentary - AAC - stereo" for one track */
static void vp_track_text(const VdTrack *t, int n, int audio, char *label, size_t lc, char *sub, size_t sc)
{
    const char *name = t->lang[0] ? vp_lang(t->lang) : NULL;
    if (name) snprintf(label, lc, "%s", name);
    else if (t->lang[0]) snprintf(label, lc, "%s", t->lang);
    else snprintf(label, lc, "Track %d", n);
    sub[0] = 0;
    if (t->title[0]) snprintf(sub, sc, "%s", t->title);
    if (t->codec[0]) {
        char c[32];
        sncpy(c, sizeof c, t->codec);
        for (char *p = c; *p; p++) *p = (char)toupper((unsigned char)*p);
        size_t k = strlen(sub);
        snprintf(sub + k, sc - k, "%s%s", k ? " \xC2\xB7 " : "", c);
    }
    if (audio && t->channels > 0) {
        size_t k = strlen(sub);
        const char *ch = t->channels == 1 ? "mono" : t->channels == 2 ? "stereo" : t->channels == 6 ? "5.1" : t->channels == 8 ? "7.1" : NULL;
        char num[16];
        if (!ch) snprintf(num, sizeof num, "%d ch", t->channels), ch = num;
        snprintf(sub + k, sc - k, "%s%s", k ? " \xC2\xB7 " : "", ch);
    }
}

static const VdTrack *vp_chosen(const VdTrack *list, int n)
{
    for (int i = 0; i < n; i++)
        if (list[i].selected) return &list[i];
    return NULL;
}

static void vp_menu_build(int view)
{
    BList *l = &vp.menu;
    l->n = 0;
    l->sel = 0;
    l->spos = 0;
    vp.view = view;
    const VdState *s = &vp.st;
    char lab[100], sub[200];
    switch (view) {
    case VV_MAIN: {
        bl_add(l, vp.user_paused ? "Play" : "Pause", NULL, vp.user_paused ? "media-play" : "media-pause", VI_PLAY, 1);
        bl_add(l, "Previous video", NULL, "media-previous", VI_PREV, vp.cur > 0);
        bl_add(l, "Next video", NULL, "media-next", VI_NEXT, vp.cur < vp.nlist - 1);
        bl_divider(l);
        const VdTrack *a = vp_chosen(s->aud, s->naud), *c = vp_chosen(s->sub, s->nsub);
        if (a) vp_track_text(a, 1, 1, lab, sizeof lab, sub, sizeof sub);
        else sncpy(lab, sizeof lab, s->naud ? "Off" : "None");
        bl_add(l, "Audio", lab, "media-volume", VI_AUDIO, s->naud > 0);
        if (c) vp_track_text(c, 1, 0, lab, sizeof lab, sub, sizeof sub);
        else sncpy(lab, sizeof lab, s->nsub ? "Off" : "None in this video");
        bl_add(l, "Subtitles", lab, "setting-language", VI_SUBS, s->nsub > 0);
        bl_add(l, "Subtitle size", vp_sub_names[vp.subsize], "setting-scale", VI_SUBSIZE, 1);
        bl_add(l, "Playback speed", vp_speed_names[vp.speed], "setting-schedule", VI_SPEED, 1);
        bl_add(l, "Picture", vp_picture_names[vp.picture], "setting-display", VI_PICTURE, 1);
        {
            BItem *lp = bl_add(l, "Repeat this video", NULL, "refresh", VI_LOOP, 1);
            lp->check = vp.loop, lp->ck = 2;
        }
        bl_divider(l);
        bl_add(l, "Video info", NULL, "setting-info", VI_INFO, 1);
        bl_add(l, "Home screen", NULL, "file-home", VI_HOME, 1);
        bl_add(l, "Close player", NULL, "search-clear", VI_QUIT, 1);
        break;
    }
    case VV_AUDIO:
        for (int i = 0; i < s->naud; i++) {
            vp_track_text(&s->aud[i], i + 1, 1, lab, sizeof lab, sub, sizeof sub);
            BItem *b = bl_add(l, lab, sub, NULL, VI_SET, 1);
            b->arg = s->aud[i].id;
            b->check = s->aud[i].selected, b->ck = 1;
            if (b->check) l->sel = l->n - 1;
        }
        break;
    case VV_SUBS: {
        BItem *b = bl_add(l, "Off", NULL, NULL, VI_SET, 1);
        b->arg = 0;
        b->check = s->sid == 0, b->ck = 1;
        if (b->check) l->sel = 0;
        for (int i = 0; i < s->nsub; i++) {
            vp_track_text(&s->sub[i], i + 1, 0, lab, sizeof lab, sub, sizeof sub);
            b = bl_add(l, lab, sub, NULL, VI_SET, 1);
            b->arg = s->sub[i].id;
            b->check = s->sub[i].selected, b->ck = 1;
            if (b->check) l->sel = l->n - 1;
        }
        break;
    }
    case VV_SPEED:
        for (int i = 0; i < 6; i++) {
            BItem *b = bl_add(l, vp_speed_names[i], NULL, NULL, VI_SET, 1);
            b->arg = i;
            b->check = i == vp.speed, b->ck = 1;
            if (b->check) l->sel = i;
        }
        break;
    case VV_PICTURE:
        for (int i = 0; i < 5; i++) {
            BItem *b = bl_add(l, vp_picture_names[i], NULL, NULL, VI_SET, 1);
            b->arg = i;
            b->check = i == vp.picture, b->ck = 1;
            if (b->check) l->sel = i;
        }
        break;
    case VV_SUBSIZE:
        for (int i = 0; i < 4; i++) {
            BItem *b = bl_add(l, vp_sub_names[i], NULL, NULL, VI_SET, 1);
            b->arg = i;
            b->check = i == vp.subsize, b->ck = 1;
            if (b->check) l->sel = i;
        }
        break;
    }
    l->spos = (float)l->sel;
}

static void vp_load_current(void)
{
    const char *path = vp.list[vp.cur];
    const char *base = basename_of(path);
    sncpy(vp.title, sizeof vp.title, base);
    strip_ext(vp.title);
    vd_load(path);
    vp.auto_next = 0;
    vp.user_paused = 0;
    vp.applied = -1;
    vp.have_frame = 0;
    vp.seek_acc = 0;
    memset(&vp.st, 0, sizeof vp.st);
    vp.st.running = 1;
    vp_show();
}

static void video_free_list(void)
{
    for (int i = 0; i < vp.nlist; i++) free(vp.list[i]);
    free(vp.list);
    vp.list = NULL;
    vp.nlist = vp.cur = 0;
}

static void video_close(void)
{
    if (vd_running()) vd_stop(); /* needs the GL context, which is still ours here */
    vp.running = vp.open = vp.panel = 0;
    video_free_list();
    if (vp.tex) glDeleteTextures(1, &vp.tex);
    if (vp.fb) glDeleteFramebuffers(1, &vp.fb);
    vp.tex = vp.fb = 0;
    vp.tex_w = vp.tex_h = vp.have_frame = 0;
    memset(&vp.st, 0, sizeof vp.st);
}

/* 0 when the player has started, -1 when it couldn't (the caller may try an outside player) */
static int video_open_list_ok(const char *const *paths, int n, int start)
{
    if (n <= 0) return -1;
    if (!vp.running) {
        if (vd_start(cfg.video_hwdec) != 0) {
            fprintf(stderr, "pixi-shell: the built-in video player (libvlc) couldn't start\n");
            return -1;
        }
        vp.running = 1;
        vp.speed = 2, vp.picture = 0, vp.subsize = 1, vp.loop = 0;
        vp.tex_w = vp.tex_h = 0;
    }
    video_free_list();
    vp.list = xcalloc((size_t)n, sizeof(char *));
    for (int i = 0; i < n; i++) vp.list[i] = xstrdup(paths[i]);
    vp.nlist = n;
    vp.cur = start >= 0 && start < n ? start : 0;
    app_hide_all();
    vp.open = 1;
    vp.panel = 0;
    vp.ctl = 1;
    vp_load_current();
    snd(SND_SELECT);
    return 0;
}

/* Put the video pass's changes to the GL state right again, so the rest of the frame can't tell it ran. */
static void vp_gl_restore(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glUseProgram(0);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

/* once per frame, before anything is drawn: put VLC's newest picture into our texture */
static void video_frame(void)
{
    if (!vp.running || !vp.open || guide.open || hidden) return;
    int force = 0;
    if (vp.tex_w != W || vp.tex_h != H) {
        make_target(&vp.fb, &vp.tex, W, H, 0);
        vp.tex_w = W, vp.tex_h = H;
        force = 1;
    }
    if (vd_render(vp.fb, W, H, force)) {
        vp.have_frame = 1;
        vp_gl_restore();
    }
}

/* a line every 10 seconds while a video plays, so running out of memory shows in shell.log */
static void video_note_memory(void)
{
    static double next_note;
    if (!vp.open || !vp.st.loaded || now_s < next_note) return;
    next_note = now_s + 10.0;
    long rss_pages = 0, tmp = 0, avail_kb = -1;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        if (fscanf(f, "%ld %ld", &tmp, &rss_pages) != 2) rss_pages = 0;
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char ln[128];
        while (fgets(ln, sizeof ln, f))
            if (sscanf(ln, "MemAvailable: %ld kB", &avail_kb) == 1) break;
        fclose(f);
    }
    fprintf(stderr, "video: %.0f s in, shell %ld MB, free memory %ld MB, %s\n", vp.st.pos,
            rss_pages * (sysconf(_SC_PAGESIZE) / 1024) / 1024, avail_kb / 1024, vp.title);
}

static void video_tick(void)
{
    if (!vp.running) return;
    vd_poll(&vp.st);
    video_note_memory();
    if (vp.st.failed && vp.auto_next && vp.cur + 1 < vp.nlist) {
        /* a bad file in the middle of the playlist is passed over */
        char msg[160];
        snprintf(msg, sizeof msg, "Skipped %.80s: it can't be played", vp.title);
        vp.cur++;
        vp_load_current();
        vp.auto_next = 1;
        vp_status(msg);
        return;
    }
    if (vp.st.failed) {
        char msg[260];
        snprintf(msg, sizeof msg, "%s", vp.st.error[0] ? vp.st.error : "The file couldn't be played.");
        video_close();
        show_message("Can't play this video", msg);
        snd(SND_ERROR);
        return;
    }
    if (vp.st.ended) {
        if (vp.cur + 1 < vp.nlist) {
            vp.cur++;
            vp_load_current();
            vp.auto_next = 1;
        } else {
            video_close(); /* the last one finished: back to the list */
            return;
        }
    }
    /* it plays only while it's on the screen and nobody has paused it */
    int visible = vp.open && !guide.open && !hidden;
    int want = vp.user_paused || !visible;
    if (want != vp.applied) {
        vd_pause(want);
        vp.applied = want;
    }
    int show = vp.panel || vp.user_paused || vp.st.buffering || now_s < vp.show_until;
    vp.ctl += ((show ? 1.0f : 0.0f) - vp.ctl) * 0.22f;
    if (vp.ctl < 0.01f) vp.ctl = 0;
}

static void vp_toggle(void)
{
    vp.user_paused = !vp.user_paused;
    if (!vp.user_paused) vp.resume_flash = now_s + 0.55f;
    vp_show();
}

static void vp_skip(double secs)
{
    if (!vp.st.loaded) return;
    vd_seek(secs, 0, fabs(secs) <= 15);
    vp.seek_acc = (now_s < vp.seek_until ? vp.seek_acc : 0) + secs;
    vp.seek_until = now_s + 1.0f;
    vp_show();
}

static void vp_volume(int step)
{
    int v = volume_get();
    if (v < 0) return;
    v += step;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    volume_set(v);
    vp.volume = v;
    vp.vol_until = now_s + 1.6f;
    vp_show();
}

static void vp_step(int dir)
{
    int n = vp.cur + dir;
    if (n < 0 || n >= vp.nlist) return;
    vp.cur = n;
    vp_load_current();
}

/* the Square button: through the subtitles, one after another, then off */
static void vp_cycle_subs(void)
{
    const VdState *s = &vp.st;
    if (!s->nsub) {
        vp_status("No subtitles in this video");
        return;
    }
    int at = -1;
    for (int i = 0; i < s->nsub; i++)
        if (s->sub[i].selected) at = i;
    at++;
    if (at >= s->nsub) {
        vd_set_sid(0);
        vp_status("Subtitles: Off");
    } else {
        char lab[100], sub[200], msg[140];
        vp_track_text(&s->sub[at], at + 1, 0, lab, sizeof lab, sub, sizeof sub);
        vd_set_sid(s->sub[at].id);
        snprintf(msg, sizeof msg, "Subtitles: %s", lab);
        vp_status(msg);
    }
    vp_show();
}

static void vp_menu_perform(const BItem *b)
{
    char msg[140], lab[100], sub[200];
    switch (b->id) {
    case VI_PLAY: vp_toggle(); break;
    case VI_PREV: vp_step(-1); break;
    case VI_NEXT: vp_step(1); break;
    case VI_AUDIO: vp_menu_build(VV_AUDIO); snd(SND_SELECT); return;
    case VI_SUBS: vp_menu_build(VV_SUBS); snd(SND_SELECT); return;
    case VI_SPEED: vp_menu_build(VV_SPEED); snd(SND_SELECT); return;
    case VI_PICTURE: vp_menu_build(VV_PICTURE); snd(SND_SELECT); return;
    case VI_SUBSIZE: vp_menu_build(VV_SUBSIZE); snd(SND_SELECT); return;
    case VI_LOOP:
        vp.loop = !vp.loop;
        vd_set_loop(vp.loop);
        vp_status(vp.loop ? "Repeat: on" : "Repeat: off");
        break;
    case VI_INFO: {
        char info[1024];
        vd_info(info, sizeof info);
        vp.panel = 0;
        show_message("Video info", info);
        snd(SND_SELECT);
        return;
    }
    case VI_HOME:
        vp.panel = 0;
        vp.open = 0; /* it stays in the Guide, paused */
        snd(SND_BACK);
        return;
    case VI_QUIT:
        video_close();
        snd(SND_BACK);
        return;
    case VI_SET:
        switch (vp.view) {
        case VV_AUDIO:
            vd_set_aid(b->arg);
            for (int i = 0; i < vp.st.naud; i++)
                if (vp.st.aud[i].id == b->arg) {
                    vp_track_text(&vp.st.aud[i], i + 1, 1, lab, sizeof lab, sub, sizeof sub);
                    snprintf(msg, sizeof msg, "Audio: %s", lab);
                    vp_status(msg);
                }
            break;
        case VV_SUBS:
            vd_set_sid(b->arg);
            if (!b->arg) vp_status("Subtitles: Off");
            else
                for (int i = 0; i < vp.st.nsub; i++)
                    if (vp.st.sub[i].id == b->arg) {
                        vp_track_text(&vp.st.sub[i], i + 1, 0, lab, sizeof lab, sub, sizeof sub);
                        snprintf(msg, sizeof msg, "Subtitles: %s", lab);
                        vp_status(msg);
                    }
            break;
        case VV_SPEED:
            vp.speed = b->arg;
            vd_set_speed(vp_speeds[vp.speed]);
            snprintf(msg, sizeof msg, "Speed: %s", vp_speed_names[vp.speed]);
            vp_status(msg);
            break;
        case VV_PICTURE:
            vp.picture = b->arg;
            vd_set_picture(vp.picture);
            snprintf(msg, sizeof msg, "Picture: %s", vp_picture_names[vp.picture]);
            vp_status(msg);
            break;
        case VV_SUBSIZE:
            vp.subsize = b->arg;
            vd_set_sub_scale(vp_sub_scales[vp.subsize]);
            snprintf(msg, sizeof msg, "Subtitle size: %s", vp_sub_names[vp.subsize]);
            vp_status(msg);
            break;
        }
        break;
    }
    vp.panel = 0;
    vp_show();
    snd(SND_SELECT);
}

static void video_action(int a)
{
    if (vp.panel) { /* the options, down the right side */
        BList *l = &vp.menu;
        switch (a) {
        case ACT_UP: bl_move(l, -1); break;
        case ACT_DOWN: bl_move(l, 1); break;
        case ACT_PGUP: bl_move(l, -5); break;
        case ACT_PGDN: bl_move(l, 5); break;
        case ACT_BACK:
            if (vp.view != VV_MAIN) vp_menu_build(VV_MAIN), snd(SND_BACK);
            else vp.panel = 0, snd(SND_BACK);
            break;
        case ACT_MENU: vp.panel = 0; snd(SND_BACK); break;
        case ACT_OK:
        case ACT_START:
            if (l->sel < l->n) {
                if (!l->it[l->sel].enabled) snd(SND_ERROR);
                else {
                    BItem copy = l->it[l->sel];
                    vp_menu_perform(&copy);
                }
            }
            break;
        }
        return;
    }
    /* The bottom button plays and pauses, whichever way the confirm button is set. */
    int south = cfg.confirm_east ? ACT_BACK : ACT_OK, east = cfg.confirm_east ? ACT_OK : ACT_BACK;
    if (a == south || a == ACT_START) {
        vp_toggle();
        return;
    }
    if (a == east) {
        video_close();
        snd(SND_BACK);
        return;
    }
    switch (a) {
    case ACT_LEFT: vp_skip(-cfg.video_skip); break;
    case ACT_RIGHT: vp_skip(cfg.video_skip); break;
    case ACT_PGUP: vp_skip(-cfg.video_skip_long); break;
    case ACT_PGDN: vp_skip(cfg.video_skip_long); break;
    case ACT_UP: vp_volume(5); break;
    case ACT_DOWN: vp_volume(-5); break;
    case ACT_SQUARE: vp_cycle_subs(); break;
    case ACT_MENU:
        vp.panel = 1;
        vp_menu_build(VV_MAIN);
        vp_show();
        snd(SND_SELECT);
        break;
    }
}

static void picture_card(GLuint tex, float x, float y, float w, float h, Col c, float fade, float radius, int flip)
{
    if (!tex || fade <= 0.003f) return;
    QuadInst *q = quad_new(tex);
    if (!q) return;
    set4(q->rect, x, y, w, h);
    set4(q->color, c.r, c.g, c.b, c.a);
    if (flip) set4(q->uv, 0, 1, 1, 0);
    set4(q->shape, radius, 0, 0, 0);
    set4(q->material, 0, 0, 0, fade);
    set4(q->misc, radius > 0 ? SQUIRCLE : CIRCLE, 0, -1, KIND_PLAIN);
}

static void draw_video(void)
{
    const Col black = { 0, 0, 0, 1 }, white = { 1, 1, 1, 1 };
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, black, 0, 0, 0, 1, 0);
    if (vp.tex && vp.have_frame) picture_card(vp.tex, 0, 0, (float)W, (float)H, white, 1, 0, 0);

    float c = vp.ctl;
    int loading = !vp.st.loaded || vp.st.buffering;
    if (c > 0.01f) {
        /* shading, so the controls read over any picture */
        for (int i = 0; i < 6; i++) {
            float k = (float)(i + 1) / 6.0f;
            pane(0, (float)H - 330 * S * k, (float)W, 330 * S * k, 0, CIRCLE, (Col){ 0, 0, 0, 0.13f }, 0, 0, 0, c, 0);
            pane(0, 0, (float)W, 220 * S * k, 0, CIRCLE, (Col){ 0, 0, 0, 0.11f }, 0, 0, 0, c, 0);
        }
        /* title and clock */
        float mx = 60 * S;
        text(vp.title, mx, 38 * S, 38 * S, 1, th.text, c, AL_LEFT, (int)(W - mx * 2 - 200 * S));
        if (vp.nlist > 1) {
            char n[48];
            snprintf(n, sizeof n, "%d of %d", vp.cur + 1, vp.nlist);
            text(n, mx, 38 * S + 46 * S, 22 * S, 0, th.text_soft, c, AL_LEFT, 0);
        }
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char hm[32];
        clock_time_text(hm, sizeof hm, &tm);
        text(hm, (float)W - mx, 40 * S, 30 * S, 1, th.text, c, AL_RIGHT, 0);
        /* the progress bar */
        float bx0 = 170 * S, bx1 = (float)W - 170 * S, by = (float)H - 128 * S, bh = 12 * S;
        char t1[24], t2[24];
        vp_time(t1, sizeof t1, vp.st.pos);
        vp_time(t2, sizeof t2, vp.st.dur);
        text(t1, mx, by + bh / 2 - 15 * S, 27 * S, 1, th.text, c, AL_LEFT, 0);
        text(vp.st.dur > 0 ? t2 : "--:--", (float)W - mx, by + bh / 2 - 15 * S, 27 * S, 0, th.text_soft, c, AL_RIGHT, 0);
        float fr = vp.st.dur > 0 ? clampf((float)(vp.st.pos / vp.st.dur), 0, 1) : 0;
        pane(bx0, by, bx1 - bx0, bh, bh / 2, CIRCLE, with_alpha(white, 0.28f), 0, 0, 0, c, 0);
        pane(bx0, by, (bx1 - bx0) * fr, bh, bh / 2, CIRCLE, th.accent, 0, 0, 0, c, 0);
        float kr = 15 * S;
        pane(bx0 + (bx1 - bx0) * fr - kr, by + bh / 2 - kr, kr * 2, kr * 2, kr, CIRCLE, white, 0, 0, 0, c, 0);
    }

    if (loading) {
        float p = 0.5f + 0.5f * sinf(now_s * 5.0f);
        float r = (16 + 5 * p) * S;
        pane((float)W / 2 - r, (float)H / 2 - r, r * 2, r * 2, r, CIRCLE, with_alpha(th.accent, 0.5f + 0.4f * p), 0, 0, 0, 1, 0);
        text(vp.st.loaded ? "Buffering\xE2\x80\xA6" : "Loading\xE2\x80\xA6", (float)W / 2, (float)H / 2 + 44 * S, 28 * S, 1, th.text, 0.95f, AL_CENTER, 0);
    } else if (vp.user_paused && !vp.panel) {
        float r = 105 * S;
        pane((float)W / 2 - r, (float)H / 2 - r, r * 2, r * 2, r, CIRCLE, (Col){ 0, 0, 0, 0.5f }, 0, 0, 0, 1, 0);
        glyph("media-play", (float)W / 2, (float)H / 2, 130 * S, th.text, 1);
    } else if (now_s < vp.resume_flash) {
        float t = (vp.resume_flash - now_s) / 0.55f;
        float r = (105 + (1 - t) * 40) * S;
        pane((float)W / 2 - r, (float)H / 2 - r, r * 2, r * 2, r, CIRCLE, (Col){ 0, 0, 0, 0.5f }, 0, 0, 0, t, 0);
        glyph("media-pause", (float)W / 2, (float)H / 2, 130 * S * (1 + (1 - t) * 0.3f), th.text, t);
    }

    /* how far the last skips went, on the side they went toward */
    if (now_s < vp.seek_until && vp.seek_acc != 0) {
        float t = clampf((vp.seek_until - now_s) / 0.25f, 0, 1);
        char s[32];
        vp_skip_text(s, sizeof s, vp.seek_acc);
        float pw = 300 * S, ph = 120 * S, cx = (float)W * (vp.seek_acc > 0 ? 0.76f : 0.24f), cy = (float)H * 0.46f;
        pane(cx - pw / 2, cy - ph / 2, pw, ph, ph / 2, SQUIRCLE, (Col){ 0, 0, 0, 0.62f }, 0, 0, 0, t, 0);
        glyph(vp.seek_acc > 0 ? "arrow-right" : "arrow-left", cx - 82 * S, cy, 50 * S, th.text, t);
        text(s, cx - 38 * S, cy - 25 * S, 46 * S, 1, th.text, t, AL_LEFT, 0);
    }
    if (now_s < vp.vol_until) {
        float t = clampf((vp.vol_until - now_s) / 0.25f, 0, 1);
        float pw = 520 * S, ph = 84 * S, px = (float)W / 2 - pw / 2, py = 70 * S;
        pane(px, py, pw, ph, ph / 2, SQUIRCLE, (Col){ 0, 0, 0, 0.7f }, 0, 0, 0, t, 0);
        glyph(vp.volume == 0 ? "volume-muted" : "volume", px + 56 * S, py + ph / 2, 42 * S, th.text, t);
        float sx = px + 100 * S, sw = pw - 190 * S;
        pane(sx, py + ph / 2 - 5 * S, sw, 10 * S, 5 * S, CIRCLE, with_alpha(white, 0.28f), 0, 0, 0, t, 0);
        pane(sx, py + ph / 2 - 5 * S, sw * (float)vp.volume / 100.0f, 10 * S, 5 * S, CIRCLE, th.accent, 0, 0, 0, t, 0);
        char v[16];
        snprintf(v, sizeof v, "%d%%", vp.volume);
        text(v, px + pw - 28 * S, py + ph / 2 - 15 * S, 28 * S, 1, th.text, t, AL_RIGHT, 0);
    }
    if (vp.status[0] && now_s < vp.status_until) {
        float tw = fmaxf((float)text_width(vp.status, 30 * S, 1) + 90 * S, 300 * S);
        float py = (float)H - 270 * S;
        pane((float)W / 2 - tw / 2, py, tw, 76 * S, 38 * S, SQUIRCLE, (Col){ 0, 0, 0, 0.72f }, 0, 0, 0, 1, 0);
        text(vp.status, (float)W / 2, py + 21 * S, 30 * S, 1, white, 1, AL_CENTER, 0);
    }

    /* the options, down the right side */
    float pw = fmaxf(W * 0.36f, 330 * S);
    vp.menu.anim += ((vp.panel ? 1.0f : 0.0f) - vp.menu.anim) * 0.3f;
    if (!vp.panel && vp.menu.anim < 0.02f) vp.menu.anim = 0;
    if (vp.menu.anim > 0) {
        float a = vp.menu.anim, x = (float)W - pw * a;
        pane(x, 0, pw + 40 * S, (float)H, 0, CIRCLE, (Col){ 0.03f, 0.03f, 0.05f, 0.94f }, 0, 0, 0, 1, 0);
        pane(x, 0, pw + 40 * S, (float)H, 0, SQUIRCLE, (Col){ 0.05f + th.accent.r * 0.16f, 0.05f + th.accent.g * 0.16f, 0.07f + th.accent.b * 0.16f, 0.97f },
             0, 0, GLOSS_QUIET, 1, 0);
        pane(x, 0, 2 * S, (float)H, 0, SQUIRCLE, with_alpha(th.text, 0.35f), 0, 0, 0, 1, 0);
        static const char *titles[] = { "Options", "Audio", "Subtitles", "Playback speed", "Picture", "Subtitle size" };
        draw_blist(&vp.menu, x, pw, titles[vp.view], 1);
    }

    if (dlg.kind == D_NONE) {
        if (vp.panel) {
            Hint h[2] = { { pad_ok(), "Select" }, { pad_back(), vp.view == VV_MAIN ? "Close" : "Back" } };
            draw_hints(h, 2);
        } else if (vp.ctl > 0.6f) {
            char lo[24], hi[24];
            vp_skip_text(lo, sizeof lo, -cfg.video_skip_long);
            vp_skip_text(hi, sizeof hi, cfg.video_skip_long);
            Hint h[3] = { { "pad-south", vp.user_paused ? "Play" : "Pause" }, { "pad-north", "Options" }, { "pad-east", "Close" } };
            draw_hints(h, 3);
            Hint l[2] = { { "pad-left-bumper", lo }, { "pad-right-bumper", hi } };
            draw_hints_at(l, 2, 1);
        }
    }
}

/* ======================================================================== */
/* The terminal                                                             */
/* ======================================================================== */
/* Pixi's own terminal. The screen is drawn from the engine's cells (terminal.c) with the
 * keyboard always docked below it, so nothing the program prints is ever hidden behind keys. */

static struct {
    Term *t;
    int open, ended, code, is_shell;
    char name[96];
    int fpx, cols, rows;
    float cw, chh, x0, y0, kbh;
    int r, c; /* the highlighted key (Shift, Ctrl and the rest are the PC keyboard's, pk) */
    int sq_down, sq_used; /* Square held: the D-pad sends arrows and the sticks scroll; a lone tap is a space */
    float scroll_acc;
    float hint_until;
} tm;

static int term_is_open(void) { return tm.open && tm.t; }
static int term_is_running(void) { return tm.t != NULL; }

static void term_set_open(int open)
{
    if (open && tm.t) {
        if (!tm.open) SDL_StartTextInput();
        tm.open = 1;
    } else {
        if (tm.open) SDL_StopTextInput();
        tm.open = 0;
    }
}

static void term_quit(void)
{
    if (tm.open) SDL_StopTextInput();
    if (tm.t) term_free(tm.t);
    memset(&tm, 0, sizeof tm);
}

static int term_child_hook(pid_t pid, int status) { return tm.t ? term_child_reaped(tm.t, pid, status) : 0; }

/* what the screen needs to fit: font, cell size, and how many columns and rows are left above the keys */
static void term_layout_metrics(void)
{
    tm.fpx = (int)lroundf(H * 0.026f);
    if (tm.fpx < 9) tm.fpx = 9;
    TTF_Font *f = font_get(tm.fpx, 2, 0);
    int w = 0;
    TTF_SizeUTF8(f, "MMMMMMMMMMMMMMMMMMMM", &w, NULL);
    tm.cw = w > 0 ? w / 20.0f : tm.fpx * 0.6f;
    tm.chh = (float)TTF_FontLineSkip(f);
    if (tm.chh < tm.fpx) tm.chh = (float)tm.fpx;
    tm.kbh = floorf(H * 0.40f);
    float pad = 6 * S;
    tm.cols = (int)floorf((W - 2 * pad) / tm.cw);
    tm.rows = (int)floorf((H - tm.kbh - pad) / tm.chh);
    if (tm.cols < 20) tm.cols = 20;
    if (tm.rows < 4) tm.rows = 4;
    tm.x0 = floorf((W - tm.cols * tm.cw) / 2);
    tm.y0 = floorf(pad * 0.5f);
}

static void term_open(char **argv, const char *name)
{
    if (tm.t && argv && !tm.ended) {
        show_message("The terminal is in use", "Close it from the Guide first, then open this again.");
        snd(SND_ERROR);
        return;
    }
    if (tm.t && (argv || tm.ended)) term_quit();
    if (!tm.t) {
        term_layout_metrics();
        tm.t = term_new(tm.cols, tm.rows);
        if (!tm.t || term_spawn(tm.t, argv, home_dir()) != 0) {
            if (tm.t) term_free(tm.t);
            memset(&tm, 0, sizeof tm);
            show_message("Couldn't open the terminal", "It couldn't start a shell.");
            snd(SND_ERROR);
            return;
        }
        sncpy(tm.name, sizeof tm.name, name ? name : "");
        tm.is_shell = argv == NULL;
        pk_reset();
        tm.r = 3;
        tm.c = 3; /* the space bar */
    }
    app_hide_all();
    term_set_open(1);
    tm.hint_until = now_s + 7.0f;
    snd(SND_SELECT);
}

/* typed text on its way to the program: Ctrl and Alt apply to the first character */
static void tk_send(const char *s, int ctrl, int alt)
{
    if (!tm.t || tm.ended) return;
    unsigned char c = (unsigned char)s[0];
    if (alt) term_write(tm.t, "\033", 1);
    if (ctrl && c && !s[1]) {
        if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 1);
        else if (c == '[') c = 0x1B;
        else if (c == '\\') c = 0x1C;
        else if (c == ']') c = 0x1D;
        else if (c == '^' || c == '6') c = 0x1E;
        else if (c == '_' || c == '/' || c == '-') c = 0x1F;
        else if (c == '@' || c == ' ' || c == '2') c = 0;
        else if (c == '?') c = 0x7F;
        char b[1] = { (char)c };
        term_write(tm.t, b, 1);
    } else term_write(tm.t, s, strlen(s));
    snd(SND_KEY);
}

/* the escape sequences xterm sends for F1 to F12 */
static void tk_fkey(int n, int alt)
{
    static const char *seq[13] = { "", "\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~",
                                   "\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~" };
    if (!tm.t || tm.ended || n < 1 || n > 12) return;
    if (alt) term_write(tm.t, "\033", 1);
    term_write(tm.t, seq[n], strlen(seq[n]));
    snd(SND_KEY);
}

static void tk_special(int key)
{
    term_key(tm.t, key);
    snd(SND_KEY);
}

static void tk_press(const PKey *k)
{
    PKOut o;
    if (!k || !pk_press(k, &o)) return;
    switch (o.type) {
    case KY_CHAR:
        if (o.fkey) tk_fkey(o.fkey, o.alt);
        else tk_send(o.text, o.ctrl, o.alt);
        break;
    case KY_SPACE: tk_send(" ", o.ctrl, o.alt); break;
    case KY_ENTER: tk_send("\r", 0, o.alt); break;
    case KY_BKSP: tk_send(o.ctrl ? "\x08" : "\x7f", 0, o.alt); break;
    case KY_ESC: tk_send("\033", 0, 0); break;
    case KY_TAB:
        if (o.shift) tk_special(TK_TAB_BACK);
        else tk_send("\t", 0, o.alt);
        break;
    case KY_UP: tk_special(TK_UP); break;
    case KY_DOWN: tk_special(TK_DOWN); break;
    case KY_LEFT: tk_special(TK_LEFT); break;
    case KY_RIGHT: tk_special(TK_RIGHT); break;
    case KY_HOME: tk_special(o.shift ? TK_END : TK_HOME); break;
    case KY_DEL: tk_special(o.shift ? TK_INS : TK_DEL); break;
    case KY_PGUP: /* Page Up, or Page Down with Shift; with Ctrl (R) it scrolls the history instead */
        if (o.ctrl) term_scroll_view(tm.t, (o.shift ? -1 : 1) * (tm.rows / 2 > 0 ? tm.rows / 2 : 1)), snd(SND_STEP);
        else tk_special(o.shift ? TK_PGDN : TK_PGUP);
        break;
    case KY_SUPER: handle_action(ACT_GUIDE); break;
    default: break;
    }
}

static void term_action(int a)
{
    if (!tm.t) return;
    if (tm.ended) {
        if (a == ACT_OK || a == ACT_BACK || a == ACT_START || a == ACT_MENU) {
            term_quit();
            snd(SND_BACK);
        } else if (a == ACT_PGUP) term_scroll_view(tm.t, tm.rows / 2);
        else if (a == ACT_PGDN) term_scroll_view(tm.t, -tm.rows / 2);
        return;
    }
    switch (a) {
    case ACT_UP: case ACT_DOWN: case ACT_LEFT: case ACT_RIGHT:
        if (tm.sq_down) { /* Square + D-pad: the arrow keys */
            tk_special(a == ACT_UP ? TK_UP : a == ACT_DOWN ? TK_DOWN : a == ACT_LEFT ? TK_LEFT : TK_RIGHT);
            tm.sq_used = 1;
        } else pk_nav(a, &tm.r, &tm.c);
        break;
    case ACT_OK: tk_press(pk_at(tm.r, tm.c)); break;
    case ACT_BACK: tk_send("\x7f", 0, 0); break;
    case ACT_SQUARE: /* a space when it comes back up on its own (see term_square_up) */
        tm.sq_down = 1;
        tm.sq_used = 0;
        tm.scroll_acc = 0;
        break;
    case ACT_MENU: tk_send("\x7f", 0, 0); break; /* Triangle: backspace, like B */
    case ACT_START: tk_send("\t", 0, 0); break;
    case ACT_PGUP: term_scroll_view(tm.t, tm.rows / 2 > 0 ? tm.rows / 2 : 1); snd(SND_STEP); break;
    case ACT_PGDN: term_scroll_view(tm.t, -(tm.rows / 2 > 0 ? tm.rows / 2 : 1)); snd(SND_STEP); break;
    }
}

/* Square came back up: a space if nothing was done with it held */
static void term_square_up(void)
{
    int tap = tm.sq_down && !tm.sq_used;
    tm.sq_down = tm.sq_used = 0;
    if (tap && term_is_open() && !tm.ended && !guide.open && dlg.kind == D_NONE) tk_send(" ", 0, 0);
}

/* Square + either stick scrolls the history: the further it's pushed, the faster (up goes back) */
static void term_stick_tick(Uint32 now)
{
    static Uint32 last;
    float dt = last ? (float)(now - last) / 1000.0f : 0;
    last = now;
    if (!tm.sq_down || !term_is_open() || guide.open || dlg.kind != D_NONE || dt <= 0 || dt > 0.25f) return;
    float v = stick_axis(1), v2 = stick_axis(3);
    if (fabsf(v2) > fabsf(v)) v = v2;
    if (v == 0) {
        tm.scroll_acc = 0;
        return;
    }
    tm.sq_used = 1;
    tm.scroll_acc += -v * (6.0f + 54.0f * fabsf(v)) * dt; /* 6 to 60 lines a second */
    int n = (int)tm.scroll_acc;
    if (n) {
        term_scroll_view(tm.t, n);
        tm.scroll_acc -= (float)n;
    }
}

/* a real keyboard plugged in: its keys go to the program. Returns 1 when it used the key. */
static int term_sdl_key(const SDL_KeyboardEvent *k)
{
    if (!term_is_open() || guide.open || dlg.kind != D_NONE || hidden) return 0;
    SDL_Keycode sym = k->keysym.sym;
    if (sym == SDLK_F1 || sym == SDLK_HOME) return 0; /* the guide */
    int shift = (k->keysym.mod & KMOD_SHIFT) != 0, ctrl = (k->keysym.mod & KMOD_CTRL) != 0;
    if (tm.ended) {
        if (!k->repeat && (sym == SDLK_RETURN || sym == SDLK_ESCAPE || sym == SDLK_SPACE || sym == SDLK_BACKSPACE)) term_quit();
        return 1;
    }
    const char *s = NULL;
    switch (sym) {
    case SDLK_RETURN: case SDLK_KP_ENTER: s = "\r"; break;
    case SDLK_BACKSPACE: s = "\x7f"; break;
    case SDLK_TAB: s = shift ? "\033[Z" : "\t"; break;
    case SDLK_ESCAPE: s = "\033"; break;
    case SDLK_UP: term_key(tm.t, TK_UP); return 1;
    case SDLK_DOWN: term_key(tm.t, TK_DOWN); return 1;
    case SDLK_LEFT: term_key(tm.t, TK_LEFT); return 1;
    case SDLK_RIGHT: term_key(tm.t, TK_RIGHT); return 1;
    case SDLK_END: term_key(tm.t, TK_END); return 1;
    case SDLK_DELETE: term_key(tm.t, TK_DEL); return 1;
    case SDLK_INSERT: term_key(tm.t, TK_INS); return 1;
    case SDLK_PAGEUP:
        if (shift) term_scroll_view(tm.t, tm.rows / 2);
        else term_key(tm.t, TK_PGUP);
        return 1;
    case SDLK_PAGEDOWN:
        if (shift) term_scroll_view(tm.t, -tm.rows / 2);
        else term_key(tm.t, TK_PGDN);
        return 1;
    default: break;
    }
    if (s) {
        term_write(tm.t, s, strlen(s));
        return 1;
    }
    if (ctrl && sym >= 'a' && sym <= 'z') {
        char b[2] = { (char)(sym - 'a' + 1), 0 };
        if (k->keysym.mod & KMOD_ALT) term_write(tm.t, "\033", 1);
        term_write(tm.t, b, 1);
        return 1;
    }
    if (ctrl && (sym == SDLK_LEFTBRACKET || sym == SDLK_RIGHTBRACKET || sym == SDLK_BACKSLASH || sym == SDLK_SPACE || sym == SDLK_2)) {
        char b = sym == SDLK_LEFTBRACKET ? 0x1B : sym == SDLK_RIGHTBRACKET ? 0x1D : sym == SDLK_BACKSLASH ? 0x1C : 0;
        term_write(tm.t, &b, 1);
        return 1;
    }
    return 1; /* printable keys arrive as text; nothing else should reach the menus */
}

static void term_sdl_text(const char *s)
{
    if (!term_is_open() || guide.open || dlg.kind != D_NONE || tm.ended) return;
    SDL_Keymod m = SDL_GetModState();
    if (m & KMOD_CTRL) return;
    if (m & KMOD_ALT) term_write(tm.t, "\033", 1);
    term_write(tm.t, s, strlen(s));
}

static void term_tick(void)
{
    if (!tm.t) return;
    int r = term_pump(tm.t);
    term_take_bell(tm.t);
    if ((r & 2) && !tm.ended) {
        term_pump(tm.t); /* whatever it printed last */
        tm.ended = 1;
        tm.code = term_exit_code(tm.t);
        if (tm.code == 0 && (tm.is_shell || term_age(tm.t) > 3.0)) {
            term_quit(); /* closed on purpose: `exit` */
            return;
        }
        char msg[160];
        snprintf(msg, sizeof msg, "\r\n\033[7m  Finished%s  \033[0m  Press any button to close.\r\n",
                 tm.code ? (tm.code == 127 ? " (couldn't be started)" : " with an error") : "");
        term_feed(tm.t, msg, strlen(msg));
    }
}

/* ---------- drawing ----------------------------------------------------- */

static Col term_palette(int n)
{
    static const Col base[16] = {
        { 0.13f, 0.14f, 0.17f, 1 }, { 0.82f, 0.28f, 0.28f, 1 }, { 0.36f, 0.76f, 0.38f, 1 }, { 0.87f, 0.77f, 0.32f, 1 },
        { 0.36f, 0.52f, 0.92f, 1 }, { 0.76f, 0.42f, 0.82f, 1 }, { 0.30f, 0.76f, 0.82f, 1 }, { 0.80f, 0.82f, 0.85f, 1 },
        { 0.42f, 0.44f, 0.50f, 1 }, { 1.00f, 0.42f, 0.42f, 1 }, { 0.47f, 0.92f, 0.47f, 1 }, { 0.97f, 0.90f, 0.42f, 1 },
        { 0.52f, 0.67f, 1.00f, 1 }, { 0.92f, 0.57f, 0.97f, 1 }, { 0.42f, 0.92f, 0.97f, 1 }, { 0.98f, 0.98f, 0.99f, 1 } };
    if (n < 16) return base[n];
    if (n < 232) {
        static const float lv[6] = { 0, 95 / 255.0f, 135 / 255.0f, 175 / 255.0f, 215 / 255.0f, 1 };
        int i = n - 16;
        return (Col){ lv[i / 36], lv[(i / 6) % 6], lv[i % 6], 1 };
    }
    float g = (8 + 10 * (n - 232)) / 255.0f;
    return (Col){ g, g, g, 1 };
}

static const Col term_default_fg = { 0.88f, 0.90f, 0.92f, 1 };
static const Col term_default_bg = { 0.03f, 0.03f, 0.06f, 1 };

static Col term_color(uint32_t c, int is_fg)
{
    switch (TC_KIND(c)) {
    case 1: return term_palette((int)(c & 255));
    case 2: return (Col){ ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, 1 };
    default: return is_fg ? term_default_fg : term_default_bg;
    }
}

static void term_utf8(uint32_t cp, char *o, int *n)
{
    if (cp < 0x80) o[(*n)++] = (char)cp;
    else if (cp < 0x800) {
        o[(*n)++] = (char)(0xC0 | (cp >> 6));
        o[(*n)++] = (char)(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        o[(*n)++] = (char)(0xE0 | (cp >> 12));
        o[(*n)++] = (char)(0x80 | ((cp >> 6) & 63));
        o[(*n)++] = (char)(0x80 | (cp & 63));
    } else {
        o[(*n)++] = (char)(0xF0 | (cp >> 18));
        o[(*n)++] = (char)(0x80 | ((cp >> 12) & 63));
        o[(*n)++] = (char)(0x80 | ((cp >> 6) & 63));
        o[(*n)++] = (char)(0x80 | (cp & 63));
    }
}

/* the plain text of one screen row, for the Guide's little picture */
static void term_row_text(int y, char *out, size_t cap)
{
    int n = 0;
    char buf[1024];
    for (int x = 0; x < term_cols(tm.t) && n < (int)sizeof buf - 8; x++) {
        const TCell *c = term_cell(tm.t, x, y);
        if (c->attr & TA_CONT) continue;
        term_utf8(c->ch >= 32 ? c->ch : ' ', buf, &n);
    }
    while (n > 0 && buf[n - 1] == ' ') n--;
    buf[n] = 0;
    sncpy(out, cap, buf);
}

static void draw_term_screen(void)
{
    const int cols = term_cols(tm.t), rows = term_rows(tm.t);
    const float cw = tm.cw, chh = tm.chh, fs = (float)tm.fpx, lift = fs * 0.18f;
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, with_alpha(term_default_bg, 0.96f), 0, 0, 0, 1, 0);
    int cxp = 0, cyp = 0;
    int cur = term_cursor(tm.t, &cxp, &cyp);
    for (int y = 0; y < rows; y++) {
        float ry = tm.y0 + y * chh;
        /* backgrounds first: runs of the same color are one rectangle */
        for (int x = 0; x < cols;) {
            const TCell *c = term_cell(tm.t, x, y);
            uint32_t bg = (c->attr & TA_REVERSE) ? (TC_KIND(c->fg) ? c->fg : TC_PAL(7)) : c->bg;
            int rev_default = (c->attr & TA_REVERSE) && !TC_KIND(c->fg);
            int has = TC_KIND(bg) != 0 || rev_default;
            int x2 = x + 1;
            while (x2 < cols) {
                const TCell *d = term_cell(tm.t, x2, y);
                uint32_t b2 = (d->attr & TA_REVERSE) ? (TC_KIND(d->fg) ? d->fg : TC_PAL(7)) : d->bg;
                int r2 = (d->attr & TA_REVERSE) && !TC_KIND(d->fg);
                if (b2 != bg || r2 != rev_default) break;
                x2++;
            }
            if (has) {
                Col col = rev_default ? term_default_fg : term_color(bg, 0);
                pane(floorf(tm.x0 + x * cw), floorf(ry), ceilf((x2 - x) * cw), ceilf(chh), 0, CIRCLE, col, 0, 0, 0, 1, 0);
            }
            x = x2;
        }
        /* then the text, in runs that share a color and weight */
        char run[1024];
        for (int x = 0; x < cols;) {
            const TCell *c = term_cell(tm.t, x, y);
            if (c->attr & TA_CONT) {
                x++;
                continue;
            }
            uint32_t fg = c->fg, bgc = c->bg;
            int rev = (c->attr & TA_REVERSE) != 0;
            Col col = rev ? (TC_KIND(bgc) ? term_color(bgc, 0) : term_default_bg) : term_color(fg, 1);
            int bold = (c->attr & TA_BOLD) ? 1 : 0;
            if (c->attr & TA_DIM) col.r *= 0.65f, col.g *= 0.65f, col.b *= 0.65f;
            if ((c->attr & TA_WIDE) && c->ch > 32) { /* a wide character is drawn on its own, two cells across */
                char one[8];
                int n = 0;
                term_utf8(c->ch, one, &n);
                one[n] = 0;
                text(one, tm.x0 + x * cw + (2 * cw - fs) * 0.5f, ry + lift, fs, bold, col, (c->attr & TA_DIM) ? 0.6f : 1, AL_LEFT, 0);
                x += 2;
                continue;
            }
            int n = 0, x2 = x, last_ink = x - 1;
            while (x2 < cols && n < (int)sizeof run - 8) {
                const TCell *d = term_cell(tm.t, x2, y);
                if (d->attr & TA_CONT) break;
                if ((d->attr & TA_WIDE) && d->ch > 32) break;
                int same_rev = ((d->attr & TA_REVERSE) != 0) == rev;
                if (!(d->ch == ' ' || d->ch == 0 || d->ch < 32) &&
                    (d->fg != fg || (rev && d->bg != bgc) || ((d->attr & TA_BOLD) != 0) != bold || !same_rev))
                    break;
                if (!(d->ch == ' ' || d->ch == 0 || d->ch < 32)) last_ink = x2;
                term_utf8(d->ch >= 32 ? d->ch : ' ', run, &n);
                x2++;
            }
            int keep = last_ink - x + 1; /* trailing spaces aren't worth drawing */
            if (keep > 0) {
                /* cut the run back to the last cell that has ink */
                int m = 0, cells = 0;
                for (; m < n && cells < keep; cells++) {
                    unsigned char b = (unsigned char)run[m];
                    m += b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
                }
                run[m] = 0;
                text(run, tm.x0 + x * cw, ry + lift, fs, bold | 2, col, 1, AL_LEFT, 0);
            }
            /* underline and strike-through ride along per cell run */
            for (int u = x; u < x2; u++) {
                const TCell *d = term_cell(tm.t, u, y);
                if (d->attr & (TA_UNDER | TA_STRIKE)) {
                    Col uc = (d->attr & TA_REVERSE) ? term_default_bg : term_color(d->fg, 1);
                    if (d->attr & TA_UNDER) pane(floorf(tm.x0 + u * cw), floorf(ry + chh - 2), ceilf(cw), 1, 0, CIRCLE, uc, 0, 0, 0, 1, 0);
                    if (d->attr & TA_STRIKE) pane(floorf(tm.x0 + u * cw), floorf(ry + chh * 0.55f), ceilf(cw), 1, 0, CIRCLE, uc, 0, 0, 0, 1, 0);
                }
            }
            x = x2 > x ? x2 : x + 1;
        }
    }
    if (cur && cyp < rows && cxp < cols) {
        const TCell *c = term_cell(tm.t, cxp, cyp);
        float px = floorf(tm.x0 + cxp * cw), py = floorf(tm.y0 + cyp * chh);
        float pulse = 0.78f + 0.22f * sinf(now_s * 5.0f);
        pane(px, py, ceilf(cw * ((c->attr & TA_WIDE) ? 2 : 1)), ceilf(chh), 0, CIRCLE, with_alpha(th.accent, pulse), 0, 0, 0, 1, 0);
        if (c->ch > 32 && !(c->attr & TA_CONT)) {
            char one[8];
            int n = 0;
            term_utf8(c->ch, one, &n);
            one[n] = 0;
            text(one, px, py + lift, fs, ((c->attr & TA_BOLD) ? 1 : 0) | 2, term_default_bg, 1, AL_LEFT, 0);
        }
    }
    if (term_scrolled(tm.t)) {
        char b[48];
        snprintf(b, sizeof b, "%d lines back", term_scrolled(tm.t));
        float tw = (float)text_width(b, 15 * S, 0);
        pane(W - tw - 30 * S, 8 * S, tw + 22 * S, 26 * S, 13 * S, SQUIRCLE, with_alpha(th.glass, 0.85f), DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_QUIET, 1, 0);
        text(b, W - tw - 19 * S, 13 * S, 15 * S, 0, th.text, 1, AL_LEFT, 0);
    }
    if (now_s < tm.hint_until) {
        float a = clampf(tm.hint_until - now_s, 0, 1);
        const char *h = pk_hint(0);
        float tw = (float)text_width(h, 16 * S, 0);
        float px = (W - tw) / 2 - 14 * S, py = tm.y0 + 8 * S;
        pane(px, py, tw + 28 * S, 28 * S, 14 * S, SQUIRCLE, with_alpha(th.glass, 0.88f), DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_QUIET, a, 0);
        text(h, px + 14 * S, py + 6 * S, 16 * S, 0, th.text, a, AL_LEFT, 0);
    }
}

static void draw_term_keys(void)
{
    float y0 = (float)H - tm.kbh, pad = 6 * S;
    pane(0, y0, (float)W, tm.kbh + 40 * S, 0, SQUIRCLE, with_alpha(th.glass, 0.92f), DEPTH_PANEL * S, FROST_PANEL, GLOSS_QUIET, 1, 1);
    pane(0, y0, (float)W, 2 * S, 0, CIRCLE, with_alpha(th.text, 0.30f), 0, 0, 0, 1, 0);
    pk_draw(pad, y0 + pad, W - 2 * pad, tm.kbh - 2 * pad, tm.r, tm.c, 1, 0);
}

static void draw_terminal(void)
{
    if (!tm.t) return;
    draw_term_screen();
    draw_term_keys();
}

/* the Guide's card: the last lines on the screen, small */
static void term_draw_card(float x, float y, float w, float h, float fade)
{
    pane(x, y, w, h, 20 * S, SQUIRCLE, with_alpha(term_default_bg, 0.96f), 0, 0, GLOSS_QUIET, fade, 0);
    if (!tm.t) return;
    int rows = term_rows(tm.t), last = 0;
    char line[400];
    for (int r = rows - 1; r >= 0; r--) {
        term_row_text(r, line, sizeof line);
        if (line[0]) {
            last = r;
            break;
        }
    }
    float fs = fmaxf(8.0f, h * 0.06f), step = fs * 1.3f;
    int n = (int)((h - 28 * S) / step);
    if (n > last + 1) n = last + 1;
    for (int i = 0; i < n; i++) {
        term_row_text(last - n + 1 + i, line, sizeof line);
        text(line, x + 14 * S, y + 14 * S + i * step, fs, 2, term_default_fg, fade * 0.9f, AL_LEFT, (int)(w - 28 * S));
    }
}

/* typed from a test script: \n is the Enter key */
static void term_script_text(const char *s)
{
    if (!tm.t) term_open(NULL, NULL);
    if (!tm.t) return;
    char buf[512];
    size_t o = 0;
    for (; *s && o + 1 < sizeof buf; s++) {
        if (s[0] == '\\' && s[1] == 'n') buf[o++] = '\r', s++;
        else if (s[0] == '\\' && s[1] == 'e') buf[o++] = '\033', s++;
        else buf[o++] = *s;
    }
    term_write(tm.t, buf, o);
}


/* ======================================================================== */
/* Pixi's own apps, among the windows in the Guide                          */
/* ======================================================================== */

static int app_is_ours(uint32_t id) { return id == APP_BROWSER || id == APP_VIDEO || id == APP_MUSIC || id == APP_STORE || id == APP_TERMINAL || id == APP_FTP || id == APP_WEB; }

static int app_running(uint32_t id)
{
    return id == APP_BROWSER ? bui.running : id == APP_VIDEO ? vp.running : id == APP_MUSIC ? mp.nq > 0 : id == APP_STORE ? store_is_running() : id == APP_TERMINAL ? term_is_running() : id == APP_FTP ? ftpui.running : id == APP_WEB ? web_ui_running() : 0;
}

/* the one of them that's on the screen, if any */
static uint32_t app_front(void)
{
    if (bui.open) return APP_BROWSER;
    if (vp.open) return APP_VIDEO;
    if (np.open) return APP_MUSIC;
    if (store_is_open()) return APP_STORE;
    if (term_is_open()) return APP_TERMINAL;
    if (ftpui.open) return APP_FTP;
    if (webui.open) return APP_WEB;
    return 0;
}

static void app_hide_all(void)
{
    ed_abandon();
    bui.open = vp.open = np.open = 0;
    store_set_open(0);
    term_set_open(0);
    ftpui.open = 0;
    webui.open = 0;
    bui.panel = vp.panel = 0;
}

static void app_switch(uint32_t id)
{
    guide.open = 0;
    guide.power_open = 0;
    comp_clear_previews();
    app_hide_all();
    if (id == APP_BROWSER) bui.open = 1;
    else if (id == APP_VIDEO) vp.open = 1, vp.ctl = 1, vp_show();
    else if (id == APP_MUSIC) np.open = 1;
    else if (id == APP_STORE) store_set_open(1);
    else if (id == APP_TERMINAL) term_set_open(1);
    else if (id == APP_FTP) ftpui.open = 1;
    else if (id == APP_WEB) webui.open = 1;
    comp_set_mode(MODE_HOME); /* hide any program's window: this is on the shell's own screen */
    snd(SND_GUIDE_SELECT);
}

static void app_close(uint32_t id)
{
    if (id == APP_BROWSER) browser_quit();
    else if (id == APP_VIDEO) video_close();
    else if (id == APP_MUSIC) music_stop();
    else if (id == APP_STORE) store_quit();
    else if (id == APP_TERMINAL) term_quit();
    else if (id == APP_FTP) ftp_ui_stop();
    else if (id == APP_WEB) web_ui_stop();
}

static const char *app_label(uint32_t id)
{
    if (id == APP_BROWSER) return "Web browser";
    if (id == APP_VIDEO) return vp.title[0] ? vp.title : "Video player";
    if (id == APP_STORE) return "App Store";
    if (id == APP_TERMINAL) return "Terminal";
    if (id == APP_FTP) return "FTP Server";
    if (id == APP_WEB) return "Web Server";
    return "Music";
}

/* what each shows in its card */
static void app_draw_card(uint32_t id, float x, float y, float w, float h, float fade)
{
    const Col white = { 1, 1, 1, 1 };
    if (id == APP_BROWSER) {
        if (bui.tex && bui.tw) picture_card(bui.tex, x, y, w, h, white, fade, 20 * S, 0);
        else glyph("category-internet", x + w / 2, y + h / 2, fminf(w, h) * 0.34f, th.text, fade * 0.9f);
    } else if (id == APP_VIDEO) {
        if (vp.tex && vp.have_frame) picture_card(vp.tex, x, y, w, h, white, fade, 20 * S, 0);
        else {
            pane(x, y, w, h, 20 * S, SQUIRCLE, (Col){ 0, 0, 0, 0.85f }, 0, 0, 0, fade, 0);
            glyph("category-video", x + w / 2, y + h / 2, fminf(w, h) * 0.34f, th.text, fade * 0.9f);
        }
    } else if (id == APP_TERMINAL) {
        term_draw_card(x, y, w, h, fade);
    } else if (id == APP_FTP) {
        char ip[64], line[120];
        ftp_address(ip, sizeof ip);
        pane(x, y, w, h, 20 * S, SQUIRCLE, with_alpha(th.accent, 0.38f), 0, 0, GLOSS_QUIET, fade, 0);
        glyph("setting-network", x + w / 2, y + h * 0.38f, fminf(w, h) * 0.30f, th.text, fade);
        text("FTP Server", x + w / 2, y + h * 0.64f, 27 * S, 1, th.text, fade, AL_CENTER, (int)(w - 40 * S));
        if (ip[0]) snprintf(line, sizeof line, "ftp://%s:%d", ip, cfg.ftp_port);
        else sncpy(line, sizeof line, "Not connected to a network");
        text(line, x + w / 2, y + h * 0.64f + 40 * S, 20 * S, 0, th.text_soft, fade, AL_CENTER, (int)(w - 40 * S));
    } else if (id == APP_WEB) {
        char ip[64], line[120];
        WebStatus st;
        web_status(&st);
        ftp_address(ip, sizeof ip);
        pane(x, y, w, h, 20 * S, SQUIRCLE, with_alpha(th.accent, 0.38f), 0, 0, GLOSS_QUIET, fade, 0);
        glyph("category-internet", x + w / 2, y + h * 0.38f, fminf(w, h) * 0.30f, th.text, fade);
        text("Web Server", x + w / 2, y + h * 0.64f, 27 * S, 1, th.text, fade, AL_CENTER, (int)(w - 40 * S));
        if (ip[0]) snprintf(line, sizeof line, "http://%s:%d", ip, st.port);
        else sncpy(line, sizeof line, "Not connected to a network");
        text(line, x + w / 2, y + h * 0.64f + 40 * S, 20 * S, 0, th.text_soft, fade, AL_CENTER, (int)(w - 40 * S));
    } else if (id == APP_STORE) {
        pane(x, y, w, h, 20 * S, SQUIRCLE, with_alpha(th.accent, 0.38f), 0, 0, GLOSS_QUIET, fade, 0);
        glyph("category-software", x + w / 2, y + h * 0.42f, fminf(w, h) * 0.34f, th.text, fade);
        text("App Store", x + w / 2, y + h * 0.70f, 27 * S, 1, th.text, fade, AL_CENTER, (int)(w - 40 * S));
    } else {
        pane(x, y, w, h, 20 * S, SQUIRCLE, with_alpha(th.accent, 0.38f), 0, 0, GLOSS_QUIET, fade, 0);
        glyph("category-music", x + w / 2, y + h * 0.40f, fminf(w, h) * 0.32f, th.text, fade);
        text(mp.title, x + w / 2, y + h * 0.66f, 27 * S, 1, th.text, fade, AL_CENTER, (int)(w - 40 * S));
        if (mp.artist[0]) text(mp.artist, x + w / 2, y + h * 0.66f + 40 * S, 20 * S, 0, th.text_soft, fade, AL_CENTER, (int)(w - 40 * S));
        glyph(mp.paused || !mp.active ? "media-play" : "media-pause", x + w - 40 * S, y + h - 38 * S, 30 * S, th.text, fade * 0.8f);
    }
}

/* ======================================================================== */
/* The app store                                                            */
/* ======================================================================== */


#define ST_POP 1
#define ST_FEAT 2
#define ST_TERM 4

typedef struct {
    const char *pkg, *name, *by, *summary, *cmd;
    int cat, flags;
} CatApp;

enum { SC_GAMES, SC_MULTIMEDIA, SC_GRAPHICS, SC_INTERNET, SC_OFFICE, SC_DEVELOPMENT, SC_EDUCATION, SC_UTILITIES, SC_SYSTEM, SC_COUNT };
static const char *store_cat_names[SC_COUNT] = { "Games", "Multimedia", "Graphics", "Internet", "Office",
                                                  "Development", "Education", "Utilities", "System" };
static const char *store_cat_glyphs[SC_COUNT] = { "category-games", "category-multimedia", "category-graphics", "category-internet",
                                                   "category-office", "category-development", "category-education", "category-utilities",
                                                   "category-system" };

/* What the store offers. Anything the system's package lists don't carry is left out. */
static const CatApp catalog[] = {
    /* games */
    { "retroarch", "RetroArch", "Libretro", "Play classic console games", "retroarch", SC_GAMES, ST_POP | ST_FEAT },
    { "supertuxkart", "SuperTuxKart", "SuperTuxKart team", "Kart racing with Tux and friends", "supertuxkart", SC_GAMES, ST_POP },
    { "openttd", "OpenTTD", "OpenTTD team", "Build a transport empire", "openttd", SC_GAMES, ST_POP },
    { "scummvm", "ScummVM", "ScummVM team", "Run classic adventure games", "scummvm", SC_GAMES, ST_POP },
    { "ppsspp-sdl", "PPSSPP", "Henrik Rydgard", "A PSP emulator", "PPSSPPSDL", SC_GAMES, ST_FEAT },
    { "mednafen", "Mednafen", "Mednafen team", "A multi-system emulator", "mednafen", SC_GAMES, 0 },
    { "dosbox-staging", "DOSBox Staging", "DOSBox Staging team", "Run old DOS programs and games", "dosbox", SC_GAMES, 0 },
    { "dosbox", "DOSBox", "DOSBox team", "An emulator for DOS games", "dosbox", SC_GAMES, 0 },
    { "mame", "MAME", "MAME team", "Arcade machine emulator", "mame", SC_GAMES, 0 },
    { "supertux", "SuperTux", "SuperTux team", "A side-scrolling platform game", "supertux2", SC_GAMES, ST_POP },
    { "luanti", "Luanti", "Luanti team", "A block-building sandbox", "luanti", SC_GAMES, ST_POP },
    { "minetest", "Minetest", "Minetest team", "A block-building sandbox", "minetest", SC_GAMES, 0 },
    { "0ad", "0 A.D.", "Wildfire Games", "Ancient warfare strategy", "0ad", SC_GAMES, 0 },
    { "wesnoth", "Battle for Wesnoth", "Wesnoth team", "Turn-based fantasy strategy", "wesnoth", SC_GAMES, 0 },
    { "extremetuxracer", "Extreme Tux Racer", "Tux Racer team", "Race down the slopes as Tux", "etr", SC_GAMES, 0 },
    { "neverball", "Neverball", "Neverball team", "Tilt the floor to roll the ball", "neverball", SC_GAMES, 0 },
    { "openarena", "OpenArena", "OpenArena team", "A fast arena shooter", "openarena", SC_GAMES, 0 },
    { "xonotic", "Xonotic", "Xonotic team", "A fast-paced arena shooter", "xonotic", SC_GAMES, 0 },
    { "gnome-chess", "Chess", "GNOME", "Play chess against the computer", "gnome-chess", SC_GAMES, 0 },
    /* multimedia */
    { "vlc", "VLC media player", "VideoLAN", "Play almost any video or audio", "vlc", SC_MULTIMEDIA, ST_POP | ST_FEAT },
    { "kodi", "Kodi", "Kodi Foundation", "A home theater and media center", "kodi", SC_MULTIMEDIA, ST_POP | ST_FEAT },
    { "mpv", "mpv", "mpv team", "A small, fast video player", "mpv", SC_MULTIMEDIA, ST_POP },
    { "audacity", "Audacity", "Audacity team", "Record and edit audio", "audacity", SC_MULTIMEDIA, ST_POP },
    { "obs-studio", "OBS Studio", "OBS Project", "Record and stream video", "obs", SC_MULTIMEDIA, 0 },
    { "handbrake", "HandBrake", "HandBrake team", "Convert video between formats", "ghb", SC_MULTIMEDIA, 0 },
    { "kdenlive", "Kdenlive", "KDE", "A video editor", "kdenlive", SC_MULTIMEDIA, 0 },
    { "strawberry", "Strawberry", "Strawberry team", "A music collection player", "strawberry", SC_MULTIMEDIA, 0 },
    { "rhythmbox", "Rhythmbox", "GNOME", "Play and organize your music", "rhythmbox", SC_MULTIMEDIA, 0 },
    { "celluloid", "Celluloid", "Celluloid team", "A simple video player", "celluloid", SC_MULTIMEDIA, 0 },
    { "cheese", "Cheese", "GNOME", "Take photos and video with a webcam", "cheese", SC_MULTIMEDIA, 0 },
    /* graphics */
    { "gimp", "GIMP", "GIMP team", "Edit photos and images", "gimp", SC_GRAPHICS, ST_POP | ST_FEAT },
    { "krita", "Krita", "KDE", "Digital painting", "krita", SC_GRAPHICS, ST_POP },
    { "inkscape", "Inkscape", "Inkscape team", "Vector graphics editor", "inkscape", SC_GRAPHICS, ST_POP },
    { "blender", "Blender", "Blender Foundation", "3D modeling and animation", "blender", SC_GRAPHICS, 0 },
    { "darktable", "darktable", "darktable team", "Develop RAW photos", "darktable", SC_GRAPHICS, 0 },
    { "rawtherapee", "RawTherapee", "RawTherapee team", "Develop RAW photos", "rawtherapee", SC_GRAPHICS, 0 },
    { "pinta", "Pinta", "Pinta team", "Simple drawing and image editing", "pinta", SC_GRAPHICS, 0 },
    { "mypaint", "MyPaint", "MyPaint team", "Paint with a pressure-sensitive pen", "mypaint", SC_GRAPHICS, 0 },
    { "eog", "Image Viewer", "GNOME", "View pictures", "eog", SC_GRAPHICS, 0 },
    { "gthumb", "gThumb", "GNOME", "View and organize pictures", "gthumb", SC_GRAPHICS, 0 },
    { "scribus", "Scribus", "Scribus team", "Page layout for print", "scribus", SC_GRAPHICS, 0 },
    /* internet */
    { "firefox-esr", "Firefox ESR", "Mozilla", "Fast, private and safe web browser", "firefox-esr", SC_INTERNET, ST_POP | ST_FEAT },
    { "firefox", "Firefox", "Mozilla", "Fast, private and safe web browser", "firefox", SC_INTERNET, ST_POP },
    { "chromium", "Chromium", "The Chromium Authors", "A fast web browser", "chromium", SC_INTERNET, ST_POP },
    { "thunderbird", "Thunderbird", "Mozilla", "Email, calendar and contacts", "thunderbird", SC_INTERNET, 0 },
    { "transmission-gtk", "Transmission", "Transmission team", "Download torrents", "transmission-gtk", SC_INTERNET, 0 },
    { "qbittorrent", "qBittorrent", "qBittorrent team", "Download torrents", "qbittorrent", SC_INTERNET, 0 },
    { "filezilla", "FileZilla", "FileZilla team", "Transfer files over FTP and SFTP", "filezilla", SC_INTERNET, 0 },
    { "hexchat", "HexChat", "HexChat team", "Chat on IRC", "hexchat", SC_INTERNET, 0 },
    { "telegram-desktop", "Telegram", "Telegram", "Chat with friends", "telegram-desktop", SC_INTERNET, 0 },
    { "remmina", "Remmina", "Remmina team", "Connect to other computers' desktops", "remmina", SC_INTERNET, 0 },
    /* office */
    { "libreoffice", "LibreOffice", "The Document Foundation", "Documents, spreadsheets and slides", "libreoffice", SC_OFFICE, ST_POP | ST_FEAT },
    { "evince", "Document Viewer", "GNOME", "Read PDF documents", "evince", SC_OFFICE, 0 },
    { "okular", "Okular", "KDE", "Read PDFs, comics and e-books", "okular", SC_OFFICE, 0 },
    { "calibre", "calibre", "Kovid Goyal", "Manage and read e-books", "calibre", SC_OFFICE, 0 },
    { "abiword", "AbiWord", "AbiWord team", "A light word processor", "abiword", SC_OFFICE, 0 },
    { "gnumeric", "Gnumeric", "GNOME", "A light spreadsheet", "gnumeric", SC_OFFICE, 0 },
    { "keepassxc", "KeePassXC", "KeePassXC team", "Keep your passwords safe", "keepassxc", SC_OFFICE, 0 },
    { "xournalpp", "Xournal++", "Xournal++ team", "Take handwritten notes", "xournalpp", SC_OFFICE, 0 },
    /* development */
    { "code", "Visual Studio Code", "Microsoft", "A code editor", "code", SC_DEVELOPMENT, ST_POP },
    { "geany", "Geany", "Geany team", "A fast, light code editor", "geany", SC_DEVELOPMENT, ST_POP },
    { "thonny", "Thonny", "Thonny team", "A friendly Python editor", "thonny", SC_DEVELOPMENT, 0 },
    { "codeblocks", "Code::Blocks", "Code::Blocks team", "A C and C++ environment", "codeblocks", SC_DEVELOPMENT, 0 },
    { "kate", "Kate", "KDE", "A powerful text editor", "kate", SC_DEVELOPMENT, 0 },
    { "git-cola", "Git Cola", "Git Cola team", "A friendly Git client", "git-cola", SC_DEVELOPMENT, 0 },
    { "vim-gtk3", "Vim", "Vim team", "A powerful text editor", "gvim", SC_DEVELOPMENT, 0 },
    /* education */
    { "stellarium", "Stellarium", "Stellarium team", "A planetarium for your screen", "stellarium", SC_EDUCATION, ST_POP },
    { "gcompris-qt", "GCompris", "KDE", "Learning games for children", "gcompris-qt", SC_EDUCATION, 0 },
    { "tuxpaint", "Tux Paint", "Tux Paint team", "Drawing for young children", "tuxpaint", SC_EDUCATION, 0 },
    { "celestia", "Celestia", "Celestia team", "Explore the universe in 3D", "celestia", SC_EDUCATION, 0 },
    { "ktouch", "KTouch", "KDE", "Learn to touch type", "ktouch", SC_EDUCATION, 0 },
    { "kalzium", "Kalzium", "KDE", "The periodic table of elements", "kalzium", SC_EDUCATION, 0 },
    /* utilities */
    { "pcmanfm", "PCManFM", "LXDE", "A light file manager", "pcmanfm", SC_UTILITIES, 0 },
    { "gparted", "GParted", "GParted team", "Manage disks and partitions", "gparted", SC_UTILITIES, 0 },
    { "galculator", "Galculator", "Galculator team", "A calculator", "galculator", SC_UTILITIES, 0 },
    { "mousepad", "Mousepad", "Xfce", "A simple text editor", "mousepad", SC_UTILITIES, 0 },
    { "baobab", "Disk Usage Analyzer", "GNOME", "See what uses your storage", "baobab", SC_UTILITIES, 0 },
    { "synaptic", "Synaptic", "Synaptic team", "Browse every package", "synaptic", SC_UTILITIES, 0 },
    { "bleachbit", "BleachBit", "BleachBit team", "Free up disk space", "bleachbit", SC_UTILITIES, 0 },
    { "htop", "htop", "htop team", "Watch what the system is doing", "htop", SC_UTILITIES, ST_TERM },
    { "btop", "btop", "btop team", "A colorful system monitor", "btop", SC_UTILITIES, ST_TERM },
    { "ncdu", "ncdu", "ncdu team", "See what fills a disk", "ncdu", SC_UTILITIES, ST_TERM },
    { "mc", "Midnight Commander", "MC team", "A file manager for the terminal", "mc", SC_UTILITIES, ST_TERM },
    /* system */
    { "rpi-imager", "Raspberry Pi Imager", "Raspberry Pi", "Write system images to cards", "rpi-imager", SC_SYSTEM, 0 },
    { "gnome-system-monitor", "System Monitor", "GNOME", "Watch processes and resources", "gnome-system-monitor", SC_SYSTEM, 0 },
    { "gnome-disk-utility", "Disks", "GNOME", "Manage drives and images", "gnome-disks", SC_SYSTEM, 0 },
    { "timeshift", "Timeshift", "Timeshift team", "Back up and restore the system", "timeshift", SC_SYSTEM, 0 },
};
#define NCATALOG ((int)(sizeof catalog / sizeof *catalog))

typedef struct {
    char pkg[72], name[96], by[56], summary[200], cmd[72];
    char icon[200]; /* a picture file, for apps that bring their own (Pi-Apps) */
    int cat, flags, inst, avail, upgrade, special, pi;
} StoreItem;

enum { SP_HOME, SP_SEARCH, SP_UPDATES, SP_INSTALLED, SP_DIVIDER, SP_EVERYTHING, SP_CAT0 };
#define SIDE_ROWS (SP_CAT0 + SC_COUNT)

typedef struct {
    char pkg[72], from[40], to[40];
} Upgrade;

static struct {
    int open, running, focus, side, cell, detail, dbtn;
    float side_pos, scroll;
    StoreItem *items;
    int nitems, citems;
    unsigned char inst[NCATALOG], avail[NCATALOG];
    char ver[NCATALOG][40];
    Upgrade *upg;
    int nupg;
    char query[96];
    StoreItem *results;
    int nresults;
    StoreItem *pi;
    int npi;
    int ready;
    struct {
        char desc[900], version[48], homepage[160];
        long size_kb;
        int loaded;
    } info;
} store;

/* Run a command and keep what it prints. */
static char *shell_capture(const char *cmd)
{
    FILE *p = popen(cmd, "r");
    if (!p) return NULL;
    size_t cap = 8192, n = 0;
    char *buf = xcalloc(cap, 1);
    size_t got;
    while ((got = fread(buf + n, 1, cap - n - 1, p)) > 0) {
        n += got;
        if (n + 1 >= cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
            memset(buf + n, 0, cap - n);
        }
        if (n > (8u << 20)) break;
    }
    pclose(p);
    buf[n] = 0;
    return buf;
}

static int store_find_catalog(const char *pkg)
{
    for (int i = 0; i < NCATALOG; i++)
        if (!strcmp(catalog[i].pkg, pkg)) return i;
    return -1;
}

/* ---------- Pi-Apps: its apps shown here as if they were the store's own --------- */
/* Pi-Apps keeps one folder per app in ~/pi-apps/apps (a description, an icon and install scripts), the
 * category of each in etc/categories, and what is installed in data/status. Installing and removing is its
 * own `manage` script, run here without a screen of its own so the progress shows in Pixi's dialog. */

static char piapps_dir[600];
static int piapps_can; /* this is the kind of computer Pi-Apps is made for */
static int piapps_arm64;

static char *pi_read(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char *buf = xcalloc(1, 65536);
    size_t n = fread(buf, 1, 65535, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

static int piapps_locate(void)
{
    struct utsname u;
    piapps_dir[0] = 0;
    piapps_can = piapps_arm64 = 0;
    const char *ov = getenv("PIXI_PIAPPS_DIR"); /* for testing somewhere else */
    if (uname(&u) != 0) return 0;
    int a64 = !strncmp(u.machine, "aarch64", 7) || !strncmp(u.machine, "arm64", 5);
    int a32 = !strncmp(u.machine, "arm", 3) && !a64;
    if (ov && *ov) {
        sncpy(piapps_dir, sizeof piapps_dir, ov);
        a64 = 1;
        piapps_can = 1;
    } else {
        piapps_can = a64 || a32;
        if (!piapps_can) return 0;
        snprintf(piapps_dir, sizeof piapps_dir, "%s/pi-apps", home_dir());
    }
    piapps_arm64 = a64;
    char p[700];
    snprintf(p, sizeof p, "%s/manage", piapps_dir);
    if (access(p, R_OK) != 0) {
        piapps_dir[0] = 0;
        return 0;
    }
    return 1;
}

static int pi_store_cat(const char *c)
{
    if (!strncasecmp(c, "Games", 5)) return SC_GAMES;
    if (!strncasecmp(c, "Multimedia", 10)) return SC_MULTIMEDIA;
    if (!strncasecmp(c, "Creative", 8)) return SC_GRAPHICS;
    if (!strncasecmp(c, "Internet", 8)) return SC_INTERNET;
    if (!strncasecmp(c, "Office", 6)) return SC_OFFICE;
    if (!strncasecmp(c, "Programming", 11) || !strncasecmp(c, "Engineering", 11)) return SC_DEVELOPMENT;
    if (!strncasecmp(c, "System", 6)) return SC_SYSTEM;
    return SC_UTILITIES;
}

/* the category line "App name|Category" in one of Pi-Apps' lists; 1 if found */
static int pi_category_of(const char *text, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    const char *l = text;
    while (l && *l) {
        if (!strncmp(l, name, nl) && l[nl] == '|') {
            const char *c = l + nl + 1;
            size_t n = strcspn(c, "\r\n");
            if (n >= cap) n = cap - 1;
            memcpy(out, c, n);
            out[n] = 0;
            return 1;
        }
        l = strchr(l, '\n');
        if (l) l++;
    }
    return 0;
}

static int pi_name_cmp(const void *a, const void *b)
{
    return strcasecmp(((const StoreItem *)a)->name, ((const StoreItem *)b)->name);
}

static void piapps_load(void)
{
    free(store.pi);
    store.pi = NULL;
    store.npi = 0;
    if (!piapps_locate()) return;
    char p[900];
    snprintf(p, sizeof p, "%s/etc/categories", piapps_dir);
    char *cats = pi_read(p);
    snprintf(p, sizeof p, "%s/data/category-overrides", piapps_dir);
    char *over = pi_read(p); /* the categories this person moved apps to */
    snprintf(p, sizeof p, "%s/apps", piapps_dir);
    DIR *d = opendir(p);
    if (!d) {
        free(cats);
        free(over);
        return;
    }
    int cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *name = de->d_name;
        if (name[0] == '.' || strlen(name) > 66) continue;
        char f[1000];
        snprintf(f, sizeof f, "%s/apps/%s/packages", piapps_dir, name);
        if (access(f, F_OK) == 0) continue; /* only an alias for an apt package: the store finds those itself */
        snprintf(f, sizeof f, "%s/apps/%s/%s", piapps_dir, name, piapps_arm64 ? "install-64" : "install-32");
        int ok = access(f, R_OK) == 0;
        snprintf(f, sizeof f, "%s/apps/%s/install", piapps_dir, name);
        if (!ok && access(f, R_OK) != 0) continue;
        int dup = 0;
        for (int i = 0; i < NCATALOG; i++)
            if (!strcasecmp(catalog[i].name, name)) dup = 1;
        if (dup) continue;
        char cat[64] = "Tools", status[32] = "";
        if (!(over && pi_category_of(over, name, cat, sizeof cat))) pi_category_of(cats ? cats : "", name, cat, sizeof cat);
        if (!strcasecmp(cat, "hidden")) continue;
        snprintf(f, sizeof f, "%s/data/status/%s", piapps_dir, name);
        FILE *sf = fopen(f, "r");
        if (sf) {
            if (!fgets(status, sizeof status, sf)) status[0] = 0;
            fclose(sf);
            status[strcspn(status, "\r\n")] = 0;
        }
        if (!strcmp(status, "disabled")) continue;
        if (store.npi == cap) {
            cap = cap ? cap * 2 : 128;
            store.pi = xrealloc(store.pi, sizeof(StoreItem) * (size_t)cap);
        }
        StoreItem *it = &store.pi[store.npi++];
        memset(it, 0, sizeof *it);
        snprintf(it->pkg, sizeof it->pkg, "pi:%s", name);
        sncpy(it->name, sizeof it->name, name);
        snprintf(f, sizeof f, "%s/apps/%s/description", piapps_dir, name);
        char *desc = pi_read(f);
        if (desc) {
            size_t n = strcspn(desc, "\r\n");
            if (n >= sizeof it->summary) n = sizeof it->summary - 1;
            memcpy(it->summary, desc, n);
            it->summary[n] = 0;
            free(desc);
        }
        snprintf(it->icon, sizeof it->icon, "%s/apps/%s/icon-64.png", piapps_dir, name);
        it->cat = pi_store_cat(cat);
        it->avail = 1;
        it->inst = !strcmp(status, "installed") || !strcmp(status, "corrupted");
        it->pi = 1;
    }
    closedir(d);
    free(cats);
    free(over);
    if (store.npi > 1) qsort(store.pi, (size_t)store.npi, sizeof(StoreItem), pi_name_cmp);
}

static int piapps_installed_count(void)
{
    int n = 0;
    for (int i = 0; i < store.npi; i++) n += store.pi[i].inst;
    return n;
}

static void piapps_quote(char *out, size_t cap, const char *s)
{
    size_t o = 0;
    if (o < cap - 1) out[o++] = '\'';
    for (; *s && o + 5 < cap; s++) {
        if (*s == '\'') {
            memcpy(out + o, "'\\''", 4);
            o += 4;
        } else out[o++] = *s;
    }
    if (o < cap - 1) out[o++] = '\'';
    out[o] = 0;
}

/* Pi-Apps' own scripts run sudo themselves. The progress dialog has no keyboard, so it only works when sudo
 * doesn't ask for a password; otherwise the job runs in Pixi's Terminal, where the password can be typed. */
static int piapps_nopass(void)
{
    return system("sudo -n true >/dev/null 2>&1") == 0;
}

/* sudo can ask on screen (SUDO_ASKPASS, see askpass_listen), even over the install dialog */
static int piapps_can_ask(void)
{
    return piapps_nopass() || (getenv("SUDO_ASKPASS") && *getenv("SUDO_ASKPASS"));
}

static void piapps_in_terminal(const char *cmd, const char *title)
{
    char line[2600];
    snprintf(line, sizeof line, "%s; echo; read -r -p 'Finished. Press Enter to close. ' _", cmd);
    char *argv[] = { "/bin/bash", "-c", line, NULL };
    term_open(argv, title);
    if (term_is_open()) store.open = 0; /* the store stays where it was; opening it again looks at Pi-Apps afresh */
}

/* A folder with "yad" in it, a link to this program, for Pi-Apps' scripts to find first in their PATH. */
static const char *piapps_shim(void)
{
    static char dir[700];
    char exe[700], link[760];
    snprintf(dir, sizeof dir, "%s/.cache/pixi/bin", home_dir());
    char up[700];
    snprintf(up, sizeof up, "%s/.cache", home_dir());
    mkdir(up, 0755);
    snprintf(up, sizeof up, "%s/.cache/pixi", home_dir());
    mkdir(up, 0755);
    mkdir(dir, 0755);
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return NULL;
    exe[n] = 0;
    snprintf(link, sizeof link, "%s/yad", dir);
    unlink(link);
    if (symlink(exe, link) != 0) return NULL;
    return dir;
}

/* install or remove one Pi-Apps app, through the same progress dialog as the rest */
static void piapps_job(int install, const char *name)
{
    char q[300], lab[96], script[3200];
    piapps_quote(q, sizeof q, name);
    size_t o = 0;
    for (const char *c = name; *c && o < sizeof lab - 1; c++)
        if (isalnum((unsigned char)*c) || *c == ' ' || *c == '-' || *c == '.' || *c == '+') lab[o++] = *c;
    lab[o] = 0;
    const char *shim = piapps_shim();
    char pathset[760] = "";
    if (shim) snprintf(pathset, sizeof pathset, "export PATH='%s':\"$PATH\"\n", shim); /* "yad" questions come up in Pixi's own dialog */
    if (!piapps_can_ask()) {
        char cmd[1700];
        if (shim) snprintf(cmd, sizeof cmd, "export PATH='%s':\"$PATH\"; '%s/manage' %s %s", shim, piapps_dir, install ? "install" : "uninstall", q);
        else snprintf(cmd, sizeof cmd, "'%s/manage' %s %s", piapps_dir, install ? "install" : "uninstall", q);
        piapps_in_terminal(cmd, lab);
        return;
    }
    /* DISPLAY and WAYLAND_DISPLAY stay set: some Pi-Apps installers (Chrome, for one) ask which version
     * to get in a yad window, which then shows up like any other app. */
    snprintf(script, sizeof script,
             "export LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive\n"
             "%s"
             "d='%s'\n"
             "mkdir -p \"$HOME/.cache/pixi\"; log=\"$HOME/.cache/pixi/piapps-app.log\"\n"
             "echo 'PHASE:%s %s'\n"
             "\"$d/manage\" %s %s 2>&1 | tee \"$log\"\n"
             "s=$(cat \"$d/data/status/\"%s 2>/dev/null)\n"
             "if [ \"$s\" != %s ]; then\n"
             "  m=$(sed 's/\\x1b\\[[0-9;]*[A-Za-z]//g' \"$log\" | grep -v '^[[:space:]]*$' | tail -n 3 | tr '\\n' ' ' | cut -c1-230)\n"
             "  echo \"FAIL:Pi-Apps could not %s %s. Last output: $m (full log: ~/.cache/pixi/piapps-app.log)\"; exit 1\n"
             "fi\n",
             pathset, piapps_dir, install ? "Installing" : "Removing", lab, install ? "install" : "uninstall", q, q,
             install ? "installed" : "uninstalled", install ? "install" : "remove", lab);
    job_start(install ? 1 : 2, install ? "Install" : "Remove", lab, script);
}

/* Pi-Apps itself, from its own installer (the way its README says to) */
static void piapps_install_start(void)
{
    static const char *get = "wget -qO- https://raw.githubusercontent.com/Botspot/pi-apps/master/install | bash";
    if (!piapps_can_ask()) { /* its installer uses sudo: let the password be typed */
        piapps_in_terminal(get, "Pi-Apps");
        return;
    }
    static const char *script =
        "export LC_ALL=C.UTF-8 DEBIAN_FRONTEND=noninteractive\n"
        "echo 'PHASE:Getting Pi-Apps'\n"
        "mkdir -p \"$HOME/.cache/pixi\"; log=\"$HOME/.cache/pixi/piapps-install.log\"\n"
        "wget -qO- https://raw.githubusercontent.com/Botspot/pi-apps/master/install | bash 2>&1 | tee \"$log\"\n"
        "if [ ! -f \"$HOME/pi-apps/manage\" ]; then\n"
        "  if grep -q 'password is required\\|no tty present' \"$log\"; then echo 'FAIL:Pi-Apps needs sudo without a password.'\n"
        "  else echo 'FAIL:Pi-Apps did not install (the full output is in ~/.cache/pixi/piapps-install.log).'; fi\n"
        "  exit 1\n"
        "fi\n";
    job_start(1, "Install", "Pi-Apps", script);
}

/* Which of the store's apps this system can install, and which it already has. */
static void store_refresh_status(void)
{
    piapps_load();
    memset(store.inst, 0, sizeof store.inst);
    memset(store.avail, 0, sizeof store.avail);
    memset(store.ver, 0, sizeof store.ver);
    char cmd[8192];
    size_t o = (size_t)snprintf(cmd, sizeof cmd, "LC_ALL=C apt-cache policy");
    for (int i = 0; i < NCATALOG && o < sizeof cmd - 100; i++) o += (size_t)snprintf(cmd + o, sizeof cmd - o, " %s", catalog[i].pkg);
    snprintf(cmd + o, sizeof cmd - o, " 2>/dev/null");
    char *out = shell_capture(cmd);
    if (out) {
        int cur = -1;
        for (char *line = strtok(out, "\n"); line; line = strtok(NULL, "\n")) {
            if (line[0] != ' ') {
                char name[96];
                sncpy(name, sizeof name, line);
                char *colon = strchr(name, ':');
                if (colon) *colon = 0;
                cur = store_find_catalog(name);
            } else if (cur >= 0) {
                char *p = line;
                while (*p == ' ') p++;
                if (!strncmp(p, "Installed: ", 11)) store.inst[cur] = strcmp(p + 11, "(none)") != 0;
                else if (!strncmp(p, "Candidate: ", 11)) {
                    store.avail[cur] = strcmp(p + 11, "(none)") != 0;
                    sncpy(store.ver[cur], sizeof store.ver[cur], p + 11);
                }
            }
        }
        free(out);
    }
    /* updates waiting */
    free(store.upg);
    store.upg = NULL;
    store.nupg = 0;
    out = shell_capture("LC_ALL=C apt list --upgradable 2>/dev/null");
    if (out) {
        int cap = 0;
        for (char *line = strtok(out, "\n"); line; line = strtok(NULL, "\n")) {
            char pkg[72], to[40], from[40] = "";
            if (!strchr(line, '/') || sscanf(line, "%71[^/]/%*s %39s", pkg, to) != 2) continue;
            const char *f = strstr(line, "upgradable from: ");
            if (f) sscanf(f + 17, "%39[^]]", from);
            if (store.nupg == cap) {
                cap = cap ? cap * 2 : 32;
                store.upg = xrealloc(store.upg, sizeof(Upgrade) * (size_t)cap);
            }
            sncpy(store.upg[store.nupg].pkg, sizeof store.upg[0].pkg, pkg);
            sncpy(store.upg[store.nupg].from, sizeof store.upg[0].from, from);
            sncpy(store.upg[store.nupg].to, sizeof store.upg[0].to, to);
            store.nupg++;
        }
        free(out);
    }
    store.ready = 1;
}

static int store_installed_count(void)
{
    int n = piapps_installed_count();
    for (int i = 0; i < NCATALOG; i++) n += store.inst[i];
    return n;
}

static StoreItem *store_item_add(void)
{
    if (store.nitems == store.citems) {
        store.citems = store.citems ? store.citems * 2 : 64;
        store.items = xrealloc(store.items, sizeof(StoreItem) * (size_t)store.citems);
    }
    StoreItem *it = &store.items[store.nitems++];
    memset(it, 0, sizeof *it);
    return it;
}

static void store_item_from_catalog(StoreItem *it, int i)
{
    const CatApp *c = &catalog[i];
    sncpy(it->pkg, sizeof it->pkg, c->pkg);
    sncpy(it->name, sizeof it->name, c->name);
    sncpy(it->by, sizeof it->by, c->by);
    sncpy(it->summary, sizeof it->summary, c->summary);
    sncpy(it->cmd, sizeof it->cmd, c->cmd);
    it->cat = c->cat;
    it->flags = c->flags;
    it->inst = store.inst[i];
    it->avail = store.avail[i];
    for (int u = 0; u < store.nupg; u++)
        if (!strcmp(store.upg[u].pkg, c->pkg)) it->upgrade = 1;
}

static int store_name_cmp(const void *a, const void *b)
{
    return strcasecmp(((const StoreItem *)a)->name, ((const StoreItem *)b)->name);
}

static int store_cols(void) { return store.side == SP_UPDATES ? 1 : 2; }

/* Fill store.items for the page being shown. */
static void store_build_page(void)
{
    store.nitems = 0;
    int page = store.side;
    if (page == SP_HOME) {
        /* one featured app, picked by the day, then what's popular */
        int feats[NCATALOG], nf = 0;
        for (int i = 0; i < NCATALOG; i++)
            if (store.avail[i] && (catalog[i].flags & ST_FEAT)) feats[nf++] = i;
        int fi = -1;
        if (nf) {
            time_t t = time(NULL);
            struct tm tm;
            localtime_r(&t, &tm);
            fi = feats[tm.tm_yday % nf];
            store_item_from_catalog(store_item_add(), fi);
        } else {
            store_item_add()->special = 1; /* nothing to feature */
        }
        int shown = 0;
        for (int i = 0; i < NCATALOG && shown < 9; i++)
            if (i != fi && store.avail[i] && (catalog[i].flags & ST_POP)) {
                store_item_from_catalog(store_item_add(), i);
                shown++;
            }
        if (piapps_can && !store.npi) store_item_add()->special = 3; /* Pi-Apps isn't here yet */
    } else if (page == SP_SEARCH) {
        for (int i = 0; i < store.nresults; i++) *store_item_add() = store.results[i];
        for (int i = 0; i < store.npi && store.query[0] && store.nitems < 80; i++)
            if (strcasestr(store.pi[i].name, store.query) || strcasestr(store.pi[i].summary, store.query)) *store_item_add() = store.pi[i];
    } else if (page == SP_UPDATES) {
        StoreItem *all = store_item_add();
        all->special = 2; /* update everything */
        for (int u = 0; u < store.nupg; u++) {
            StoreItem *it = store_item_add();
            int ci = store_find_catalog(store.upg[u].pkg);
            if (ci >= 0) store_item_from_catalog(it, ci);
            else {
                sncpy(it->pkg, sizeof it->pkg, store.upg[u].pkg);
                sncpy(it->name, sizeof it->name, store.upg[u].pkg);
                it->avail = it->inst = 1;
            }
            it->upgrade = 1;
            snprintf(it->summary, sizeof it->summary, "%s to %s", store.upg[u].from[0] ? store.upg[u].from : "installed", store.upg[u].to);
            it->by[0] = 0;
        }
    } else {
        if (page == SP_EVERYTHING && piapps_can && !store.npi) store_item_add()->special = 3;
        int first = store.nitems;
        for (int i = 0; i < NCATALOG; i++) {
            if (page == SP_INSTALLED && !store.inst[i]) continue;
            if (page != SP_INSTALLED && !store.avail[i]) continue;
            if (page >= SP_CAT0 && catalog[i].cat != page - SP_CAT0) continue;
            store_item_from_catalog(store_item_add(), i);
        }
        for (int i = 0; i < store.npi; i++) {
            if (page == SP_INSTALLED && !store.pi[i].inst) continue;
            if (page >= SP_CAT0 && store.pi[i].cat != page - SP_CAT0) continue;
            *store_item_add() = store.pi[i];
        }
        if (store.nitems - first > 1) qsort(store.items + first, (size_t)(store.nitems - first), sizeof(StoreItem), store_name_cmp);
    }
    if (store.cell >= store.nitems) store.cell = store.nitems ? store.nitems - 1 : 0;
}

static const char *store_page_title(void)
{
    static const char *titles[] = { "Home", "Search", "Updates", "Installed", "", "Everything" };
    if (store.side >= SP_CAT0) return store_cat_names[store.side - SP_CAT0];
    return titles[store.side];
}

/* The icons of apps that aren't installed yet come from Debian's package icon set (the same one software centers
 * use). It's downloaded once into ~/.cache/pixi/store-icons and the wanted icons are picked out of it, in the
 * background; the page picks them up as they arrive. */
static time_t store_icons_stamp;
static int store_icons_busy;
static Uint32 store_icons_poll;

static void store_fetch_icons(const char *const *pkgs, int n)
{
    char dir[600], list[6000];
    snprintf(dir, sizeof dir, "%s/.cache/pixi/store-icons", home_dir());
    size_t o = 0;
    int want = 0;
    char path[1024];
    for (int i = 0; i < n; i++) {
        const char *p = pkgs[i];
        if (!*p || find_icon_file(p, path, sizeof path)) continue;
        int ok = 1;
        for (const char *c = p; *c; c++)
            if (!(isalnum((unsigned char)*c) || *c == '-' || *c == '+' || *c == '.')) ok = 0;
        if (!ok || o + strlen(p) + 2 >= sizeof list) continue;
        o += (size_t)sprintf(list + o, " %s", p);
        want++;
    }
    if (!want || store_icons_busy) return;
    char *script = xcalloc(1, 4000 + o);
    sprintf(script,
        "d='%s'; mkdir -p \"$d\"; t=\"$d/icons-64x64.tar.gz\"\n"
        "cn=$(. /etc/os-release 2>/dev/null; echo \"${VERSION_CODENAME:-trixie}\")\n"
        "url=\"https://deb.debian.org/debian/dists/$cn/main/dep11/icons-64x64.tar.gz\"\n"
        "if [ ! -s \"$t\" ]; then\n"
        "  if command -v curl >/dev/null; then curl -fsSL --max-time 900 -o \"$t.part\" \"$url\"; else wget -q -T 60 -O \"$t.part\" \"$url\"; fi && mv \"$t.part\" \"$t\"\n"
        "  rm -f \"$t.part\"\n"
        "fi\n"
        "[ -s \"$t\" ] && python3 - \"$d\" %s <<'PY'\n"
        "import sys, os, tarfile\n"
        "d = sys.argv[1]; want = set(sys.argv[2:])\n"
        "try:\n"
        "    with tarfile.open(os.path.join(d, 'icons-64x64.tar.gz'), 'r|gz') as tf:\n"
        "        for m in tf:\n"
        "            if not m.isfile() or not m.name.endswith('.png'): continue\n"
        "            pkg = os.path.basename(m.name).split('_', 1)[0]\n"
        "            if pkg in want:\n"
        "                data = tf.extractfile(m).read()\n"
        "                tmp = os.path.join(d, pkg + '.png.tmp')\n"
        "                open(tmp, 'wb').write(data)\n"
        "                os.replace(tmp, os.path.join(d, pkg + '.png'))\n"
        "                want.discard(pkg)\n"
        "                if not want: break\n"
        "except Exception:\n"
        "    pass\n"
        "PY\n"
        "touch \"$d/.done\"\n",
        dir, list);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        signal(SIGCHLD, SIG_DFL);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 0); dup2(dn, 1); dup2(dn, 2); }
        execl("/bin/sh", "sh", "-c", script, (char *)NULL);
        _exit(127);
    }
    free(script);
    if (pid > 0) {
        store_icons_busy = 1;
        char st[700];
        snprintf(st, sizeof st, "%s/.done", dir);
        struct stat sb;
        store_icons_stamp = stat(st, &sb) == 0 ? sb.st_mtime : 0;
    }
}

/* a running download is checked on once a second, and the page shows what has arrived */
static void store_icons_tick(Uint32 now)
{
    if (!store_icons_busy || now < store_icons_poll) return;
    store_icons_poll = now + 1000;
    char st[700];
    snprintf(st, sizeof st, "%s/.cache/pixi/store-icons/.done", home_dir());
    struct stat sb;
    if (stat(st, &sb) == 0 && sb.st_mtime != store_icons_stamp) {
        store_icons_busy = 0;
        icon_retry();
    }
}

static void store_fetch_catalog_icons(void)
{
    static int done;
    if (done) return;
    done = 1;
    const char *pk[NCATALOG * 2];
    int n = 0;
    for (int i = 0; i < NCATALOG; i++) pk[n++] = catalog[i].pkg;
    store_fetch_icons(pk, n);
}

static void store_open(void)
{
    if (store.running) { /* it's still there from the Guide */
        piapps_load(); /* a Pi-Apps job in the Terminal may have changed things */
        store_build_page();
        store.open = 1;
        snd(SND_SELECT);
        return;
    }
    show_busy("App Store", "Checking what's available\xE2\x80\xA6");
    store_refresh_status();
    dlg.kind = D_NONE;
    memset(&store.info, 0, sizeof store.info);
    store.open = store.running = 1;
    store_fetch_catalog_icons();
    store.focus = 1;
    store.side = SP_HOME;
    store.cell = 0;
    store.detail = 0;
    store.scroll = 0;
    store_build_page();
    snd(SND_SELECT);
}

static void store_goto(int page)
{
    store.side = page;
    store.cell = page == SP_UPDATES && store.nupg ? 1 : 0;
    store.scroll = 0;
    store_build_page();
}

static void store_search(const char *term)
{
    char clean[96];
    size_t o = 0;
    for (const char *p = term; *p && o < sizeof clean - 1; p++)
        clean[o++] = (isalnum((unsigned char)*p) || *p == '-' || *p == '+' || *p == '.') ? *p : ' ';
    clean[o] = 0;
    sncpy(store.query, sizeof store.query, term);
    free(store.results);
    store.results = NULL;
    store.nresults = 0;
    if (!strlen(clean)) return;
    show_busy("Searching", "Looking through the package lists\xE2\x80\xA6");
    char cmd[400];
    snprintf(cmd, sizeof cmd, "LC_ALL=C apt-cache search --names-only -- '%s' 2>/dev/null | sort | head -300", clean);
    char *out = shell_capture(cmd);
    dlg.kind = D_NONE;
    if (!out) return;
    int cap = 0;
    for (char *line = strtok(out, "\n"); line && store.nresults < 60; line = strtok(NULL, "\n")) {
        char *sep = strstr(line, " - ");
        if (!sep) continue;
        *sep = 0;
        const char *pkg = line, *desc = sep + 3;
        size_t pl = strlen(pkg);
        if (!strncmp(pkg, "lib", 3) || (pl > 4 && !strcmp(pkg + pl - 4, "-dev")) || (pl > 4 && !strcmp(pkg + pl - 4, "-doc")) ||
            (pl > 5 && !strcmp(pkg + pl - 5, "-data")) || (pl > 7 && !strcmp(pkg + pl - 7, "-common")) || strstr(pkg, "-dbg") ||
            strstr(pkg, "-l10n") || strstr(pkg, "-locale") || strstr(pkg, "-help-") || strstr(pkg, "-i18n"))
            continue;
        if (store.nresults == cap) {
            cap = cap ? cap * 2 : 32;
            store.results = xrealloc(store.results, sizeof(StoreItem) * (size_t)cap);
        }
        StoreItem *it = &store.results[store.nresults++];
        memset(it, 0, sizeof *it);
        int ci = store_find_catalog(pkg);
        if (ci >= 0) store_item_from_catalog(it, ci);
        else {
            sncpy(it->pkg, sizeof it->pkg, pkg);
            sncpy(it->name, sizeof it->name, pkg);
            sncpy(it->summary, sizeof it->summary, desc);
            it->avail = 1;
            it->cat = SC_UTILITIES;
        }
    }
    free(out);
    /* find out which of the results are installed */
    if (store.nresults) {
        char *c2 = xcalloc(1, 400 + (size_t)store.nresults * 80);
        size_t k = (size_t)sprintf(c2, "dpkg-query -W -f='${Package} ${db:Status-Status}\\n'");
        for (int i = 0; i < store.nresults; i++) k += (size_t)sprintf(c2 + k, " '%s'", store.results[i].pkg);
        strcat(c2, " 2>/dev/null");
        char *st = shell_capture(c2);
        free(c2);
        if (st) {
            for (char *line = strtok(st, "\n"); line; line = strtok(NULL, "\n")) {
                char name[96], state[32];
                if (sscanf(line, "%95s %31s", name, state) == 2 && !strcmp(state, "installed"))
                    for (int i = 0; i < store.nresults; i++)
                        if (!strcmp(store.results[i].pkg, name)) store.results[i].inst = 1;
            }
            free(st);
        }
        const char *pk[64];
        int np = 0;
        for (int i = 0; i < store.nresults && np < 64; i++) pk[np++] = store.results[i].pkg;
        store_fetch_icons(pk, np);
    }
}

static void store_load_info(const StoreItem *it)
{
    memset(&store.info, 0, sizeof store.info);
    store.info.loaded = 1;
    if (it->pi) { /* Pi-Apps keeps the text and the web page in files of the app's folder */
        char f[1000];
        snprintf(f, sizeof f, "%s/apps/%s/description", piapps_dir, it->name);
        char *d = pi_read(f);
        if (d) {
            for (char *c = d; *c; c++)
                if (*c == '\r') *c = ' ';
            sncpy(store.info.desc, sizeof store.info.desc, d);
            free(d);
        }
        snprintf(f, sizeof f, "%s/apps/%s/website", piapps_dir, it->name);
        d = pi_read(f);
        if (d) {
            d[strcspn(d, "\r\n")] = 0;
            sncpy(store.info.homepage, sizeof store.info.homepage, d);
            free(d);
        }
        return;
    }
    char cmd[200];
    snprintf(cmd, sizeof cmd, "LC_ALL=C apt-cache show --no-all-versions -- '%s' 2>/dev/null", it->pkg);
    char *out = shell_capture(cmd);
    if (!out) return;
    int in_desc = 0, first_desc = 1;
    for (char *line = strtok(out, "\n"); line; line = strtok(NULL, "\n")) {
        if (!*line) break; /* the first stanza only */
        if (!strncmp(line, "Version: ", 9)) sncpy(store.info.version, sizeof store.info.version, line + 9);
        else if (!strncmp(line, "Installed-Size: ", 16)) store.info.size_kb = atol(line + 16);
        else if (!strncmp(line, "Homepage: ", 10)) sncpy(store.info.homepage, sizeof store.info.homepage, line + 10);
        else if (!strncmp(line, "Description", 11) && strchr(line, ':')) {
            in_desc = 1;
            first_desc = 1;
        } else if (in_desc && line[0] == ' ') {
            if (first_desc) {
                first_desc = 0; /* the long description follows the one-line summary */
                continue;
            }
            const char *t = line + 1;
            if (!strcmp(t, ".")) t = "\n";
            size_t l = strlen(store.info.desc);
            if (l + strlen(t) + 2 < sizeof store.info.desc) {
                if (l && store.info.desc[l - 1] != '\n') strcat(store.info.desc, " ");
                strcat(store.info.desc, t);
            }
        } else if (line[0] != ' ') {
            in_desc = 0;
        }
    }
    free(out);
}

static void store_open_detail(void)
{
    if (store.cell < 0 || store.cell >= store.nitems) return;
    StoreItem *it = &store.items[store.cell];
    if (it->special == 2) {
        update_start();
        return;
    }
    if (it->special == 3) {
        show_confirm("Add Pi-Apps to the store?", "Adds more than 200 extra apps. This downloads and runs the Pi-Apps installer from GitHub, which also installs a few helper packages.", A_PIAPPS_INSTALL, "");
        dlg.choice = 1; /* No unless asked */
        snd(SND_SELECT);
        return;
    }
    if (it->special) return;
    store.detail = 1;
    store.dbtn = 0;
    store_load_info(it);
    snd(SND_SELECT);
}

static void store_launch(const StoreItem *it)
{
    if (it->pi) { /* the app puts its own entry in Apps: find it by name */
        const AppEntry *hit = NULL;
        for (int i = 0; i < apps.n && !hit; i++)
            if (!strcasecmp(apps.v[i].name, it->name)) hit = &apps.v[i];
        for (int i = 0; i < apps.n && !hit; i++)
            if (!strncasecmp(apps.v[i].name, it->name, strlen(it->name)) || !strncasecmp(it->name, apps.v[i].name, strlen(apps.v[i].name))) hit = &apps.v[i];
        char **ta = hit ? exec_to_argv(hit->exec) : NULL;
        if (!ta) {
            show_message("Can't open this one", "It doesn't have an entry in Apps. Look for it there, or in the Terminal.");
            snd(SND_ERROR);
            return;
        }
        Item tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.title = (char *)it->name;
        tmp.glyph = (char *)store_cat_glyphs[it->cat];
        tmp.argv = ta;
        store.open = store.running = 0;
        launch_item(&tmp);
        argv_free(ta);
        return;
    }
    if (!it->cmd[0]) {
        show_message("Can't open this one", "This package doesn't say how to start it. Look for it in Apps.");
        snd(SND_ERROR);
        return;
    }
    if (it->flags & ST_TERM) { /* a command-line app: Pixi's own terminal runs it */
        char **ta = exec_to_argv(it->cmd);
        if (!ta) return;
        term_open(ta, it->name);
        argv_free(ta);
        if (term_is_open()) store.open = store.running = 0;
        return;
    }
    char *a[6] = { 0 };
    int n = 0;
    a[n++] = (char *)it->cmd;
    Item tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.title = (char *)it->name;
    tmp.glyph = (char *)store_cat_glyphs[it->cat];
    tmp.argv = a;
    store.open = store.running = 0;
    launch_item(&tmp);
}

/* The jobs are done and the dialog is gone: look again at what's installed. */
static void store_after_job(void)
{
    store_refresh_status();
    apps_scan();
    categories_rebuild();
    if (store.open) store_build_page();
}

static void store_action(int a)
{
    int cols = store_cols();
    if (store.detail) {
        StoreItem *it = &store.items[store.cell < store.nitems ? store.cell : 0];
        int nb = it->inst ? 2 : 1;
        switch (a) {
        case ACT_LEFT: if (store.dbtn > 0) store.dbtn--, snd(SND_STEP); break;
        case ACT_RIGHT: if (store.dbtn < nb - 1) store.dbtn++, snd(SND_STEP); break;
        case ACT_BACK: store.detail = 0; snd(SND_BACK); break;
        case ACT_OK:
        case ACT_START:
            if (!it->inst) {
                if (it->avail) package_job(1, it->pkg, it->name);
                snd(SND_SELECT);
            } else if (store.dbtn == 0) {
                if (it->upgrade) package_job(1, it->pkg, it->name); /* install again brings the newest version */
                else store_launch(it);
            } else {
                char q[200];
                snprintf(q, sizeof q, "Remove %s?", it->name);
                show_confirm(q, "The app and its files will be removed. Your own files stay.", A_STORE_REMOVE, it->pkg);
                snd(SND_SELECT);
            }
            break;
        }
        return;
    }
    if (store.focus == 0) {
        int before = store.side;
        switch (a) {
        case ACT_UP: if (store.side > 0) store.side--, snd(SND_STEP); if (store.side == SP_DIVIDER) store.side--; break;
        case ACT_DOWN: if (store.side < SIDE_ROWS - 1) store.side++, snd(SND_STEP); if (store.side == SP_DIVIDER) store.side++; break;
        case ACT_RIGHT:
        case ACT_OK:
        case ACT_START:
            if (store.side == SP_SEARCH && a != ACT_RIGHT) {
                memset(&dlg, 0, sizeof dlg);
                dlg.kind = D_OSK;
                dlg.action = A_STORE_SEARCH;
                sncpy(dlg.title, sizeof dlg.title, "Search for an app");
                osk_begin();
                SDL_StartTextInput();
                snd(SND_SELECT);
            } else {
                store.focus = 1;
                snd(SND_STEP);
            }
            break;
        case ACT_BACK: store.open = store.running = 0; snd(SND_BACK); return;
        case ACT_MENU:
            store.side = SP_SEARCH;
            break;
        }
        if (before != store.side || a == ACT_MENU) store_goto(store.side);
        return;
    }
    switch (a) {
    case ACT_BACK:
        store.focus = 0;
        snd(SND_BACK);
        break;
    case ACT_MENU: /* search from anywhere */
        memset(&dlg, 0, sizeof dlg);
        dlg.kind = D_OSK;
        dlg.action = A_STORE_SEARCH;
        sncpy(dlg.title, sizeof dlg.title, "Search for an app");
        osk_begin();
        SDL_StartTextInput();
        snd(SND_SELECT);
        break;
    case ACT_LEFT:
        if (store.nitems == 0 || cols == 1 || (store.side == SP_HOME ? (store.cell == 0 || (store.cell - 1) % cols == 0) : store.cell % cols == 0)) {
            store.focus = 0;
            snd(SND_STEP);
        } else {
            store.cell--;
            snd(SND_STEP);
        }
        break;
    case ACT_RIGHT:
        if (store.cell < store.nitems - 1 && !(store.side == SP_HOME && store.cell == 0) && cols > 1 &&
            (store.side == SP_HOME ? store.cell % cols != 0 : store.cell % cols != cols - 1))
            store.cell++, snd(SND_STEP);
        break;
    case ACT_UP:
        if (store.side == SP_HOME) {
            if (store.cell >= 1 && store.cell <= cols) store.cell = 0;
            else if (store.cell > cols) store.cell -= cols;
        } else if (store.cell >= cols) store.cell -= cols;
        snd(SND_STEP);
        break;
    case ACT_DOWN:
        if (store.side == SP_HOME) {
            if (store.cell == 0 && store.nitems > 1) store.cell = 1;
            else if (store.cell + cols < store.nitems) store.cell += cols;
        } else if (store.cell + cols < store.nitems) store.cell += cols;
        else if (store.cell / cols < (store.nitems - 1) / cols) store.cell = store.nitems - 1;
        snd(SND_STEP);
        break;
    case ACT_PGUP: store.cell = store.cell > 6 ? store.cell - 6 : 0; snd(SND_STEP); break;
    case ACT_PGDN: store.cell = store.cell + 6 < store.nitems ? store.cell + 6 : (store.nitems ? store.nitems - 1 : 0); snd(SND_STEP); break;
    case ACT_OK:
    case ACT_START:
        store_open_detail();
        break;
    }
}

/* ---------- drawing ----------------------------------------------------- */

static void store_icon(const StoreItem *it, float cx, float cy, float size, float alpha)
{
    GLuint t = it->icon[0] ? icon_tex(it->icon) : 0;
    if (!t && !it->pi) t = it->cmd[0] || it->pkg[0] ? icon_tex(it->cmd[0] ? it->cmd : it->pkg) : 0;
    if (!t && it->pkg[0] && !it->pi) t = icon_tex(it->pkg);
    if (t) {
        picture(t, cx - size / 2, cy - size / 2, size, size, with_alpha(th.text, 1), alpha);
        return;
    }
    unsigned h = 5381;
    for (const char *p = it->pkg; *p; p++) h = h * 33 + (unsigned char)*p;
    Col base = h % 3 == 0 ? th.accent : h % 3 == 1 ? th.accent_soft : th.accent_deep;
    pane(cx - size / 2, cy - size / 2, size, size, size * 0.24f, SQUIRCLE, with_alpha(base, 0.85f), DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_QUIET,
         alpha, 0);
    char ch[2] = { (char)toupper((unsigned char)it->name[0]), 0 };
    text(ch, cx, cy - size * 0.30f, size * 0.55f, 1, th.text, alpha, AL_CENTER, 0);
}

/* The rectangle a cell takes up, in the content's own coordinates. */
static void store_cell_rect(int i, float cw, float *x, float *y, float *w, float *h)
{
    int cols = store_cols();
    float gap = 14 * S;
    float cell_w = (cw - gap * (cols - 1)) / cols;
    float cell_h = (cols == 1 ? 84 : 128) * S;
    float top = 0;
    if (store.side == SP_HOME) {
        float fh = 270 * S;
        if (i == 0) {
            *x = 0, *y = 0, *w = cw, *h = fh;
            return;
        }
        top = fh + 78 * S;
        i -= 1;
    } else if (store.side == SP_SEARCH) {
        top = 56 * S;
    }
    *w = cell_w, *h = cell_h;
    *x = (float)(i % cols) * (cell_w + gap);
    *y = top + (float)(i / cols) * (cell_h + gap);
}

static void draw_store_cell(const StoreItem *it, float x, float y, float w, float h, int sel, float a)
{
    pane(x, y, w, h, 22 * S, SQUIRCLE, sel ? with_alpha(th.accent, 0.40f) : with_alpha(th.glass_raised, 0.07f), DEPTH_CONTROL * S, FROST_CONTROL,
         sel ? GLOSS_FULL : GLOSS_QUIET, a, 0);
    if (sel) outline(x - 2 * S, y - 2 * S, w + 4 * S, h + 4 * S, 24 * S, 2.5f * S, th.accent_soft, a * 0.9f);
    if (it->special == 2) {
        glyph("setting-update-all", x + 52 * S, y + h / 2, 56 * S, th.text, a);
        text("Update everything", x + 100 * S, y + h / 2 - 14 * S, 26 * S, 1, th.text, a, AL_LEFT, (int)(w - 130 * S));
        return;
    }
    if (it->special == 3) {
        glyph("category-software", x + 52 * S, y + h / 2, 56 * S, th.text, a);
        text("Add Pi-Apps", x + 100 * S, y + h / 2 - 30 * S, 26 * S, 1, th.text, a, AL_LEFT, (int)(w - 130 * S));
        text("200+ more apps", x + 100 * S, y + h / 2 + 4 * S, 19 * S, 0, th.text_soft, a, AL_LEFT, (int)(w - 130 * S));
        return;
    }
    float is = fminf(h - 34 * S, 76 * S);
    store_icon(it, x + 24 * S + is / 2, y + h / 2, is, a);
    float tx = x + 24 * S + is + 16 * S, tw = w - (tx - x) - 20 * S;
    float row = h * 0.5f;
    if (it->by[0]) {
        text(it->name, tx, y + row - 52 * S, 23 * S, 1, th.text, a, AL_LEFT, (int)tw);
        text(it->by[0] ? it->by : "", tx, y + row - 18 * S, 19 * S, 0, th.text, a * 0.85f, AL_LEFT, (int)tw);
        text(it->summary, tx, y + row + 8 * S, 19 * S, 0, th.text_soft, a, AL_LEFT, (int)tw);
    } else {
        text(it->name, tx, y + row - 34 * S, 23 * S, 1, th.text, a, AL_LEFT, (int)tw);
        text(it->summary, tx, y + row, 19 * S, 0, th.text_soft, a, AL_LEFT, (int)tw);
    }
    if (it->inst || it->upgrade) {
        const char *b = it->upgrade && h > 100 * S ? "Update" : it->inst && h > 100 * S ? "Installed" : NULL;
        if (b) {
            float bw = (float)text_width(b, 18 * S, 0) + 26 * S;
            pane(x + w - bw - 14 * S, y + 12 * S, bw, 30 * S, 15 * S, SQUIRCLE, with_alpha(th.text, 0.14f), 0, 0, 0, a, 0);
            text(b, x + w - bw / 2 - 14 * S, y + 17 * S, 18 * S, 0, th.text, a, AL_CENTER, 0);
        }
    }
}

static void draw_store(void)
{
    float a = 1;
    pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.30f }, 0, 0, 0, a, 0);
    float m = 22 * S;
    float wx = m, wy = m, ww = W - 2 * m, wh = H - 2 * m;
    pane(wx, wy, ww, wh, 28 * S, SQUIRCLE, with_alpha(th.glass, 0.50f), DEPTH_PANEL * S, FROST_PANEL, GLOSS_QUIET, a, 1);

    /* the sidebar */
    float sw = 360 * S, sx = wx + 20 * S, sy = wy + 24 * S, sh = wh - 48 * S;
    pane(sx, sy, sw, sh, 22 * S, SQUIRCLE, with_alpha(th.glass_raised, 0.05f), DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_QUIET, a, 0);
    float rh = 62 * S;
    int visible = (int)((sh - 20 * S) / rh);
    float target = (float)(store.side) - (float)visible / 2 + 0.5f;
    float maxs = (float)(SIDE_ROWS - visible);
    if (target > maxs) target = maxs;
    if (target < 0) target = 0;
    store.side_pos += (target - store.side_pos) * 0.25f;
    static const char *glyphs[6] = { "file-home", "search", "setting-updates", "chosen", "", "category-software" };
    static const char *labels[6] = { "Home", "Search", "Updates", "Installed", "", "Everything" };
    for (int i = 0; i < SIDE_ROWS; i++) {
        float ry = sy + 10 * S + ((float)i - store.side_pos) * rh;
        if (ry < sy - rh * 0.2f || ry + rh > sy + sh + rh * 0.2f) continue;
        float fa = a * clampf(fminf((ry - sy + rh * 0.5f) / rh, (sy + sh - ry) / rh), 0, 1);
        if (i == SP_DIVIDER) {
            pane(sx + 24 * S, ry + rh / 2, sw - 48 * S, 1.5f * S, 0.75f * S, CIRCLE, with_alpha(th.text, 0.2f), 0, 0, 0, fa, 0);
            continue;
        }
        int sel = i == store.side;
        int count = i == SP_UPDATES ? store.nupg : i == SP_INSTALLED ? store_installed_count() : -1;
        if (sel)
            pane(sx + 8 * S, ry + 4 * S, sw - 16 * S, rh - 8 * S, (rh - 8 * S) / 2, SQUIRCLE, with_alpha(th.accent, store.focus == 0 ? 0.62f : 0.34f),
                 DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_FULL, fa, 0);
        const char *g = i < SP_CAT0 ? glyphs[i] : store_cat_glyphs[i - SP_CAT0];
        const char *lb = i < SP_CAT0 ? labels[i] : store_cat_names[i - SP_CAT0];
        glyph(g, sx + 46 * S, ry + rh / 2, 34 * S, th.text, fa * (sel ? 1 : 0.85f));
        text(lb, sx + 82 * S, ry + rh / 2 - 12 * S, 24 * S, sel, th.text, fa * (sel ? 1 : 0.8f), AL_LEFT, (int)(sw - 160 * S));
        if (count > 0) {
            char c[16];
            snprintf(c, sizeof c, "%d", count);
            text(c, sx + sw - 28 * S, ry + rh / 2 - 11 * S, 22 * S, 0, th.accent_soft, fa, AL_RIGHT, 0);
        }
    }

    /* the page */
    float cx = sx + sw + 36 * S, cw = wx + ww - cx - 36 * S, top = wy + 70 * S, bottom = wy + wh - 54 * S;
    text(store_page_title(), cx, wy + 20 * S, 40 * S, 1, th.text, a, AL_LEFT, (int)cw);
    if (store.side == SP_SEARCH && store.query[0]) {
        char q[160];
        snprintf(q, sizeof q, "Results for \"%s\"", store.query);
        text(q, cx, top + 6 * S, 22 * S, 0, th.text_soft, a, AL_LEFT, (int)cw);
    }
    if (store.nitems == 0) {
        const char *msg = store.side == SP_SEARCH ? (store.query[0] ? "Nothing found. Try another word." : "Press the confirm button to search for an app.")
                          : store.side == SP_INSTALLED ? "None of the store's apps are installed yet."
                          : "Nothing here. Run Update OS in Settings to refresh the package lists.";
        text(msg, cx, top + 80 * S, 24 * S, 0, th.text_soft, a, AL_LEFT, (int)cw);
        if (store.side == SP_UPDATES) text("Everything is up to date.", cx, top + 80 * S, 24 * S, 0, th.text_soft, a, AL_LEFT, (int)cw);
    }
    /* keep the selected cell in view */
    if (store.nitems) {
        float x, y, w, h;
        store_cell_rect(store.cell, cw, &x, &y, &w, &h);
        float view = bottom - top;
        float want = store.scroll;
        if (y - want < 0) want = y - 10 * S;
        if (y + h - want > view) want = y + h - view + 10 * S;
        if (store.cell == 0 || y < view * 0.5f) want = fminf(want, 0);
        if (want < 0) want = 0;
        store.scroll += (want - store.scroll) * 0.3f;
    }
    for (int i = 0; i < store.nitems; i++) {
        float x, y, w, h;
        store_cell_rect(i, cw, &x, &y, &w, &h);
        float dy = top + y - store.scroll;
        if (dy + h < top - 4 || dy > bottom + 4) continue;
        float fa = a * clampf(fminf((dy + h - top) / (40 * S), (bottom - dy) / (40 * S)), 0, 1);
        const StoreItem *it = &store.items[i];
        int sel = i == store.cell && store.focus == 1;
        if (store.side == SP_HOME && i == 0) {
            pane(cx + x, dy, w, h, 26 * S, SQUIRCLE, sel ? with_alpha(th.accent, 0.34f) : with_alpha(th.glass_raised, 0.07f), DEPTH_PANEL * S,
                 FROST_PANEL, GLOSS_QUIET, fa, 1);
            if (sel) outline(cx + x - 2 * S, dy - 2 * S, w + 4 * S, h + 4 * S, 28 * S, 2.5f * S, th.accent_soft, fa);
            if (it->special) {
                text("Welcome to the App Store", cx + x + 36 * S, dy + h / 2 - 20 * S, 34 * S, 1, th.text, fa, AL_LEFT, (int)(w - 72 * S));
            } else {
                text("Featured", cx + x + 38 * S, dy + 34 * S, 22 * S, 0, th.text_soft, fa, AL_LEFT, 0);
                text(it->name, cx + x + 36 * S, dy + 66 * S, 64 * S, 1, th.text, fa, AL_LEFT, (int)(w * 0.55f));
                text(it->by, cx + x + 38 * S, dy + 150 * S, 22 * S, 0, th.text, fa * 0.9f, AL_LEFT, (int)(w * 0.55f));
                text(it->summary, cx + x + 38 * S, dy + 182 * S, 24 * S, 0, th.text_soft, fa, AL_LEFT, (int)(w * 0.55f));
                store_icon(it, cx + x + w - 150 * S, dy + h / 2, 160 * S, fa);
                if (it->inst) {
                    float bw = (float)text_width("Installed", 20 * S, 0) + 30 * S;
                    pane(cx + x + 38 * S, dy + h - 54 * S, bw, 34 * S, 17 * S, SQUIRCLE, with_alpha(th.text, 0.16f), 0, 0, 0, fa, 0);
                    text("Installed", cx + x + 38 * S + bw / 2, dy + h - 48 * S, 20 * S, 0, th.text, fa, AL_CENTER, 0);
                }
            }
            continue;
        }
        if (store.side == SP_HOME && i == 1)
            text("Popular apps", cx, top + 270 * S + 22 * S - store.scroll, 30 * S, 1, th.text, fa, AL_LEFT, 0), text("What people install most", cx, top + 270 * S + 56 * S - store.scroll, 19 * S, 0, th.text_soft, fa, AL_LEFT, 0);
        draw_store_cell(it, cx + x, dy, w, h, sel, fa);
    }

    /* the details of one app */
    if (store.detail && store.cell < store.nitems) {
        const StoreItem *it = &store.items[store.cell];
        pane(0, 0, (float)W, (float)H, 0, CIRCLE, (Col){ 0, 0, 0, 0.55f }, 0, 0, 0, a, 0);
        float pw = fminf(W * 0.78f, 1000 * S), ph = fminf(H * 0.80f, 620 * S), px = (W - pw) / 2, py = (H - ph) / 2;
        pane(px, py, pw, ph, 30 * S, SQUIRCLE, with_alpha(th.glass, 0.62f), DEPTH_PANEL * S, FROST_PANEL, GLOSS_QUIET, a, 1);
        store_icon(it, px + 100 * S, py + 100 * S, 120 * S, a);
        text(it->name, px + 190 * S, py + 44 * S, 42 * S, 1, th.text, a, AL_LEFT, (int)(pw - 230 * S));
        if (it->by[0]) text(it->by, px + 190 * S, py + 96 * S, 22 * S, 0, th.text_soft, a, AL_LEFT, (int)(pw - 230 * S));
        char line[200];
        snprintf(line, sizeof line, "%s%s%s", store.info.version[0] ? "Version " : "", store.info.version,
                 store.info.size_kb ? "" : "");
        if (store.info.size_kb) {
            char sz[32];
            human_size(sz, sizeof sz, (double)store.info.size_kb * 1024);
            snprintf(line + strlen(line), sizeof line - strlen(line), "%s%s on disk", line[0] ? "  \xC2\xB7  " : "", sz);
        }
        if (it->inst) snprintf(line + strlen(line), sizeof line - strlen(line), "%s%s", line[0] ? "  \xC2\xB7  " : "", it->upgrade ? "Update available" : "Installed");
        text(line, px + 190 * S, py + 130 * S, 20 * S, 0, th.text_soft, a, AL_LEFT, (int)(pw - 230 * S));
        const char *desc = store.info.desc[0] ? store.info.desc : it->summary;
        char dl[8][256];
        int n = wrap_text(desc, 22 * S, (int)(pw - 80 * S), dl, 8);
        for (int i = 0; i < n; i++) text(dl[i], px + 40 * S, py + 196 * S + i * 30 * S, 22 * S, 0, th.text, a * 0.92f, AL_LEFT, 0);
        float bh = 58 * S, by = py + ph - bh - 30 * S;
        const char *b1 = !it->inst ? (it->avail ? "Install" : "Not available") : it->upgrade ? "Update" : (it->cmd[0] || it->pi ? "Open" : "Installed");
        const char *bl[2] = { b1, "Remove" };
        int nb = it->inst ? 2 : 1;
        for (int i = 0; i < nb; i++) {
            float bw = 190 * S, bx = px + 40 * S + i * (bw + 16 * S);
            int sel = store.dbtn == i;
            pane(bx, by, bw, bh, bh / 2, SQUIRCLE, sel ? with_alpha(th.accent, 0.7f) : with_alpha(th.glass_raised, 0.10f), DEPTH_CONTROL * S,
                 FROST_CONTROL, sel ? GLOSS_FULL : GLOSS_QUIET, a, 0);
            text(bl[i], bx + bw / 2, by + bh / 2 - 12 * S, 24 * S, sel, th.text, a, AL_CENTER, 0);
        }
    }

    Hint h[3] = { { pad_ok(), store.detail ? "Choose" : "Open" }, { "pad-north", "Search" }, { pad_back(), "Back" } };
    if (store.detail) {
        h[1] = h[2];
        draw_hints(h, 2);
    } else {
        draw_hints(h, 3);
    }
}


static int store_is_open(void) { return store.open; }
static int store_is_running(void) { return store.running; }
static void store_set_open(int open) { store.open = open; }
static void store_quit(void) { store.open = store.running = 0; }

static void store_remove_now(const char *pkg)
{
    store.detail = 0;
    package_job(0, pkg, pkg);
}

static void store_search_done(const char *q)
{
    store_search(q);
    store_goto(SP_SEARCH);
    store.focus = 1;
}

static void yad_action(int a);

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
    case D_CHECK:
        yad_action(a);
        break;
    case D_UPDATE:
        if (upd.done && (a == ACT_OK || a == ACT_BACK || a == ACT_START)) {
            dlg.kind = D_NONE;
            upd.active = 0;
            snd(SND_BACK);
            if (upd.kind == 3 && upd.ok) {
                show_confirm("Restart now?", "Pixi was updated. Restart to finish.", A_REBOOT, "");
            } else if (store.open) store_after_job();
            else if (upd.kind) {
                apps_scan();
                categories_rebuild();
                menu_rebuild(stack[depth]);
            }
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
        if (dlg.action == A_APP_TYPE) {
            live_action(a);
            break;
        }
        switch (a) {
        case ACT_UP: case ACT_DOWN: case ACT_LEFT: case ACT_RIGHT: pk_nav(a, &dlg.r, &dlg.c); break;
        case ACT_OK: {
            const PKey *k = pk_at(dlg.r, dlg.c);
            PKOut o;
            if (!k || !pk_press(k, &o)) break;
            switch (o.type) {
            case KY_CHAR:
                if (o.fkey) snd(SND_ERROR); /* no F keys in a text box */
                else osk_type(o.text);
                break;
            case KY_SPACE: osk_type(" "); break;
            case KY_BKSP: osk_backspace(); snd(SND_KEY); break;
            case KY_ENTER: osk_done(); break;
            case KY_ESC:
                SDL_StopTextInput();
                dlg.kind = D_NONE;
                snd(SND_BACK);
                break;
            default: snd(SND_ERROR); break; /* nothing to move or send in a text box */
            }
            break;
        }
        case ACT_BACK:
            if (dlg.buf[0]) osk_backspace(), snd(SND_KEY);
            else {
                SDL_StopTextInput();
                dlg.kind = D_NONE;
                snd(SND_BACK);
            }
            break;
        case ACT_MENU: /* Triangle: backspace too (but unlike B it never closes the keyboard) */
            if (dlg.buf[0]) osk_backspace(), snd(SND_KEY);
            break;
        case ACT_PGUP: osk_type(" "); break; /* Square (and Page Up on a keyboard) */
        case ACT_START: osk_start_pressed = 1; osk_done(); osk_start_pressed = 0; break;
        }
        break;
    }
    }
}

static void guide_activate_entry(void)
{
    if (guide.entry >= guide.nentries) return;
    GuideEntry *e = &guide.entries[guide.entry];
    if (e->what == GE_RESUME) guide_close(1);
    else if (e->what == GE_CLOSE) {
        if (app_is_ours(e->id)) app_close(e->id);
        else comp_close(e->id);
        guide_entries();
        snd(SND_GUIDE_SELECT);
    } else if (e->what == GE_SWITCH) {
        if (app_is_ours(e->id)) app_switch(e->id);
        else {
            guide.open = 0;
            comp_clear_previews();
            app_hide_all();
            comp_activate(e->id);
        }
    } else guide_close(0);
}

static int guide_sidebar_rows(int *rows)
{
    int n = 0;
    rows[n++] = G_TILE;
    rows[n++] = G_SLIDER;
    if (guide.brightness >= 0) rows[n++] = G_BRIGHT;
    if (mp.nq > 0) rows[n++] = G_MEDIA;
    rows[n++] = G_ENTRY;
    rows[n++] = G_POWER;
    return n;
}

/* ---------- the Guide's quick tiles -------------------------------------- */

static void guide_toggle_radio(int bluetooth)
{
    int cur = bluetooth ? guide.bt_on : guide.wifi_on;
    const char *name = bluetooth ? "Bluetooth" : "Wi-Fi";
    if (cur < 0) {
        show_message(name, bluetooth ? "No Bluetooth adapter was found." : "Wi-Fi isn't available here.");
        snd(SND_ERROR);
        return;
    }
    char msg[64];
    snprintf(msg, sizeof msg, "Turning %s %s\xE2\x80\xA6", name, cur ? "off" : "on");
    show_busy(name, msg);
    if (bluetooth) {
        bluetooth_set_radio(!cur);
        guide.bt_on = bluetooth_radio();
    } else {
        wifi_set_radio(!cur);
        guide.wifi_on = wifi_radio();
    }
    dlg.kind = D_NONE;
    snd(SND_SELECT);
}

static void guide_toggle_padmode(void)
{
    Win *w = guide_target_win();
    if (!w) {
        GuideCard *c = guide_selected();
        int ours = app_front() || (c && c->id && app_is_ours(c->id));
        show_message("Gamepad / mouse mode", ours ? "Pixi's own apps already work with the gamepad."
                                                   : "Open an app first, then switch how it is controlled.");
        snd(SND_ERROR);
        return;
    }
    w->padmode = w->padmode == PAD_MOUSE ? PAD_GAMEPAD : PAD_MOUSE;
    snd(SND_SELECT);
}

/* The Guide's Screenshot button: the compositor writes the picked app's picture to a temporary PPM, and when it
 * shows up it's saved as a PNG in ~/Pictures/Screenshots. */
static struct {
    int active;
    char ppm[300], name[160];
    float deadline;
} shotreq;

static void guide_screenshot(void)
{
    Win *w = guide_target_win();
    if (!w) {
        show_message("Screenshot", "Nothing to take a picture of. Open an app, then pick it in the Guide.");
        snd(SND_ERROR);
        return;
    }
    if (!pixi || pixi_ver < 4) {
        show_message("Screenshot", "The compositor is too old for this. Reinstall Pixi to update it.");
        snd(SND_ERROR);
        return;
    }
    const char *rt = getenv("XDG_RUNTIME_DIR");
    snprintf(shotreq.ppm, sizeof shotreq.ppm, "%s/pixi-shot-%d.ppm", rt && *rt ? rt : "/tmp", (int)getpid());
    unlink(shotreq.ppm);
    sncpy(shotreq.name, sizeof shotreq.name, *w->title ? w->title : *w->app_id ? w->app_id : "app");
    pixi_shell_v1_screenshot(pixi, w->id, shotreq.ppm);
    wl_display_flush(wl_dpy);
    shotreq.active = 1;
    shotreq.deadline = now_s + 3.0f;
    snd(SND_SELECT);
}

/* read the compositor's PPM (P6, 255) into a surface */
static SDL_Surface *ppm_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    int w = 0, h = 0, mx = 0;
    if (fscanf(f, "P6 %d %d %d", &w, &h, &mx) != 3 || w < 1 || h < 1 || w > 8192 || h > 8192 || mx != 255) {
        fclose(f);
        return NULL;
    }
    fgetc(f); /* the one whitespace after the header */
    SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 24, SDL_PIXELFORMAT_RGB24);
    int ok = sf != NULL;
    for (int y = 0; ok && y < h; y++) ok = fread((unsigned char *)sf->pixels + (size_t)y * sf->pitch, 3, (size_t)w, f) == (size_t)w;
    fclose(f);
    if (!ok) {
        if (sf) SDL_FreeSurface(sf);
        return NULL;
    }
    return sf;
}

static void shot_tick(void)
{
    if (!shotreq.active) return;
    if (access(shotreq.ppm, R_OK) != 0) {
        if (now_s > shotreq.deadline) {
            shotreq.active = 0;
            show_message("Screenshot", "Couldn't get a picture of that app. It may not have drawn anything yet.");
            snd(SND_ERROR);
        }
        return;
    }
    shotreq.active = 0;
    SDL_Surface *sf = ppm_load(shotreq.ppm);
    unlink(shotreq.ppm);
    if (!sf) {
        show_message("Screenshot", "Couldn't read the picture.");
        snd(SND_ERROR);
        return;
    }
    char dir[1100], file[1400], nm[160];
    snprintf(dir, sizeof dir, "%s/Pictures", home_dir());
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/Pictures/Screenshots", home_dir());
    mkdir(dir, 0755);
    size_t n = 0;
    for (const char *c = shotreq.name; *c && n < sizeof nm - 1; c++) nm[n++] = isalnum((unsigned char)*c) ? *c : '-';
    nm[n] = 0;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    (void)nm;
    snprintf(file, sizeof file, "%s/%04d-%02d-%02d_%02d-%02d-%02d.png", dir, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min, tm.tm_sec);
    int ok = IMG_SavePNG(sf, file) == 0;
    SDL_FreeSurface(sf);
    if (ok) {
        show_message("Screenshot saved", "Saved to Pictures > Screenshots.");
        snd(SND_SELECT);
    } else {
        show_message("Screenshot", "Couldn't save the picture.");
        snd(SND_ERROR);
    }
}

static void guide_tile_press(int i)
{
    if (i == 0) guide_toggle_radio(0);
    else if (i == 1) guide_toggle_radio(1);
    else if (i == 2) guide_toggle_padmode();
    else guide_screenshot();
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
    if (a == ACT_PGUP) { /* L: over to the side bar */
        if (guide.row == G_SWITCH) {
            guide.row = G_ENTRY;
            guide.entry = 0;
            snd(SND_GUIDE_STEP);
        }
        return;
    }
    if (a == ACT_PGDN) { /* R: over to the app switcher */
        if (guide.row != G_SWITCH) {
            guide.row = G_SWITCH;
            snd(SND_GUIDE_STEP);
        }
        return;
    }
    int rows[8];
    int nrows = guide_sidebar_rows(rows);
    int ri = 0;
    for (int i = 0; i < nrows; i++)
        if (rows[i] == guide.row) ri = i;

    if (guide.row == G_SWITCH) {
        switch (a) {
        case ACT_GUIDE:
        case ACT_BACK:
            guide_close(1);
            break;
        case ACT_UP:
            if (guide.card > 0) guide.card--, guide_entries(), snd(SND_GUIDE_STEP);
            break;
        case ACT_DOWN:
            if (guide.card < guide.ncards - 1) guide.card++, guide_entries(), snd(SND_GUIDE_STEP);
            break;
        case ACT_LEFT:
            guide.row = G_ENTRY;
            guide.entry = 0;
            snd(SND_GUIDE_STEP);
            break;
        case ACT_MENU: { /* Options closes the selected app */
            GuideCard *c = guide_selected();
            if (c && c->id) {
                if (app_is_ours(c->id)) app_close(c->id), guide_entries();
                else comp_close(c->id);
                snd(SND_GUIDE_SELECT);
            }
            break;
        }
        case ACT_OK:
        case ACT_START: {
            GuideCard *c = guide_selected();
            snd(SND_GUIDE_SELECT);
            if (!c) break;
            if (!c->id) guide_close(0);
            else if (c->id == guide_current()) guide_close(1);
            else if (app_is_ours(c->id)) app_switch(c->id);
            else {
                guide.open = 0;
                comp_clear_previews();
                app_hide_all();
                comp_activate(c->id);
            }
            break;
        }
        }
        return;
    }

    switch (a) {
    case ACT_GUIDE:
    case ACT_BACK:
        guide_close(1);
        break;
    case ACT_UP:
        if (guide.row == G_ENTRY && guide.entry > 0) guide.entry--;
        else if (ri > 0) {
            guide.row = rows[ri - 1];
            if (guide.row == G_ENTRY) guide.entry = guide.nentries - 1;
        }
        snd(SND_GUIDE_STEP);
        break;
    case ACT_DOWN:
        if (guide.row == G_ENTRY && guide.entry < guide.nentries - 1) guide.entry++;
        else if (ri < nrows - 1) {
            guide.row = rows[ri + 1];
            if (guide.row == G_ENTRY) guide.entry = 0;
        }
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
        } else if (guide.row == G_BRIGHT && guide.brightness >= 0) {
            int v = guide.brightness + d * 5;
            v = v < 5 ? 5 : v > 100 ? 100 : v;
            if (v != guide.brightness) {
                guide.brightness = v;
                brightness_set(v);
                snd(SND_GUIDE_STEP);
            }
        } else if (guide.row == G_MEDIA) {
            int m = guide.mbtn + d;
            if (m >= 0 && m <= 2) guide.mbtn = m, snd(SND_GUIDE_STEP);
            else if (d > 0) guide.row = G_SWITCH, snd(SND_GUIDE_STEP);
        } else if (d > 0) {
            guide.row = G_SWITCH; /* from the buttons and the power key, across to the cards */
            snd(SND_GUIDE_STEP);
        }
        break;
    }
    case ACT_OK:
        snd(SND_GUIDE_SELECT);
        if (guide.row == G_POWER) {
            guide.power_open = 1;
            guide.power_sel = 0;
        } else if (guide.row == G_MEDIA) {
            if (guide.mbtn == 0) music_go(-1, 0);
            else if (guide.mbtn == 1) music_toggle();
            else music_go(1, 0);
        } else if (guide.row == G_TILE) {
            guide_tile_press(guide.tile);
        } else if (guide.row == G_ENTRY) {
            guide_activate_entry();
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
    if (vis[i] != CAT_GAMES && vis[i] != CAT_APPS) menu_rebuild(stack[0]);
    snd(SND_STEP);
}

static Uint32 last_guide_ms;

static void handle_action(int a)
{
    if (gptest_is_open()) { /* every button belongs to the tester, the guide button included */
        gptest_action(a);
        return;
    }
    if (a == ACT_GUIDE) {
        Uint32 t = SDL_GetTicks();
        if (t - last_guide_ms < 300 && last_guide_ms) return; /* the same press, heard twice */
        last_guide_ms = t;
        if (guide.open) guide_close(1);
        else guide_open();
        return;
    }
    if (hidden && a == ACT_MENU && pm_active_fwd()) { /* R + Triangle in Keyboard + Mouse mode: the on-screen keyboard */
        if (tri_with_r) appkb_open();
        tri_with_r = 0;
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
    if (side.open) {
        side_action(a);
        return;
    }
    if (ed_is_open()) {
        ed_action(a);
        return;
    }
    if (bui.open) {
        browser_action(a);
        return;
    }
    if (vp.open) {
        video_action(a);
        return;
    }
    if (term_is_open()) {
        term_action(a);
        return;
    }
    if (sysui.open) {
        sysinfo_action(a);
        return;
    }
    if (aps.open) {
        aps_action(a);
        return;
    }
    if (al.open) {
        alist_action(a);
        return;
    }
    if (netui.open) {
        net_action(a);
        return;
    }
    if (ftpui.open) {
        ftp_ui_action(a);
        return;
    }
    if (webui.open) {
        web_ui_action(a);
        return;
    }
    if (store.open) {
        store_action(a);
        return;
    }
    if (viewer.open) {
        viewer_action(a);
        return;
    }
    if (np.open) {
        np_action(a);
        return;
    }
    Menu *m = stack[depth];
    Item *it = (m->n && m->sel < m->n) ? &m->items[m->sel] : NULL;
    switch (a) {
    case ACT_UP: if (m->n > 1) m->sel = m->sel > 0 ? m->sel - 1 : m->n - 1, snd(SND_STEP); break; /* wraps to the last item */
    case ACT_DOWN: if (m->n > 1) m->sel = m->sel < m->n - 1 ? m->sel + 1 : 0, snd(SND_STEP); break;
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
        if (m->build == build_files_dir) {
            side_open_files(m, it && (it->flags & IF_FILE) && it->data && strlen(it->data) < 250 ? it : NULL);
        } else if (it && it->action == A_WIFI_NET) {
            side_open_wifi(it);
        } else if (it && it->action == A_STORE_OPEN) {
            side_open_store();
            snd(SND_SELECT);
        } else if (it && (it->flags & (IF_APP | IF_ROM | IF_MEDIA))) {
            side_open_item(it);
        } else if (it && (it->flags & IF_FILE) && it->data && strlen(it->data) < 250) {
            char q[300];
            snprintf(q, sizeof q, "Delete \"%s\"?", it->title);
            show_confirm(q, "This can't be undone.", A_DELETE, it->data);
            dlg.choice = 1; /* No is the safe default */
            snd(SND_SELECT);
        } else if (it && it->detail) show_message(it->title, it->detail), snd(SND_SELECT);
        break;
    }
}

/* ---------- buttons, straight from the kernel ----------------------------- */
/* HOME, the volume buttons and the like don't always reach SDL, so every input device that has any of those
 * keys is read here too: HOME opens the Guide, the volume buttons change the volume, and the Gamepad
 * Tester lights the button. */

/* HOME can arrive from SDL, the raw input device and the controller's shared memory at once;
   only the first one counts */
static Uint32 guide_at;
static int guide_gate(void)
{
    Uint32 n = SDL_GetTicks();
    if (guide_at && n - guide_at < 250) return 0;
    guide_at = n;
    return 1;
}

#define PAD_EV_MAX 16
typedef struct {
    int fd, idx;
    char name[64];
} EvDev;
static EvDev evd[PAD_EV_MAX];
static int ev_n, ev_denied;
static unsigned char ev_seen[64];
static Uint32 ev_scan_at;

static int ev_bit(const unsigned char *bits, int code) { return (bits[code / 8] >> (code % 8)) & 1; }

/* does this device have a key that matters here? */
static int ev_interesting(const unsigned char *bits)
{
    static const int codes[] = { BTN_MODE, BTN_TL2, BTN_TR2, BTN_SELECT, BTN_START, BTN_SOUTH, KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE,
                                 KEY_HOMEPAGE, KEY_BRIGHTNESSDOWN, KEY_BRIGHTNESSUP, KEY_BRIGHTNESS_CYCLE, KEY_DISPLAYTOGGLE, KEY_SWITCHVIDEOMODE };
    for (size_t i = 0; i < sizeof codes / sizeof *codes; i++)
        if (ev_bit(bits, codes[i])) return 1;
    return 0;
}

static void evdev_scan(Uint32 now)
{
    if (now < ev_scan_at) return;
    ev_scan_at = now + 3000;
    ev_denied = 0;
    for (int i = 0; i < 64 && ev_n < PAD_EV_MAX; i++) {
        if (ev_seen[i]) continue;
        char p[48];
        snprintf(p, sizeof p, "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES) ev_denied++; /* there, but this account can't read it (the input group) */
            continue; /* not there yet; look again later */
        }
        unsigned char bits[KEY_MAX / 8 + 1];
        memset(bits, 0, sizeof bits);
        ev_seen[i] = 1;
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) >= 0 && ev_interesting(bits)) {
            EvDev *d = &evd[ev_n++];
            d->fd = fd;
            d->idx = i;
            if (ioctl(fd, EVIOCGNAME(sizeof d->name), d->name) < 0) snprintf(d->name, sizeof d->name, "event%d", i);
            if (cfg.debug_input) printf("input: watching %s (%s)\n", p, d->name);
        } else {
            close(fd);
        }
    }
}

/* the volume buttons: 5% a press, with a small bar on screen */
static struct {
    float until;
    int pct, muted;
} volhud;

static void vol_button(int step)
{
    if (step == 0) {
        volume_toggle_mute();
        volhud.muted = volume_muted();
        volhud.pct = volume_get();
    } else {
        if (volume_muted()) volume_toggle_mute();
        int v = volume_get();
        if (v < 0) return;
        v += step;
        v = v < 0 ? 0 : v > 100 ? 100 : v;
        volume_set(v);
        volhud.pct = v;
        volhud.muted = 0;
        guide.volume = v;
    }
    volhud.until = now_s + 1.6f;
    snd(SND_STEP);
}

static void draw_volhud(void)
{
    if (now_s >= volhud.until) return;
    float a = clampf(volhud.until - now_s, 0, 1);
    float w = 300 * S, h = 56 * S, x = (W - w) / 2, y = 22 * S;
    pane(x, y, w, h, h / 2, SQUIRCLE, with_alpha(th.glass, 0.9f), DEPTH_CONTROL * S, FROST_CONTROL, GLOSS_QUIET, a, 0);
    glyph(volhud.muted || volhud.pct == 0 ? "volume-muted" : "volume", x + 34 * S, y + h / 2, 30 * S, th.text, a);
    float bx = x + 66 * S, bw = w - 66 * S - 74 * S;
    pane(bx, y + h / 2 - 3 * S, bw, 6 * S, 3 * S, CIRCLE, with_alpha(th.text, 0.25f), 0, 0, 0, a, 0);
    float f = volhud.muted ? 0 : clampf(volhud.pct / 100.0f, 0, 1);
    if (f > 0) pane(bx, y + h / 2 - 3 * S, fmaxf(bw * f, 6 * S), 6 * S, 3 * S, CIRCLE, th.text, 0, 0, 0, a, 0);
    char b[16];
    if (volhud.muted) snprintf(b, sizeof b, "Mute");
    else snprintf(b, sizeof b, "%d%%", volhud.pct);
    text(b, x + w - 22 * S, y + h / 2 - 12 * S, 24 * S, 1, th.text, a, AL_RIGHT, 0);
}

/* Volume steps from the buttons can arrive from the input device and the shared memory both; one press is one step. */
static void vol_step(int step)
{
    static Uint32 at;
    Uint32 n = SDL_GetTicks();
    if (at && n - at < 100) return;
    at = n;
    vol_button(step);
}

/* The PSPi controller driver keeps the raw state of every button in shared memory ("my_shm"). Reading it needs no
   special permission and shows the whole board, including DISPLAY, which the driver never turns into an input
   event (the board changes the brightness by itself, so the brightness level is what moves). */
static const volatile unsigned char *pad_shm;
static Uint32 pad_shm_try;
static int shm_prev = -1, shm_bright = -1;
static Uint32 shm_display_until, shm_vol_rep;

static void pad_shm_poll(void)
{
    Uint32 now = SDL_GetTicks();
    if (!pad_shm) {
        if (now < pad_shm_try) return;
        pad_shm_try = now + 3000;
        int fd = open("/dev/shm/my_shm", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;
        struct stat st;
        void *m = MAP_FAILED;
        if (fstat(fd, &st) == 0 && st.st_size >= 11) m = mmap(NULL, 11, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED) return;
        pad_shm = m;
        if (cfg.debug_input) printf("input: reading the controller board's shared memory\n");
    }
    int b = pad_shm[0] | pad_shm[1] << 8, bright = pad_shm[4] & 7;
    int sel = b >> 1 & 1, sta = b >> 2 & 1, vup = b >> 13 & 1, vdn = b >> 14 & 1, mute = b & 1, home = b >> 15 & 1;
    if (shm_prev < 0) shm_prev = b; /* whatever is held at the start isn't a press */
    if (shm_bright < 0) shm_bright = bright;
    if (bright != shm_bright) {
        shm_bright = bright;
        shm_display_until = now + 500;
        if (gpt.open) snprintf(gpt.last, sizeof gpt.last, "Controller board: brightness changed (DISPLAY)");
    }
    if (gpt.open) {
        gpt.shm[GB_SELECT] = (unsigned char)sel;
        gpt.shm[GB_START] = (unsigned char)sta;
        gpt.shm[GB_HOME] = (unsigned char)home;
        gpt.shm[GB_VOLUP] = (unsigned char)vup;
        gpt.shm[GB_VOLDN] = (unsigned char)vdn;
        gpt.shm[GB_MUTE] = (unsigned char)mute;
        gpt.shm[GB_DISPLAY] = now < shm_display_until;
        static const char *nm[16] = { "Mute", "Select", "Start", "Cross", "Square", "Triangle", "Circle", "R1", "L1", "Left", "Up", "Down", "Right", "Vol+", "Vol-", "Home" };
        int fresh = b & ~shm_prev;
        for (int i = 0; i < 16; i++)
            if (fresh >> i & 1 && (i == 0 || i == 1 || i == 2 || i >= 13)) snprintf(gpt.last, sizeof gpt.last, "Controller board: %s", nm[i]);
    } else {
        memset(gpt.shm, 0, sizeof gpt.shm);
        if (home && !(shm_prev >> 15 & 1) && guide_gate()) handle_action(ACT_GUIDE);
        if (vup && !(shm_prev >> 13 & 1)) { vol_step(5); shm_vol_rep = now + 400; }
        else if (vdn && !(shm_prev >> 14 & 1)) { vol_step(-5); shm_vol_rep = now + 400; }
        else if ((vup || vdn) && now >= shm_vol_rep) { vol_button(vup ? 5 : -5); shm_vol_rep = now + 150; }
    }
    shm_prev = b;
}

static void evdev_poll(void)
{
    pad_shm_poll();
    evdev_scan(SDL_GetTicks());
    for (int k = 0; k < ev_n; k++) {
        struct input_event evs[16];
        ssize_t n;
        while ((n = read(evd[k].fd, evs, sizeof evs)) > 0) {
            for (size_t i = 0; i < (size_t)n / sizeof evs[0]; i++) {
                if (evs[i].type != EV_KEY) continue;
                int code = evs[i].code, val = evs[i].value;
                if (gpt.open) {
                    gp_evdev_key(evd[k].name, code, val); /* the tester only looks */
                    continue;
                }
                if ((code == BTN_MODE || code == KEY_HOMEPAGE) && val == 1) {
                    if (cfg.debug_input) printf("input: guide button (evdev)\n");
                    if (guide_gate()) handle_action(ACT_GUIDE);
                } else if (code == KEY_VOLUMEUP && val >= 1) vol_step(5);
                else if (code == KEY_VOLUMEDOWN && val >= 1) vol_step(-5);
                else if (code == KEY_MUTE && val == 1) vol_button(0);
            }
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) { /* unplugged */
            close(evd[k].fd);
            ev_seen[evd[k].idx] = 0;
            evd[k] = evd[ev_n - 1];
            ev_n--;
            k--;
        }
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
    if (a == ACT_SQUARE && !((browser_is_open() || video_is_open() || (term_is_open() && !guide.open)) && dlg.kind == D_NONE)) a = ACT_PGUP;
    if (a < 0 || a >= ACT_COUNT) return;
    held[a] = 1;
    next_rep[a] = SDL_GetTicks() + REPEAT_DELAY;
    if (SDL_GetTicks() < quiet_until && a != ACT_GUIDE) return;
    handle_action(a);
}

static void live_up(void);

static void act_release(int a)
{
    if (a == ACT_OK) live_up(); /* a key on the keyboard over an app is let go */
    if (a >= 0 && a < ACT_COUNT) held[a] = 0;
    if (a == ACT_SQUARE && tm.sq_down) term_square_up();
}

static void osk_repeat(Uint32 now)
{
    if (dlg.kind != D_OSK) return;
    int back = held[ACT_BACK] && dlg.buf[0], ok_bksp = 0;
    if (held[ACT_OK]) {
        const PKey *k = pk_at(dlg.r, dlg.c);
        ok_bksp = k && (k->type == KY_BKSP || k->type == KY_SPACE);
    }
    int a = back ? ACT_BACK : ok_bksp ? ACT_OK : -1;
    if (a >= 0 && now >= next_rep[a]) {
        next_rep[a] = now + 45;
        handle_action(a);
    }
}

/* L and Triangle are special: pressed together they open the Guide, so each one waits to see
 * if the other joins in. A quick tap still does its normal job, on release. */
static struct {
    int down, cancelled;
    Uint32 at;
} lt[2]; /* 0: L (page up), 1: Triangle (options) */

static void pad_press(int a)
{
    if (a != ACT_PGUP && a != ACT_MENU) {
        act_press(a);
        return;
    }
    int me = a == ACT_MENU, other = !me;
    if (me) tri_with_r = pm_r_down || pk.rhold; /* R + Triangle is the keyboard (in an app set to Keyboard + Mouse) */
    lt[me].down = 1;
    lt[me].cancelled = 0;
    lt[me].at = SDL_GetTicks();
    if (lt[other].down && !lt[other].cancelled) {
        lt[me].cancelled = lt[other].cancelled = 1;
        held[ACT_PGUP] = held[ACT_MENU] = 0;
        handle_action(ACT_GUIDE);
        return;
    }
    if (a == ACT_PGUP) { /* keeps paging if held */
        held[a] = 1;
        next_rep[a] = lt[me].at + REPEAT_DELAY;
    }
}

static void pad_release(int a)
{
    if (a != ACT_PGUP && a != ACT_MENU) {
        act_release(a);
        return;
    }
    int me = a == ACT_MENU;
    int tap = lt[me].down && !lt[me].cancelled && (me || SDL_GetTicks() - lt[me].at < REPEAT_DELAY);
    lt[me].down = lt[me].cancelled = 0;
    held[a] = 0;
    if (tap && SDL_GetTicks() >= quiet_until) handle_action(a);
}

/* While a keyboard is up, L and R are Shift and Ctrl (see pk_trigger), taken straight from the
 * buttons so holding them works; L still opens the Guide with Triangle. Returns 1 when it used it. */
static int pk_active(void)
{
    return !hidden && !guide.open && (dlg.kind == D_OSK || (dlg.kind == D_NONE && ((term_is_open() && !tm.ended) || ed_is_open())));
}

static int pk_trigger_event(int right, int down)
{
    if (down) {
        if (!pk_active()) return 0;
        pk_trigger(right, 1);
        if (!right) { /* so Triangle + L still opens the Guide */
            lt[0].down = 1;
            lt[0].cancelled = 0;
            lt[0].at = SDL_GetTicks();
        }
        return 1;
    }
    if (!(right ? pk.rhold : pk.lhold)) return 0;
    pk_trigger(right, 0);
    if (!right) lt[0].down = lt[0].cancelled = 0;
    return 1;
}

/* the keys that repeat while they're held: delete, space and the arrows */
static void term_repeat(Uint32 now)
{
    if (!term_is_open() || guide.open || dlg.kind != D_NONE || tm.ended) return;
    int a = -1;
    if (held[ACT_BACK]) a = ACT_BACK;
    else if (held[ACT_OK]) {
        const PKey *k = pk_at(tm.r, tm.c);
        if (k && (k->type == KY_BKSP || k->type == KY_SPACE || k->type == KY_DEL || (k->type >= KY_UP && k->type <= KY_RIGHT))) a = ACT_OK;
    }
    if (a >= 0 && now >= next_rep[a]) {
        next_rep[a] = now + 45;
        term_action(a);
    }
}

/* the editor's keys that repeat while held: delete and the key under the highlight */
static void ed_repeat(Uint32 now)
{
    if (!ed_is_open() || guide.open || dlg.kind != D_NONE) return;
    int a = -1;
    if (held[ACT_BACK]) a = ACT_BACK;
    else if (held[ACT_OK]) {
        const PKey *k = pk_at(ed.r, ed.c);
        if (k && (k->type == KY_BKSP || k->type == KY_SPACE || k->type == KY_DEL || (k->type >= KY_UP && k->type <= KY_RIGHT) || k->type == KY_CHAR)) a = ACT_OK;
    }
    if (a >= 0 && now >= next_rep[a]) {
        next_rep[a] = now + 45;
        ed_repeat_key(a);
    }
}

/* Holding Back (or Select and Start together) leaves the tester. This runs every pass of the main loop,
 * not while drawing, and either source counts: the shell's own view of the button or the tester's. */
static void gptest_tick(void)
{
    if (!gpt.open) return;
    int back = held[ACT_BACK] || gp_down(cfg.confirm_east ? GB_CROSS : GB_CIRCLE);
    int combo = gp_down(GB_SELECT) && gp_down(GB_START);
    float t = SDL_GetTicks() / 1000.0f;
    if (!back && !combo) {
        gpt.back_since = 0;
        return;
    }
    if (gpt.back_since <= 0) gpt.back_since = t;
    if (t - gpt.back_since < 1.2f) return;
    gpt.open = 0;
    gpt.back_since = 0;
    /* whatever is still held, or was pressed while testing, must not reach the menu underneath */
    memset(held, 0, sizeof held);
    memset(lt, 0, sizeof lt);
    select_held = start_held = 0;
    quiet_until = SDL_GetTicks() + 600;
    snd(SND_BACK);
}

static void repeat_tick(Uint32 now)
{
    osk_repeat(now);
    term_repeat(now);
    ed_repeat(now);
    term_stick_tick(now);
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
    case SDLK_q: return dlg.kind == D_OSK ? -1 : ACT_SQUARE;
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
    case SDL_CONTROLLER_BUTTON_X: return ACT_SQUARE;
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
    if (b == cfg.joy_square) return ACT_SQUARE;
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
    if (axis < 4) stick_raw[axis] = value;
    if (axis > 1 || slot < 0 || slot >= 8) return;
    if (browser_is_open() && dlg.kind == D_NONE) return; /* the pointer uses the stick */
    if (tm.sq_down && term_is_open() && dlg.kind == D_NONE) return; /* Square + stick scrolls the terminal */
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

/* ======================================================================== */
/* The gamepad as a keyboard and mouse, for apps that don't know controllers */
/* ======================================================================== */

/* While an app whose window is set to Keyboard + Mouse is in front, the shell turns the controller
 * into pointer and key requests for the compositor. Gamepad mode leaves the controller alone. */

enum { PK_UP, PK_DOWN, PK_LEFT, PK_RIGHT, PK_ENTER, PK_ESC, PK_TAB, PK_COUNT };
static const unsigned pm_keycode[PK_COUNT] = { 103, 108, 105, 106, 28, 1, 15 }; /* evdev codes */
static unsigned pm_kheld, pm_bheld;     /* keys and pointer buttons we're holding down */
static int pm_cursor_shown, pm_start_tap, pm_select_tap;
static int pm_stick[4], pm_trig[2];
static float pm_scroll_acc[2];
static Uint32 pm_last;

static int pm_active(void)
{
    if (!pixi || pixi_ver < 3 || !hidden) return 0;
    Win *w = front_win();
    return w && w->padmode == PAD_MOUSE;
}

static void pm_key(int k, int down)
{
    unsigned bit = 1u << k;
    if (!!(pm_kheld & bit) == !!down) return;
    pm_kheld = down ? (pm_kheld | bit) : (pm_kheld & ~bit);
    pixi_shell_v1_key(pixi, pm_keycode[k], 0, down ? 1 : 0);
}

static void pm_button(int b, int down) /* 0 left, 1 right */
{
    unsigned bit = 1u << b;
    if (!!(pm_bheld & bit) == !!down) return;
    pm_bheld = down ? (pm_bheld | bit) : (pm_bheld & ~bit);
    pixi_shell_v1_pointer_button(pixi, 272u + (unsigned)b, down ? 1 : 0);
}

static void pm_release_all(void);
static void pm_release_all_fwd(void) { pm_release_all(); }
static int pm_active_fwd(void) { return pm_active(); }

static void pm_release_all(void)
{
    if (!pixi || pixi_ver < 3) return;
    for (int k = 0; k < PK_COUNT; k++) pm_key(k, 0);
    for (int b = 0; b < 2; b++) pm_button(b, 0);
    pm_start_tap = pm_select_tap = 0;
    memset(pm_trig, 0, sizeof pm_trig);
}

/* A controller button, as the action it means. Returns 1 if it was used here. Y and the left bumper stay
 * out of it so they still make the Guide combo; Start and Select act when let go, so Select + Start
 * (the Guide again) isn't typed into the app first. */
static int pm_pad_button(int act, int is_select, int down)
{
    switch (act) {
    case ACT_OK: pm_button(0, down); return 1;
    case ACT_BACK: pm_button(1, down); return 1;
    case ACT_UP: pm_key(PK_UP, down); return 1;
    case ACT_DOWN: pm_key(PK_DOWN, down); return 1;
    case ACT_LEFT: pm_key(PK_LEFT, down); return 1;
    case ACT_RIGHT: pm_key(PK_RIGHT, down); return 1;
    case ACT_SQUARE: pm_key(PK_ESC, down); return 1;
    case ACT_PGDN: pm_r_down = down; return 1;
    case ACT_START: pm_start_tap = down; return 0; /* the caller goes on to track Start for the combo */
    case ACT_MENU:
        if (!is_select) return 0;
        pm_select_tap = down;
        return 0;
    }
    return 0;
}

/* a button let go: drop whatever it was holding, and finish a Start or Select tap */
static int pm_pad_release(int act, int is_select)
{
    int tap_start = act == ACT_START && pm_start_tap, tap_select = act == ACT_MENU && is_select && pm_select_tap;
    int used = 0;
    if (act == ACT_PGDN) pm_r_down = 0;
    if (pm_kheld || pm_bheld) used = pm_pad_button(act, is_select, 0);
    if (tap_start) {
        pm_start_tap = 0;
        if (pm_active()) pm_key(PK_ENTER, 1), pm_key(PK_ENTER, 0);
    }
    if (tap_select) {
        pm_select_tap = 0;
        if (pm_active()) pm_key(PK_TAB, 1), pm_key(PK_TAB, 0);
    }
    return used;
}

static void pm_axis_event(int axis, int value)
{
    if (axis < 4) pm_stick[axis] = value;
    else if (axis == 4 || axis == 5) { /* the triggers: the right one clicks, the left one right-clicks */
        int on = value > 8000;
        if (pm_trig[axis - 4] != on) {
            pm_trig[axis - 4] = on;
            if (pm_active()) pm_button(axis == 5 ? 0 : 1, on);
        }
    }
}

static void pm_hat_event(int v)
{
    pm_key(PK_UP, (v & SDL_HAT_UP) != 0);
    pm_key(PK_DOWN, (v & SDL_HAT_DOWN) != 0);
    pm_key(PK_LEFT, (v & SDL_HAT_LEFT) != 0);
    pm_key(PK_RIGHT, (v & SDL_HAT_RIGHT) != 0);
}

/* once per loop: show or hide the pointer, and move and scroll with the sticks */
static void pm_tick(void)
{
    int on = pm_active();
    if (on != pm_cursor_shown) {
        pm_cursor_shown = on;
        if (pixi && pixi_ver >= 3) {
            pixi_shell_v1_pointer_visible(pixi, on);
            if (!on) pm_release_all();
            wl_display_flush(wl_dpy);
        }
    }
    Uint32 now = SDL_GetTicks();
    float dt = pm_last ? fminf((now - pm_last) / 1000.0f, 0.1f) : 0.016f;
    pm_last = now;
    if (!on) return;
    float lx = stick_shaped(pm_stick[0]), ly = stick_shaped(pm_stick[1]), rx = stick_shaped(pm_stick[2]), ry = stick_shaped(pm_stick[3]);
    if (lx || ly) {
        float sp = 1640.0f * S; /* the same pace as the browser's pointer */
        pixi_shell_v1_pointer_move(pixi, wl_fixed_from_double(lx * fabsf(lx) * sp * dt), wl_fixed_from_double(ly * fabsf(ly) * sp * dt));
    }
    /* the right stick scrolls in whole notches, so apps that count wheel clicks behave */
    pm_scroll_acc[0] += rx * fabsf(rx) * 14.0f * dt;
    pm_scroll_acc[1] += ry * fabsf(ry) * 14.0f * dt;
    if (!rx) pm_scroll_acc[0] = 0;
    if (!ry) pm_scroll_acc[1] = 0;
    int nx = (int)pm_scroll_acc[0], ny = (int)pm_scroll_acc[1];
    if (nx || ny) {
        pm_scroll_acc[0] -= (float)nx;
        pm_scroll_acc[1] -= (float)ny;
        pixi_shell_v1_pointer_scroll(pixi, wl_fixed_from_int(nx), wl_fixed_from_int(ny));
    }
    wl_display_flush(wl_dpy);
}

static void handle_event(SDL_Event *e)
{
    if (gpt.open) gp_sdl_event(e);
    switch (e->type) {
    case SDL_QUIT: running = 0; break;
    case SDL_KEYDOWN:
        if (term_sdl_key(&e->key)) break; /* a keyboard typing into the terminal */
        if (ed_sdl_key(&e->key)) break;   /* or into the editor */
        if (e->key.repeat) break;
        if (dlg.kind == D_OSK) {
            if (e->key.keysym.sym == SDLK_BACKSPACE) { osk_backspace(); break; }
            if (e->key.keysym.sym == SDLK_RETURN || e->key.keysym.sym == SDLK_KP_ENTER) { osk_done(); break; }
        }
        act_press(key_action(e->key.keysym.sym));
        break;
    case SDL_KEYUP: act_release(key_action(e->key.keysym.sym)); break;
    case SDL_TEXTINPUT:
        if (dlg.kind == D_OSK) osk_type(e->text.text);
        else if (ed_is_open()) ed_sdl_text(e->text.text);
        else term_sdl_text(e->text.text);
        break;
    case SDL_CONTROLLERBUTTONDOWN:
        if (cfg.debug_input) printf("input: controller button %d\n", e->cbutton.button);
        if ((e->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || e->cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) &&
            pk_trigger_event(e->cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 1))
            break;
        if (e->cbutton.button == SDL_CONTROLLER_BUTTON_BACK) select_held = 1;
        if (e->cbutton.button == SDL_CONTROLLER_BUTTON_START) start_held = 1;
        if (select_held && start_held) { pm_start_tap = pm_select_tap = 0; combo_check(); break; }
        if (pm_active() && pm_pad_button(pad_action(e->cbutton.button), e->cbutton.button == SDL_CONTROLLER_BUTTON_BACK, 1)) break;
        if (hidden && e->cbutton.button != SDL_CONTROLLER_BUTTON_GUIDE && e->cbutton.button != SDL_CONTROLLER_BUTTON_Y &&
            e->cbutton.button != SDL_CONTROLLER_BUTTON_LEFTSHOULDER) break;
        if (e->cbutton.button == SDL_CONTROLLER_BUTTON_BACK || e->cbutton.button == SDL_CONTROLLER_BUTTON_START) {
            held[pad_action(e->cbutton.button)] = 1; /* acted on at release, so the combo can win */
            break;
        }
        if (pad_action(e->cbutton.button) == ACT_GUIDE && !guide_gate()) break;
        pad_press(pad_action(e->cbutton.button));
        break;
    case SDL_CONTROLLERBUTTONUP:
        if ((e->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || e->cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) &&
            pk_trigger_event(e->cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0))
            break;
        if (pm_pad_release(pad_action(e->cbutton.button), e->cbutton.button == SDL_CONTROLLER_BUTTON_BACK)) break;
        if (e->cbutton.button == SDL_CONTROLLER_BUTTON_BACK || e->cbutton.button == SDL_CONTROLLER_BUTTON_START) {
            int a = pad_action(e->cbutton.button);
            int was = held[a] && (e->cbutton.button == SDL_CONTROLLER_BUTTON_BACK ? select_held : start_held);
            if (e->cbutton.button == SDL_CONTROLLER_BUTTON_BACK) select_held = 0;
            else start_held = 0;
            held[a] = 0;
            if (was && !hidden) handle_action(a);
            break;
        }
        pad_release(pad_action(e->cbutton.button));
        break;
    case SDL_CONTROLLERAXISMOTION:
        pm_axis_event(e->caxis.axis, e->caxis.value);
        if (!hidden && e->caxis.axis <= SDL_CONTROLLER_AXIS_RIGHTY) axis_input(dev_slot(e->caxis.which), e->caxis.axis, e->caxis.value);
        break;
    case SDL_JOYBUTTONDOWN:
        if (SDL_GameControllerFromInstanceID(e->jbutton.which)) break;
        if (cfg.debug_input) printf("input: joystick button %d\n", e->jbutton.button);
        if ((e->jbutton.button == cfg.joy_pgup || e->jbutton.button == cfg.joy_pgdn) && pk_trigger_event(e->jbutton.button == cfg.joy_pgdn, 1)) break;
        if (e->jbutton.button == cfg.joy_select) select_held = 1;
        if (e->jbutton.button == cfg.joy_start) start_held = 1;
        if (select_held && start_held) { pm_start_tap = pm_select_tap = 0; combo_check(); break; }
        if (pm_active() && pm_pad_button(joy_action(e->jbutton.button), e->jbutton.button == cfg.joy_select, 1)) break;
        if (hidden && joy_action(e->jbutton.button) != ACT_GUIDE && joy_action(e->jbutton.button) != ACT_MENU && joy_action(e->jbutton.button) != ACT_PGUP) break;
        if (joy_action(e->jbutton.button) == ACT_GUIDE && !guide_gate()) break;
        pad_press(joy_action(e->jbutton.button));
        break;
    case SDL_JOYBUTTONUP:
        if (SDL_GameControllerFromInstanceID(e->jbutton.which)) break;
        if ((e->jbutton.button == cfg.joy_pgup || e->jbutton.button == cfg.joy_pgdn) && pk_trigger_event(e->jbutton.button == cfg.joy_pgdn, 0)) break;
        if (e->jbutton.button == cfg.joy_select) select_held = 0;
        if (e->jbutton.button == cfg.joy_start) start_held = 0;
        if (pm_pad_release(joy_action(e->jbutton.button), e->jbutton.button == cfg.joy_select)) break;
        pad_release(joy_action(e->jbutton.button));
        break;
    case SDL_JOYHATMOTION:
        if (SDL_GameControllerFromInstanceID(e->jhat.which)) break;
        if (pm_active()) pm_hat_event(e->jhat.value);
        else if (!hidden) hat_input(dev_slot(e->jhat.which), e->jhat.value);
        break;
    case SDL_JOYAXISMOTION:
        if (SDL_GameControllerFromInstanceID(e->jaxis.which)) break;
        if (e->jaxis.axis < 4) pm_axis_event(e->jaxis.axis, e->jaxis.value);
        if (!hidden) axis_input(dev_slot(e->jaxis.which), e->jaxis.axis, e->jaxis.value);
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
        pm_release_all();
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
        { "guide", ACT_GUIDE }, { "square", ACT_SQUARE },
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(cmd, map[i].name)) {
            handle_action(map[i].act);
            script_wait = 12;
            return;
        }
    if (!strncmp(cmd, "wait:", 5)) script_wait = atoi(cmd + 5);
    else if (!strncmp(cmd, "shot:", 5)) screenshot(cmd + 5);
    else if (!strncmp(cmd, "type:", 5)) { if (ed_is_open()) ed_type(cmd + 5); else osk_type(cmd + 5); }
    else if (!strcmp(cmd, "ftp")) ftp_ui_open();
    else if (!strcmp(cmd, "applist")) alist_open();
    else if (!strcmp(cmd, "web")) web_ui_open();
    else if (!strcmp(cmd, "webedit")) web_edit_config();
    else if (!strncmp(cmd, "edit:", 5)) ed_open(cmd + 5, "Web server settings", 1); /* edit:/some/file */
    else if (!strncmp(cmd, "sdlkey:", 7)) { /* sdlkey:s:ctrl pushes a real key press, as a keyboard would */
        char name[32];
        sncpy(name, sizeof name, cmd + 7);
        char *mod = strchr(name, ':');
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = SDL_KEYDOWN;
        ev.key.state = SDL_PRESSED;
        if (mod) {
            if (!strcmp(mod + 1, "ctrl")) ev.key.keysym.mod = KMOD_LCTRL;
            *mod = 0;
        }
        ev.key.keysym.sym = SDL_GetKeyFromName(name);
        handle_event(&ev);
    }
    else if (!strncmp(cmd, "term:", 5)) term_script_text(cmd + 5);
    else if (!strncmp(cmd, "osk:", 4)) osk_open_for(A_FILE_MKDIR, "Name the new folder", cmd + 4, ""); /* osk:/some/dir */
    else if (!strcmp(cmd, "openterm")) term_open(NULL, "Terminal");
    else if (!strcmp(cmd, "appkb")) appkb_open();
    else if (!strncmp(cmd, "key:", 4)) { /* key:row,col highlights a key on whichever keyboard is up */
        int r = atoi(cmd + 4), c = strchr(cmd, ',') ? atoi(strchr(cmd, ',') + 1) : 0;
        if (dlg.kind == D_OSK) dlg.r = r, dlg.c = c;
        else if (ed_is_open()) ed.r = r, ed.c = c;
        else tm.r = r, tm.c = c;
    }
    else if (!strncmp(cmd, "pm:", 3)) { /* pm:1 sets the front window to keyboard + mouse, pm:0 to gamepad */
        Win *w = front_win();
        if (w) w->padmode = atoi(cmd + 3) ? PAD_MOUSE : PAD_GAMEPAD;
    } else if (!strncmp(cmd, "ev:", 3)) { /* ev:+a  ev:-a  ev:ax=0,20000: a controller event, as if it had come from the pad */
        static const struct { const char *n; int b; } bn[] = {
            { "a", SDL_CONTROLLER_BUTTON_A }, { "b", SDL_CONTROLLER_BUTTON_B }, { "x", SDL_CONTROLLER_BUTTON_X },
            { "y", SDL_CONTROLLER_BUTTON_Y }, { "back", SDL_CONTROLLER_BUTTON_BACK }, { "start", SDL_CONTROLLER_BUTTON_START },
            { "guide", SDL_CONTROLLER_BUTTON_GUIDE }, { "l1", SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
            { "r1", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER }, { "up", SDL_CONTROLLER_BUTTON_DPAD_UP },
            { "down", SDL_CONTROLLER_BUTTON_DPAD_DOWN }, { "left", SDL_CONTROLLER_BUTTON_DPAD_LEFT },
            { "right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
        };
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        if (!strncmp(cmd + 3, "ax=", 3)) {
            ev.type = SDL_CONTROLLERAXISMOTION;
            ev.caxis.axis = (Uint8)atoi(cmd + 6);
            const char *c = strchr(cmd + 6, ',');
            ev.caxis.value = (Sint16)(c ? atoi(c + 1) : 0);
            SDL_PushEvent(&ev);
        } else if (cmd[3] == '+' || cmd[3] == '-') {
            for (size_t i = 0; i < sizeof bn / sizeof *bn; i++)
                if (!strcmp(cmd + 4, bn[i].n)) {
                    ev.type = cmd[3] == '+' ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
                    ev.cbutton.button = (Uint8)bn[i].b;
                    ev.cbutton.state = cmd[3] == '+' ? SDL_PRESSED : SDL_RELEASED;
                    SDL_PushEvent(&ev);
                }
        }
    }
    else if (!strncmp(cmd, "cursor:", 7)) {
        bui.cx = (float)atoi(cmd + 7) * W / 1280.0f;
        const char *c = strchr(cmd + 7, ',');
        bui.cy = c ? (float)atoi(c + 1) * H / 720.0f : bui.cy;
        bui.cursor_init = 1;
    }
    else if (!strncmp(cmd, "gp:", 3)) { /* gp:+cross, gp:-cross, or gp:ax=2,20000 */
        static const char *names[GB_COUNT] = { "up", "down", "left", "right", "triangle", "circle", "cross", "square", "l1", "l2", "r1", "r2",
                                               "select", "start", "home", "voldn", "volup", "mute", "display" };
        if (cmd[3] == 'a' && cmd[4] == 'x') {
            int i = atoi(cmd + 6);
            const char *c = strchr(cmd + 6, ',');
            if (i >= 0 && i < 4 && c) stick_raw[i] = atoi(c + 1);
        } else {
            for (int i = 0; i < GB_COUNT; i++)
                if (!strcmp(cmd + 4, names[i])) gpt.sdl[i] = cmd[3] == '+';
        }
    }
    else if (!strncmp(cmd, "volhud:", 7)) { volhud.pct = atoi(cmd + 7); volhud.muted = volhud.pct < 0; if (volhud.pct < 0) volhud.pct = 0; volhud.until = now_s + 30; }
    else if (!strncmp(cmd, "theme:", 6)) sncpy(cfg.theme, sizeof cfg.theme, cmd + 6), theme_apply();
    else if (!strcmp(cmd, "quit")) running = 0;
    else fprintf(stderr, "pixi-shell: script: unknown step '%s'\n", cmd);
}

/* ======================================================================== */
/* main                                                                     */
/* ======================================================================== */

static void on_sigchld(int s) { (void)s; got_sigchld = 1; }
static void on_term(int s) { (void)s; got_term = 1; }

/* ---- crash log: ~/.cache/pixi/crash.log and shell.log ------------------------------------------ */

static int crash_fd = -1;

static void on_crash(int sig)
{
    if (crash_fd >= 0) {
        char head[96];
        int n = snprintf(head, sizeof head, "\n=== pixi-shell crashed: signal %d (%s) ===\n", sig, strsignal(sig));
        if (n > 0 && write(crash_fd, head, (size_t)n) < 0) {}
        void *frames[48];
        int cnt = backtrace(frames, 48);
        backtrace_symbols_fd(frames, cnt, crash_fd);
        fsync(crash_fd);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Keep what the shell prints (and VLC's own log, see video.c) where it can be read after a crash. */
static void crash_logging_start(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home) return;
    char dir[600], path[700], old[720];
    snprintf(dir, sizeof dir, "%s/.cache", home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof dir, "%s/.cache/pixi", home);
    mkdir(dir, 0755);
    snprintf(path, sizeof path, "%s/crash.log", dir);
    crash_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    snprintf(path, sizeof path, "%s/shell.log", dir);
    snprintf(old, sizeof old, "%s.1", path);
    rename(path, old);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    static const int sigs[] = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL };
    struct sigaction ca;
    memset(&ca, 0, sizeof ca);
    ca.sa_handler = on_crash;
    ca.sa_flags = (int)SA_RESETHAND;
    for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &ca, NULL);
    void *warm[2];
    backtrace(warm, 2); /* loads the unwinder now, not inside a crash */
}

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
    {
        const char *base = strrchr(argv[0], '/');
        base = base ? base + 1 : argv[0];
        if (!strcmp(base, "yad")) return yad_client(argc, argv); /* the stand-in for Pi-Apps */
    }
    if (argc >= 2 && !strcmp(argv[1], "--askpass")) return askpass_client(argc >= 3 ? argv[2] : ""); /* run by sudo */
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
    games_folders_ensure();
    if (debug_input) cfg.debug_input = 1;
    if (cfg.debug_input) setvbuf(stdout, NULL, _IOLBF, 0);
    theme_apply();
    find_datadir(argv[0]);

    if (!script) crash_logging_start();

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

    if (cfg.video_out && !videoout_running()) {
        /* bring USB video back up after a restart: in a child process, so a slow or stuck helper
         * can never keep the shell from reaching the screen */
        pid_t vpid = fork();
        if (vpid == 0) {
            char vm[400];
            videoout_apply(1, vm, sizeof vm);
            _exit(0);
        }
    }

    setenv("SDL_VIDEO_WAYLAND_WMCLASS", "pixi-shell", 1);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_VIDEO_EGL_ALLOW_TRANSPARENCY, "1");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint("SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR", "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0) die("SDL_Init: %s", SDL_GetError());
    if (TTF_Init() != 0) die("TTF_Init: %s", TTF_GetError());
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG | IMG_INIT_WEBP);
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
    atexit(bg_wake_all); /* paused apps must never be left stopped */
    gl_init();
    audio_init();
    if (audio_ok) Mix_Init(MIX_INIT_MP3 | MIX_INIT_OGG | MIX_INIT_FLAC | MIX_INIT_OPUS);
    pics_start();
    askpass_listen();
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
        bg_tick();
        shot_tick();
        SDL_Event e;
        if (hidden) { /* quicker while a stick is moving a pointer */
            if (SDL_WaitEventTimeout(&e, pm_active() ? 8 : 100)) handle_event(&e);
        }
        while (SDL_PollEvent(&e)) handle_event(&e);
        if (got_sigchld) {
            got_sigchld = 0;
            reap_children();
        }
        Uint32 now = SDL_GetTicks();
        repeat_tick(now);
        askpass_tick();
        evdev_poll();
        browser_tick();
        web_tick();
        store_icons_tick(now);
        video_tick();
        gptest_tick();
        term_tick();
        pm_tick();
        update_tick();
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
        guide.cpos += ((float)guide.card - guide.cpos) * (1.0f - expf(-14.0f * dt));
        if (launch.active && now_s - launch.started > 25) launch.active = 0;
        if (now_s >= status_next) {
            status_read(&status);
            status_next = now_s + 3;
        }
        if (frame_no % 600 == 0) text_cache_trim();
        media_tick(!hidden);
        if (!hidden && !viewer.open) thumbs_want(stack[depth]);
        if (hidden) continue;
        render_frame();
        if (guide.open) guide_send_previews();
    }
    if (ftp_running()) ftp_stop();
    web_stop();
    browser_quit();
    video_close();
    state_save();
    music_stop();
    pics_stop();
    Mix_CloseAudio();
    SDL_GL_DeleteContext(glctx);
    SDL_DestroyWindow(win);
    TTF_Quit();
    bg_wake_all();
    SDL_Quit();
    return 0;
}
