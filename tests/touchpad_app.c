/* Real pointer protocol subscriber on the standard Wayland fixture. */
#define main base_fixture_main
#include "test_app.c"
#undef main
#include "pointer-gestures-client-protocol.h"

static struct zwp_pointer_gestures_v1 *gestures;

static void touchpad_global(void *data, struct wl_registry *registry, uint32_t name,
                           const char *interface, uint32_t version) {
  global(data, registry, name, interface, version);
  if (!strcmp(interface, zwp_pointer_gestures_v1_interface.name))
    gestures = wl_registry_bind(registry, name, &zwp_pointer_gestures_v1_interface, 1);
}

static void scroll_source(void *data, struct wl_pointer *pointer, uint32_t source) {
  (void)data; (void)pointer;
  printf("SCROLL_SOURCE %u\n", source);
}
static void scroll_update(void *data, struct wl_pointer *pointer, uint32_t time,
                          uint32_t axis, wl_fixed_t value) {
  (void)data; (void)pointer; (void)time;
  printf("SCROLL_UPDATE %u %.2f\n", axis, wl_fixed_to_double(value));
}
static void scroll_stop(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis) {
  (void)data; (void)pointer; (void)time;
  printf("SCROLL_STOP %u\n", axis);
}
static void pinch_begin(void *data, struct zwp_pointer_gesture_pinch_v1 *pinch, uint32_t serial,
                        uint32_t time, struct wl_surface *surface, uint32_t fingers) {
  (void)data; (void)pinch; (void)serial; (void)time;
  printf("PINCH_BEGIN %d %u\n", surface == windows[0].surface ? 1 : 2, fingers);
}
static void pinch_update(void *data, struct zwp_pointer_gesture_pinch_v1 *pinch, uint32_t time,
                         wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t scale, wl_fixed_t rotation) {
  (void)data; (void)pinch; (void)time; (void)dx; (void)dy; (void)rotation;
  printf("PINCH_UPDATE %.2f\n", wl_fixed_to_double(scale));
}
static void pinch_end(void *data, struct zwp_pointer_gesture_pinch_v1 *pinch, uint32_t serial,
                      uint32_t time, int32_t cancelled) {
  (void)data; (void)pinch; (void)serial; (void)time;
  printf("PINCH_END %d\n", cancelled);
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  display = wl_display_connect(NULL);
  if (!display) return 1;
  const struct wl_registry_listener registry = {.global = touchpad_global, .global_remove = removed};
  wl_registry_add_listener(wl_display_get_registry(display), &registry, NULL);
  if (wl_display_roundtrip(display) < 0 || !gestures || !seat || !shell || !shm) return 2;
  struct wl_pointer *pointer = wl_seat_get_pointer(seat);
  struct wl_pointer_listener pointer_events = pointer_listener;
  pointer_events.axis_source = scroll_source;
  pointer_events.axis = scroll_update;
  pointer_events.axis_stop = scroll_stop;
  wl_pointer_add_listener(pointer, &pointer_events, NULL);
  const struct zwp_pointer_gesture_pinch_v1_listener pinch_events = {
      .begin = pinch_begin, .update = pinch_update, .end = pinch_end};
  struct zwp_pointer_gesture_pinch_v1 *pinch = zwp_pointer_gestures_v1_get_pinch_gesture(gestures, pointer);
  zwp_pointer_gesture_pinch_v1_add_listener(pinch, &pinch_events, NULL);
  const char *first_title = getenv("WAYBRIDGE_TOUCHPAD_FIXTURE_TITLE");
  if (!first_title) first_title = "Touchpad first";
  for (int i = 0; i < 2; ++i) {
    struct window *window = &windows[i];
    window->id = i + 1;
    window->width = 400;
    window->height = 300;
    window->surface = wl_compositor_create_surface(compositor);
    window->xdg = xdg_wm_base_get_xdg_surface(shell, window->surface);
    xdg_surface_add_listener(window->xdg, &surface_listener, window);
    window->top = xdg_surface_get_toplevel(window->xdg);
    xdg_toplevel_add_listener(window->top, &top_listener, window);
    xdg_toplevel_set_title(window->top, i ? "Touchpad second" : first_title);
    wl_surface_commit(window->surface);
  }
  while (wl_display_dispatch(display) >= 0) {}
  return 0;
}
