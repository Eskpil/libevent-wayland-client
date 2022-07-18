#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

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
    struct wl_shm *shm;

    struct xdg_wm_base *wm_base;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;

    struct event_base *base;
};

static void
randname(char *buf)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long r = ts.tv_nsec;
    for (int i = 0; i < 6; ++i) {
        buf[i] = 'A'+(r&15)+(r&16)*2;
        r >>= 5;
    }
}

static int
create_shm_file(void)
{
    int retries = 100;
    do {
        char name[] = "/wl_shm-XXXXXX";
        randname(name + sizeof(name) - 7);
        --retries;
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(name);
            return fd;
        }
    } while (retries > 0 && errno == EEXIST);
    return -1;
}

static int
allocate_shm_file(size_t size)
{
    int fd = create_shm_file();
    if (fd < 0)
        return -1;
    int ret;
    do {
        ret = ftruncate(fd, size);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    /* Sent by the compositor when it's no longer using this buffer */
    wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_release,
};

static struct wl_buffer *
draw_frame(struct test_app *app)
{
    const int width = 640, height = 480;
    int stride = width * 4;
    int size = stride * height;

    int fd = allocate_shm_file(size);
    if (fd == -1) {
        return NULL;
    }

    uint32_t *data = mmap(NULL, size,
            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
            width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    /* Draw checkerboxed background */
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if ((x + y / 8 * 8) % 16 < 8)
                data[y * width + x] = 0xFF666666;
            else
                data[y * width + x] = 0xFFEEEEEE;
        }
    }

    munmap(data, size);
    wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
    return buffer;
}

static void
xdg_surface_configure(void *data,
        struct xdg_surface *xdg_surface, uint32_t serial)
{
    struct test_app *app = data;
    xdg_surface_ack_configure(xdg_surface, serial);

    struct wl_buffer *buffer = draw_frame(app);
    wl_surface_attach(app->surface, buffer, 0, 0);
    wl_surface_commit(app->surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
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
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
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

static void app_display_write(struct bufferevent *bev, void *data)
{
    struct test_app *app = data;

    wl_display_flush(app->display);
}


int main(int argc, char** argv)
{
    struct test_app *app = malloc(sizeof(struct test_app));

    app->display = wl_display_connect(NULL);
    app->base = event_base_new();

    event_enable_debug_logging(EVENT_DBG_ALL);

    if (app->display == NULL) {
        fprintf(stderr, "Can't connect to display\n");
        return 1;
    }

    app->registry = wl_display_get_registry(app->display);

    wl_registry_add_listener(app->registry, &registry_listener, app);

    wl_display_roundtrip(app->display);

    app->surface = wl_compositor_create_surface(app->compositor);

    app->xdg_surface = xdg_wm_base_get_xdg_surface(app->wm_base, app->surface);
    xdg_surface_add_listener(app->xdg_surface, &xdg_surface_listener, app);
    app->xdg_toplevel = xdg_surface_get_toplevel(app->xdg_surface);
    xdg_toplevel_set_title(app->xdg_toplevel, "Example client");
    wl_surface_commit(app->surface);

    wl_display_roundtrip(app->display);

    {
        int fd = wl_display_get_fd(app->display);

        if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) {
            perror("fcntl");
            exit(1);
        }

        printf("fcntl success\n");

        struct bufferevent *bev = bufferevent_socket_new(app->base, fd, BEV_OPT_CLOSE_ON_FREE);

        bufferevent_setcb(bev, app_display_read, app_display_write, NULL, (void *)app);
        bufferevent_enable(bev, EV_READ|EV_WRITE);

        if (wl_display_prepare_read(app->display) != 0) {
            perror("wl_display_prepare_read");
            exit(1);
        }
    }

    event_base_dispatch(app->base);

    // while (wl_display_dispatch(app->display)) {
        /* This space deliberately left blank */
    // }

    wl_display_disconnect(app->display);

    free(app);

    return 0;
}
