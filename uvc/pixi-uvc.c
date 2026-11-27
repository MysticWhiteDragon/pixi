/*
 * pixi-uvc: shows the Pixi screen to a PC as a USB webcam.
 *
 * The Pi's USB port is set up as a UVC gadget (by pixi-videoout, which runs as root). This program
 * is the other half: it answers the PC's UVC requests, grabs the screen from the compositor with
 * wlr-screencopy, turns each frame into a JPEG and hands it to the USB gadget as a Motion-JPEG frame.
 *
 * Nothing is captured until a PC actually opens the camera, so it costs nothing while idle.
 *
 *   pixi-uvc [--size WxH] [--fps N] [--quality N] [--maxpacket N] [--device /dev/videoN]
 *   pixi-uvc --test-dir DIR [--frames N]    grab N screenshots as JPEGs (no USB needed)
 *   pixi-uvc --selftest                     check the UVC request handling
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <jpeglib.h>
#include <linux/usb/ch9.h>
#include <linux/usb/g_uvc.h>
#include <linux/videodev2.h>
#include <wayland-client.h>

#include "wlr-screencopy-unstable-v1-client-protocol.h"

#define NBUF 4
#define STREAM_INTERFACE_MIN 1 /* interface 0 is the (empty) camera control interface */

static int out_w = 800, out_h = 480;
static int fps = 30;
static int quality = 70;
static int maxpacket = 1024;
static volatile sig_atomic_t quit_flag;

static void on_signal(int s) { (void)s; quit_flag = 1; }

static long now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* counters for the stats line in the log */
static long st_started_us, st_cap_n, st_cap_rt_us, st_enc_us, st_sent_n, st_req_us, st_bytes;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ======================================================================== */
/* JPEG                                                                      */
/* ======================================================================== */

static unsigned char *latest;     /* newest frame, as a JPEG */
static size_t latest_len;
static unsigned long frames_made;

#define FMT_XRGB8888 0x34325258u /* B G R x in memory */
#define FMT_XBGR8888 0x34324258u /* R G B x in memory */
#define FMT_ARGB8888 0x34325241u
#define FMT_ABGR8888 0x34324241u

static int fmt_is_bgr(uint32_t f) { return f == 0 || f == 1 || f == FMT_XRGB8888 || f == FMT_ARGB8888; }
static int fmt_is_rgb(uint32_t f) { return f == FMT_XBGR8888 || f == FMT_ABGR8888; }

/* scale (nearest) a 32-bit screen buffer to out_w x out_h and compress it. Returns 0 and replaces
   `latest` on success. */
static int encode_frame(const uint8_t *src, uint32_t bw, uint32_t bh, uint32_t stride, uint32_t fmt, int y_invert)
{
    if (!bw || !bh || (!fmt_is_bgr(fmt) && !fmt_is_rgb(fmt))) return -1;
    int swap = fmt_is_rgb(fmt); /* 1: bytes are already R,G,B */

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    unsigned char *buf = NULL;
    unsigned long size = 0;
    jpeg_mem_dest(&cinfo, &buf, &size);
    cinfo.image_width = (JDIMENSION)out_w;
    cinfo.image_height = (JDIMENSION)out_h;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);
    cinfo.dct_method = JDCT_IFAST;
    cinfo.optimize_coding = FALSE;
    jpeg_start_compress(&cinfo, TRUE);

    uint32_t *xmap = malloc(sizeof(uint32_t) * (size_t)out_w);
    uint8_t *row = malloc((size_t)out_w * 3);
    if (!xmap || !row) {
        free(xmap);
        free(row);
        jpeg_destroy_compress(&cinfo);
        free(buf);
        return -1;
    }
    for (int x = 0; x < out_w; x++) xmap[x] = (uint32_t)((uint64_t)x * bw / (uint64_t)out_w);

    for (int y = 0; y < out_h; y++) {
        uint32_t sy = (uint32_t)((uint64_t)y * bh / (uint64_t)out_h);
        if (y_invert) sy = bh - 1 - sy;
        const uint8_t *p = src + (size_t)sy * stride;
        uint8_t *d = row;
        if (swap) {
            for (int x = 0; x < out_w; x++, d += 3) {
                const uint8_t *s = p + (size_t)xmap[x] * 4;
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
            }
        } else {
            for (int x = 0; x < out_w; x++, d += 3) {
                const uint8_t *s = p + (size_t)xmap[x] * 4;
                d[0] = s[2];
                d[1] = s[1];
                d[2] = s[0];
            }
        }
        JSAMPROW rp = row;
        jpeg_write_scanlines(&cinfo, &rp, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    free(xmap);
    free(row);

    unsigned char *copy = realloc(latest, size);
    if (!copy) {
        free(buf);
        return -1;
    }
    memcpy(copy, buf, size);
    latest = copy;
    latest_len = size;
    st_bytes += (long)size;
    free(buf);
    frames_made++;
    return 0;
}

/* something to send before the first real frame exists */
static void make_black_frame(void)
{
    uint8_t *z = calloc((size_t)out_w * (size_t)out_h, 4);
    if (!z) return;
    encode_frame(z, (uint32_t)out_w, (uint32_t)out_h, (uint32_t)out_w * 4, FMT_XRGB8888, 0);
    frames_made = 0;
    free(z);
}

/* ======================================================================== */
/* screen capture: wlr-screencopy                                            */
/* ======================================================================== */

static struct wl_display *dpy;
static struct wl_shm *shm;
static struct wl_output *output;
static struct zwlr_screencopy_manager_v1 *scmgr;

static struct zwlr_screencopy_frame_v1 *frame;
static int inflight;
static long next_capture_ms;

static struct wl_buffer *wlbuf;
static void *wldata;
static size_t wlsize;
static uint32_t buf_fmt, buf_w, buf_h, buf_stride, cur_flags;
static int have_buf_info;

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, "wl_shm")) shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, "wl_output") && !output) output = wl_registry_bind(r, name, &wl_output_interface, 1);
    else if (!strcmp(iface, "zwlr_screencopy_manager_v1"))
        scmgr = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, ver < 3 ? ver : 3);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

static void release_wlbuf(void)
{
    if (wlbuf) wl_buffer_destroy(wlbuf);
    if (wldata) munmap(wldata, wlsize);
    wlbuf = NULL;
    wldata = NULL;
}

static void frame_end(void)
{
    if (frame) zwlr_screencopy_frame_v1_destroy(frame);
    frame = NULL;
    inflight = 0;
    have_buf_info = 0;
}

static void start_copy(void)
{
    size_t need = (size_t)buf_stride * buf_h;
    if (!wlbuf || wlsize != need) {
        release_wlbuf();
        int fd = memfd_create("pixi-uvc", MFD_CLOEXEC);
        if (fd < 0 || ftruncate(fd, (off_t)need) != 0) {
            if (fd >= 0) close(fd);
            fprintf(stderr, "pixi-uvc: couldn't allocate a capture buffer\n");
            frame_end();
            return;
        }
        wldata = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (wldata == MAP_FAILED) {
            wldata = NULL;
            close(fd);
            frame_end();
            return;
        }
        wlsize = need;
        struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)need);
        wlbuf = wl_shm_pool_create_buffer(pool, 0, (int32_t)buf_w, (int32_t)buf_h, (int32_t)buf_stride, buf_fmt);
        wl_shm_pool_destroy(pool);
        close(fd);
    }
    zwlr_screencopy_frame_v1_copy(frame, wlbuf);
}

static void fr_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride)
{
    (void)d; (void)f;
    buf_fmt = fmt;
    buf_w = w;
    buf_h = h;
    buf_stride = stride;
    have_buf_info = 1;
    /* version 3 compositors send buffer_done once every buffer type has been announced */
    if (zwlr_screencopy_frame_v1_get_version(frame) < 3) start_copy();
}
static void fr_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) { (void)d; (void)f; cur_flags = flags; }
static void fr_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t hi, uint32_t lo, uint32_t ns)
{
    (void)d; (void)f; (void)hi; (void)lo; (void)ns;
    if (!frames_made) fprintf(stderr, "pixi-uvc: capturing the screen (%ux%u, format 0x%x)\n", buf_w, buf_h, buf_fmt);
    long t0 = now_us();
    st_cap_n++;
    st_cap_rt_us += t0 - st_req_us;
    if (encode_frame(wldata, buf_w, buf_h, buf_stride, buf_fmt, (cur_flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT) != 0) != 0)
        fprintf(stderr, "pixi-uvc: unsupported screen format 0x%x\n", buf_fmt);
    st_enc_us += now_us() - t0;
    frame_end();
}
static void fr_failed(void *d, struct zwlr_screencopy_frame_v1 *f)
{
    (void)d; (void)f;
    static int logged;
    if (logged++ < 5) fprintf(stderr, "pixi-uvc: the compositor couldn't capture the screen\n");
    frame_end();
    next_capture_ms = now_ms() + 250;
}
static void fr_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    (void)d; (void)f; (void)x; (void)y; (void)w; (void)h;
}
static void fr_linux_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h)
{
    (void)d; (void)f; (void)fmt; (void)w; (void)h;
}
static void fr_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f)
{
    (void)d; (void)f;
    if (have_buf_info) start_copy();
}
static const struct zwlr_screencopy_frame_v1_listener fr_listener = {
    fr_buffer, fr_flags, fr_ready, fr_failed, fr_damage, fr_linux_dmabuf, fr_buffer_done,
};

static void request_capture(void)
{
    frame = zwlr_screencopy_manager_v1_capture_output(scmgr, 1, output); /* 1: include the pointer */
    zwlr_screencopy_frame_v1_add_listener(frame, &fr_listener, NULL);
    inflight = 1;
    st_req_us = now_us();
    next_capture_ms = now_ms() + 1000 / (fps > 0 ? fps : 30);
}

static int wayland_connect(void)
{
    dpy = wl_display_connect(NULL);
    if (!dpy) {
        fprintf(stderr, "pixi-uvc: can't connect to the compositor (WAYLAND_DISPLAY=%s)\n", getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "");
        return -1;
    }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!shm || !output || !scmgr) {
        fprintf(stderr, "pixi-uvc: the compositor doesn't offer screen capture\n");
        return -1;
    }
    return 0;
}

/* ======================================================================== */
/* UVC request handling                                                      */
/* ======================================================================== */

#define UVC_SET_CUR 0x01
#define UVC_GET_CUR 0x81
#define UVC_GET_MIN 0x82
#define UVC_GET_MAX 0x83
#define UVC_GET_RES 0x84
#define UVC_GET_LEN 0x85
#define UVC_GET_INFO 0x86
#define UVC_GET_DEF 0x87
#define UVC_VS_PROBE_CONTROL 0x01
#define UVC_VS_COMMIT_CONTROL 0x02

/* the 34-byte streaming control block of UVC 1.1 (written out here so it doesn't depend on the
   kernel header's spelling of its fields) */
struct uvc_streaming_control {
    uint16_t bmHint;
    uint8_t bFormatIndex, bFrameIndex;
    uint32_t dwFrameInterval;
    uint16_t wKeyFrameRate, wPFrameRate, wCompQuality, wCompWindowSize, wDelay;
    uint32_t dwMaxVideoFrameSize, dwMaxPayloadTransferSize, dwClockFrequency;
    uint8_t bmFramingInfo, bPreferredVersion, bMinVersion, bMaxVersion;
} __attribute__((packed));

static struct uvc_streaming_control probe_ctrl, commit_ctrl;
static int pending_set_cur; /* which control the data stage that's about to arrive belongs to */

static uint32_t interval_for_fps(int f) { return f >= 30 ? 333333u : f >= 15 ? 666666u : 1000000u; }
static int fps_for_interval(uint32_t iv) { return iv <= 400000 ? 30 : iv <= 800000 ? 15 : 10; }

static void fill_ctrl(struct uvc_streaming_control *c, uint32_t interval)
{
    memset(c, 0, sizeof *c);
    c->bmHint = 1; /* the frame interval is the field that's fixed */
    c->bFormatIndex = 1;
    c->bFrameIndex = 1;
    c->dwFrameInterval = interval;
    c->dwMaxVideoFrameSize = (uint32_t)(out_w * out_h * 2);
    c->dwMaxPayloadTransferSize = (uint32_t)maxpacket;
    c->bmFramingInfo = 3;
    c->bPreferredVersion = 1;
    c->bMinVersion = 1;
    c->bMaxVersion = 1;
}

/* The PC asked something. Fill resp. Length < 0 stalls the request; other requests (standard ones,
   camera controls we don't have) are refused. */
static void uvc_setup(const struct usb_ctrlrequest *r, struct uvc_request_data *resp)
{
    memset(resp, 0, sizeof *resp);
    resp->length = -EL2HLT;
    if ((r->bRequestType & USB_TYPE_MASK) != USB_TYPE_CLASS) return;
    if ((r->bRequestType & USB_RECIP_MASK) != USB_RECIP_INTERFACE) return;
    if ((r->wIndex & 0xff) < STREAM_INTERFACE_MIN) return;
    int cs = r->wValue >> 8;
    if (cs != UVC_VS_PROBE_CONTROL && cs != UVC_VS_COMMIT_CONTROL) return;

    struct uvc_streaming_control tmp;
    switch (r->bRequest) {
    case UVC_SET_CUR:
        pending_set_cur = cs;
        resp->length = (int)sizeof(struct uvc_streaming_control);
        break;
    case UVC_GET_CUR:
        memcpy(resp->data, cs == UVC_VS_PROBE_CONTROL ? &probe_ctrl : &commit_ctrl, sizeof probe_ctrl);
        resp->length = (int)sizeof probe_ctrl;
        break;
    case UVC_GET_MIN:
        fill_ctrl(&tmp, 1000000u);
        memcpy(resp->data, &tmp, sizeof tmp);
        resp->length = (int)sizeof tmp;
        break;
    case UVC_GET_MAX:
    case UVC_GET_DEF:
        fill_ctrl(&tmp, r->bRequest == UVC_GET_MAX ? 333333u : interval_for_fps(fps));
        memcpy(resp->data, &tmp, sizeof tmp);
        resp->length = (int)sizeof tmp;
        break;
    case UVC_GET_RES:
        resp->length = (int)sizeof(struct uvc_streaming_control); /* zeros */
        break;
    case UVC_GET_LEN:
        resp->data[0] = (uint8_t)sizeof(struct uvc_streaming_control);
        resp->data[1] = 0;
        resp->length = 2;
        break;
    case UVC_GET_INFO:
        resp->data[0] = 3; /* the control can be read and written */
        resp->length = 1;
        break;
    default:
        break;
    }
}

/* the data stage of a SET_CUR: the PC says which format, frame size and rate it wants.
   Returns 1 when it was the final "commit" of the settings. */
static int uvc_data(const struct uvc_request_data *d)
{
    struct uvc_streaming_control want;
    memset(&want, 0, sizeof want);
    size_t n = d->length > 0 ? (size_t)d->length : 0;
    if (n > sizeof want) n = sizeof want;
    memcpy(&want, d->data, n);
    /* there's one format and one frame size, so the only thing to accept is the frame rate */
    uint32_t iv = want.dwFrameInterval ? want.dwFrameInterval : interval_for_fps(fps);
    struct uvc_streaming_control *target = pending_set_cur == UVC_VS_COMMIT_CONTROL ? &commit_ctrl : &probe_ctrl;
    fill_ctrl(target, interval_for_fps(fps_for_interval(iv)));
    if (pending_set_cur == UVC_VS_COMMIT_CONTROL) {
        fps = fps_for_interval(iv);
        return 1;
    }
    return 0;
}

static int selftest(void)
{
    int bad = 0;
    struct usb_ctrlrequest r;
    struct uvc_request_data resp;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "selftest failed: %s (line %d)\n", #c, __LINE__); bad++; } } while (0)
    fill_ctrl(&probe_ctrl, interval_for_fps(fps));
    fill_ctrl(&commit_ctrl, interval_for_fps(fps));

    memset(&r, 0, sizeof r);
    r.bRequestType = USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    r.bRequest = UVC_GET_CUR;
    r.wValue = UVC_VS_PROBE_CONTROL << 8;
    r.wIndex = 1;
    uvc_setup(&r, &resp);
    CHECK(resp.length == 34);
    struct uvc_streaming_control *c = (struct uvc_streaming_control *)resp.data;
    CHECK(c->bFormatIndex == 1 && c->bFrameIndex == 1);
    CHECK(c->dwFrameInterval == 333333);
    CHECK(c->dwMaxVideoFrameSize == (uint32_t)(out_w * out_h * 2));
    CHECK(sizeof(struct uvc_streaming_control) == 34);

    r.bRequest = UVC_GET_LEN;
    uvc_setup(&r, &resp);
    CHECK(resp.length == 2 && resp.data[0] == 34);
    r.bRequest = UVC_GET_INFO;
    uvc_setup(&r, &resp);
    CHECK(resp.length == 1 && resp.data[0] == 3);

    /* the camera control interface and standard requests are refused */
    r.wIndex = 0;
    uvc_setup(&r, &resp);
    CHECK(resp.length < 0);
    r.wIndex = 1;
    r.bRequestType = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE;
    uvc_setup(&r, &resp);
    CHECK(resp.length < 0);

    /* the PC asks for 15 fps: probe, then commit */
    r.bRequestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    r.bRequest = UVC_SET_CUR;
    r.wValue = UVC_VS_PROBE_CONTROL << 8;
    uvc_setup(&r, &resp);
    CHECK(resp.length == 34 && pending_set_cur == UVC_VS_PROBE_CONTROL);
    struct uvc_request_data data;
    memset(&data, 0, sizeof data);
    struct uvc_streaming_control ask;
    fill_ctrl(&ask, 666666);
    memcpy(data.data, &ask, sizeof ask);
    data.length = 34;
    CHECK(uvc_data(&data) == 0);
    CHECK(probe_ctrl.dwFrameInterval == 666666);
    r.wValue = UVC_VS_COMMIT_CONTROL << 8;
    uvc_setup(&r, &resp);
    CHECK(uvc_data(&data) == 1);
    CHECK(fps == 15);
    /* a short (UVC 1.0, 26 byte) request still works */
    data.length = 26;
    r.wValue = UVC_VS_PROBE_CONTROL << 8;
    uvc_setup(&r, &resp);
    CHECK(uvc_data(&data) == 0);

    /* colour handling and scaling: one red, one blue pixel (as XRGB: B G R x) */
    uint8_t px[2 * 4] = { 0, 0, 255, 0, 255, 0, 0, 0 };
    int ow = out_w, oh = out_h;
    out_w = 16;
    out_h = 16;
    CHECK(encode_frame(px, 2, 1, 8, FMT_XRGB8888, 0) == 0);
    CHECK(latest_len > 100 && latest[0] == 0xff && latest[1] == 0xd8);
    out_w = ow;
    out_h = oh;
#undef CHECK
    if (!bad) puts("selftest ok");
    return bad ? 1 : 0;
}

/* ======================================================================== */
/* the USB gadget's video device                                             */
/* ======================================================================== */

static int vfd = -1;
static void *vmap[NBUF];
static size_t vmaplen[NBUF];
static int streaming;
static long next_send_ms;

static int find_device(const char *want)
{
    if (want) return open(want, O_RDWR | O_NONBLOCK);
    for (int i = 0; i < 32; i++) {
        char p[32];
        snprintf(p, sizeof p, "/dev/video%d", i);
        int fd = open(p, O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        struct v4l2_capability cap;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 && !strcmp((char *)cap.driver, "g_uvc")) return fd;
        close(fd);
    }
    return -1;
}

static void subscribe(void)
{
    static const unsigned evs[] = { UVC_EVENT_CONNECT, UVC_EVENT_DISCONNECT, UVC_EVENT_SETUP, UVC_EVENT_DATA, UVC_EVENT_STREAMON, UVC_EVENT_STREAMOFF };
    for (unsigned i = 0; i < sizeof evs / sizeof *evs; i++) {
        struct v4l2_event_subscription s;
        memset(&s, 0, sizeof s);
        s.type = evs[i];
        ioctl(vfd, VIDIOC_SUBSCRIBE_EVENT, &s);
    }
}

static int set_format(void)
{
    struct v4l2_format f;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    f.fmt.pix.width = (uint32_t)out_w;
    f.fmt.pix.height = (uint32_t)out_h;
    f.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    f.fmt.pix.field = V4L2_FIELD_NONE;
    f.fmt.pix.sizeimage = (uint32_t)(out_w * out_h * 2);
    return ioctl(vfd, VIDIOC_S_FMT, &f) < 0 ? -1 : 0;
}

static int fill_buffer(int idx)
{
    struct v4l2_buffer b;
    memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = (uint32_t)idx;
    size_t n = latest_len < vmaplen[idx] ? latest_len : vmaplen[idx];
    memcpy(vmap[idx], latest, n);
    b.bytesused = (uint32_t)n;
    return ioctl(vfd, VIDIOC_QBUF, &b) < 0 ? -1 : 0;
}

static void stream_stop(void)
{
    if (vfd < 0 || !streaming) return;
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(vfd, VIDIOC_STREAMOFF, &t);
    for (int i = 0; i < NBUF; i++)
        if (vmap[i]) {
            munmap(vmap[i], vmaplen[i]);
            vmap[i] = NULL;
        }
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    rb.memory = V4L2_MEMORY_MMAP;
    ioctl(vfd, VIDIOC_REQBUFS, &rb);
    streaming = 0;
    fprintf(stderr, "pixi-uvc: stream stopped\n");
    if (frame) frame_end(); /* stop grabbing */
}

static int stream_start(void)
{
    if (streaming) stream_stop();
    if (!latest) make_black_frame();
    if (set_format() != 0) fprintf(stderr, "pixi-uvc: S_FMT failed: %s\n", strerror(errno));
    struct v4l2_requestbuffers rb;
    memset(&rb, 0, sizeof rb);
    rb.count = NBUF;
    rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    rb.memory = V4L2_MEMORY_MMAP;
    int rr = ioctl(vfd, VIDIOC_REQBUFS, &rb);
    if (rr < 0 || rb.count < 2) {
        if (rr < 0) fprintf(stderr, "pixi-uvc: couldn't get video buffers: %s\n", strerror(errno));
        else fprintf(stderr, "pixi-uvc: the USB video driver only gave %u buffers\n", rb.count);
        return -1;
    }
    for (unsigned i = 0; i < rb.count && i < NBUF; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof b);
        b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (ioctl(vfd, VIDIOC_QUERYBUF, &b) < 0) return -1;
        vmaplen[i] = b.length;
        vmap[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, vfd, b.m.offset);
        if (vmap[i] == MAP_FAILED) {
            vmap[i] = NULL;
            return -1;
        }
        if (fill_buffer((int)i) != 0) return -1;
    }
    enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(vfd, VIDIOC_STREAMON, &t) < 0) {
        fprintf(stderr, "pixi-uvc: STREAMON failed: %s\n", strerror(errno));
        return -1;
    }
    streaming = 1;
    st_started_us = now_us();
    st_cap_n = st_cap_rt_us = st_enc_us = st_sent_n = st_bytes = 0;
    next_send_ms = now_ms() + 1000 / fps;
    next_capture_ms = 0;
    fprintf(stderr, "pixi-uvc: streaming %dx%d at %d fps\n", out_w, out_h, fps);
    return 0;
}

/* a finished buffer comes back: put the newest picture in it and send it again */
static void service_buffers(void)
{
    struct v4l2_buffer b;
    memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    b.memory = V4L2_MEMORY_MMAP;
    if (ioctl(vfd, VIDIOC_DQBUF, &b) < 0) return;
    fill_buffer((int)b.index);
    st_sent_n++;
    next_send_ms = now_ms() + 1000 / fps;
}

/* every few seconds while streaming: how fast the screen grab and the USB side really run */
static void print_stats(void)
{
    long t = now_us();
    if (!st_started_us) st_started_us = t;
    if (t - st_started_us < 5000000) return;
    double secs = (double)(t - st_started_us) / 1e6;
    fprintf(stderr, "pixi-uvc: stats: grabbed %.1f/s (grab %.0f ms, encode %.1f ms, %ld KB each), sent to the PC %.1f/s, PC asked for %d fps\n",
            (double)st_cap_n / secs, st_cap_n ? (double)st_cap_rt_us / (double)st_cap_n / 1000.0 : 0.0,
            st_cap_n ? (double)st_enc_us / (double)st_cap_n / 1000.0 : 0.0, st_cap_n ? st_bytes / st_cap_n / 1024 : 0L,
            (double)st_sent_n / secs, fps);
    st_started_us = t;
    st_cap_n = st_cap_rt_us = st_enc_us = st_sent_n = st_bytes = 0;
}

static void handle_events(void)
{
    for (;;) {
        struct v4l2_event ev;
        if (ioctl(vfd, VIDIOC_DQEVENT, &ev) < 0) return;
        struct uvc_event *ue = (struct uvc_event *)(void *)&ev.u.data[0];
        switch (ev.type) {
        case UVC_EVENT_SETUP: {
            struct uvc_request_data resp;
            uvc_setup(&ue->req, &resp);
            ioctl(vfd, UVCIOC_SEND_RESPONSE, &resp);
            break;
        }
        case UVC_EVENT_DATA:
            if (uvc_data(&ue->data)) set_format();
            break;
        case UVC_EVENT_STREAMON:
            if (stream_start() != 0) stream_stop();
            break;
        case UVC_EVENT_STREAMOFF:
        case UVC_EVENT_DISCONNECT:
            stream_stop();
            break;
        default:
            break;
        }
    }
}

/* ======================================================================== */
/* main                                                                      */
/* ======================================================================== */

static int pump_wayland(int timeout_ms, struct pollfd *extra, int nextra)
{
    struct pollfd pf[2];
    pf[0].fd = wl_display_get_fd(dpy);
    pf[0].events = POLLIN;
    pf[0].revents = 0;
    for (int i = 0; i < nextra; i++) pf[1 + i] = extra[i];
    while (wl_display_prepare_read(dpy) != 0) wl_display_dispatch_pending(dpy);
    if (wl_display_flush(dpy) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(dpy);
        return -1;
    }
    int r = poll(pf, (nfds_t)(1 + nextra), timeout_ms);
    if (r > 0 && (pf[0].revents & POLLIN)) {
        if (wl_display_read_events(dpy) < 0) return -1;
    } else {
        wl_display_cancel_read(dpy);
    }
    if (r > 0 && (pf[0].revents & (POLLERR | POLLHUP))) return -1;
    if (wl_display_dispatch_pending(dpy) < 0) return -1;
    for (int i = 0; i < nextra; i++) extra[i] = pf[1 + i];
    return r;
}

static void usage(void)
{
    puts("usage: pixi-uvc [--size WxH] [--fps N] [--quality N] [--maxpacket N] [--device /dev/videoN]\n"
         "       pixi-uvc --test-dir DIR [--frames N]\n"
         "       pixi-uvc --selftest");
}

int main(int argc, char **argv)
{
    const char *device = NULL, *test_dir = NULL;
    int test_frames = 3, do_selftest = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            if (sscanf(argv[++i], "%dx%d", &out_w, &out_h) != 2 || out_w < 64 || out_h < 64 || out_w > 4096 || out_h > 4096) {
                fprintf(stderr, "pixi-uvc: bad --size\n");
                return 2;
            }
        } else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--quality") && i + 1 < argc) quality = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--maxpacket") && i + 1 < argc) maxpacket = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) device = argv[++i];
        else if (!strcmp(argv[i], "--test-dir") && i + 1 < argc) test_dir = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) test_frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--selftest")) do_selftest = 1;
        else {
            usage();
            return 2;
        }
    }
    if (fps != 30 && fps != 15 && fps != 10) fps = 30;
    if (quality < 10 || quality > 95) quality = 70;
    if (maxpacket < 128 || maxpacket > 3072) maxpacket = 1024;

    if (do_selftest) return selftest();

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    if (wayland_connect() != 0) return 1;

    if (test_dir) {
        long made = 0;
        while (!quit_flag && made < test_frames) {
            if (!inflight && now_ms() >= next_capture_ms) request_capture();
            unsigned long before = frames_made;
            if (pump_wayland(100, NULL, 0) < 0) return 1;
            if (frames_made != before) {
                char p[1024];
                snprintf(p, sizeof p, "%s/frame-%ld.jpg", test_dir, ++made);
                FILE *f = fopen(p, "wb");
                if (f) {
                    fwrite(latest, 1, latest_len, f);
                    fclose(f);
                }
            }
        }
        return 0;
    }

    /* the video device appears a moment after the gadget is bound */
    for (int tries = 0; tries < 50 && !quit_flag; tries++) {
        vfd = find_device(device);
        if (vfd >= 0) break;
        usleep(100000);
    }
    if (vfd < 0) {
        fprintf(stderr, "pixi-uvc: no USB video gadget found\n");
        return 1;
    }
    subscribe();
    fill_ctrl(&probe_ctrl, interval_for_fps(fps));
    fill_ctrl(&commit_ctrl, interval_for_fps(fps));
    fprintf(stderr, "pixi-uvc: ready (%dx%d, %d fps, waiting for a PC to open the camera)\n", out_w, out_h, fps);

    while (!quit_flag) {
        long now = now_ms();
        int timeout = 1000;
        struct pollfd pv = { vfd, POLLPRI, 0 };
        if (streaming) {
            if (now >= next_send_ms) pv.events |= POLLOUT;
            else timeout = (int)(next_send_ms - now);
            if (!inflight && scmgr) {
                if (now >= next_capture_ms) request_capture();
                else if (next_capture_ms - now < timeout) timeout = (int)(next_capture_ms - now);
            }
        }
        if (pump_wayland(timeout, &pv, 1) < 0) {
            fprintf(stderr, "pixi-uvc: lost the compositor\n");
            break;
        }
        if (pv.revents & POLLPRI) handle_events();
        if (streaming && (pv.revents & POLLOUT)) service_buffers();
        if (streaming) print_stats();
    }
    stream_stop();
    return 0;
}
