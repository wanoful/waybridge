// Optional real wl_pointer.set_cursor fixture, enabled only by WAYBRIDGE_TEST_CURSOR.
#include "cursor-shape-client-protocol.h"
static struct wp_cursor_shape_manager_v1 *cursor_manager;
static struct wp_cursor_shape_device_v1 *cursor_device;
static struct wl_pointer *cursor_pointer;
static struct wl_surface *cursor_surface;
static uint32_t cursor_serial;
static int cursor_mode = -1, cursor_animated_frame;
static void cursor_draw(void);
static void cursor_done(void *data, struct wl_callback *callback, uint32_t time) {
  (void)data; (void)time;
  wl_callback_destroy(callback);
  if (cursor_mode == 3) cursor_draw();
}
static const struct wl_callback_listener cursor_frame_listener = {.done = cursor_done};
static void cursor_draw(void) {
  int width = (cursor_mode == 4 || cursor_mode == 9) ? 600 : 32, height = (cursor_mode == 4 || cursor_mode == 9) ? 600 : 24;
  size_t size = (size_t)width * height * 4;
  int fd = memfd_create("waybridge-cursor", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, size)) exit(2);
  struct pixels *p = calloc(1, sizeof(*p));
  p->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); p->size = size;
  uint32_t *out = p->data;
  int blue = cursor_animated_frame++ & 1;
  for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
    out[y * width + x] = x < width / 3 ? (cursor_mode == 3 && blue ? 0xff0000ff : 0xffff0000)
                           : (x < width * 2 / 3 ? 0x80008000 : 0);
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
  p->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool); close(fd);
  wl_buffer_add_listener(p->buffer, &buffer_listener, p);
  wl_surface_set_buffer_scale(cursor_surface, cursor_mode == 9 ? 8 : 2);
  wl_surface_attach(cursor_surface, p->buffer, 0, 0);
  wl_surface_damage_buffer(cursor_surface, 0, 0, width, height);
  if (cursor_mode == 3)
    wl_callback_add_listener(wl_surface_frame(cursor_surface), &cursor_frame_listener, NULL);
  wl_surface_commit(cursor_surface);
}
static void cursor_select(double x) {
  if (!getenv("WAYBRIDGE_TEST_CURSOR") || !cursor_pointer) return;
  int mode = (int)x / 100;
  if (mode == cursor_mode) return;
  cursor_mode = mode;
  if (mode == 0 || mode == 5) {
    if (!cursor_device && cursor_manager)
      cursor_device = wp_cursor_shape_manager_v1_get_pointer(cursor_manager, cursor_pointer);
    if (cursor_device) wp_cursor_shape_device_v1_set_shape(cursor_device, cursor_serial,
        mode == 0 ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE);
  } else if (mode == 1) {
    wl_pointer_set_cursor(cursor_pointer, cursor_serial, NULL, 0, 0);
  } else {
    if (!cursor_surface) cursor_surface = wl_compositor_create_surface(compositor);
    wl_pointer_set_cursor(cursor_pointer, cursor_serial, cursor_surface, mode == 6 ? -3 : 3, mode == 6 ? 20 : 5);
    if (mode == 7) {
      wl_surface_attach(cursor_surface, NULL, 0, 0); wl_surface_commit(cursor_surface);
    } else if (mode == 8) {
      wl_surface_destroy(cursor_surface); cursor_surface = NULL;
    } else cursor_draw();
  }
}
