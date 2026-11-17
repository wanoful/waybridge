/* Real touch serials, xdg actions/popups and data-device ownership. MIT. */
#define main base_fixture_main
#include "test_app.c"
#undef main
#include <poll.h>
static struct wl_touch *native_touch;
static struct xdg_popup *menu;
static char interaction_mode[32];
static const char *payload_mime;
static int receiver;
static int hold_feedback;
static unsigned interaction_window(struct wl_surface *surface) {
  for (unsigned i=0;i<3;++i) if (windows[i].surface==surface) return i+1;
  return 0;
}
static void menu_configure(void *d,struct xdg_popup *p,int32_t x,int32_t y,int32_t w,int32_t h) {
  (void)d;(void)p;(void)x;(void)y; windows[2].width=w; windows[2].height=h;
}
static void menu_done(void *d,struct xdg_popup *p) {
  (void)d; xdg_popup_destroy(p);menu=NULL;
  xdg_surface_destroy(windows[2].xdg);wl_surface_destroy(windows[2].surface);windows[2].surface=NULL;
  puts("MENU_DONE");fflush(stdout);
}
static const struct xdg_popup_listener menu_listener={.configure=menu_configure,.popup_done=menu_done};
static void open_menu(uint32_t serial) {
  if(menu)return;
  struct window *w=&windows[2];w->id=3;w->width=120;w->height=80;
  w->surface=wl_compositor_create_surface(compositor);
  w->xdg=xdg_wm_base_get_xdg_surface(shell,w->surface);
  xdg_surface_add_listener(w->xdg,&surface_listener,w);
  struct xdg_positioner *position=xdg_wm_base_create_positioner(shell);
  xdg_positioner_set_size(position,120,80);xdg_positioner_set_anchor_rect(position,40,50,1,1);
  xdg_positioner_set_anchor(position,XDG_POSITIONER_ANCHOR_TOP_LEFT);
  xdg_positioner_set_gravity(position,XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
  menu=xdg_surface_get_popup(w->xdg,windows[0].xdg,position);
  xdg_popup_add_listener(menu,&menu_listener,NULL);
  xdg_popup_grab(menu,seat,serial);wl_surface_commit(w->surface);xdg_positioner_destroy(position);
}
static void interaction_down(void *d,struct wl_touch *t,uint32_t serial,uint32_t time,
                             struct wl_surface *surface,int32_t slot,wl_fixed_t x,wl_fixed_t y) {
  (void)d;(void)t;(void)time;
  unsigned id=interaction_window(surface);
  printf("TOUCH_DOWN %d %u %.3f %.3f\n",slot,id,wl_fixed_to_double(x),wl_fixed_to_double(y));fflush(stdout);
  if(id!=1||slot!=0)return;
  if(!strcmp(interaction_mode,"move"))xdg_toplevel_move(windows[0].top,seat,serial);
  else if(!strcmp(interaction_mode,"resize"))xdg_toplevel_resize(windows[0].top,seat,serial,XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
  else if(!strcmp(interaction_mode,"invalid_move"))xdg_toplevel_move(windows[1].top,seat,serial);
  else if(!strcmp(interaction_mode,"menu"))open_menu(serial);
  else if(!strcmp(interaction_mode,"dnd")){
    drag_source=wl_data_device_manager_create_data_source(data_manager);
    wl_data_source_add_listener(drag_source,&source_listener,NULL);
    wl_data_source_offer(drag_source,payload_mime);
    wl_data_source_set_actions(drag_source,WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    wl_data_device_start_drag(data_device,drag_source,windows[0].surface,NULL,serial);
  }
}
static void interaction_up(void*d,struct wl_touch*t,uint32_t s,uint32_t time,int32_t slot){(void)d;(void)t;(void)s;(void)time;printf("TOUCH_UP %d\n",slot);fflush(stdout);}
static void interaction_motion(void*d,struct wl_touch*t,uint32_t time,int32_t slot,wl_fixed_t x,wl_fixed_t y){(void)d;(void)t;(void)time;printf("TOUCH_MOTION %d %.3f %.3f\n",slot,wl_fixed_to_double(x),wl_fixed_to_double(y));fflush(stdout);}
static void interaction_frame(void*d,struct wl_touch*t){(void)d;(void)t;puts("TOUCH_FRAME");fflush(stdout);}
static void interaction_cancel(void*d,struct wl_touch*t){(void)d;(void)t;puts("TOUCH_CANCEL");fflush(stdout);}
static const struct wl_touch_listener interaction_touch_listener={.down=interaction_down,.up=interaction_up,.motion=interaction_motion,.frame=interaction_frame,.cancel=interaction_cancel};
static void settle_return(void) {
  if(receiver&&drop_offer){wl_data_offer_accept(drop_offer,drop_serial,payload_mime);wl_data_offer_set_actions(drop_offer,1,1);puts("DND_SETTLED");fflush(stdout);}
}
static void interaction_enter(void*d,struct wl_data_device*device,uint32_t serial,struct wl_surface*surface,wl_fixed_t x,wl_fixed_t y,struct wl_data_offer*offer){
  (void)d;(void)device;(void)x;(void)y;drop_offer=offer;drop_serial=serial;
  if(receiver&&offer){if(!hold_feedback)settle_return();printf("DND_ENTER %u\n",interaction_window(surface));fflush(stdout);}
}
static void interaction_leave(void*d,struct wl_data_device*device){(void)d;(void)device;puts("DND_LEAVE");fflush(stdout);}
static void interaction_drop(void*d,struct wl_data_device*device){
  (void)d;(void)device;if(!receiver||!drop_offer)return;
  int fd=open(getenv("WAYBRIDGE_TEST_DROP"),O_WRONLY|O_CREAT|O_TRUNC,0600);
  if(fd<0)exit(3);
  wl_data_offer_receive(drop_offer,payload_mime,fd);close(fd);
  wl_data_offer_finish(drop_offer);puts("DND_DROP");fflush(stdout);
}
static const struct wl_data_device_listener interaction_device_listener={.data_offer=offered,.enter=interaction_enter,.leave=interaction_leave,.motion=drag_motion,.drop=interaction_drop,.selection=selection};
static void bind_touch(void){native_touch=wl_seat_get_touch(seat);wl_touch_add_listener(native_touch,&interaction_touch_listener,NULL);}
int main(void){
  display=wl_display_connect(NULL);if(!display)return 1;
  wl_registry_add_listener(wl_display_get_registry(display),&registry_listener,NULL);
  wl_display_roundtrip(display);wl_display_roundtrip(display);
  if(!seat||!shm||!shell||!data_manager)return 2;
  receiver=getenv("WAYBRIDGE_TOUCH_RECEIVER")!=NULL;
  payload_mime=getenv("WAYBRIDGE_TOUCH_MIME");if(!payload_mime)payload_mime="text/uri-list";
  bind_touch();data_device=wl_data_device_manager_get_data_device(data_manager,seat);
  wl_data_device_add_listener(data_device,&interaction_device_listener,NULL);
  for(int i=0;i<2;++i){
    struct window*w=&windows[i];w->id=i+1;w->width=400;w->height=300;
    w->surface=wl_compositor_create_surface(compositor);w->xdg=xdg_wm_base_get_xdg_surface(shell,w->surface);
    xdg_surface_add_listener(w->xdg,&surface_listener,w);w->top=xdg_surface_get_toplevel(w->xdg);
    xdg_toplevel_add_listener(w->top,&top_listener,w);
    char title[64];snprintf(title,sizeof(title),"Touch %s %d",receiver?"receiver":"source",i+1);
    xdg_toplevel_set_title(w->top,title);wl_surface_commit(w->surface);
  }
  wl_display_flush(display);puts("READY");fflush(stdout);
  struct pollfd fds[]={{wl_display_get_fd(display),POLLIN,0},{STDIN_FILENO,POLLIN,0}};
  while(1){
    if(wl_display_dispatch_pending(display)<0||wl_display_flush(display)<0)break;
    if(poll(fds,2,-1)<0)break;
    if(fds[0].revents&POLLIN)if(wl_display_dispatch(display)<0)break;
    if(fds[1].revents&POLLIN){
      char command[128];if(!fgets(command,sizeof(command),stdin))break;
      command[strcspn(command,"\n")]=0;
      if(!strcmp(command,"touch_off")){if(native_touch)wl_touch_release(native_touch);native_touch=NULL;}
      else if(!strcmp(command,"touch_on")){if(!native_touch)bind_touch();}
      else if(!strcmp(command,"hold_feedback")){hold_feedback=1;}
      else if(!strcmp(command,"auto_feedback")){hold_feedback=0;}
      else if(!strcmp(command,"settle")){settle_return();}
      else snprintf(interaction_mode,sizeof(interaction_mode),"%.31s",command);
      puts("COMMAND");fflush(stdout);
    }
  }
  return 0;
}
