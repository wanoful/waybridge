#define _POSIX_C_SOURCE 200809L
#include "xdg-shell-client-protocol.h"
#include <wayland-client.h>
#include <errno.h>
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
static struct wl_seat *seat;
static struct wl_data_device_manager *manager;
static struct xdg_wm_base *shell;
static struct wl_surface *surfaces[2];
static struct xdg_surface *xdg[2];
static int configured[2];
struct offer { struct wl_data_offer *resource; uint32_t serial; int entered; };
static struct offer offers[32];
static int count, entered;
static const char *mime = "text/plain;charset=utf-8";
static int64_t milliseconds(void) {
  struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static struct wl_buffer *paint(void) {
  char file[] = "/tmp/waybridge-offer-fixture-XXXXXX";
  int fd = mkstemp(file); unlink(file);
  if (fd < 0 || ftruncate(fd, 64 * 64 * 4) != 0) exit(2);
  uint32_t *pixels = mmap(NULL, 64 * 64 * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pixels == MAP_FAILED) exit(2);
  for (int i = 0; i < 64 * 64; i++) pixels[i] = 0xffffffff;
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, 64 * 64 * 4);
  struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, 64, 64, 64 * 4, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool); munmap(pixels, 64 * 64 * 4); close(fd); return buffer;
}
static void configure(void *data, struct xdg_surface *surface, uint32_t serial) {
  int index = (int)(intptr_t)data; xdg_surface_ack_configure(surface, serial);
  if (!configured[index]) {
    wl_surface_attach(surfaces[index], paint(), 0, 0);
    wl_surface_damage(surfaces[index], 0, 0, 64, 64);
    wl_surface_commit(surfaces[index]); configured[index] = 1;
  }
}
static const struct xdg_surface_listener configure_listener = {configure};
static void ping(void *data, struct xdg_wm_base *source, uint32_t serial) { (void)data; xdg_wm_base_pong(source, serial); }
static const struct xdg_wm_base_listener shell_listener = {ping};
static void offered(void *data, struct wl_data_offer *offer, const char *type) { (void)data; (void)offer; (void)type; }
static void actions(void *data, struct wl_data_offer *offer, uint32_t value) { (void)data; (void)offer; (void)value; }
static const struct wl_data_offer_listener offer_listener = {offered, actions, actions};
static void data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *offer) {
  (void)data; (void)device;
  if (++count >= 32) exit(3);
  offers[count].resource = offer;
  wl_data_offer_add_listener(offer, &offer_listener, NULL);
}
static void enter(void *data, struct wl_data_device *device, uint32_t serial, struct wl_surface *surface,
                  wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer) {
  (void)data; (void)device; (void)x; (void)y;
  for (int i = 1; i <= count; i++) if (offers[i].resource == offer) entered = i;
  offers[entered].serial = serial; offers[entered].entered = 1;
  int accept = surface == surfaces[0];
  wl_data_offer_accept(offer, serial, accept ? mime : NULL);
  wl_data_offer_set_actions(offer, accept ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY : 0,
                            accept ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY : 0);
  printf("ENTER %d %d\n", entered, accept ? 1 : 2); fflush(stdout);
}
static void leave(void *data, struct wl_data_device *device) {
  (void)data; (void)device; printf("LEAVE %d\n", entered); fflush(stdout); entered = 0;
}
static void motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
  (void)data; (void)device; (void)time; (void)x; (void)y;
}
static void drop(void *data, struct wl_data_device *device) {
  (void)data; (void)device;
  // Retain both the offer and its delayed receive/finish until explicitly told.
  printf("DROP %d\n", entered); fflush(stdout);
}
static void selection(void *data, struct wl_data_device *device, struct wl_data_offer *offer) { (void)data; (void)device; (void)offer; }
static const struct wl_data_device_listener device_listener = {data_offer, enter, leave, motion, drop, selection};
static void global(void *data, struct wl_registry *registry, uint32_t id, const char *name, uint32_t version) {
  (void)data;
  if (!strcmp(name, "wl_compositor")) compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
  if (!strcmp(name, "wl_shm")) shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
  if (!strcmp(name, "wl_seat")) seat = wl_registry_bind(registry, id, &wl_seat_interface, version < 3 ? version : 3);
  if (!strcmp(name, "wl_data_device_manager")) manager = wl_registry_bind(registry, id, &wl_data_device_manager_interface, 3);
  if (!strcmp(name, "xdg_wm_base")) { shell = wl_registry_bind(registry, id, &xdg_wm_base_interface, 1); xdg_wm_base_add_listener(shell, &shell_listener, NULL); }
}
static void removed(void *data, struct wl_registry *registry, uint32_t id) { (void)data; (void)registry; (void)id; }
static const struct wl_registry_listener registry_listener = {global, removed};
static void receive(int index) {
  int pipes[2]; if (pipe(pipes) != 0) exit(3);
  fcntl(pipes[0], F_SETFL, O_NONBLOCK);
  wl_data_offer_receive(offers[index].resource, mime, pipes[1]); close(pipes[1]);
  if (wl_display_roundtrip(display) < 0) exit(3);
  char *bytes = malloc(1024 * 1024 + 1); size_t length = 0;
  int64_t deadline = milliseconds() + 3000;
  for (;;) {
    ssize_t n = read(pipes[0], bytes + length, 1024 * 1024 + 1 - length);
    if (n == 0) break;
    if (n > 0) { length += (size_t)n; if (length > 1024 * 1024) exit(3); continue; }
    if (errno != EAGAIN && errno != EINTR) exit(3);
    int64_t remaining = deadline - milliseconds(); if (remaining <= 0) exit(3);
    struct pollfd ready = {pipes[0], POLLIN, 0}; if (poll(&ready, 1, (int)remaining) < 0 && errno != EINTR) exit(3);
  }
  close(pipes[0]); printf("READ %d ", index);
  for (size_t i = 0; i < length; i++) printf("%02x", (unsigned char)bytes[i]);
  printf("\n"); fflush(stdout); free(bytes);
}
int main(void) {
  display = wl_display_connect(NULL); if (!display) return 2;
  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL); wl_display_roundtrip(display);
  if (!seat || !manager || !shell || !compositor || !shm) return 2;
  struct wl_data_device *device = wl_data_device_manager_get_data_device(manager, seat);
  wl_data_device_add_listener(device, &device_listener, NULL);
  for (int i = 0; i < 2; i++) {
    surfaces[i] = wl_compositor_create_surface(compositor);
    xdg[i] = xdg_wm_base_get_xdg_surface(shell, surfaces[i]);
    xdg_surface_add_listener(xdg[i], &configure_listener, (void *)(intptr_t)i);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xdg[i]);
    xdg_toplevel_set_title(top, i ? "Delayed offer B" : "Delayed offer A");
    xdg_toplevel_set_app_id(top, "org.waybridge.DelayedOffer");
    wl_surface_commit(surfaces[i]);
  }
  while ((!configured[0] || !configured[1]) && wl_display_dispatch(display) >= 0) { }
  printf("READY\n"); fflush(stdout);
  for (;;) {
    wl_display_dispatch_pending(display); wl_display_flush(display);
    struct pollfd fds[] = {{STDIN_FILENO, POLLIN, 0}, {wl_display_get_fd(display), POLLIN, 0}};
    if (poll(fds, 2, -1) < 0) break;
    if (fds[1].revents & POLLIN) { if (wl_display_dispatch(display) < 0) break; }
    if (fds[0].revents & (POLLIN | POLLHUP)) {
      char command[64]; if (!fgets(command, sizeof(command), stdin)) break;
      int index = 0; char operation[24]; if (sscanf(command, "%23s %d", operation, &index) < 1) return 3;
      if (!strcmp(operation, "quit")) break;
      if (index < 1 || index > count) return 3;
      if (!strcmp(operation, "receive")) receive(index);
      else if (!strcmp(operation, "finish")) wl_data_offer_finish(offers[index].resource);
      else if (!strcmp(operation, "accept")) { wl_data_offer_accept(offers[index].resource, offers[index].serial, mime); wl_data_offer_set_actions(offers[index].resource, 1, 1); }
      else if (!strcmp(operation, "noactions")) wl_data_offer_set_actions(offers[index].resource, 0, 0);
      else return 3;
      if (wl_display_roundtrip(display) < 0) return 3;
      printf("ACK %s %d\n", operation, index); fflush(stdout);
    }
  }
  wl_display_disconnect(display); return 0;
}
