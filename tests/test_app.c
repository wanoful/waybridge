/* Deterministic native Wayland fixture. MIT. No host desktop required. */
#define _GNU_SOURCE
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *shell;
static struct wl_seat *seat;
static struct zxdg_decoration_manager_v1 *decoration_manager;
static int client_decorated;
static int alpha_fixture;
static struct wl_data_device_manager *data_manager;
static struct wl_data_device *data_device;
static struct wl_data_source *drag_source;
static void visual_cancel(void);
static void source_target(void *d, struct wl_data_source *source, const char *mime) {
  (void)d; (void)source;
  printf("SOURCE_TARGET %s\n", mime ? mime : "none"); fflush(stdout);
}
static void source_send(void *d, struct wl_data_source *source, const char *mime, int fd) {
  (void)d; (void)source; (void)mime;
  const char *path = getenv("WAYBRIDGE_TEST_DRAG_URIS");
  int input = path ? open(path, O_RDONLY) : -1;
  char bytes[4096]; ssize_t n;
  if (input >= 0) {
    while ((n = read(input, bytes, sizeof(bytes))) > 0) {
      ssize_t offset = 0;
      while (offset < n) { ssize_t written = write(fd, bytes + offset, n - offset); if (written <= 0) break; offset += written; }
    }
    close(input);
  }
  close(fd); printf("SOURCE_SEND\n"); fflush(stdout);
}
static void source_cancelled(void *d, struct wl_data_source *source) {
  visual_cancel();
  (void)d; wl_data_source_destroy(source); drag_source = NULL;
  printf("SOURCE_CANCELLED\n"); fflush(stdout);
}
static void source_dropped(void *d, struct wl_data_source *source) {
  (void)d; (void)source; printf("SOURCE_DROPPED\n"); fflush(stdout);
}
static void source_finished(void *d, struct wl_data_source *source) {
  visual_cancel();
  (void)d; wl_data_source_destroy(source); drag_source = NULL;
  printf("SOURCE_FINISHED\n"); fflush(stdout);
}
static void source_action(void *d, struct wl_data_source *source, uint32_t action) {
  (void)d; (void)source; printf("SOURCE_ACTION %u\n", action); fflush(stdout);
}
static const struct wl_data_source_listener source_listener = {
  .target = source_target, .send = source_send, .cancelled = source_cancelled,
  .dnd_drop_performed = source_dropped, .dnd_finished = source_finished, .action = source_action,
};
static struct wl_data_offer *drop_offer;
static void offer_mime(void *d, struct wl_data_offer *o, const char *mime) {
  (void)d;
  (void)o;
  (void)mime;
}
static void offer_actions(void *d, struct wl_data_offer *o, uint32_t actions) {
  (void)d;
  (void)o;
  (void)actions;
}
static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_mime, .source_actions = offer_actions, .action = offer_actions};
static void offered(void *d, struct wl_data_device *device, struct wl_data_offer *offer) {
  (void)d;
  (void)device;
  wl_data_offer_add_listener(offer, &offer_listener, NULL);
}
static void drag_enter(void *d, struct wl_data_device *device, uint32_t serial,
                       struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                       struct wl_data_offer *offer) {
  (void)d;
  (void)device;
  (void)surface;
  (void)x;
  (void)y;
  drop_offer = offer;
  // This fixture is also the outbound source. Accept a local drop only
  // when a destination was configured; otherwise it must reject its own drag.
  if (offer && (!drag_source || getenv("WAYBRIDGE_TEST_DROP"))) {
    wl_data_offer_accept(offer, serial, "text/uri-list");
    wl_data_offer_set_actions(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                              WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
  }
}
static void drag_leave(void *d, struct wl_data_device *device) {
  (void)d;
  (void)device;
}
static void drag_motion(void *d, struct wl_data_device *device, uint32_t time, wl_fixed_t x,
                        wl_fixed_t y) {
  (void)d;
  (void)device;
  (void)time;
  (void)x;
  (void)y;
}
static void drag_drop(void *d, struct wl_data_device *device) {
  (void)d;
  (void)device;
  const char *path = getenv("WAYBRIDGE_TEST_DROP");
  if (drop_offer && path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    wl_data_offer_receive(drop_offer, "text/uri-list", fd);
    close(fd);
    printf("DROP\n");
    fflush(stdout);
  }
}
static void selection(void *d, struct wl_data_device *device, struct wl_data_offer *offer) {
  (void)d;
  (void)device;
  (void)offer;
}
static const struct wl_data_device_listener device_listener = {.data_offer = offered,
                                                               .enter = drag_enter,
                                                               .leave = drag_leave,
                                                               .motion = drag_motion,
                                                               .drop = drag_drop,
                                                               .selection = selection};
struct window {
  struct wl_surface *surface;
  struct xdg_surface *xdg;
  struct xdg_toplevel *top;
  struct zxdg_toplevel_decoration_v1 *decoration;
  int width, height, id;
  int maximized, fullscreen, hidden, sem_invert;
};
static struct window windows[3];
static struct window *pointer_window;
static double pointer_x, pointer_y;
static void decoration_configured(void *data, struct zxdg_toplevel_decoration_v1 *decoration,
                                  uint32_t mode) {
  (void)decoration;
  struct window *w = data;
  printf("DECORATION %d %u\n", w->id, mode);
  fflush(stdout);
}
static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configured};
struct pixels {
  struct wl_buffer *buffer;
  void *data;
  size_t size;
};
static void released(void *data, struct wl_buffer *buffer) {
  struct pixels *p = data;
  wl_buffer_destroy(buffer);
  munmap(p->data, p->size);
  free(p);
}
static const struct wl_buffer_listener buffer_listener = {.release = released};
#include "cursor_fixture.h"
#include "drag_visual_fixture.h"
static void draw(struct window *w) {
  int shadow = client_decorated ? 12 : 0;
  int width = w->width + shadow * 2, height = w->height + shadow * 2;
  size_t size = (size_t)width * height * 4;
  int fd = memfd_create("waybridge-test", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, size))
    exit(2);
  struct pixels *p = calloc(1, sizeof(*p));
  p->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  p->size = size;
  uint32_t *out = p->data;
  for (int y = 0; y < height; y++)
    for (int x = 0; x < width; x++) {
      int cx = x - shadow, cy = y - shadow;
      uint32_t color = cy < w->height / 2 ? 0xffff0000 : 0xff0000ff;
      if (w->sem_invert)
        color = cy < w->height / 2 ? 0xff0000ff : 0xffff0000;
      if (client_decorated && cy < 32)
        color = 0xff282828;
      if (cx < 0 || cy < 0 || cx >= w->width || cy >= w->height)
        color = 0xff00ff00;
      if (alpha_fixture) color = x < width / 3 ? 0xffff0000 : (x < width * 2 / 3 ? 0x80008000 : 0);
      out[y * width + x] = color;
    }
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
  p->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  wl_buffer_add_listener(p->buffer, &buffer_listener, p);
  wl_surface_attach(w->surface, p->buffer, 0, 0);
  xdg_surface_set_window_geometry(w->xdg, shadow, shadow, w->width, w->height);
  wl_surface_damage_buffer(w->surface, 0, 0, width, height);
  wl_surface_commit(w->surface);
  printf("CONFIGURE %d %d %d\n", w->id, w->width, w->height);
  fflush(stdout);
}
static void configured(void *data, struct xdg_surface *s, uint32_t serial) {
  xdg_surface_ack_configure(s, serial);
  if (!((struct window *)data)->hidden)
    draw(data);
}
static const struct xdg_surface_listener surface_listener = {.configure = configured};
static void sized(void *data, struct xdg_toplevel *top, int32_t width, int32_t height,
                  struct wl_array *states) {
  (void)top;
  struct window *w = data;
  w->maximized = 0;
  w->fullscreen = 0;
  int activated = 0, suspended = 0;
  uint32_t *state;
  wl_array_for_each(state, states) {
    if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED)
      w->maximized = 1;
    if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
      w->fullscreen = 1;
    if (*state == XDG_TOPLEVEL_STATE_ACTIVATED)
      activated = 1;
    if (*state == XDG_TOPLEVEL_STATE_SUSPENDED)
      suspended = 1;
  }
  if (getenv("WAYBRIDGE_TEST_SEMANTICS")) {
    printf("SEM_CONFIGURE %d %d %d %d %d %d %d\n", w->id, width, height, w->fullscreen,
           w->maximized, activated, suspended);
    fflush(stdout);
  }
  if (width > 0)
    w->width = width;
  if (height > 0)
    w->height = height;
}
static void closed(void *data, struct xdg_toplevel *top) {
  struct window *w = data;
  printf("CLOSE %d\n", w->id);
  fflush(stdout);
  if (w->decoration)
    zxdg_toplevel_decoration_v1_destroy(w->decoration);
  xdg_toplevel_destroy(top);
  xdg_surface_destroy(w->xdg);
  wl_surface_destroy(w->surface);
  w->surface = NULL;
  if (!windows[0].surface && !windows[1].surface && !windows[2].surface)
    exit(0);
}
static void configure_bounds(void *d, struct xdg_toplevel *top, int32_t w, int32_t h) {
  (void)d;
  (void)top;
  (void)w;
  (void)h;
}
static void wm_capabilities(void *d, struct xdg_toplevel *top, struct wl_array *capabilities) {
  (void)d;
  (void)top;
  if (getenv("WAYBRIDGE_TEST_SEMANTICS")) {
    printf("SEM_CAPS");
    uint32_t *cap;
    wl_array_for_each(cap, capabilities) printf(" %u", *cap);
    printf("\n");
    fflush(stdout);
  }
}
static const struct xdg_toplevel_listener top_listener = {.configure = sized,
                                                          .close = closed,
                                                          .configure_bounds = configure_bounds,
                                                          .wm_capabilities = wm_capabilities};
#include "semantics_fixture.h"
#include "activation_fixture.h"
static void ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener shell_listener = {.ping = ping};
static void pointer_enter(void *data, struct wl_pointer *p, uint32_t s, struct wl_surface *surface,
                          wl_fixed_t x, wl_fixed_t y) {
  (void)data;
  (void)p;
  (void)s;
  (void)surface;
  for (int i = 0; i < 3; i++)
    if (windows[i].surface == surface)
      pointer_window = &windows[i];
  cursor_pointer = p; cursor_serial = s; cursor_mode = -1;
  cursor_select(wl_fixed_to_double(x));
  pointer_x = wl_fixed_to_double(x);
  pointer_y = wl_fixed_to_double(y);
  printf("ENTER %.1f %.1f\n", wl_fixed_to_double(x), wl_fixed_to_double(y));
  fflush(stdout);
}
static void pointer_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *surface) {
  (void)d;
  (void)p;
  (void)s;
  (void)surface;
  pointer_window = NULL;
  cursor_mode = -1;
}
static void motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
  (void)d;
  (void)p;
  (void)t;
  pointer_x = wl_fixed_to_double(x);
  pointer_y = wl_fixed_to_double(y);
  cursor_select(wl_fixed_to_double(x));
  printf("MOTION %.1f %.1f\n", wl_fixed_to_double(x), wl_fixed_to_double(y));
  fflush(stdout);
}
static void button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b,
                   uint32_t state) {
  (void)d;
  (void)p;
  (void)s;
  (void)t;
  if (getenv("WAYBRIDGE_TEST_SEMANTICS")) {
    printf("SEM_BUTTON %d %u %u\n", pointer_window ? pointer_window->id : 0, b, state);
    fflush(stdout);
  }
  printf("BUTTON %u %u\n", b, state);
  const char *drag_enabled = getenv("WAYBRIDGE_TEST_DRAG_ENABLED");
  if (getenv("WAYBRIDGE_TEST_DRAG_ICON") && !drag_source && pointer_window && b == 272 && state == WL_POINTER_BUTTON_STATE_PRESSED) {
    drag_source = wl_data_device_manager_create_data_source(data_manager);
    wl_data_source_add_listener(drag_source, &source_listener, NULL);
    wl_data_source_offer(drag_source, "text/plain");
    wl_data_source_set_actions(drag_source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    wl_data_device_start_drag(data_device, drag_source, pointer_window->surface, visual_start(), s);
  }
  if (getenv("WAYBRIDGE_TEST_DRAG_URIS") && (!drag_enabled || access(drag_enabled, F_OK) == 0) && !drag_source && pointer_window &&
      b == 272 && state == WL_POINTER_BUTTON_STATE_PRESSED) {
    drag_source = wl_data_device_manager_create_data_source(data_manager);
    wl_data_source_add_listener(drag_source, &source_listener, NULL);
    wl_data_source_offer(drag_source, "text/uri-list");
    wl_data_source_set_actions(drag_source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    wl_data_device_start_drag(data_device, drag_source, pointer_window->surface, NULL, s);
  }
  if (client_decorated && pointer_window && b == 272 && state == WL_POINTER_BUTTON_STATE_PRESSED &&
      pointer_y >= 12 && pointer_y < 44) {
    struct window *w = pointer_window;
    double x = pointer_x - 12;
    if (x >= w->width - 40)
      closed(w, w->top);
    else if (x >= w->width - 80) {
      if (w->maximized)
        xdg_toplevel_unset_maximized(w->top);
      else
        xdg_toplevel_set_maximized(w->top);
    } else if (x >= w->width - 120)
      xdg_toplevel_set_minimized(w->top);
    else
      xdg_toplevel_move(w->top, seat, s);
  }
  fflush(stdout);
}
static void axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {
  (void)d;
  (void)p;
  (void)t;
  (void)a;
  (void)v;
}
static void frame(void *d, struct wl_pointer *p) {
  (void)d;
  (void)p;
}
static void axis_source(void *d, struct wl_pointer *p, uint32_t source) {
  (void)d;
  (void)p;
  (void)source;
}
static void axis_stop(void *d, struct wl_pointer *p, uint32_t time, uint32_t a) {
  (void)d;
  (void)p;
  (void)time;
  (void)a;
}
static void axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {
  (void)d;
  (void)p;
  (void)a;
  (void)v;
}
static const struct wl_pointer_listener pointer_listener = {.enter = pointer_enter,
                                                            .leave = pointer_leave,
                                                            .motion = motion,
                                                            .button = button,
                                                            .axis = axis,
                                                            .frame = frame,
                                                            .axis_source = axis_source,
                                                            .axis_stop = axis_stop,
                                                            .axis_discrete = axis_discrete};
static void keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int fd, uint32_t size) {
  (void)d;
  (void)k;
  (void)fmt;
  (void)size;
  close(fd);
}
static void keyboard_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *surface,
                           struct wl_array *keys) {
  (void)d;
  (void)k;
  (void)s;
  (void)surface;
  (void)keys;
  keyboard_window = 0;
  for (int i = 0; i < 3; i++)
    if (windows[i].surface == surface)
      keyboard_window = i + 1;
}
static void keyboard_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *surface) {
  (void)d;
  (void)k;
  (void)s;
  (void)surface;
}
static void key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t code,
                uint32_t state) {
  (void)d;
  (void)k;
  (void)s;
  (void)t;
  if (getenv("WAYBRIDGE_TEST_ICON") && state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    const char *id = code == 30 ? "org.waybridge.Png" : code == 48 ? "org.waybridge.Svg" :
        code == 46 ? "org.waybridge.Missing" : code == 32 ? "org.waybridge.Absolute" :
        code == 18 ? "org.waybridge.Inherited" : code == 33 ? "org.waybridge.Hicolor" :
        code == 34 ? "org.waybridge.Pixmap" : code == 35 ? "org.waybridge.Hidden" :
        code == 23 ? "org.waybridge.Malformed" : code == 24 ? "org.waybridge.Huge" :
        code == 25 ? "org.waybridge.Slow" : code == 49 ? "nested-app" : NULL;
    if (id) { xdg_toplevel_set_app_id(windows[0].top, id); wl_surface_commit(windows[0].surface); }
  }
  if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
    semantics_key(code);
  if (state == WL_KEYBOARD_KEY_STATE_PRESSED) activation_key(s, code);
  printf("KEY %u %u\n", code, state);
  fflush(stdout);
}
static void modifiers(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat,
                      uint32_t lock, uint32_t grp) {
  (void)d;
  (void)k;
  (void)s;
  (void)dep;
  (void)lat;
  (void)lock;
  (void)grp;
}
static void repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
  (void)d;
  (void)k;
  (void)rate;
  (void)delay;
}
static const struct wl_keyboard_listener keyboard_listener = {.keymap = keymap,
                                                              .enter = keyboard_enter,
                                                              .leave = keyboard_leave,
                                                              .key = key,
                                                              .modifiers = modifiers,
                                                              .repeat_info = repeat};
static void capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
  (void)data;
  if (caps & WL_SEAT_CAPABILITY_POINTER)
    wl_pointer_add_listener(wl_seat_get_pointer(seat), &pointer_listener, NULL);
  if (caps & WL_SEAT_CAPABILITY_KEYBOARD)
    wl_keyboard_add_listener(wl_seat_get_keyboard(seat), &keyboard_listener, NULL);
}
static void seat_name(void *d, struct wl_seat *seat, const char *name) {
  (void)d;
  (void)seat;
  (void)name;
}
static const struct wl_seat_listener seat_listener = {.capabilities = capabilities,
                                                      .name = seat_name};
static void global(void *data, struct wl_registry *r, uint32_t name, const char *interface,
                   uint32_t version) {
  (void)data;
  if (!strcmp(interface, "wp_cursor_shape_manager_v1"))
    cursor_manager = wl_registry_bind(r, name, &wp_cursor_shape_manager_v1_interface, 1);
  if (!strcmp(interface, "xdg_wm_dialog_v1"))
    dialog_manager = wl_registry_bind(r, name, &xdg_wm_dialog_v1_interface, 1);
  if (!strcmp(interface, "wl_subcompositor"))
    subcompositor = wl_registry_bind(r, name, &wl_subcompositor_interface, 1);
  if (!strcmp(interface, "xdg_activation_v1"))
    activation_manager = wl_registry_bind(r, name, &xdg_activation_v1_interface, 1);
  if (!strcmp(interface, "wl_compositor"))
    compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4);
  if (!strcmp(interface, "wl_shm"))
    shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
  if (!strcmp(interface, "xdg_wm_base")) {
    shell = wl_registry_bind(r, name, &xdg_wm_base_interface,
                             getenv("WAYBRIDGE_TEST_SEMANTICS") ? (version < 6 ? version : 6) : 1);
    xdg_wm_base_add_listener(shell, &shell_listener, NULL);
  }
  if (!strcmp(interface, "wl_seat")) {
    seat = wl_registry_bind(r, name, &wl_seat_interface, version < 5 ? version : 5);
    wl_seat_add_listener(seat, &seat_listener, NULL);
  }
  if (!strcmp(interface, "wl_data_device_manager"))
    data_manager = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
  if (!strcmp(interface, "zxdg_decoration_manager_v1"))
    decoration_manager = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
}
static void removed(void *data, struct wl_registry *r, uint32_t name) {
  (void)data;
  (void)r;
  (void)name;
}
static const struct wl_registry_listener registry_listener = {.global = global,
                                                              .global_remove = removed};
int main(int argc, char **argv) {
  (void)argv;
  client_decorated = getenv("WAYBRIDGE_FIXTURE_CSD") != NULL;
  alpha_fixture = getenv("WAYBRIDGE_FIXTURE_ALPHA") != NULL;
  display = wl_display_connect(NULL);
  if (!display)
    return 1;
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  wl_display_roundtrip(display);
  data_device = wl_data_device_manager_get_data_device(data_manager, seat);
  wl_data_device_add_listener(data_device,
                              &device_listener, NULL);
  for (int i = 0; i < (getenv("WAYBRIDGE_TEST_SEMANTICS") ? 3 : (argc > 1 ? 2 : 1)); i++) {
    struct window *w = &windows[i];
    w->id = i + 1;
    w->width = 400;
    w->height = 300;
    w->surface = wl_compositor_create_surface(compositor);
    w->xdg = xdg_wm_base_get_xdg_surface(shell, w->surface);
    xdg_surface_add_listener(w->xdg, &surface_listener, w);
    w->top = xdg_surface_get_toplevel(w->xdg);
    xdg_toplevel_add_listener(w->top, &top_listener, w);
    if (decoration_manager && !client_decorated) {
      w->decoration =
          zxdg_decoration_manager_v1_get_toplevel_decoration(decoration_manager, w->top);
      zxdg_toplevel_decoration_v1_add_listener(w->decoration, &decoration_listener, w);
      zxdg_toplevel_decoration_v1_set_mode(w->decoration,
                                           ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    char title[64];
    snprintf(title, sizeof(title), "Waybridge fixture %d", w->id);
    if (getenv("WAYBRIDGE_TEST_SEMANTICS"))
      snprintf(title, sizeof(title), "Waybridge semantics %d", w->id);
    xdg_toplevel_set_title(w->top, title);
    const char *app_id = getenv("WAYBRIDGE_TEST_APP_ID");
    xdg_toplevel_set_app_id(w->top, app_id ? app_id : "waybridge.fixture");
    wl_surface_commit(w->surface);
  }
  if (getenv("WAYBRIDGE_TEST_SEMANTICS")) {
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    xdg_toplevel_set_parent(windows[2].top, windows[0].top);
  }
  while (wl_display_dispatch(display) >= 0) {
  }
  return 0;
}
