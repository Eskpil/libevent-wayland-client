#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>

#include <wayland-client.h>

#include <event2/event-config.h>
#include <event2/event.h>
#include <event2/event_struct.h>
#include <event2/util.h>
#include <event2/bufferevent.h>

#include "xdg-client-shell.h"

struct test_app {
    struct wl_display *display;
    struct wl_registry* registry;
    struct wl_compositor *compositor;
    struct wl_surface *surface;

    struct xdg_wm_base *wm_base;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    struct event_base *base;
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
    printf("Ping Pong!\n");
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void global_registry_handler(void* data, struct wl_registry* registry, uint32_t name,
    const char* interface, uint32_t version)
{
    struct test_app *app = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        app->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 2);

        xdg_wm_base_add_listener(app->wm_base,
            &xdg_wm_base_listener, app);
    }
}

static void
global_registry_remover(void* data, struct wl_registry* registry, uint32_t id)
{
}

static const struct wl_registry_listener registry_listener = {
    global_registry_handler,
    global_registry_remover
};

static void app_display_read(struct bufferevent *bev, void *data)
{
    struct test_app *app = data;

    printf("Application read\n");
}

int main(int argc, char** argv)
{
    struct test_app *app = malloc(sizeof(struct test_app));

    app->display = wl_display_connect(NULL);
    app->base = event_base_new();

    if (app->display == NULL) {
        fprintf(stderr, "Can't connect to display\n");
        return 1;
    }

    app->registry = wl_display_get_registry(app->display);

    wl_registry_add_listener(app->registry, &registry_listener, app);

    wl_display_roundtrip(app->display);

    app->surface = wl_compositor_create_surface(app->compositor);

    wl_display_roundtrip(app->display);

    {
        int fd = wl_display_get_fd(app->display);

        if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) {
            perror("fcntl");
            exit(1);
        }

        printf("fcntl success\n");

        struct bufferevent *bev = bufferevent_socket_new(app->base, fd, BEV_OPT_CLOSE_ON_FREE);

        bufferevent_setcb(bev, app_display_read, NULL, NULL, (void *)app);
        bufferevent_disable(bev, EV_WRITE);
        bufferevent_enable(bev, EV_READ);

        if (wl_display_prepare_read(app->display) != 0) {
            perror("wl_display_prepare_read");
            exit(1);
        }
    }

    event_base_dispatch(app->base);

    wl_display_disconnect(app->display);

    free(app);

    return 0;
}
