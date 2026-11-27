// An incompressible real Wayland buffer keeps a 1 Mbps datagram send in progress at disconnect.
#define _GNU_SOURCE
#include "xdg-shell-client-protocol.h"
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
static struct wl_surface *surface;
static int width = 960, height = 640;
struct pixels {
  struct wl_buffer *buffer;
  void *data;
  size_t size;
};
static void release(void *data, struct wl_buffer *buffer) {
  struct pixels *pixels = data;
  wl_buffer_destroy(buffer);
  munmap(pixels->data, pixels->size);
  free(pixels);
}
static const struct wl_buffer_listener buffer_listener = {.release = release};
static void configure(void *data, struct xdg_surface *xdg, uint32_t serial) {
  (void)data;
  xdg_surface_ack_configure(xdg, serial);
  size_t size = (size_t)width * height * 4;
  int fd = memfd_create("udp-noise-fixture", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, (off_t)size))
    exit(2);
  struct pixels *pixels = calloc(1, sizeof(*pixels));
  pixels->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pixels->data == MAP_FAILED)
    exit(2);
  pixels->size = size;
  for (size_t i = 0; i < size / 4; ++i) {
    uint32_t value = (uint32_t)i * 2654435761u;
    value ^= value >> 13;
    value *= 2246822519u;
    value ^= value >> 16;
    ((uint32_t *)pixels->data)[i] = 0xff000000u | (value & 0xffffffu);
  }
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
  pixels->buffer =
      wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  wl_buffer_add_listener(pixels->buffer, &buffer_listener, pixels);
  wl_surface_attach(surface, pixels->buffer, 0, 0);
  wl_surface_damage(surface, 0, 0, width, height);
  wl_surface_commit(surface);
}
static const struct xdg_surface_listener surface_listener = {.configure = configure};
static void sized(void *data, struct xdg_toplevel *top, int32_t w, int32_t h,
                  struct wl_array *states) {
  (void)data;
  (void)top;
  (void)states;
  if (w > 0 && w <= 4096)
    width = w;
  if (h > 0 && h <= 4096)
    height = h;
}
static void closed(void *data, struct xdg_toplevel *top) {
  (void)data;
  (void)top;
  exit(0);
}
static const struct xdg_toplevel_listener top_listener = {.configure = sized, .close = closed};
static void ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
  (void)data;
  xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener shell_listener = {.ping = ping};
static void global(void *data, struct wl_registry *registry, uint32_t id, const char *name,
                   uint32_t version) {
  (void)data;
  (void)version;
  if (!strcmp(name, "wl_compositor"))
    compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 1);
  if (!strcmp(name, "wl_shm"))
    shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
  if (!strcmp(name, "xdg_wm_base")) {
    shell = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1);
    xdg_wm_base_add_listener(shell, &shell_listener, NULL);
  }
}
static void removed(void *data, struct wl_registry *registry, uint32_t id) {
  (void)data;
  (void)registry;
  (void)id;
}
static const struct wl_registry_listener registry_listener = {.global = global,
                                                              .global_remove = removed};
int main(int argc, char **argv) {
  if (argc == 3) {
    width = atoi(argv[1]);
    height = atoi(argv[2]);
    if (width < 16 || height < 16 || width > 4096 || height > 4096)
      return 1;
  }
  display = wl_display_connect(NULL);
  if (!display)
    return 1;
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  if (wl_display_roundtrip(display) < 0 || !compositor || !shm || !shell)
    return 2;
  surface = wl_compositor_create_surface(compositor);
  struct xdg_surface *xdg = xdg_wm_base_get_xdg_surface(shell, surface);
  xdg_surface_add_listener(xdg, &surface_listener, NULL);
  struct xdg_toplevel *top = xdg_surface_get_toplevel(xdg);
  xdg_toplevel_add_listener(top, &top_listener, NULL);
  xdg_toplevel_set_title(top, "Waybridge UDP noise fixture");
  wl_surface_commit(surface);
  while (wl_display_dispatch(display) >= 0) {
  }
  wl_display_disconnect(display);
  return 0;
}
