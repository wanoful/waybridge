#define _POSIX_C_SOURCE 200809L
#include "xdg-shell-client-protocol.h"
#include "xdg-toplevel-icon-v1-client-protocol.h"
#include <wayland-client.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *shell;
static struct xdg_toplevel_icon_manager_v1 *manager;
static struct wl_surface *surface;
static struct xdg_surface *xdg;
static struct xdg_toplevel *top;
static int configured;
static struct wl_buffer *buffer(int edge, uint32_t color) {
  char path[] = "/tmp/waybridge-icon-fixture-XXXXXX";
  int fd = mkstemp(path); unlink(path);
  size_t length = (size_t)edge * edge * 4;
  if (fd < 0 || ftruncate(fd, (off_t)length) != 0) exit(3);
  uint32_t *pixels = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pixels == MAP_FAILED) exit(3);
  for (size_t i = 0; i < length / 4; i++) pixels[i] = color;
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)length);
  struct wl_buffer *result = wl_shm_pool_create_buffer(pool, 0, edge, edge, edge * 4, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool); munmap(pixels, length); close(fd);
  return result;
}
static void configure(void *data, struct xdg_surface *target, uint32_t serial) {
  (void)data; xdg_surface_ack_configure(target, serial);
  if (!configured) {
    struct wl_buffer *paint = buffer(64, 0xffffffffu);
    wl_surface_attach(surface, paint, 0, 0); wl_surface_damage(surface, 0, 0, 64, 64);
    wl_surface_commit(surface); configured = 1;
  }
}
static const struct xdg_surface_listener configure_listener = {configure};
static void ping(void *data, struct xdg_wm_base *target, uint32_t serial) { (void)data; xdg_wm_base_pong(target, serial); }
static const struct xdg_wm_base_listener shell_listener = {ping};
static void size(void *data, struct xdg_toplevel_icon_manager_v1 *target, int32_t value) { (void)data; (void)target; (void)value; }
static void done(void *data, struct xdg_toplevel_icon_manager_v1 *target) { (void)data; (void)target; }
static const struct xdg_toplevel_icon_manager_v1_listener manager_listener = {size, done};
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
  (void)data; (void)version;
  if (!strcmp(interface, "wl_compositor")) compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
  if (!strcmp(interface, "wl_shm")) shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  if (!strcmp(interface, "xdg_wm_base")) { shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(shell, &shell_listener, NULL); }
  if (!strcmp(interface, "xdg_toplevel_icon_manager_v1")) { manager = wl_registry_bind(registry, name, &xdg_toplevel_icon_manager_v1_interface, 1); xdg_toplevel_icon_manager_v1_add_listener(manager, &manager_listener, NULL); }
}
static void removed(void *data, struct wl_registry *registry, uint32_t name) { (void)data; (void)registry; (void)name; }
static const struct wl_registry_listener registry_listener = {global, removed};
static void wayland_command(const char *command) {
  if (!strcmp(command, "clear")) xdg_toplevel_icon_manager_v1_set_icon(manager, top, NULL);
  else if (!strcmp(command, "commit")) { wl_surface_commit(surface); return; }
  else if (!strcmp(command, "identity")) { xdg_toplevel_set_app_id(top, "fixture-native-alias"); wl_surface_commit(surface); return; }
  else if (!strcmp(command, "canonicalidentity")) { xdg_toplevel_set_app_id(top, "fixture-runtime-class"); wl_surface_commit(surface); return; }
  else if (!strcmp(command, "caseidentity")) { xdg_toplevel_set_app_id(top, "CASE-DESKTOP"); wl_surface_commit(surface); return; }
  else if (!strcmp(command, "slow")) { xdg_toplevel_set_app_id(top, "org.waybridge.Slow"); wl_surface_commit(surface); return; }
  else {
    struct xdg_toplevel_icon_v1 *icon = xdg_toplevel_icon_manager_v1_create_icon(manager);
    struct wl_buffer *pixels = NULL;
    if (!strcmp(command, "named")) xdg_toplevel_icon_v1_set_name(icon, "bridge-svg");
    else if (strcmp(command, "empty")) {
      uint32_t color = !strcmp(command, "blue") ? 0xff0000ffu : !strcmp(command, "alpha") ? 0x80400000u : 0xff00ff00u;
      pixels = buffer(!strcmp(command, "huge") ? 1024 : 128, color);
      xdg_toplevel_icon_v1_add_buffer(icon, pixels, 1);
      if (!strcmp(command, "both")) xdg_toplevel_icon_v1_set_name(icon, "bridge-svg");
    }
    if (!strcmp(command, "bad-destroy")) { wl_buffer_destroy(pixels); return; }
    xdg_toplevel_icon_manager_v1_set_icon(manager, top, icon);
    // Protocol explicitly permits immediate icon destruction, followed by its
    // buffers. This also exercises deferred surface commit of a copied snapshot.
    xdg_toplevel_icon_v1_destroy(icon); if (pixels) wl_buffer_destroy(pixels);
  }
  if (strcmp(command, "stage")) wl_surface_commit(surface);
}
static int wayland(void) {
  display = wl_display_connect(NULL); if (!display) return 2;
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL); wl_display_roundtrip(display);
  if (!manager || !shm || !shell || !compositor) return 3;
  surface = wl_compositor_create_surface(compositor); xdg = xdg_wm_base_get_xdg_surface(shell, surface);
  xdg_surface_add_listener(xdg, &configure_listener, NULL); top = xdg_surface_get_toplevel(xdg);
  xdg_toplevel_set_title(top, "Wayland runtime icon fixture"); xdg_toplevel_set_app_id(top, "org.waybridge.Png");
  wl_surface_commit(surface); while (!configured && wl_display_dispatch(display) >= 0) { }
  printf("READY\n"); fflush(stdout);
  for (;;) {
    wl_display_dispatch_pending(display); wl_display_flush(display);
    struct pollfd fds[] = {{STDIN_FILENO, POLLIN, 0}, {wl_display_get_fd(display), POLLIN, 0}};
    if (poll(fds, 2, -1) < 0) break;
    if (fds[1].revents & POLLIN) { if (wl_display_dispatch(display) < 0) break; }
    if (fds[0].revents & (POLLIN | POLLHUP)) {
      char command[32]; if (!fgets(command, sizeof(command), stdin)) break;
      command[strcspn(command, "\n")] = 0; if (!strcmp(command, "quit")) break;
      wayland_command(command); if (wl_display_roundtrip(display) < 0) {
        const struct wl_interface *interface = NULL; uint32_t object = 0;
        uint32_t error = wl_display_get_protocol_error(display, &interface, &object);
        fprintf(stderr,"ROUNDTRIP ERROR errno=%d protocol=%u iface=%s object=%u\n",wl_display_get_error(display),error,interface?interface->name:"none",object); fflush(stderr); break;
      }
      printf("ACK %s\n", command); fflush(stdout);
    }
  }
  wl_display_disconnect(display); return 0;
}
static int x11(const char *mode) {
  Display *connection = XOpenDisplay(NULL); if (!connection) return 2;
  Window window = XCreateSimpleWindow(connection, DefaultRootWindow(connection), 0, 0, 160, 100, 0, 0, 0xffffff);
  XClassHint hint = {(char *)"fixture-instance", (char *)"fixture-runtime-class"};
  if (strcmp(mode, "x11-empty")) XSetClassHint(connection, window, &hint);
  XStoreName(connection, window, "X11 runtime icon fixture");
  Atom icon = XInternAtom(connection, "_NET_WM_ICON", False), cardinal = XInternAtom(connection, "CARDINAL", False);
  if (strcmp(mode, "x11")) {
    unsigned long words[128 * 128 + 2] = {128, 128};
    for (size_t i = 2; i < sizeof(words) / sizeof(words[0]); i++) words[i] = 0xff00ff00u;
    XChangeProperty(connection, window, icon, cardinal, 32, PropModeReplace, (unsigned char *)words, (int)(sizeof(words)/sizeof(words[0])));
  }
  Atom supported = XInternAtom(connection, "_NET_SUPPORTED", False), type;
  int format = 0; unsigned long items = 0, after = 0; unsigned char *values = NULL;
  if (XGetWindowProperty(connection, DefaultRootWindow(connection), supported, 0, 4096, False,
                        XA_ATOM, &type, &format, &items, &after, &values) != Success) return 3;
  int advertised = 0;
  if (format == 32) for (unsigned long i = 0; i < items; i++) if (((Atom *)values)[i] == icon) advertised = 1;
  if (values) XFree(values);
  if (!advertised) return 3;
  XMapWindow(connection, window); XSync(connection, False); printf("READY\n"); fflush(stdout);
  char command[32];
  while (fgets(command, sizeof(command), stdin)) {
    command[strcspn(command, "\n")] = 0; if (!strcmp(command, "quit")) break;
    if (!strcmp(command, "clear")) XDeleteProperty(connection, window, icon);
    else if (!strcmp(command, "instance")) { hint.res_name = (char *)"fixture-instance-match"; hint.res_class = (char *)"unmatched-class"; XSetClassHint(connection, window, &hint); }
    else if (!strcmp(command, "shadow")) { hint.res_class = (char *)"fixture-shadow"; hint.res_name = (char *)"unmatched-instance"; XSetClassHint(connection, window, &hint); }
    else if (!strcmp(command, "case")) { hint.res_class = (char *)"CASE-DESKTOP"; hint.res_name = (char *)"unmatched-instance"; XSetClassHint(connection, window, &hint); }
    else {
      int edge = !strcmp(command, "huge") ? 1024 : 128;
      size_t count = (size_t)edge * edge + 2; unsigned long *words = calloc(count, sizeof(*words));
      if (!words) return 3;
      words[0] = edge; words[1] = edge;
      unsigned long color = !strcmp(command, "blue") ? 0xff0000ffu : 0xff00ff00u;
      for (size_t i = 2; i < count; i++) words[i] = color;
      if (!strcmp(command, "bad")) words[0] = 0xffffffffu;
      if (!strcmp(command, "stall")) XGrabServer(connection);
      XChangeProperty(connection, window, icon, cardinal, 32, PropModeReplace, (unsigned char *)words, (int)count); free(words);
      if (!strcmp(command, "stall")) {
        XSync(connection, False); printf("STALLED\n"); fflush(stdout);
        struct timespec delay = {0, 800000000}; nanosleep(&delay, NULL);
        XUngrabServer(connection);
      }
    }
    XSync(connection, False); printf("ACK %s\n", command); fflush(stdout);
  }
  XDestroyWindow(connection, window); XCloseDisplay(connection); return 0;
}
int main(int argc, char **argv) { return argc == 2 && !strncmp(argv[1], "x11", 3) ? x11(argv[1]) : wayland(); }
