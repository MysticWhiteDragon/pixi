/*
 * pixi - the Pixi ("Pi Cross Interface") Wayland compositor.
 *
 * Built on wlroots 0.18. It runs the Pixi shell as a privileged client and
 * shows every application full screen, one in front at a time, the way a
 * console does. The shell decides what is on screen through the private
 * pixi_shell_v1 protocol: itself alone (home), itself over the front
 * application (overlay, the guide), or the application alone (app).
 *
 * Structure follows wlroots' tinywl example.
 *
 * Copyright (C) 2026 Pixi contributors.
 * SPDX-License-Identifier: GPL-3.0-only
 */

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/xwayland.h>
#include <wlr/xwayland/shell.h>
#include <xkbcommon/xkbcommon.h>

#include "pixi-shell-v1-protocol.h"

#define PIXI_VERSION "1.0.0"

enum pixi_mode { MODE_HOME = 0, MODE_OVERLAY = 1, MODE_APP = 2 };
enum view_kind { VIEW_XDG, VIEW_X11 };

struct server;

struct view {
    struct wl_list link;            /* server.views, front first */
    struct server *server;
    enum view_kind kind;
    struct wlr_xdg_toplevel *xdg;
    struct wlr_xwayland_surface *xsurf;
    struct wlr_scene_tree *tree;
    struct wlr_xdg_toplevel_decoration_v1 *deco;
    uint32_t id;
    bool mapped, is_shell, in_list, override_redirect;

    struct wl_listener map, unmap, commit, destroy;
    struct wl_listener set_title, set_app_id, request_fullscreen, request_maximize;
    struct wl_listener associate, dissociate, request_configure, request_activate, set_class;
};

struct output {
    struct wl_list link;
    struct server *server;
    struct wlr_output *wlr_output;
    struct wlr_scene_output *scene_output;
    struct wl_listener frame, request_state, destroy;
};

struct keyboard {
    struct wl_list link;
    struct server *server;
    struct wlr_keyboard *wlr_keyboard;
    struct wl_listener modifiers, key, destroy;
};

struct server {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wlr_backend *backend;
    struct wlr_session *session;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_compositor *compositor;
    struct wlr_scene *scene;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_output_layout *output_layout;
    struct wl_list outputs;
    struct wl_listener new_output;

    struct wlr_scene_rect *backdrop;
    struct wlr_scene_tree *apps_tree, *shell_tree, *popup_tree;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_xdg_toplevel, new_xdg_popup;
    struct wlr_xdg_decoration_manager_v1 *deco_mgr;
    struct wl_listener new_decoration;
    struct wlr_xwayland *xwayland;
    struct wl_listener xwayland_ready, new_xwayland_surface;

    struct wl_list views;
    struct view *shell_view;
    uint32_t next_id;
    enum pixi_mode mode;

    struct wlr_seat *seat;
    struct wl_list keyboards;
    struct wl_listener new_input, request_cursor, request_set_selection;
    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion, cursor_motion_absolute, cursor_button, cursor_axis, cursor_frame;
    bool cursor_visible;
    bool super_alone;

    struct wl_global *shell_global;
    struct wl_list shell_resources;
    char *shell_cmd;
    pid_t shell_pid;
    int shell_restarts;
    struct wl_event_source *sigchld, *sigterm, *sigint, *respawn_timer;
    bool insecure, quitting;
    enum wl_output_transform transform;
    const char *socket;
};

/* ======================================================================== */
/* Views                                                                    */
/* ======================================================================== */

static struct wlr_surface *view_surface(struct view *v)
{
    if (v->kind == VIEW_XDG) return v->xdg->base->surface;
    return v->xsurf ? v->xsurf->surface : NULL;
}

static pid_t view_pid(struct view *v)
{
    if (v->kind == VIEW_X11) return v->xsurf ? v->xsurf->pid : 0;
    pid_t pid = 0;
    wl_client_get_credentials(v->xdg->base->client->client, &pid, NULL, NULL);
    return pid;
}

static const char *view_app_id(struct view *v)
{
    const char *s = v->kind == VIEW_XDG ? v->xdg->app_id : (v->xsurf ? v->xsurf->class : NULL);
    return s ? s : "";
}

static const char *view_title(struct view *v)
{
    const char *s = v->kind == VIEW_XDG ? v->xdg->title : (v->xsurf ? v->xsurf->title : NULL);
    return s ? s : "";
}

static struct output *primary_output(struct server *s)
{
    if (wl_list_empty(&s->outputs)) return NULL;
    struct output *o = wl_container_of(s->outputs.next, o, link);
    return o;
}

static void display_size(struct server *s, int *w, int *h)
{
    struct output *o = primary_output(s);
    if (!o) {
        *w = 1280;
        *h = 720;
        return;
    }
    wlr_output_effective_resolution(o->wlr_output, w, h);
}

static bool view_is_child(struct view *v)
{
    if (v->kind == VIEW_XDG) return v->xdg->parent != NULL;
    return v->xsurf && v->xsurf->parent != NULL;
}

/* Move a window's scene node; X11 windows have none until they are shown. */
static void view_place(struct view *v, int x, int y)
{
    if (v->tree) wlr_scene_node_set_position(&v->tree->node, x, y);
}

/* Fill the display, unless this is a dialog of another window. */
static void view_fit(struct view *v)
{
    int w, h;
    display_size(v->server, &w, &h);
    if (v->kind == VIEW_XDG) {
        if (!v->xdg->base->initialized) return;
        if (v->is_shell || !view_is_child(v)) {
            wlr_xdg_toplevel_set_size(v->xdg, w, h);
            wlr_xdg_toplevel_set_fullscreen(v->xdg, true);
            view_place(v, 0, 0);
        } else {
            struct wlr_box geo;
            wlr_xdg_surface_get_geometry(v->xdg->base, &geo);
            view_place(v, (w - geo.width) / 2, (h - geo.height) / 2);
        }
    } else if (v->xsurf) {
        if (v->override_redirect) {
            view_place(v, v->xsurf->x, v->xsurf->y);
        } else if (!view_is_child(v)) {
            wlr_xwayland_surface_configure(v->xsurf, 0, 0, (uint16_t)w, (uint16_t)h);
            wlr_xwayland_surface_set_fullscreen(v->xsurf, true);
            view_place(v, 0, 0);
        } else {
            int cw = v->xsurf->width, ch = v->xsurf->height;
            int x = (w - cw) / 2, y = (h - ch) / 2;
            wlr_xwayland_surface_configure(v->xsurf, (int16_t)x, (int16_t)y, (uint16_t)cw, (uint16_t)ch);
            view_place(v, x, y);
        }
    }
}

static void view_set_activated(struct view *v, bool on)
{
    if (v->kind == VIEW_XDG) {
        if (v->xdg->base->initialized) wlr_xdg_toplevel_set_activated(v->xdg, on);
    } else if (v->xsurf && !v->override_redirect) {
        wlr_xwayland_surface_activate(v->xsurf, on);
    }
}

static void view_close(struct view *v)
{
    if (v->kind == VIEW_XDG) wlr_xdg_toplevel_send_close(v->xdg);
    else if (v->xsurf) wlr_xwayland_surface_close(v->xsurf);
}

static struct view *front_app(struct server *s)
{
    struct view *v;
    wl_list_for_each(v, &s->views, link) {
        if (v->mapped && !v->is_shell && !v->override_redirect) return v;
    }
    return NULL;
}

static struct view *view_by_id(struct server *s, uint32_t id)
{
    struct view *v;
    wl_list_for_each(v, &s->views, link) {
        if (v->id == id) return v;
    }
    return NULL;
}

/* ======================================================================== */
/* The shell protocol                                                       */
/* ======================================================================== */

#define each_shell_resource(s, r) wl_resource_for_each(r, &(s)->shell_resources)

static void shell_send_window(struct server *s, struct view *v)
{
    struct wl_resource *r;
    each_shell_resource(s, r) pixi_shell_v1_send_window(r, v->id, view_app_id(v), view_title(v), (int32_t)view_pid(v));
}

static void shell_send_closed(struct server *s, uint32_t id)
{
    struct wl_resource *r;
    each_shell_resource(s, r) pixi_shell_v1_send_window_closed(r, id);
}

static void shell_send_state(struct server *s)
{
    struct view *f = front_app(s);
    struct wl_resource *r;
    each_shell_resource(s, r) {
        pixi_shell_v1_send_front(r, f ? f->id : 0);
        pixi_shell_v1_send_mode(r, s->mode);
    }
}

static void shell_send_guide(struct server *s)
{
    struct wl_resource *r;
    each_shell_resource(s, r) pixi_shell_v1_send_guide(r);
}

static void focus_surface(struct server *s, struct wlr_surface *surface)
{
    struct wlr_seat *seat = s->seat;
    if (!surface) {
        wlr_seat_keyboard_notify_clear_focus(seat);
        return;
    }
    if (seat->keyboard_state.focused_surface == surface) return;
    struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
    if (kb) wlr_seat_keyboard_notify_enter(seat, surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
    else wlr_seat_keyboard_notify_enter(seat, surface, NULL, 0, NULL);
}

/* Put the scene in order for the current mode and give focus to whoever owns it. */
static void apply_mode(struct server *s)
{
    struct view *app = front_app(s);
    if (!app && s->mode != MODE_HOME) s->mode = MODE_HOME;

    bool shell_shown = s->mode != MODE_APP;
    bool apps_shown = s->mode != MODE_HOME;
    wlr_scene_node_set_enabled(&s->shell_tree->node, shell_shown);
    wlr_scene_node_set_enabled(&s->apps_tree->node, apps_shown);
    wlr_scene_node_raise_to_top(&s->shell_tree->node);
    wlr_scene_node_raise_to_top(&s->popup_tree->node);

    struct view *v;
    wl_list_for_each(v, &s->views, link) {
        if (!v->is_shell && !v->override_redirect) view_set_activated(v, s->mode == MODE_APP && v == app);
    }
    if (s->shell_view) view_set_activated(s->shell_view, shell_shown);

    if (s->mode == MODE_APP && app) focus_surface(s, view_surface(app));
    else if (shell_shown && s->shell_view && s->shell_view->mapped) focus_surface(s, view_surface(s->shell_view));
    else focus_surface(s, NULL);
    shell_send_state(s);
}

static void set_mode(struct server *s, enum pixi_mode mode)
{
    s->mode = mode;
    apply_mode(s);
}

static void raise_view(struct server *s, struct view *v)
{
    wl_list_remove(&v->link);
    wl_list_insert(&s->views, &v->link);
    if (v->tree) wlr_scene_node_raise_to_top(&v->tree->node);
}

static void shell_handle_set_mode(struct wl_client *c, struct wl_resource *r, uint32_t mode)
{
    struct server *s = wl_resource_get_user_data(r);
    if (mode > MODE_APP) return;
    set_mode(s, (enum pixi_mode)mode);
}

static void shell_handle_activate(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct server *s = wl_resource_get_user_data(r);
    struct view *v = view_by_id(s, id);
    if (!v || v->is_shell || !v->mapped) return;
    raise_view(s, v);
    set_mode(s, MODE_APP);
}

static void shell_handle_close(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct server *s = wl_resource_get_user_data(r);
    struct view *v = view_by_id(s, id);
    if (v && !v->is_shell) view_close(v);
}

static void shell_handle_kill(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct server *s = wl_resource_get_user_data(r);
    struct view *v = view_by_id(s, id);
    if (!v || v->is_shell) return;
    pid_t pid = view_pid(v);
    if (pid > 1 && pid != getpid() && pid != s->shell_pid) kill(pid, SIGKILL);
    else view_close(v);
}

static void shell_handle_quit(struct wl_client *c, struct wl_resource *r)
{
    struct server *s = wl_resource_get_user_data(r);
    s->quitting = true;
    wl_display_terminate(s->display);
}

static const struct pixi_shell_v1_interface shell_impl = {
    .set_mode = shell_handle_set_mode,
    .activate = shell_handle_activate,
    .close = shell_handle_close,
    .kill = shell_handle_kill,
    .quit = shell_handle_quit,
};

static void shell_resource_destroy(struct wl_resource *r)
{
    wl_list_remove(wl_resource_get_link(r));
}

static void shell_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    struct server *s = data;
    struct wl_resource *r = wl_resource_create(client, &pixi_shell_v1_interface, (int)version, id);
    if (!r) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(r, &shell_impl, s, shell_resource_destroy);
    wl_list_insert(&s->shell_resources, wl_resource_get_link(r));
    /* Bring the new shell up to date. */
    struct view *v;
    wl_list_for_each_reverse(v, &s->views, link) {
        if (v->mapped && !v->is_shell && !v->override_redirect) pixi_shell_v1_send_window(r, v->id, view_app_id(v), view_title(v), (int32_t)view_pid(v));
    }
    struct view *f = front_app(s);
    pixi_shell_v1_send_front(r, f ? f->id : 0);
    pixi_shell_v1_send_mode(r, s->mode);
}

static bool global_filter(const struct wl_client *client, const struct wl_global *global, void *data)
{
    struct server *s = data;
    /* wlroots asks compositors to show the xwayland_shell global to Xwayland alone. */
    if (s->xwayland && s->xwayland->shell_v1 && global == s->xwayland->shell_v1->global)
        return s->xwayland->server && client == s->xwayland->server->client;
    if (global != s->shell_global) return true;
    if (s->insecure) return true;
    pid_t pid = 0;
    wl_client_get_credentials((struct wl_client *)client, &pid, NULL, NULL);
    return s->shell_pid > 0 && pid == s->shell_pid;
}

static bool client_is_shell(struct server *s, struct wl_client *client)
{
    pid_t pid = 0;
    wl_client_get_credentials(client, &pid, NULL, NULL);
    return (s->shell_pid > 0 && pid == s->shell_pid) || (s->insecure && !s->shell_view);
}

/* ======================================================================== */
/* Mapping and unmapping windows                                            */
/* ======================================================================== */

static void view_mapped(struct view *v)
{
    struct server *s = v->server;
    v->mapped = true;
    if (v->is_shell) {
        s->shell_view = v;
        view_fit(v);
        apply_mode(s);
        return;
    }
    if (!v->in_list) {
        wl_list_insert(&s->views, &v->link);
        v->in_list = true;
    }
    if (v->override_redirect) {
        view_fit(v);
        wlr_scene_node_raise_to_top(&v->tree->node);
        return;
    }
    v->id = ++s->next_id;
    view_fit(v);
    raise_view(s, v);
    shell_send_window(s, v);
    /* A new application comes straight to the front, as on a console. */
    set_mode(s, MODE_APP);
}

static void view_unmapped(struct view *v)
{
    struct server *s = v->server;
    if (!v->mapped) return;
    v->mapped = false;
    if (v->is_shell) {
        if (s->shell_view == v) s->shell_view = NULL;
        apply_mode(s);
        return;
    }
    if (v->in_list) {
        wl_list_remove(&v->link);
        v->in_list = false;
    }
    if (v->override_redirect) return;
    if (v->id) shell_send_closed(s, v->id);
    v->id = 0;
    /* Back to the next application, or home when there is none. */
    if (s->mode == MODE_APP && !front_app(s)) s->mode = MODE_HOME;
    apply_mode(s);
}

/* ---------- xdg-shell --------------------------------------------------- */

static void xdg_map(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, map);
    view_mapped(v);
}

static void xdg_unmap(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, unmap);
    view_unmapped(v);
}

static void xdg_commit(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, commit);
    if (v->xdg->base->initial_commit) {
        if (v->deco) wlr_xdg_toplevel_decoration_v1_set_mode(v->deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        int w, h;
        display_size(v->server, &w, &h);
        if (v->is_shell || !view_is_child(v)) {
            wlr_xdg_toplevel_set_size(v->xdg, w, h);
            wlr_xdg_toplevel_set_fullscreen(v->xdg, true);
        } else {
            wlr_xdg_toplevel_set_size(v->xdg, 0, 0);
        }
    } else if (v->mapped && view_is_child(v) && !v->is_shell) {
        view_fit(v); /* keep dialogs centred as they resize */
    }
}

static void xdg_title(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, set_title);
    if (v->mapped && !v->is_shell && v->id) shell_send_window(v->server, v);
}

static void xdg_app_id(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, set_app_id);
    if (v->mapped && !v->is_shell && v->id) shell_send_window(v->server, v);
}

static void xdg_request_state(struct wl_listener *l, void *data)
{
    /* Everything is full screen already; just acknowledge with a configure. */
    struct view *v = wl_container_of(l, v, request_fullscreen);
    if (v->xdg->base->initialized) wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void xdg_request_maximize(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, request_maximize);
    if (v->xdg->base->initialized) wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void xdg_destroy(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, destroy);
    if (v->mapped) view_unmapped(v);
    if (v->server->shell_view == v) v->server->shell_view = NULL;
    wl_list_remove(&v->map.link);
    wl_list_remove(&v->unmap.link);
    wl_list_remove(&v->commit.link);
    wl_list_remove(&v->destroy.link);
    wl_list_remove(&v->set_title.link);
    wl_list_remove(&v->set_app_id.link);
    wl_list_remove(&v->request_fullscreen.link);
    wl_list_remove(&v->request_maximize.link);
    free(v);
}

static void new_xdg_toplevel(struct wl_listener *l, void *data)
{
    struct server *s = wl_container_of(l, s, new_xdg_toplevel);
    struct wlr_xdg_toplevel *toplevel = data;
    struct view *v = calloc(1, sizeof *v);
    if (!v) return;
    v->server = s;
    v->kind = VIEW_XDG;
    v->xdg = toplevel;
    v->is_shell = client_is_shell(s, toplevel->base->client->client) && !s->shell_view;
    v->tree = wlr_scene_xdg_surface_create(v->is_shell ? s->shell_tree : s->apps_tree, toplevel->base);
    v->tree->node.data = v;
    toplevel->base->data = v->tree;

    v->map.notify = xdg_map;
    wl_signal_add(&toplevel->base->surface->events.map, &v->map);
    v->unmap.notify = xdg_unmap;
    wl_signal_add(&toplevel->base->surface->events.unmap, &v->unmap);
    v->commit.notify = xdg_commit;
    wl_signal_add(&toplevel->base->surface->events.commit, &v->commit);
    v->destroy.notify = xdg_destroy;
    wl_signal_add(&toplevel->events.destroy, &v->destroy);
    v->set_title.notify = xdg_title;
    wl_signal_add(&toplevel->events.set_title, &v->set_title);
    v->set_app_id.notify = xdg_app_id;
    wl_signal_add(&toplevel->events.set_app_id, &v->set_app_id);
    v->request_fullscreen.notify = xdg_request_state;
    wl_signal_add(&toplevel->events.request_fullscreen, &v->request_fullscreen);
    v->request_maximize.notify = xdg_request_maximize;
    wl_signal_add(&toplevel->events.request_maximize, &v->request_maximize);
}

struct popup {
    struct wlr_xdg_popup *xdg;
    struct wl_listener commit, destroy;
};

static void popup_commit(struct wl_listener *l, void *data)
{
    struct popup *p = wl_container_of(l, p, commit);
    if (p->xdg->base->initial_commit) wlr_xdg_surface_schedule_configure(p->xdg->base);
}

static void popup_destroy(struct wl_listener *l, void *data)
{
    struct popup *p = wl_container_of(l, p, destroy);
    wl_list_remove(&p->commit.link);
    wl_list_remove(&p->destroy.link);
    free(p);
}

static void new_xdg_popup(struct wl_listener *l, void *data)
{
    struct wlr_xdg_popup *xdg_popup = data;
    struct popup *p = calloc(1, sizeof *p);
    if (!p) return;
    p->xdg = xdg_popup;
    struct wlr_xdg_surface *parent = xdg_popup->parent ? wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent) : NULL;
    if (parent && parent->data) {
        struct wlr_scene_tree *parent_tree = parent->data;
        xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
    }
    p->commit.notify = popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &p->commit);
    p->destroy.notify = popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &p->destroy);
}

/* Applications are full screen and have no frames; tell them not to draw any. */
static void new_decoration(struct wl_listener *l, void *data)
{
    struct wlr_xdg_toplevel_decoration_v1 *deco = data;
    struct wlr_scene_tree *tree = deco->toplevel->base->data;
    if (tree && tree->node.data) {
        struct view *v = tree->node.data;
        v->deco = deco;
    }
    if (deco->toplevel->base->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(deco, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

/* ---------- XWayland ---------------------------------------------------- */

static void x11_map(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, map);
    if (!v->tree) {
        v->tree = wlr_scene_tree_create(v->override_redirect ? v->server->popup_tree : v->server->apps_tree);
        v->tree->node.data = v;
        wlr_scene_subsurface_tree_create(v->tree, v->xsurf->surface);
    }
    view_mapped(v);
}

static void x11_unmap(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, unmap);
    view_unmapped(v);
    if (v->tree) {
        wlr_scene_node_destroy(&v->tree->node);
        v->tree = NULL;
    }
}

static void x11_associate(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, associate);
    v->map.notify = x11_map;
    wl_signal_add(&v->xsurf->surface->events.map, &v->map);
    v->unmap.notify = x11_unmap;
    wl_signal_add(&v->xsurf->surface->events.unmap, &v->unmap);
}

static void x11_dissociate(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, dissociate);
    wl_list_remove(&v->map.link);
    wl_list_remove(&v->unmap.link);
    wl_list_init(&v->map.link);
    wl_list_init(&v->unmap.link);
}

static void x11_request_configure(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, request_configure);
    struct wlr_xwayland_surface_configure_event *ev = data;
    if (v->override_redirect || view_is_child(v)) {
        wlr_xwayland_surface_configure(v->xsurf, ev->x, ev->y, ev->width, ev->height);
        view_place(v, ev->x, ev->y);
    } else {
        view_fit(v);
    }
}

static void x11_request_activate(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, request_activate);
    if (v->mapped && !v->override_redirect && v->server->mode == MODE_APP) {
        raise_view(v->server, v);
        apply_mode(v->server);
    }
}

static void x11_set_title(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, set_title);
    if (v->mapped && v->id) shell_send_window(v->server, v);
}

static void x11_set_class(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, set_class);
    if (v->mapped && v->id) shell_send_window(v->server, v);
}

static void x11_destroy(struct wl_listener *l, void *data)
{
    struct view *v = wl_container_of(l, v, destroy);
    if (v->mapped) view_unmapped(v);
    if (v->tree) wlr_scene_node_destroy(&v->tree->node);
    wl_list_remove(&v->map.link);
    wl_list_remove(&v->unmap.link);
    wl_list_remove(&v->associate.link);
    wl_list_remove(&v->dissociate.link);
    wl_list_remove(&v->request_configure.link);
    wl_list_remove(&v->request_activate.link);
    wl_list_remove(&v->set_title.link);
    wl_list_remove(&v->set_class.link);
    wl_list_remove(&v->destroy.link);
    free(v);
}

static void new_xwayland_surface(struct wl_listener *l, void *data)
{
    struct server *s = wl_container_of(l, s, new_xwayland_surface);
    struct wlr_xwayland_surface *xs = data;
    struct view *v = calloc(1, sizeof *v);
    if (!v) return;
    v->server = s;
    v->kind = VIEW_X11;
    v->xsurf = xs;
    v->override_redirect = xs->override_redirect;
    wl_list_init(&v->map.link);
    wl_list_init(&v->unmap.link);
    v->associate.notify = x11_associate;
    wl_signal_add(&xs->events.associate, &v->associate);
    v->dissociate.notify = x11_dissociate;
    wl_signal_add(&xs->events.dissociate, &v->dissociate);
    v->request_configure.notify = x11_request_configure;
    wl_signal_add(&xs->events.request_configure, &v->request_configure);
    v->request_activate.notify = x11_request_activate;
    wl_signal_add(&xs->events.request_activate, &v->request_activate);
    v->set_title.notify = x11_set_title;
    wl_signal_add(&xs->events.set_title, &v->set_title);
    v->set_class.notify = x11_set_class;
    wl_signal_add(&xs->events.set_class, &v->set_class);
    v->destroy.notify = x11_destroy;
    wl_signal_add(&xs->events.destroy, &v->destroy);
}

static void xwayland_ready(struct wl_listener *l, void *data)
{
    struct server *s = wl_container_of(l, s, xwayland_ready);
    wlr_xwayland_set_seat(s->xwayland, s->seat);
    wlr_log(WLR_INFO, "XWayland is ready on %s", s->xwayland->display_name);
}

/* ======================================================================== */
/* Outputs                                                                  */
/* ======================================================================== */

static void refit_all(struct server *s)
{
    int w, h;
    display_size(s, &w, &h);
    wlr_scene_rect_set_size(s->backdrop, w, h);
    struct view *v;
    wl_list_for_each(v, &s->views, link) {
        if (v->mapped) view_fit(v);
    }
    if (s->shell_view) view_fit(s->shell_view);
}

static void output_frame(struct wl_listener *l, void *data)
{
    struct output *o = wl_container_of(l, o, frame);
    wlr_scene_output_commit(o->scene_output, NULL);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(o->scene_output, &now);
}

static void output_request_state(struct wl_listener *l, void *data)
{
    struct output *o = wl_container_of(l, o, request_state);
    const struct wlr_output_event_request_state *ev = data;
    wlr_output_commit_state(o->wlr_output, ev->state);
    refit_all(o->server);
}

static void output_destroy(struct wl_listener *l, void *data)
{
    struct output *o = wl_container_of(l, o, destroy);
    struct server *s = o->server;
    wl_list_remove(&o->frame.link);
    wl_list_remove(&o->request_state.link);
    wl_list_remove(&o->destroy.link);
    wl_list_remove(&o->link);
    free(o);
    refit_all(s);
}

static void new_output(struct wl_listener *l, void *data)
{
    struct server *s = wl_container_of(l, s, new_output);
    struct wlr_output *wo = data;
    wlr_output_init_render(wo, s->allocator, s->renderer);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wo);
    int cw = 0, ch = 0;
    const char *custom = getenv("PIXI_MODE"); /* WIDTHxHEIGHT, for testing on headless/nested outputs */
    if (custom && sscanf(custom, "%dx%d", &cw, &ch) == 2 && cw > 0 && ch > 0) wlr_output_state_set_custom_mode(&state, cw, ch, 0);
    else if (mode) wlr_output_state_set_mode(&state, mode);
    wlr_output_state_set_transform(&state, s->transform);
    wlr_output_commit_state(wo, &state);
    wlr_output_state_finish(&state);

    struct output *o = calloc(1, sizeof *o);
    if (!o) return;
    o->server = s;
    o->wlr_output = wo;
    o->frame.notify = output_frame;
    wl_signal_add(&wo->events.frame, &o->frame);
    o->request_state.notify = output_request_state;
    wl_signal_add(&wo->events.request_state, &o->request_state);
    o->destroy.notify = output_destroy;
    wl_signal_add(&wo->events.destroy, &o->destroy);
    wl_list_insert(s->outputs.prev, &o->link);

    struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(s->output_layout, wo);
    o->scene_output = wlr_scene_output_create(s->scene, wo);
    wlr_scene_output_layout_add_output(s->scene_layout, lo, o->scene_output);
    refit_all(s);
    wlr_log(WLR_INFO, "output %s ready", wo->name);
}

/* ======================================================================== */
/* Input                                                                    */
/* ======================================================================== */

static bool handle_binding(struct server *s, xkb_keysym_t sym, uint32_t mods, bool pressed)
{
    if (pressed && (mods & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) == (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) {
        if (sym == XKB_KEY_BackSpace) {
            s->quitting = true;
            wl_display_terminate(s->display);
            return true;
        }
        if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12) {
            if (s->session) wlr_session_change_vt(s->session, sym - XKB_KEY_XF86Switch_VT_1 + 1);
            return true;
        }
    }
    if (sym == XKB_KEY_XF86HomePage || sym == XKB_KEY_XF86Launch0) {
        if (pressed) shell_send_guide(s);
        return true;
    }
    return false;
}

static void keyboard_modifiers(struct wl_listener *l, void *data)
{
    struct keyboard *k = wl_container_of(l, k, modifiers);
    wlr_seat_set_keyboard(k->server->seat, k->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(k->server->seat, &k->wlr_keyboard->modifiers);
}

static void keyboard_key(struct wl_listener *l, void *data)
{
    struct keyboard *k = wl_container_of(l, k, key);
    struct server *s = k->server;
    struct wlr_keyboard_key_event *ev = data;
    uint32_t keycode = ev->keycode + 8;
    const xkb_keysym_t *syms;
    int n = xkb_state_key_get_syms(k->wlr_keyboard->xkb_state, keycode, &syms);
    uint32_t mods = wlr_keyboard_get_modifiers(k->wlr_keyboard);
    bool pressed = ev->state == WL_KEYBOARD_KEY_STATE_PRESSED;
    bool handled = false;

    for (int i = 0; i < n; i++) {
        bool super = syms[i] == XKB_KEY_Super_L || syms[i] == XKB_KEY_Super_R;
        if (super) {
            /* Super on its own is the guide; Super held with another key goes to the app. */
            if (pressed) s->super_alone = true;
            else if (s->super_alone) {
                s->super_alone = false;
                shell_send_guide(s);
            }
        } else if (pressed) {
            s->super_alone = false;
        }
        if (handle_binding(s, syms[i], mods, pressed)) handled = true;
    }
    if (!handled) {
        wlr_seat_set_keyboard(s->seat, k->wlr_keyboard);
        wlr_seat_keyboard_notify_key(s->seat, ev->time_msec, ev->keycode, ev->state);
    }
}

static void keyboard_destroy(struct wl_listener *l, void *data)
{
    struct keyboard *k = wl_container_of(l, k, destroy);
    wl_list_remove(&k->modifiers.link);
    wl_list_remove(&k->key.link);
    wl_list_remove(&k->destroy.link);
    wl_list_remove(&k->link);
    free(k);
}

static void new_keyboard(struct server *s, struct wlr_input_device *dev)
{
    struct wlr_keyboard *wk = wlr_keyboard_from_input_device(dev);
    struct keyboard *k = calloc(1, sizeof *k);
    if (!k) return;
    k->server = s;
    k->wlr_keyboard = wk;
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap) wlr_keyboard_set_keymap(wk, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(ctx);
    wlr_keyboard_set_repeat_info(wk, 25, 600);
    k->modifiers.notify = keyboard_modifiers;
    wl_signal_add(&wk->events.modifiers, &k->modifiers);
    k->key.notify = keyboard_key;
    wl_signal_add(&wk->events.key, &k->key);
    k->destroy.notify = keyboard_destroy;
    wl_signal_add(&dev->events.destroy, &k->destroy);
    wlr_seat_set_keyboard(s->seat, wk);
    wl_list_insert(&s->keyboards, &k->link);
}

static void new_input(struct wl_listener *l, void *data)
{
    struct server *s = wl_container_of(l, s, new_input);
    struct wlr_input_device *dev = data;
    switch (dev->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        new_keyboard(s, dev);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(s->cursor, dev);
        break;
    default:
        break;
    }
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&s->keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(s->seat, caps);
}

static void show_cursor(struct server *s)
{
    if (!s->cursor_visible) {
        wlr_cursor_set_xcursor(s->cursor, s->cursor_mgr, "default");
        s->cursor_visible = true;
    }
}

static void process_motion(struct server *s, uint32_t time)
{
    show_cursor(s);
    double sx, sy;
    struct wlr_scene_node *node = wlr_scene_node_at(&s->scene->tree.node, s->cursor->x, s->cursor->y, &sx, &sy);
    struct wlr_surface *surface = NULL;
    if (node && node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
        if (ss) surface = ss->surface;
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(s->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(s->seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(s->seat);
    }
}

static void cursor_motion(struct wl_listener *l, void *data)
{
    struct server *s = wl_container_of(l, s, cursor_motion);
    struct wlr_pointer_motion_event *ev = data;
    wlr_cursor_move(s->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
    process_motion(s, ev->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *l, void *data)
{
    struct server *s = wl_container_of(l, s, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *ev = data;
    wlr_cursor_warp_absolute(s->cursor, &ev->pointer->base, ev->x, ev->y);
    process_motion(s, ev->time_msec);
}

static void cursor_button(struct wl_listener *l, void *data)
{
    struct server *s = wl_container_of(l, s, cursor_button);
    struct wlr_pointer_button_event *ev = data;
    wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
}

static void cursor_axis(struct wl_listener *l, void *data)
{
    struct server *s = wl_container_of(l, s, cursor_axis);
    struct wlr_pointer_axis_event *ev = data;
    wlr_seat_pointer_notify_axis(s->seat, ev->time_msec, ev->orientation, ev->delta, ev->delta_discrete, ev->source,
                                 ev->relative_direction);
}

static void cursor_frame(struct wl_listener *l, void *data)
{
    struct server *s = wl_container_of(l, s, cursor_frame);
    wlr_seat_pointer_notify_frame(s->seat);
}

static void request_cursor(struct wl_listener *l, void *data)
{
    struct server *s = wl_container_of(l, s, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *ev = data;
    if (s->seat->pointer_state.focused_client == ev->seat_client && s->cursor_visible)
        wlr_cursor_set_surface(s->cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
}

static void request_set_selection(struct wl_listener *l, void *data)
{
    struct server *s = wl_container_of(l, s, request_set_selection);
    struct wlr_seat_request_set_selection_event *ev = data;
    wlr_seat_set_selection(s->seat, ev->source, ev->serial);
}

/* ======================================================================== */
/* The shell process                                                        */
/* ======================================================================== */

static void spawn_shell(struct server *s)
{
    if (s->quitting || !s->shell_cmd) return;
    pid_t pid = fork();
    if (pid < 0) {
        wlr_log_errno(WLR_ERROR, "couldn't start the shell");
        return;
    }
    if (pid == 0) {
        sigset_t set;
        sigemptyset(&set);
        sigprocmask(SIG_SETMASK, &set, NULL);
        setenv("WAYLAND_DISPLAY", s->socket, 1);
        if (s->xwayland) setenv("DISPLAY", s->xwayland->display_name, 1);
        setenv("XDG_CURRENT_DESKTOP", "Pixi", 1);
        setenv("XDG_SESSION_TYPE", "wayland", 1);
        setenv("SDL_VIDEODRIVER", "wayland", 0);
        setenv("QT_QPA_PLATFORM", "wayland;xcb", 0);
        setenv("GDK_BACKEND", "wayland,x11", 0);
        setenv("MOZ_ENABLE_WAYLAND", "1", 0);
        /* exec, so the shell keeps the pid the protocol is granted to */
        char *cmd = malloc(strlen(s->shell_cmd) + 6);
        if (!cmd) _exit(127);
        strcpy(cmd, "exec ");
        strcat(cmd, s->shell_cmd);
        execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    s->shell_pid = pid;
    wlr_log(WLR_INFO, "started shell (pid %d): %s", (int)pid, s->shell_cmd);
}

static int respawn_shell(void *data)
{
    spawn_shell(data);
    return 0;
}

static int handle_sigchld(int sig, void *data)
{
    struct server *s = data;
    if (s->shell_pid <= 0) return 0;
    int st;
    pid_t r = waitpid(s->shell_pid, &st, WNOHANG);
    if (r == s->shell_pid) {
        wlr_log(WLR_INFO, "shell exited (status %d)", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        s->shell_pid = 0;
        if (!s->quitting) {
            s->shell_restarts++;
            int delay = s->shell_restarts > 5 ? 5000 : 800;
            wl_event_source_timer_update(s->respawn_timer, delay);
        }
    }
    return 0;
}

static int handle_terminate(int sig, void *data)
{
    struct server *s = data;
    s->quitting = true;
    wl_display_terminate(s->display);
    return 0;
}

/* ======================================================================== */
/* main                                                                     */
/* ======================================================================== */

static void usage(void)
{
    printf("pixi " PIXI_VERSION " - the Pixi compositor\n\n"
           "usage: pixi [options]\n"
           "  -s, --shell CMD     shell command to run (default: pixi-shell)\n"
           "  -r, --rotate DEG    rotate the display 0, 90, 180 or 270 degrees\n"
           "      --no-shell      start without a shell (for development)\n"
           "      --insecure      let any client use the shell protocol (development only)\n"
           "  -d, --debug         verbose wlroots logging\n"
           "  -v, --version       print the version\n\n"
           "Keys: Super or Home opens the guide, Ctrl+Alt+Backspace ends the session.\n");
}

int main(int argc, char **argv)
{
    struct server s = { 0 };
    s.shell_cmd = "pixi-shell";
    s.transform = WL_OUTPUT_TRANSFORM_NORMAL;
    bool debug = false, no_shell = false;
    const char *rot = getenv("PIXI_ROTATE");
    int rotate = rot ? atoi(rot) : 0;

    static const struct option opts[] = {
        { "shell", required_argument, NULL, 's' }, { "rotate", required_argument, NULL, 'r' },
        { "no-shell", no_argument, NULL, 'n' },    { "insecure", no_argument, NULL, 'i' },
        { "debug", no_argument, NULL, 'd' },       { "version", no_argument, NULL, 'v' },
        { "help", no_argument, NULL, 'h' },        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "s:r:dvh", opts, NULL)) != -1) {
        switch (c) {
        case 's': s.shell_cmd = optarg; break;
        case 'r': rotate = atoi(optarg); break;
        case 'n': no_shell = true; break;
        case 'i': s.insecure = true; break;
        case 'd': debug = true; break;
        case 'v': puts("pixi " PIXI_VERSION); return 0;
        case 'h': usage(); return 0;
        default: usage(); return 2;
        }
    }
    switch (((rotate % 360) + 360) % 360) {
    case 90: s.transform = WL_OUTPUT_TRANSFORM_90; break;
    case 180: s.transform = WL_OUTPUT_TRANSFORM_180; break;
    case 270: s.transform = WL_OUTPUT_TRANSFORM_270; break;
    default: break;
    }
    if (no_shell) s.shell_cmd = NULL;
    wlr_log_init(debug ? WLR_DEBUG : WLR_INFO, NULL);

    s.display = wl_display_create();
    s.loop = wl_display_get_event_loop(s.display);
    s.backend = wlr_backend_autocreate(s.loop, &s.session);
    if (!s.backend) {
        wlr_log(WLR_ERROR, "couldn't open the display or input devices. Run pixi from a text console as a user "
                           "in the video, render and input groups.");
        return 1;
    }
    s.renderer = wlr_renderer_autocreate(s.backend);
    if (!s.renderer) {
        wlr_log(WLR_ERROR, "couldn't create a renderer");
        return 1;
    }
    wlr_renderer_init_wl_display(s.renderer, s.display);
    s.allocator = wlr_allocator_autocreate(s.backend, s.renderer);
    if (!s.allocator) {
        wlr_log(WLR_ERROR, "couldn't create an allocator");
        return 1;
    }

    s.compositor = wlr_compositor_create(s.display, 5, s.renderer);
    wlr_subcompositor_create(s.display);
    wlr_data_device_manager_create(s.display);
    wlr_primary_selection_v1_device_manager_create(s.display);
    wlr_viewporter_create(s.display);
    wlr_single_pixel_buffer_manager_v1_create(s.display);
    wlr_screencopy_manager_v1_create(s.display);

    s.output_layout = wlr_output_layout_create(s.display);
    wlr_xdg_output_manager_v1_create(s.display, s.output_layout);
    wl_list_init(&s.outputs);
    s.new_output.notify = new_output;
    wl_signal_add(&s.backend->events.new_output, &s.new_output);

    s.scene = wlr_scene_create();
    s.scene_layout = wlr_scene_attach_output_layout(s.scene, s.output_layout);
    const float black[4] = { 0, 0, 0, 1 };
    s.backdrop = wlr_scene_rect_create(&s.scene->tree, 1280, 720, black);
    s.apps_tree = wlr_scene_tree_create(&s.scene->tree);
    s.shell_tree = wlr_scene_tree_create(&s.scene->tree);
    s.popup_tree = wlr_scene_tree_create(&s.scene->tree);

    wl_list_init(&s.views);
    s.xdg_shell = wlr_xdg_shell_create(s.display, 3);
    s.new_xdg_toplevel.notify = new_xdg_toplevel;
    wl_signal_add(&s.xdg_shell->events.new_toplevel, &s.new_xdg_toplevel);
    s.new_xdg_popup.notify = new_xdg_popup;
    wl_signal_add(&s.xdg_shell->events.new_popup, &s.new_xdg_popup);
    s.deco_mgr = wlr_xdg_decoration_manager_v1_create(s.display);
    s.new_decoration.notify = new_decoration;
    wl_signal_add(&s.deco_mgr->events.new_toplevel_decoration, &s.new_decoration);

    s.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(s.cursor, s.output_layout);
    s.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
    s.cursor_motion.notify = cursor_motion;
    wl_signal_add(&s.cursor->events.motion, &s.cursor_motion);
    s.cursor_motion_absolute.notify = cursor_motion_absolute;
    wl_signal_add(&s.cursor->events.motion_absolute, &s.cursor_motion_absolute);
    s.cursor_button.notify = cursor_button;
    wl_signal_add(&s.cursor->events.button, &s.cursor_button);
    s.cursor_axis.notify = cursor_axis;
    wl_signal_add(&s.cursor->events.axis, &s.cursor_axis);
    s.cursor_frame.notify = cursor_frame;
    wl_signal_add(&s.cursor->events.frame, &s.cursor_frame);

    wl_list_init(&s.keyboards);
    s.new_input.notify = new_input;
    wl_signal_add(&s.backend->events.new_input, &s.new_input);
    s.seat = wlr_seat_create(s.display, "seat0");
    s.request_cursor.notify = request_cursor;
    wl_signal_add(&s.seat->events.request_set_cursor, &s.request_cursor);
    s.request_set_selection.notify = request_set_selection;
    wl_signal_add(&s.seat->events.request_set_selection, &s.request_set_selection);

    wl_list_init(&s.shell_resources);
    s.shell_global = wl_global_create(s.display, &pixi_shell_v1_interface, 1, &s, shell_bind);
    wl_display_set_global_filter(s.display, global_filter, &s);

    s.xwayland = wlr_xwayland_create(s.display, s.compositor, true);
    if (s.xwayland) {
        s.xwayland_ready.notify = xwayland_ready;
        wl_signal_add(&s.xwayland->events.ready, &s.xwayland_ready);
        s.new_xwayland_surface.notify = new_xwayland_surface;
        wl_signal_add(&s.xwayland->events.new_surface, &s.new_xwayland_surface);
        setenv("DISPLAY", s.xwayland->display_name, 1);
    } else {
        wlr_log(WLR_INFO, "XWayland isn't available; X11 applications won't run");
    }

    s.socket = wl_display_add_socket_auto(s.display);
    if (!s.socket) {
        wlr_log(WLR_ERROR, "couldn't create the Wayland socket");
        return 1;
    }
    if (!wlr_backend_start(s.backend)) {
        wlr_log(WLR_ERROR, "couldn't start the backend");
        return 1;
    }
    setenv("WAYLAND_DISPLAY", s.socket, 1);
    wlr_log(WLR_INFO, "Pixi running on WAYLAND_DISPLAY=%s", s.socket);

    s.sigchld = wl_event_loop_add_signal(s.loop, SIGCHLD, handle_sigchld, &s);
    s.sigterm = wl_event_loop_add_signal(s.loop, SIGTERM, handle_terminate, &s);
    s.sigint = wl_event_loop_add_signal(s.loop, SIGINT, handle_terminate, &s);
    s.respawn_timer = wl_event_loop_add_timer(s.loop, respawn_shell, &s);
    s.mode = MODE_HOME;
    spawn_shell(&s);

    wl_display_run(s.display);

    s.quitting = true;
    if (s.shell_pid > 0) {
        kill(s.shell_pid, SIGTERM);
        struct timespec tick = { 0, 50 * 1000 * 1000 };
        for (int i = 0; i < 20 && waitpid(s.shell_pid, NULL, WNOHANG) == 0; i++) nanosleep(&tick, NULL);
        if (waitpid(s.shell_pid, NULL, WNOHANG) == 0) kill(s.shell_pid, SIGKILL);
    }
    wl_event_source_remove(s.respawn_timer);
    wl_event_source_remove(s.sigchld);
    wl_event_source_remove(s.sigterm);
    wl_event_source_remove(s.sigint);
    wl_display_destroy_clients(s.display);
    if (s.xwayland) wlr_xwayland_destroy(s.xwayland);
    wlr_scene_node_destroy(&s.scene->tree.node);
    wlr_xcursor_manager_destroy(s.cursor_mgr);
    wlr_cursor_destroy(s.cursor);
    wlr_allocator_destroy(s.allocator);
    wlr_renderer_destroy(s.renderer);
    wlr_backend_destroy(s.backend);
    wl_display_destroy(s.display);
    return 0;
}
