// Real xdg-dialog/fullscreen/input-region protocol actions; opt-in test fixture.
#include "xdg-dialog-client-protocol.h"
static struct xdg_wm_dialog_v1 *dialog_manager;
static struct xdg_dialog_v1 *test_dialog;
static struct wl_subcompositor *subcompositor;
static struct wl_surface *input_child;
static struct wl_subsurface *input_subsurface;
static int keyboard_window;
static void semantics_child(void) {
  if (input_child || !subcompositor)
    return;
  input_child = wl_compositor_create_surface(compositor);
  input_subsurface =
      wl_subcompositor_get_subsurface(subcompositor, input_child, windows[0].surface);
  wl_subsurface_set_position(input_subsurface, windows[0].width / 2 + 20, 70);
  int fd = memfd_create("input-child", MFD_CLOEXEC);
  size_t size = 32 * 32 * 4;
  if (fd < 0 || ftruncate(fd, size))
    exit(2);
  struct pixels *pixels = calloc(1, sizeof(*pixels));
  pixels->size = size;
  pixels->data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  for (int i = 0; i < 32 * 32; i++)
    ((uint32_t *)pixels->data)[i] = 0xff00ff00;
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int)size);
  pixels->buffer = wl_shm_pool_create_buffer(pool, 0, 32, 32, 32 * 4, WL_SHM_FORMAT_ARGB8888);
  close(fd);
  wl_shm_pool_destroy(pool);
  wl_buffer_add_listener(pixels->buffer, &buffer_listener, pixels);
  wl_surface_attach(input_child, pixels->buffer, 0, 0);
  wl_surface_damage_buffer(input_child, 0, 0, 32, 32);
  wl_surface_commit(input_child);
}
static void semantics_key(uint32_t key) {
  if (!getenv("WAYBRIDGE_TEST_SEMANTICS"))
    return;
  struct window *main = &windows[0], *dialog = &windows[2];
  if (key == 30 && main->surface) {
    if (main->fullscreen)
      xdg_toplevel_unset_fullscreen(main->top);
    else
      xdg_toplevel_set_fullscreen(main->top, NULL);
  }
  if (key == 50 && main->surface) {
    if (main->maximized)
      xdg_toplevel_unset_maximized(main->top);
    else
      xdg_toplevel_set_maximized(main->top);
  }
  if (key == 19 && main->surface)
    xdg_toplevel_unset_maximized(main->top);
  if ((key == 49 || key == 31) && main->surface) {
    xdg_toplevel_set_min_size(main->top, key == 49 ? 320 : 0, key == 49 ? 200 : 0);
    xdg_toplevel_set_max_size(main->top, key == 49 ? 640 : 0, key == 49 ? 480 : 0);
    wl_surface_commit(main->surface);
  }
  if ((key == 25 || key == 48 || key == 22) && dialog->surface) {
    xdg_toplevel_set_parent(dialog->top, key == 25   ? windows[0].top
                                         : key == 48 ? windows[1].top
                                                     : NULL);
    wl_surface_commit(dialog->surface);
  }
  if ((key == 32 || key == 33 || key == 34) && dialog->surface && dialog_manager) {
    if (!test_dialog)
      test_dialog = xdg_wm_dialog_v1_get_xdg_dialog(dialog_manager, dialog->top);
    if (key == 32)
      xdg_dialog_v1_set_modal(test_dialog);
    else if (key == 33)
      xdg_dialog_v1_unset_modal(test_dialog);
    else {
      xdg_dialog_v1_destroy(test_dialog);
      test_dialog = NULL;
    }
  }
  if ((key == 23 || key == 36 || key == 37) && main->surface) {
    if (input_child) {
      wl_subsurface_destroy(input_subsurface);
      wl_surface_destroy(input_child);
      input_child = NULL;
      input_subsurface = NULL;
    }
    if (key == 36)
      wl_surface_set_input_region(main->surface, NULL);
    else {
      struct wl_region *region = wl_compositor_create_region(compositor);
      if (key == 23) {
        wl_region_add(region, 0, 0, main->width / 2, main->height);
        wl_region_subtract(region, 20, 20, 30, 30);
        semantics_child();
      }
      wl_surface_set_input_region(main->surface, region);
      wl_region_destroy(region);
    }
    wl_surface_commit(main->surface);
  }
  if (key == 35 && main->surface) {
    main->hidden = 1;
    wl_surface_attach(main->surface, NULL, 0, 0);
    wl_surface_commit(main->surface);
  }
  if (key == 47 && main->surface) {
    main->hidden = 0;
    xdg_toplevel_set_title(main->top, "Waybridge semantics 1");
    xdg_toplevel_set_app_id(main->top, "waybridge.fixture");
    wl_surface_commit(main->surface);
    wl_display_roundtrip(display);
    draw(main);
  }
  if (key == 24 && main->surface && !main->hidden) {
    main->sem_invert = !main->sem_invert;
    draw(main);
  }
  if (key == 45 && main->surface)
    closed(main, main->top);
  if (key == 46 && dialog->surface)
    closed(dialog, dialog->top);
  printf("SEM_KEY %d %u\n", keyboard_window, key);
  fflush(stdout);
}
