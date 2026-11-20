/* Real tablet-v2 resources and event listener; no host desktop. MIT. */
#define main base_fixture_main
#include "test_app.c"
#undef main
#include "tablet-v2-client-protocol.h"
#include <poll.h>
static struct zwp_tablet_manager_v2 *tablet_manager;
static struct zwp_tablet_seat_v2 *tablet_seat;
static struct zwp_tablet_v2 *tablet;
static struct zwp_tablet_tool_v2 *pen_tools[64];
static unsigned tool_count;
static struct wl_surface *pen_focus;
static char pen_action[32];
static uint32_t last_tip_serial;
static unsigned pen_window(struct wl_surface*s){for(unsigned i=0;i<2;i++)if(windows[i].surface==s)return i+1;return 0;}
static void pen_drag(struct wl_surface *surface, uint32_t serial) {
 if(drag_source||!data_device)return;
 drag_source=wl_data_device_manager_create_data_source(data_manager);
 wl_data_source_add_listener(drag_source,&source_listener,NULL);
 wl_data_source_offer(drag_source,"text/uri-list");
 wl_data_source_set_actions(drag_source,WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
 wl_data_device_start_drag(data_device,drag_source,surface,NULL,serial);
}
static void tname(void*d,struct zwp_tablet_v2*t,const char*n){(void)d;(void)t;printf("TABLET_NAME %s\n",n);fflush(stdout);}
static void tid(void*d,struct zwp_tablet_v2*t,uint32_t v,uint32_t p){(void)d;(void)t;(void)v;(void)p;}
static void tpath(void*d,struct zwp_tablet_v2*t,const char*p){(void)d;(void)t;(void)p;}
static void tdone(void*d,struct zwp_tablet_v2*t){(void)d;(void)t;puts("TABLET_DONE");fflush(stdout);}
static void tremoved(void*d,struct zwp_tablet_v2*t){(void)d;zwp_tablet_v2_destroy(t);tablet=NULL;}
static const struct zwp_tablet_v2_listener tablet_listener={.name=tname,.id=tid,.path=tpath,.done=tdone,.removed=tremoved};
static void ptype(void*d,struct zwp_tablet_tool_v2*t,uint32_t type){(void)d;(void)t;printf("PEN_TYPE %u\n",type);fflush(stdout);}
static void pserial(void*d,struct zwp_tablet_tool_v2*t,uint32_t h,uint32_t l){(void)d;(void)t;printf("PEN_SERIAL %u %u\n",h,l);fflush(stdout);}
static void pid(void*d,struct zwp_tablet_tool_v2*t,uint32_t h,uint32_t l){(void)d;(void)t;(void)h;(void)l;}
static void pcap(void*d,struct zwp_tablet_tool_v2*t,uint32_t cap){(void)d;(void)t;printf("PEN_CAP %u\n",cap);fflush(stdout);}
static void pdone(void*d,struct zwp_tablet_tool_v2*t){(void)d;(void)t;puts("PEN_READY");fflush(stdout);}
static void premoved(void*d,struct zwp_tablet_tool_v2*t){(void)d;for(unsigned i=0;i<tool_count;i++)if(pen_tools[i]==t)pen_tools[i]=NULL;zwp_tablet_tool_v2_destroy(t);puts("PEN_REMOVED");fflush(stdout);}
static void pin(void*d,struct zwp_tablet_tool_v2*t,uint32_t serial,struct zwp_tablet_v2*tab,struct wl_surface*s){(void)d;(void)t;(void)serial;(void)tab;pen_focus=s;printf("PEN_IN %u\n",pen_window(s));fflush(stdout);}
static void pout(void*d,struct zwp_tablet_tool_v2*t){(void)d;(void)t;puts("PEN_OUT");fflush(stdout);}
static void pdown(void*d,struct zwp_tablet_tool_v2*t,uint32_t serial){
 (void)d;(void)t;last_tip_serial=serial;puts("PEN_DOWN");fflush(stdout);
 if(pen_focus!=windows[0].surface)return;
 if(!strcmp(pen_action,"move"))xdg_toplevel_move(windows[0].top,seat,serial);
 else if(!strcmp(pen_action,"resize"))xdg_toplevel_resize(windows[0].top,seat,serial,XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
 else if(!strcmp(pen_action,"invalid_move"))xdg_toplevel_move(windows[1].top,seat,serial);
 else if(!strcmp(pen_action,"drag"))pen_drag(pen_focus,serial);
}
static void pup(void*d,struct zwp_tablet_tool_v2*t){(void)d;(void)t;puts("PEN_UP");fflush(stdout);}
static void pmotion(void*d,struct zwp_tablet_tool_v2*t,wl_fixed_t x,wl_fixed_t y){(void)d;(void)t;printf("PEN_MOTION %.3f %.3f\n",wl_fixed_to_double(x),wl_fixed_to_double(y));fflush(stdout);}
static void ppressure(void*d,struct zwp_tablet_tool_v2*t,uint32_t pressure){(void)d;(void)t;printf("PEN_PRESSURE %u\n",pressure);fflush(stdout);}
static void pdistance(void*d,struct zwp_tablet_tool_v2*t,uint32_t n){(void)d;(void)t;(void)n;}
static void ptilt(void*d,struct zwp_tablet_tool_v2*t,wl_fixed_t x,wl_fixed_t y){(void)d;(void)t;printf("PEN_TILT %.3f %.3f\n",wl_fixed_to_double(x),wl_fixed_to_double(y));fflush(stdout);}
static void protation(void*d,struct zwp_tablet_tool_v2*t,wl_fixed_t r){(void)d;(void)t;printf("PEN_ROTATION %.3f\n",wl_fixed_to_double(r));fflush(stdout);}
static void pslider(void*d,struct zwp_tablet_tool_v2*t,int32_t n){(void)d;(void)t;(void)n;}
static void pwheel(void*d,struct zwp_tablet_tool_v2*t,wl_fixed_t n,int32_t c){(void)d;(void)t;(void)n;(void)c;}
static void pbutton(void*d,struct zwp_tablet_tool_v2*t,uint32_t serial,uint32_t button,uint32_t state){(void)d;(void)t;(void)serial;printf("PEN_BUTTON %u %u\n",button,state);fflush(stdout);}
static void pframe(void*d,struct zwp_tablet_tool_v2*t,uint32_t time){(void)d;(void)t;(void)time;puts("PEN_FRAME");fflush(stdout);}
static const struct zwp_tablet_tool_v2_listener pen_listener={.type=ptype,.hardware_serial=pserial,.hardware_id_wacom=pid,.capability=pcap,.done=pdone,.removed=premoved,.proximity_in=pin,.proximity_out=pout,.down=pdown,.up=pup,.motion=pmotion,.pressure=ppressure,.distance=pdistance,.tilt=ptilt,.rotation=protation,.slider=pslider,.wheel=pwheel,.button=pbutton,.frame=pframe};
static void tablet_added(void*d,struct zwp_tablet_seat_v2*s,struct zwp_tablet_v2*t){(void)d;(void)s;tablet=t;zwp_tablet_v2_add_listener(t,&tablet_listener,NULL);}
static void tool_added(void*d,struct zwp_tablet_seat_v2*s,struct zwp_tablet_tool_v2*t){(void)d;(void)s;if(tool_count>=64)exit(4);pen_tools[tool_count++]=t;zwp_tablet_tool_v2_add_listener(t,&pen_listener,NULL);}
static void pad_added(void*d,struct zwp_tablet_seat_v2*s,struct zwp_tablet_pad_v2*p){(void)d;(void)s;zwp_tablet_pad_v2_destroy(p);}
static const struct zwp_tablet_seat_v2_listener tablet_seat_listener={.tablet_added=tablet_added,.tool_added=tool_added,.pad_added=pad_added};
static void pen_global(void*d,struct wl_registry*r,uint32_t name,const char*interface,uint32_t version){
 global(d,r,name,interface,version);
 if(!strcmp(interface,"zwp_tablet_manager_v2"))tablet_manager=wl_registry_bind(r,name,&zwp_tablet_manager_v2_interface,1);
}
static const struct wl_registry_listener pen_registry_listener={.global=pen_global,.global_remove=removed};
static void bind_pen(void){tablet_seat=zwp_tablet_manager_v2_get_tablet_seat(tablet_manager,seat);zwp_tablet_seat_v2_add_listener(tablet_seat,&tablet_seat_listener,NULL);}
int main(void){
 display=wl_display_connect(NULL);if(!display)return 1;
 wl_registry_add_listener(wl_display_get_registry(display),&pen_registry_listener,NULL);
 wl_display_roundtrip(display);wl_display_roundtrip(display);if(!tablet_manager||!seat||!shm||!shell)return 2;
 if(getenv("WAYBRIDGE_TEST_DRAG_URIS")&&data_manager){
  data_device=wl_data_device_manager_get_data_device(data_manager,seat);
  wl_data_device_add_listener(data_device,&device_listener,NULL);
 }
 for(int i=0;i<2;i++){
  struct window*w=&windows[i];w->id=i+1;w->width=400;w->height=300;w->surface=wl_compositor_create_surface(compositor);
  w->xdg=xdg_wm_base_get_xdg_surface(shell,w->surface);xdg_surface_add_listener(w->xdg,&surface_listener,w);
  w->top=xdg_surface_get_toplevel(w->xdg);xdg_toplevel_add_listener(w->top,&top_listener,w);
  xdg_toplevel_set_title(w->top,i?"Pen fixture other":"Pen fixture first");wl_surface_commit(w->surface);
 }
 puts("READY");fflush(stdout);wl_display_flush(display);
 struct pollfd fds[]={{wl_display_get_fd(display),POLLIN,0},{STDIN_FILENO,POLLIN,0}};
 while(1){
  if(wl_display_dispatch_pending(display)<0||wl_display_flush(display)<0)break;
  if(poll(fds,2,-1)<0)break;
  if(fds[0].revents&POLLIN)if(wl_display_dispatch(display)<0)break;
  if(fds[1].revents&POLLIN){char command[64];if(!fgets(command,sizeof(command),stdin))break;
   if(!strncmp(command,"bind",4)){if(!tablet_seat)bind_pen();}
   else if(!strncmp(command,"unbind",6)){if(tablet_seat){zwp_tablet_seat_v2_destroy(tablet_seat);tablet_seat=NULL;} /* Child resources stay live to observe policy releases. */}
   else if(!strncmp(command,"hide",4)){windows[0].hidden=1;wl_surface_attach(windows[0].surface,NULL,0,0);wl_surface_commit(windows[0].surface);}
   else if(!strncmp(command,"show",4)){windows[0].hidden=0;wl_surface_commit(windows[0].surface);wl_display_roundtrip(display);draw(&windows[0]);}
   else if(!strncmp(command,"stale_move",10))xdg_toplevel_move(windows[0].top,seat,last_tip_serial);
   else {command[strcspn(command,"\n")]=0;snprintf(pen_action,sizeof(pen_action),"%.31s",command);}
   puts("COMMAND");fflush(stdout);
  }
 }
 return 0;
}
