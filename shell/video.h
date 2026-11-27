/* The video engine: libvlc decoding into a texture the shell draws.
 * Pixi draws every control itself; this file only plays, seeks and reports. */
#ifndef PIXI_VIDEO_H
#define PIXI_VIDEO_H

#include <stddef.h>

#define VD_MAX_TRACKS 24

typedef struct {
    int id, selected, channels;
    char lang[24], title[120], codec[32];
} VdTrack;

typedef struct {
    int running;  /* the player exists */
    int loaded;   /* a file is open and playing or paused */
    int ended;    /* the file played to its end (cleared by vd_poll once read) */
    int failed;   /* the file couldn't be played */
    char error[200];
    int paused, buffering, seeking;
    double pos, dur;
    int w, h;     /* size of the picture, 0 for audio only */
    int naud, nsub;
    VdTrack aud[VD_MAX_TRACKS], sub[VD_MAX_TRACKS];
    int aid, sid; /* the chosen tracks; 0 means none */
} VdState;

/* Call with the GL context current. hwdec "no" decodes in software; anything else lets VLC use the
 * hardware decoder where it can. Returns 0 on success. */
int vd_start(const char *hwdec);
void vd_stop(void);
int vd_running(void);

int vd_load(const char *path);
void vd_poll(VdState *st);                           /* once a frame: events, position, tracks */
int vd_render(unsigned fbo, int w, int h, int force); /* draws a new frame if there is one; 1 when it did */

void vd_pause(int on);
void vd_seek(double seconds, int absolute, int exact);
void vd_set_aid(int id);
void vd_set_sid(int id);                  /* 0 turns subtitles off */
void vd_set_speed(double speed);
void vd_set_loop(int on);
void vd_set_sub_scale(double scale);
void vd_set_picture(int mode);            /* 0 original, 1 16:9, 2 4:3, 3 zoom to fill, 4 stretch */
void vd_info(char *out, size_t cap);

#endif
