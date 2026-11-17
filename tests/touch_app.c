/* Real wl_touch fixture, reusing the project's standard SHM/xdg surfaces. */
#define main base_fixture_main
#include "test_app.c"
#undef main
#include <poll.h>
static unsigned window_id(struct wl_surface *surface) {
  for(unsigned i=0;i<2;++i)if(windows[i].surface==surface)return i+1;
  return 0;
}
static void touched(void *d,struct wl_touch *touch,uint32_t serial,uint32_t time,
                    struct wl_surface *surface,int32_t id,wl_fixed_t x,wl_fixed_t y){
  (void)d;(void)touch;(void)serial;(void)time;
  printf("TOUCH_DOWN %d %u %.3f %.3f\n",id,window_id(surface),wl_fixed_to_double(x),wl_fixed_to_double(y));fflush(stdout);
}
static void lifted(void *d,struct wl_touch *touch,uint32_t serial,uint32_t time,int32_t id){
 (void)d;(void)touch;(void)serial;(void)time;printf("TOUCH_UP %d\n",id);fflush(stdout);
}
static void moved_touch(void *d,struct wl_touch *touch,uint32_t time,int32_t id,wl_fixed_t x,wl_fixed_t y){
 (void)d;(void)touch;(void)time;printf("TOUCH_MOTION %d %.3f %.3f\n",id,wl_fixed_to_double(x),wl_fixed_to_double(y));fflush(stdout);
}
static void touch_frame(void *d,struct wl_touch *touch){(void)d;(void)touch;puts("TOUCH_FRAME");fflush(stdout);}
static void touch_cancel(void *d,struct wl_touch *touch){(void)d;(void)touch;puts("TOUCH_CANCEL");fflush(stdout);}
static const struct wl_touch_listener touch_listener={.down=touched,.up=lifted,.motion=moved_touch,.frame=touch_frame,.cancel=touch_cancel};
int main(void){
 display=wl_display_connect(NULL);if(!display)return 1;
 wl_registry_add_listener(wl_display_get_registry(display),&registry_listener,NULL);
 wl_display_roundtrip(display);wl_display_roundtrip(display);
 if(!seat||!shm||!shell)return 2;
 wl_touch_add_listener(wl_seat_get_touch(seat),&touch_listener,NULL);
 for(int i=0;i<2;++i){
  struct window *w=&windows[i];w->id=i+1;w->width=400;w->height=300;
  w->surface=wl_compositor_create_surface(compositor);
  w->xdg=xdg_wm_base_get_xdg_surface(shell,w->surface);
  xdg_surface_add_listener(w->xdg,&surface_listener,w);
  w->top=xdg_surface_get_toplevel(w->xdg);
  xdg_toplevel_add_listener(w->top,&top_listener,w);
  xdg_toplevel_set_title(w->top,i?"Touch fixture other":"Touch fixture first");
  wl_surface_commit(w->surface);
 }
 wl_display_flush(display);puts("READY");fflush(stdout);
 struct pollfd fds[]={{wl_display_get_fd(display),POLLIN,0},{STDIN_FILENO,POLLIN,0}};
 while(1){
  if(wl_display_dispatch_pending(display)<0||wl_display_flush(display)<0)break;
  if(poll(fds,2,-1)<0)break;
  if(fds[0].revents&POLLIN)if(wl_display_dispatch(display)<0)break;
  if(fds[1].revents&POLLIN){
   char command[128];if(!fgets(command,sizeof(command),stdin))break;
   if(!strncmp(command,"hide",4)){windows[0].hidden=1;wl_surface_attach(windows[0].surface,NULL,0,0);wl_surface_commit(windows[0].surface);}
   else if(!strncmp(command,"show",4)){windows[0].hidden=0;wl_surface_commit(windows[0].surface);wl_display_roundtrip(display);draw(&windows[0]);}
   else if(!strncmp(command,"destroy",7))closed(&windows[0],windows[0].top);
   puts("COMMAND");fflush(stdout);
  }
 }
 return 0;
}
