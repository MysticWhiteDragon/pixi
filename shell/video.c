/* The video engine: libvlc decodes, Pixi draws. See video.h.
 *
 * VLC hands over each picture as raw YUV (I420) through its "vmem" callbacks. The three planes are
 * uploaded as textures and a small shader turns them into color and scales them on the GPU, so the
 * CPU only decodes. None of VLC's own drawing code runs, which keeps it clear of the Pi's GL driver. */

#define _POSIX_C_SOURCE 200809L
#include "video.h"

#include <GLES3/gl3.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <vlc/vlc.h>

static libvlc_instance_t *V;
static libvlc_media_player_t *P;
static char cur_path[4096];
static int sub_scale_pct = 100;
static int loop_on, picture_mode, hw_on;

/* ---- what VLC's threads tell us (read on the main thread) ----------------------------------- */

static int ev_end, ev_error, ev_playing, ev_tracks, ev_size;

static void flag(int *f) { __atomic_store_n(f, 1, __ATOMIC_RELEASE); }
static int take(int *f) { return __atomic_exchange_n(f, 0, __ATOMIC_ACQ_REL); }

static void on_event(const struct libvlc_event_t *e, void *ctx)
{
    (void)ctx;
    switch (e->type) {
    case libvlc_MediaPlayerEndReached: flag(&ev_end); break;
    case libvlc_MediaPlayerEncounteredError: flag(&ev_error); break;
    case libvlc_MediaPlayerPlaying: flag(&ev_playing); break;
    case libvlc_MediaPlayerESAdded:
    case libvlc_MediaPlayerESDeleted:
    case libvlc_MediaPlayerESSelected: flag(&ev_tracks); break;
    case libvlc_MediaPlayerVout:
        flag(&ev_size);
        flag(&ev_tracks);
        break;
    default: break;
    }
}

/* ---- VLC's log: ~/.cache/pixi/vlc.log, and the last error for the "can't play" message -------- */

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file;
static char last_err[200];

static void on_log(void *data, int level, const libvlc_log_t *ctx, const char *fmt, va_list args)
{
    (void)data;
    if (level < (getenv("PIXI_VLC_DEBUG") ? LIBVLC_DEBUG : LIBVLC_WARNING)) return;
    const char *module = NULL, *file = NULL;
    unsigned ln = 0;
    libvlc_log_get_context(ctx, &module, &file, &ln);
    char msg[400];
    vsnprintf(msg, sizeof msg, fmt, args);
    pthread_mutex_lock(&log_lock);
    if (log_file) {
        fprintf(log_file, "[%s] %s: %s\n", level >= LIBVLC_ERROR ? "error" : "warning", module ? module : "vlc", msg);
        fflush(log_file);
    }
    if (level >= LIBVLC_ERROR) snprintf(last_err, sizeof last_err, "%s", msg);
    pthread_mutex_unlock(&log_lock);
}

static void open_log(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home || log_file) return;
    char p[700];
    snprintf(p, sizeof p, "%s/.cache", home);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/.cache/pixi", home);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/.cache/pixi/vlc.log", home);
    log_file = fopen(p, "w");
}

/* ---- the picture: VLC writes YUV planes, the main thread uploads them ----------------------- */

static pthread_mutex_t frame_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *plane[3];
static unsigned pitch[3], lines[3];
static unsigned fw, fh; /* the picture VLC is sending */
static int frame_new;

static unsigned align_up(unsigned v, unsigned a) { return (v + a - 1) / a * a; }

static unsigned on_format(void **opaque, char *chroma, unsigned *width, unsigned *height, unsigned *pitches, unsigned *lns)
{
    (void)opaque;
    memcpy(chroma, "I420", 4);
    unsigned w = *width, h = *height;
    unsigned cw = (w + 1) / 2, ch = (h + 1) / 2;
    pthread_mutex_lock(&frame_lock);
    pitches[0] = align_up(w, 32);
    pitches[1] = pitches[2] = align_up(cw, 32);
    lns[0] = align_up(h, 16);
    lns[1] = lns[2] = align_up(ch, 16);
    for (int i = 0; i < 3; i++) {
        free(plane[i]);
        pitch[i] = pitches[i];
        lines[i] = lns[i];
        plane[i] = malloc((size_t)pitch[i] * lines[i]);
        if (plane[i]) memset(plane[i], i ? 128 : 16, (size_t)pitch[i] * lines[i]);
    }
    fw = w, fh = h;
    pthread_mutex_unlock(&frame_lock);
    flag(&ev_size);
    return 1;
}

static void on_cleanup(void *opaque)
{
    (void)opaque;
    pthread_mutex_lock(&frame_lock);
    for (int i = 0; i < 3; i++) {
        free(plane[i]);
        plane[i] = NULL;
    }
    fw = fh = 0;
    pthread_mutex_unlock(&frame_lock);
}

static void *on_lock(void *opaque, void **planes)
{
    (void)opaque;
    pthread_mutex_lock(&frame_lock);
    for (int i = 0; i < 3; i++) planes[i] = plane[i];
    return NULL;
}

static void on_unlock(void *opaque, void *picture, void *const *planes)
{
    (void)opaque, (void)picture, (void)planes;
    pthread_mutex_unlock(&frame_lock);
}

static void on_display(void *opaque, void *picture)
{
    (void)opaque, (void)picture;
    flag(&frame_new);
}

/* ---- GPU side: three R8 textures and the shader that turns them into a picture -------------- */

static GLuint tex_yuv[3], prog, vao;
static unsigned tw, th; /* the size the textures were made for */
static int have_upload;
static GLint u_matrix;
static float sar = 1.0f; /* pixel shape of the video, from its track */

static const char *vs_src = "#version 300 es\n"
                            "out vec2 v_uv;\n"
                            "void main() {\n"
                            "  vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);\n"
                            "  v_uv = vec2((p.x + 1.0) * 0.5, (p.y + 1.0) * 0.5);\n"
                            "  gl_Position = vec4(p, 0.0, 1.0);\n"
                            "}\n";

/* limited-range YUV to RGB, then to linear light: the shell's targets are sRGB and encode on write */
static const char *fs_src = "#version 300 es\n"
                            "precision mediump float;\n"
                            "in vec2 v_uv;\n"
                            "uniform sampler2D u_y, u_u, u_v;\n"
                            "uniform mat3 u_matrix;\n"
                            "out vec4 o;\n"
                            "vec3 lin(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }\n"
                            "void main() {\n"
                            "  float y = (texture(u_y, v_uv).r - 0.0627451) * 1.164384;\n"
                            "  float u = (texture(u_u, v_uv).r - 0.5019608) * 1.138393;\n"
                            "  float v = (texture(u_v, v_uv).r - 0.5019608) * 1.138393;\n"
                            "  vec3 rgb = clamp(u_matrix * vec3(y, u, v), 0.0, 1.0);\n"
                            "  o = vec4(lin(rgb), 1.0);\n"
                            "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "video: shader failed: %s\n", log);
    }
    return s;
}

static int gl_setup(void)
{
    if (prog) return 0;
    GLuint v = compile(GL_VERTEX_SHADER, vs_src), f = compile(GL_FRAGMENT_SHADER, fs_src);
    prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(prog);
        prog = 0;
        return -1;
    }
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "u_y"), 0);
    glUniform1i(glGetUniformLocation(prog, "u_u"), 1);
    glUniform1i(glGetUniformLocation(prog, "u_v"), 2);
    u_matrix = glGetUniformLocation(prog, "u_matrix");
    glUseProgram(0);
    glGenVertexArrays(1, &vao);
    return 0;
}

static void gl_free(void)
{
    if (tex_yuv[0]) glDeleteTextures(3, tex_yuv);
    memset(tex_yuv, 0, sizeof tex_yuv);
    if (prog) glDeleteProgram(prog);
    if (vao) glDeleteVertexArrays(1, &vao);
    prog = vao = 0;
    tw = th = 0;
    have_upload = 0;
}

static void make_planes(unsigned w, unsigned h)
{
    if (tex_yuv[0]) glDeleteTextures(3, tex_yuv);
    glGenTextures(3, tex_yuv);
    for (int i = 0; i < 3; i++) {
        unsigned pw = i ? (w + 1) / 2 : w, ph = i ? (h + 1) / 2 : h;
        glBindTexture(GL_TEXTURE_2D, tex_yuv[i]);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, (GLsizei)pw, (GLsizei)ph);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    tw = w, th = h;
    have_upload = 0;
}

/* copies VLC's newest picture into the textures; 1 when there was one */
static int upload(void)
{
    int fresh = take(&frame_new);
    pthread_mutex_lock(&frame_lock);
    if (!fw || !fh || !plane[0]) {
        pthread_mutex_unlock(&frame_lock);
        return 0;
    }
    if (fw != tw || fh != th) make_planes(fw, fh);
    if (fresh) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        for (int i = 0; i < 3; i++) {
            unsigned pw = i ? (fw + 1) / 2 : fw, ph = i ? (fh + 1) / 2 : fh;
            glActiveTexture(GL_TEXTURE0 + (GLenum)i);
            glBindTexture(GL_TEXTURE_2D, tex_yuv[i]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, (GLint)pitch[i]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)pw, (GLsizei)ph, GL_RED, GL_UNSIGNED_BYTE, plane[i]);
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glActiveTexture(GL_TEXTURE0);
        have_upload = 1;
    }
    pthread_mutex_unlock(&frame_lock);
    return fresh;
}

/* ---- starting and stopping ------------------------------------------------------------------ */

static int loaded;
static int want_aid = -2, want_sid = -2; /* tracks to restore after a restart, -2 for none */
static int vid_w, vid_h;
static double seek_target = -1, seek_set_at;

static double now_clock(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int vd_running(void) { return P != NULL; }

/* VLC itself: an instance and a player. The subtitle size is fixed when the instance is made. */
static int engine_make(void)
{
    const char *args[48];
    char scale_arg[48];
    int n = 0;
    snprintf(scale_arg, sizeof scale_arg, "--sub-text-scale=%d", sub_scale_pct);
    args[n++] = scale_arg;
    args[n++] = "--no-video-title-show";
    args[n++] = "--no-osd";
    args[n++] = "--no-snapshot-preview";
    args[n++] = "--no-xlib";
    args[n++] = "--sub-autodetect-file"; /* subtitle files that sit beside the video */
    args[n++] = "--avcodec-threads=0";
    args[n++] = hw_on ? "--avcodec-hw=any" : "--avcodec-hw=none";
    /* an escape hatch for odd machines: PIXI_VLC_OPTS="--aout=alsa --alsa-audio-device=hw:1" */
    static char extra[600];
    const char *env = getenv("PIXI_VLC_OPTS");
    if (env && *env) {
        snprintf(extra, sizeof extra, "%s", env);
        char *save = NULL;
        for (char *tok = strtok_r(extra, " ", &save); tok && n < 46; tok = strtok_r(NULL, " ", &save)) args[n++] = tok;
    }
    V = libvlc_new(n, args);
    if (!V) return -2;
    libvlc_log_set(V, on_log, NULL);
    libvlc_set_user_agent(V, "Pixi", "Pixi");
    P = libvlc_media_player_new(V);
    if (!P) {
        libvlc_release(V);
        V = NULL;
        return -3;
    }
    libvlc_video_set_callbacks(P, on_lock, on_unlock, on_display, NULL);
    libvlc_video_set_format_callbacks(P, on_format, on_cleanup);
    libvlc_event_manager_t *em = libvlc_media_player_event_manager(P);
    static const libvlc_event_type_t evs[] = {
        libvlc_MediaPlayerEndReached, libvlc_MediaPlayerEncounteredError, libvlc_MediaPlayerPlaying, libvlc_MediaPlayerESAdded,
        libvlc_MediaPlayerESDeleted,  libvlc_MediaPlayerESSelected,       libvlc_MediaPlayerVout,
    };
    for (size_t i = 0; i < sizeof evs / sizeof *evs; i++) libvlc_event_attach(em, evs[i], on_event, NULL);
    return 0;
}

static void engine_free(void)
{
    if (P) {
        libvlc_media_player_stop(P);
        libvlc_media_player_release(P);
    }
    P = NULL;
    if (V) libvlc_release(V);
    V = NULL;
}

int vd_start(const char *hwdec)
{
    if (P) return 0;
    open_log();
    hw_on = hwdec && *hwdec && strcmp(hwdec, "no") != 0;
    sub_scale_pct = 100;
    int rc = engine_make();
    if (rc) return rc;
    if (gl_setup() != 0) {
        vd_stop();
        return -4;
    }
    loaded = 0;
    vid_w = vid_h = 0;
    loop_on = picture_mode = 0;
    return 0;
}

void vd_stop(void)
{
    engine_free();
    gl_free(); /* the GL context must still be current */
    on_cleanup(NULL);
    loaded = 0;
    take(&ev_end), take(&ev_error), take(&ev_playing), take(&ev_tracks), take(&ev_size), take(&frame_new);
}

static int start_media(double at)
{
    libvlc_media_t *m = libvlc_media_new_path(V, cur_path);
    if (!m) return -1;
    char o[64];
    libvlc_media_add_option(m, hw_on ? ":avcodec-hw=any" : ":avcodec-hw=none");
    if (at > 0.5) {
        snprintf(o, sizeof o, ":start-time=%.2f", at);
        libvlc_media_add_option(m, o);
    }
    libvlc_media_player_set_media(P, m);
    libvlc_media_release(m);
    return libvlc_media_player_play(P);
}

int vd_load(const char *path)
{
    if (!P) return -1;
    snprintf(cur_path, sizeof cur_path, "%s", path);
    take(&ev_end), take(&ev_error), take(&ev_playing);
    pthread_mutex_lock(&log_lock);
    last_err[0] = 0;
    pthread_mutex_unlock(&log_lock);
    loaded = 0;
    want_aid = want_sid = -2;
    vid_w = vid_h = 0;
    have_upload = 0;
    seek_target = -1;
    sar = 1.0f;
    return start_media(0);
}

/* ---- reading VLC's answers ------------------------------------------------------------------- */

static void codec_name(uint32_t fourcc, char *out, size_t cap)
{
    static const struct {
        const char *f, *n;
    } t[] = {
        { "mp4a", "aac" },   { "a52 ", "ac3" },  { "eac3", "eac3" }, { "mpga", "mp3" },  { "mp3 ", "mp3" },   { "dts ", "dts" },
        { "vorb", "vorbis" }, { "Opus", "opus" }, { "flac", "flac" }, { "araw", "pcm" },  { "h264", "h264" },  { "hevc", "hevc" },
        { "VP80", "vp8" },   { "VP90", "vp9" },  { "av01", "av1" },  { "mp4v", "mpeg4" }, { "mpgv", "mpeg2" }, { "subt", "text" },
        { "ssa ", "ass" },   { "tx3g", "mov_text" }, { "spu ", "dvd" }, { "pgs ", "pgs" }, { "WVC1", "vc1" },
    };
    char f[5];
    memcpy(f, &fourcc, 4);
    f[4] = 0;
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (!memcmp(f, t[i].f, 4)) {
            snprintf(out, cap, "%s", t[i].n);
            return;
        }
    for (int i = 3; i >= 0 && (f[i] == ' ' || !f[i]); i--) f[i] = 0;
    snprintf(out, cap, "%s", f);
}

/* VLC numbers tracks from 0; Pixi keeps 0 for "none", so ids are shifted by one */
static void read_tracks(VdState *st)
{
    st->naud = st->nsub = 0;
    st->aid = st->sid = 0;
    libvlc_media_track_t **mt = NULL;
    unsigned nmt = 0;
    libvlc_media_t *m = libvlc_media_player_get_media(P);
    if (m) nmt = libvlc_media_tracks_get(m, &mt);
    int cur_a = libvlc_audio_get_track(P), cur_s = libvlc_video_get_spu(P);
    for (int kind = 0; kind < 2; kind++) {
        libvlc_track_description_t *list = kind == 0 ? libvlc_audio_get_track_description(P) : libvlc_video_get_spu_description(P);
        for (libvlc_track_description_t *d = list; d; d = d->p_next) {
            if (d->i_id < 0) continue; /* "Disable" */
            VdTrack *t;
            if (kind == 0 && st->naud < VD_MAX_TRACKS) t = &st->aud[st->naud++];
            else if (kind == 1 && st->nsub < VD_MAX_TRACKS) t = &st->sub[st->nsub++];
            else continue;
            memset(t, 0, sizeof *t);
            t->id = d->i_id + 1;
            t->selected = d->i_id == (kind == 0 ? cur_a : cur_s);
            int found = 0;
            for (unsigned i = 0; i < nmt; i++) {
                if (mt[i]->i_id != d->i_id) continue;
                found = 1;
                if (mt[i]->psz_language) snprintf(t->lang, sizeof t->lang, "%s", mt[i]->psz_language);
                if (mt[i]->psz_description) snprintf(t->title, sizeof t->title, "%s", mt[i]->psz_description);
                codec_name(mt[i]->i_codec, t->codec, sizeof t->codec);
                if (mt[i]->i_type == libvlc_track_audio && mt[i]->audio) t->channels = (int)mt[i]->audio->i_channels;
            }
            /* a subtitle file found beside the video isn't in the media's own list */
            if (!found && d->psz_name) snprintf(t->title, sizeof t->title, "%s", d->psz_name);
            if (t->selected) {
                if (kind == 0) st->aid = t->id;
                else st->sid = t->id;
            }
        }
        if (list) libvlc_track_description_list_release(list);
    }
    if (mt) libvlc_media_tracks_release(mt, nmt);
    if (m) libvlc_media_release(m);
}

static void read_size(void)
{
    unsigned w = 0, h = 0;
    if (libvlc_video_get_size(P, 0, &w, &h) == 0 && w && h) vid_w = (int)w, vid_h = (int)h;
    sar = 1.0f;
    libvlc_media_t *m = libvlc_media_player_get_media(P);
    if (!m) return;
    libvlc_media_track_t **mt = NULL;
    unsigned n = libvlc_media_tracks_get(m, &mt);
    for (unsigned i = 0; i < n; i++)
        if (mt[i]->i_type == libvlc_track_video && mt[i]->video && mt[i]->video->i_sar_num && mt[i]->video->i_sar_den) {
            sar = (float)mt[i]->video->i_sar_num / (float)mt[i]->video->i_sar_den;
            break;
        }
    if (mt) libvlc_media_tracks_release(mt, n);
    libvlc_media_release(m);
}

void vd_poll(VdState *st)
{
    st->running = P != NULL;
    if (!P) {
        st->loaded = 0;
        return;
    }
    st->ended = st->failed = 0;
    if (take(&ev_playing)) {
        loaded = 1;
        if (want_aid != -2) libvlc_audio_set_track(P, want_aid);
        if (want_sid != -2) libvlc_video_set_spu(P, want_sid);
        want_aid = want_sid = -2;
        flag(&ev_tracks);
        flag(&ev_size);
    }
    if (take(&ev_error)) {
        loaded = 0;
        st->failed = 1;
        pthread_mutex_lock(&log_lock);
        snprintf(st->error, sizeof st->error, "%s", last_err[0] ? last_err : "VLC couldn't play this file.");
        pthread_mutex_unlock(&log_lock);
    }
    if (take(&ev_end)) {
        if (loop_on) { /* repeat: open it again from the top (VLC can't be called from its own events) */
            start_media(0);
        } else {
            loaded = 0;
            st->ended = 1;
        }
    }
    if (take(&ev_tracks)) read_tracks(st);
    if (take(&ev_size)) read_size();
    st->loaded = loaded;
    st->w = vid_w, st->h = vid_h;
    if (!loaded) {
        st->pos = st->dur = 0;
        st->paused = st->buffering = st->seeking = 0;
        return;
    }
    libvlc_time_t t = libvlc_media_player_get_time(P), len = libvlc_media_player_get_length(P);
    st->pos = t > 0 ? (double)t / 1000.0 : 0;
    st->dur = len > 0 ? (double)len / 1000.0 : 0;
    if (seek_target >= 0 && now_clock() - seek_set_at < 0.6) st->pos = seek_target; /* until VLC catches up */
    libvlc_state_t s = libvlc_media_player_get_state(P);
    st->paused = s == libvlc_Paused;
    st->buffering = s == libvlc_Opening || s == libvlc_Buffering || (!have_upload && vid_w > 0);
    st->seeking = 0;
}

int vd_render(unsigned fbo, int w, int h, int force)
{
    if (!P || !prog) return 0;
    int fresh = upload();
    if (!have_upload || (!fresh && !force)) return 0;

    /* where the picture goes: keep its shape unless told otherwise */
    float aspect = th ? (float)tw * sar / (float)th : 16.0f / 9.0f;
    if (picture_mode == 1) aspect = 16.0f / 9.0f;
    else if (picture_mode == 2) aspect = 4.0f / 3.0f;
    float rw = (float)w, rh = (float)h;
    if (picture_mode != 4) {
        float screen = (float)w / (float)h;
        int fill = picture_mode == 3;
        if ((aspect > screen) != fill) rh = (float)w / aspect;
        else rw = (float)h * aspect;
    }
    GLint old_vp[4];
    glGetIntegerv(GL_VIEWPORT, old_vp);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glViewport((GLint)(((float)w - rw) / 2), (GLint)(((float)h - rh) / 2), (GLsizei)(rw + 0.5f), (GLsizei)(rh + 0.5f));
    glUseProgram(prog);
    /* columns of the YUV to RGB matrix: BT.709 for HD, BT.601 below it */
    static const float m709[9] = { 1, 1, 1, 0, -0.187324f, 1.8556f, 1.5748f, -0.468124f, 0 };
    static const float m601[9] = { 1, 1, 1, 0, -0.344136f, 1.772f, 1.402f, -0.714136f, 0 };
    glUniformMatrix3fv(u_matrix, 1, GL_FALSE, th >= 600 ? m709 : m601);
    for (int i = 0; i < 3; i++) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        glBindTexture(GL_TEXTURE_2D, tex_yuv[i]);
    }
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    for (int i = 2; i >= 0; i--) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)i);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    glViewport(old_vp[0], old_vp[1], old_vp[2], old_vp[3]);
    return 1;
}

/* ---- commands -------------------------------------------------------------------------------- */

void vd_pause(int on)
{
    if (P) libvlc_media_player_set_pause(P, on);
}

void vd_seek(double seconds, int absolute, int exact)
{
    (void)exact;
    if (!P || !loaded) return;
    double base = seek_target >= 0 && now_clock() - seek_set_at < 0.6 ? seek_target : (double)libvlc_media_player_get_time(P) / 1000.0;
    double t = absolute ? seconds : base + seconds;
    double len = (double)libvlc_media_player_get_length(P) / 1000.0;
    if (len > 0 && t > len - 1) t = len - 1;
    if (t < 0) t = 0;
    libvlc_media_player_set_time(P, (libvlc_time_t)(t * 1000.0));
    seek_target = t;
    seek_set_at = now_clock();
}

void vd_set_aid(int id)
{
    if (P) libvlc_audio_set_track(P, id > 0 ? id - 1 : -1);
    flag(&ev_tracks);
}

void vd_set_sid(int id)
{
    if (P) libvlc_video_set_spu(P, id > 0 ? id - 1 : -1);
    flag(&ev_tracks);
}

void vd_set_speed(double speed)
{
    if (P) libvlc_media_player_set_rate(P, (float)speed);
}

void vd_set_loop(int on) { loop_on = on; }

/* VLC only reads the subtitle size when it starts, so it starts again and carries on at the same
 * spot, with the same tracks, speed and pause */
void vd_set_sub_scale(double scale)
{
    int pct = (int)(scale * 100.0 + 0.5);
    if (pct == sub_scale_pct || !P) return;
    sub_scale_pct = pct;
    if (!loaded) {
        engine_free();
        engine_make();
        return;
    }
    double at = (double)libvlc_media_player_get_time(P) / 1000.0;
    int paused = libvlc_media_player_get_state(P) == libvlc_Paused;
    float rate = libvlc_media_player_get_rate(P);
    int a = libvlc_audio_get_track(P), sp = libvlc_video_get_spu(P);
    engine_free();
    if (engine_make() != 0) {
        loaded = 0;
        flag(&ev_error);
        return;
    }
    take(&ev_end), take(&ev_error), take(&ev_playing);
    start_media(at);
    libvlc_media_player_set_rate(P, rate);
    if (paused) libvlc_media_player_set_pause(P, 1);
    want_aid = a, want_sid = sp; /* tracks exist only once it plays: set in vd_poll */
}

void vd_set_picture(int mode) { picture_mode = mode; }

/* ---- video info ------------------------------------------------------------------------------ */

static void line(char *out, size_t cap, const char *label, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void line(char *out, size_t cap, const char *label, const char *fmt, ...)
{
    size_t n = strlen(out);
    if (n + 4 >= cap) return;
    char val[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(val, sizeof val, fmt, ap);
    va_end(ap);
    snprintf(out + n, cap - n, "%s%s: %s", n ? "\n" : "", label, val);
}

static void clock_text(char *out, size_t cap, double s)
{
    long t = (long)(s + 0.5);
    if (t >= 3600) snprintf(out, cap, "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    else snprintf(out, cap, "%ld:%02ld", t / 60, t % 60);
}

void vd_info(char *out, size_t cap)
{
    out[0] = 0;
    if (!P || !loaded) {
        snprintf(out, cap, "Nothing is playing.");
        return;
    }
    const char *dot = strrchr(cur_path, '.');
    if (dot && dot[1]) {
        char ext[16];
        snprintf(ext, sizeof ext, "%s", dot + 1);
        for (char *p = ext; *p; p++)
            if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        line(out, cap, "Container", "%s", ext);
    }
    struct stat sb;
    if (stat(cur_path, &sb) == 0 && sb.st_size > 0) line(out, cap, "Size", "%.1f MB", (double)sb.st_size / 1048576.0);
    libvlc_time_t len = libvlc_media_player_get_length(P);
    char tm[32];
    if (len > 0) {
        clock_text(tm, sizeof tm, (double)len / 1000.0);
        line(out, cap, "Length", "%s", tm);
    }
    libvlc_media_t *m = libvlc_media_player_get_media(P);
    libvlc_media_track_t **mt = NULL;
    unsigned n = m ? libvlc_media_tracks_get(m, &mt) : 0;
    int video = 0, audio = 0, cur_a = libvlc_audio_get_track(P);
    char c[32];
    for (unsigned i = 0; i < n; i++) {
        libvlc_media_track_t *t = mt[i];
        if (t->i_type == libvlc_track_video && t->video && !video) {
            video = 1;
            codec_name(t->i_codec, c, sizeof c);
            line(out, cap, "Video", "%s", c);
            if (t->video->i_width && t->video->i_height) line(out, cap, "Picture", "%u x %u", t->video->i_width, t->video->i_height);
            if (t->video->i_frame_rate_num && t->video->i_frame_rate_den)
                line(out, cap, "Frame rate", "%.3g fps", (double)t->video->i_frame_rate_num / t->video->i_frame_rate_den);
            if (t->i_bitrate) line(out, cap, "Video bitrate", "%u kb/s", t->i_bitrate / 1000);
            line(out, cap, "Decoder", "VLC, %s", hw_on ? "hardware when possible" : "software");
        } else if (t->i_type == libvlc_track_audio && t->audio && t->i_id == cur_a && !audio) {
            audio = 1;
            codec_name(t->i_codec, c, sizeof c);
            line(out, cap, "Audio", "%s", c);
            unsigned ch = t->audio->i_channels;
            line(out, cap, "Sound", "%u Hz, %s", t->audio->i_rate,
                 ch == 1 ? "mono" : ch == 2 ? "stereo" : ch == 6 ? "5.1" : ch == 8 ? "7.1" : "multichannel");
            if (t->i_bitrate) line(out, cap, "Audio bitrate", "%u kb/s", t->i_bitrate / 1000);
        }
    }
    if (!video) line(out, cap, "Video", "none");
    libvlc_media_stats_t stats;
    if (m && libvlc_media_get_stats(m, &stats)) line(out, cap, "Dropped frames", "%d", stats.i_lost_pictures);
    if (mt) libvlc_media_tracks_release(mt, n);
    if (m) libvlc_media_release(m);
}
