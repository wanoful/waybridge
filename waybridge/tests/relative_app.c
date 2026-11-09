/* Reuse the real shm/xdg fixture, adding real pointer-constraint resources. */
#define main base_fixture_main
#include "test_app.c"
#undef main
#include "relative-pointer-client-protocol.h"
#include "pointer-constraints-client-protocol.h"
#include <poll.h>

static struct zwp_relative_pointer_manager_v1 *relative_manager;
static struct zwp_pointer_constraints_v1 *constraints;
static struct zwp_locked_pointer_v1 *locked;
static struct zwp_confined_pointer_v1 *confined;
static struct wl_pointer *lock_pointer;
static void rel_motion(void *data, struct zwp_relative_pointer_v1 *p, uint32_t hi, uint32_t lo,
                       wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t ux, wl_fixed_t uy) {
  (void)data; (void)p;
  printf("REL %llu %.3f %.3f %.3f %.3f\n", (unsigned long long)(((uint64_t)hi << 32) | lo),
         wl_fixed_to_double(dx), wl_fixed_to_double(dy), wl_fixed_to_double(ux), wl_fixed_to_double(uy));
  fflush(stdout);
}
static const struct zwp_relative_pointer_v1_listener rel_listener = {.relative_motion = rel_motion};
static void lock_on(void *d, struct zwp_locked_pointer_v1 *p) {
  (void)d; (void)p; puts("LOCKED"); fflush(stdout);
}
static void lock_off(void *d, struct zwp_locked_pointer_v1 *p) {
  (void)d; (void)p; puts("UNLOCKED"); fflush(stdout);
}
static const struct zwp_locked_pointer_v1_listener lock_listener = {.locked = lock_on, .unlocked = lock_off};
static void confine_on(void *d, struct zwp_confined_pointer_v1 *p) {
  (void)d; (void)p; puts("CONFINED"); fflush(stdout);
}
static void confine_off(void *d, struct zwp_confined_pointer_v1 *p) {
  (void)d; (void)p; puts("UNCONFINED"); fflush(stdout);
}
static const struct zwp_confined_pointer_v1_listener confine_listener = {.confined = confine_on, .unconfined = confine_off};
static void bind_global(void *d, struct wl_registry *r, uint32_t name, const char *interface, uint32_t version) {
  global(d, r, name, interface, version);
  if (!strcmp(interface, "zwp_relative_pointer_manager_v1"))
    relative_manager = wl_registry_bind(r, name, &zwp_relative_pointer_manager_v1_interface, 1);
  if (!strcmp(interface, "zwp_pointer_constraints_v1"))
    constraints = wl_registry_bind(r, name, &zwp_pointer_constraints_v1_interface, 1);
}
static const struct wl_registry_listener relative_registry = {.global = bind_global, .global_remove = removed};
static void clear_constraint(void) {
  if (locked) { zwp_locked_pointer_v1_destroy(locked); locked = NULL; }
  if (confined) { zwp_confined_pointer_v1_destroy(confined); confined = NULL; }
}
static void make_lock(uint32_t lifetime) {
  clear_constraint();
  locked = zwp_pointer_constraints_v1_lock_pointer(constraints, windows[0].surface, lock_pointer, NULL, lifetime);
  zwp_locked_pointer_v1_add_listener(locked, &lock_listener, NULL);
}
int main(void) {
  display = wl_display_connect(NULL);
  if (!display) return 1;
  wl_registry_add_listener(wl_display_get_registry(display), &relative_registry, NULL);
  wl_display_roundtrip(display);
  wl_display_roundtrip(display);
  if (!relative_manager || !constraints || !seat || !shm || !shell) return 2;
  lock_pointer = wl_seat_get_pointer(seat);
  struct zwp_relative_pointer_v1 *relative = zwp_relative_pointer_manager_v1_get_relative_pointer(relative_manager, lock_pointer);
  zwp_relative_pointer_v1_add_listener(relative, &rel_listener, NULL);
  for (int i = 0; i < 2; ++i) {
    struct window *w = &windows[i];
    w->id = i + 1; w->width = 400; w->height = 300;
    w->surface = wl_compositor_create_surface(compositor);
    w->xdg = xdg_wm_base_get_xdg_surface(shell, w->surface);
    xdg_surface_add_listener(w->xdg, &surface_listener, w);
    w->top = xdg_surface_get_toplevel(w->xdg);
    xdg_toplevel_add_listener(w->top, &top_listener, w);
    xdg_toplevel_set_title(w->top, i ? "Relative fixture other" : "Relative fixture lock");
    wl_surface_commit(w->surface);
  }
  make_lock(ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
  wl_display_flush(display);
  puts("READY"); fflush(stdout);
  struct pollfd fds[] = {{wl_display_get_fd(display), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
  while (1) {
    if (wl_display_dispatch_pending(display) < 0 || wl_display_flush(display) < 0) break;
    if (poll(fds, 2, -1) < 0) break;
    if (fds[0].revents & POLLIN)
      if (wl_display_dispatch(display) < 0) break;
    if (fds[1].revents & POLLIN) {
      char command[128];
      if (!fgets(command, sizeof(command), stdin)) break;
      if (!strncmp(command, "cursor_bitmap", 13) || !strncmp(command, "cursor_text", 11)) {
        setenv("WAYBRIDGE_TEST_CURSOR", "1", 1);
        cursor_mode = -1;
        cursor_select(!strncmp(command, "cursor_bitmap", 13) ? 200 : 0);
        unsetenv("WAYBRIDGE_TEST_CURSOR");
      } else if (!strncmp(command, "destroy_surface", 15)) {
        closed(&windows[0], windows[0].top);
      } else if (!strncmp(command, "destroy", 7)) {
        clear_constraint(); puts("DESTROYED");
      } else if (!strncmp(command, "oneshot", 7)) {
        make_lock(ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_ONESHOT);
      } else if (!strncmp(command, "persistent", 10)) {
        make_lock(ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
      } else if (!strncmp(command, "confine", 7)) {
        clear_constraint();
        confined = zwp_pointer_constraints_v1_confine_pointer(constraints, windows[0].surface, lock_pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
        zwp_confined_pointer_v1_add_listener(confined, &confine_listener, NULL);
      } else if (!strncmp(command, "region", 6) && locked) {
        struct wl_region *r = wl_compositor_create_region(compositor);
        wl_region_add(r, 0, 0, 10, 10);
        zwp_locked_pointer_v1_set_region(locked, r);
        wl_region_destroy(r); wl_surface_commit(windows[0].surface);
      } else if (!strncmp(command, "unmap", 5)) {
        wl_surface_attach(windows[0].surface, NULL, 0, 0); wl_surface_commit(windows[0].surface);
      } else if (!strncmp(command, "remap", 5)) {
        draw(&windows[0]);
      } else if (!strncmp(command, "quit", 4)) break;
      puts("COMMAND"); fflush(stdout);
    }
  }
  return 0;
}
