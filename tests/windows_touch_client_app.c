/* Actual production Windows PT_TOUCH -> wl_touch -> xdg interaction fixture. */
#define main base_fixture_main
#include "test_app.c"
#undef main
static void client_touch_down(void *data, struct wl_touch *touch, uint32_t serial, uint32_t time,
                              struct wl_surface *surface, int32_t id, wl_fixed_t x, wl_fixed_t y) {
  (void)data; (void)touch; (void)time; (void)surface;
  double px = wl_fixed_to_double(x);
  printf("CLIENT_TOUCH_DOWN %d %.3f %.3f\n", id, px, wl_fixed_to_double(y));
  if (px >= windows[0].width * 2.0 / 3) {
    xdg_toplevel_resize(windows[0].top, seat, serial, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
    puts("CLIENT_REQUEST_RESIZE");
  } else if (px >= windows[0].width / 3.0) {
    xdg_toplevel_move(windows[0].top, seat, serial);
    puts("CLIENT_REQUEST_MOVE");
  }
  fflush(stdout);
}
static void client_touch_up(void *d, struct wl_touch *t, uint32_t serial, uint32_t time, int32_t id) {
  (void)d; (void)t; (void)serial; (void)time;
  printf("CLIENT_TOUCH_UP %d\n", id); fflush(stdout);
}
static void client_touch_motion(void *d, struct wl_touch *t, uint32_t time, int32_t id, wl_fixed_t x, wl_fixed_t y) {
  (void)d; (void)t; (void)time;
  printf("CLIENT_TOUCH_MOTION %d %.3f %.3f\n", id, wl_fixed_to_double(x), wl_fixed_to_double(y)); fflush(stdout);
}
static void client_touch_frame(void *d, struct wl_touch *t) { (void)d; (void)t; puts("CLIENT_TOUCH_FRAME"); fflush(stdout); }
static void client_touch_cancel(void *d, struct wl_touch *t) { (void)d; (void)t; puts("CLIENT_TOUCH_CANCEL"); fflush(stdout); }
static const struct wl_touch_listener client_touch_listener = {
  .down=client_touch_down,.up=client_touch_up,.motion=client_touch_motion,.frame=client_touch_frame,.cancel=client_touch_cancel
};
int main(void) {
  display=wl_display_connect(NULL); if(!display)return 1;
  wl_registry_add_listener(wl_display_get_registry(display),&registry_listener,NULL);
  wl_display_roundtrip(display); wl_display_roundtrip(display);
  if(!seat||!shm||!shell)return 2;
  wl_touch_add_listener(wl_seat_get_touch(seat),&client_touch_listener,NULL);
  struct window *w=&windows[0]; w->id=1; w->width=420; w->height=280;
  w->surface=wl_compositor_create_surface(compositor);
  w->xdg=xdg_wm_base_get_xdg_surface(shell,w->surface);
  xdg_surface_add_listener(w->xdg,&surface_listener,w);
  w->top=xdg_surface_get_toplevel(w->xdg);
  xdg_toplevel_add_listener(w->top,&top_listener,w);
  xdg_toplevel_set_title(w->top,getenv("WAYBRIDGE_TOUCH_FIXTURE_TITLE"));
  xdg_toplevel_set_app_id(w->top,"waybridge.touch.client.fixture");
  wl_surface_commit(w->surface);
  puts("CLIENT_READY"); fflush(stdout);
  while(wl_display_dispatch(display)>=0){}
  return 0;
}
