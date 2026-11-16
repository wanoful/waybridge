// A real wl_data_device DND icon animated solely by compositor frame callbacks.
static struct wl_surface *visual_icon;
static struct wl_callback *visual_callback;
static unsigned visual_frames;
static void visual_draw(void);
static void visual_done(void *data,struct wl_callback *callback,uint32_t time) {
  (void)data;(void)time;wl_callback_destroy(callback);visual_callback=NULL;
  visual_frames++;printf("DND_ICON_FRAME %u\n",visual_frames);fflush(stdout);
  const char *destroy=getenv("WAYBRIDGE_TEST_ICON_DESTROY_AFTER");
  if(destroy&&visual_frames>=(unsigned)atoi(destroy)){visual_cancel();return;}
  visual_draw();
}
static const struct wl_callback_listener visual_listener={.done=visual_done};
static void visual_cancel(void) {
  if(visual_callback){wl_callback_destroy(visual_callback);visual_callback=NULL;}
  if(visual_icon){wl_surface_destroy(visual_icon);visual_icon=NULL;}
}
static void visual_draw(void) {
  if(!visual_icon)return;
  const char *edge=getenv("WAYBRIDGE_TEST_DRAG_ICON_EDGE");int width=edge?atoi(edge):64,height=32;
  size_t size=(size_t)width*height*4;int fd=memfd_create("dnd-icon",MFD_CLOEXEC);
  if(fd<0||ftruncate(fd,size))exit(2);
  struct pixels *pixels=calloc(1,sizeof(*pixels));pixels->size=size;
  pixels->data=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  for(int i=0;i<width*height;i++)((uint32_t *)pixels->data)[i]=visual_frames%2?0x80008000:0x80800000;
  ((uint32_t *)pixels->data)[0]=0;
  struct wl_shm_pool *pool=wl_shm_create_pool(shm,fd,(int)size);
  pixels->buffer=wl_shm_pool_create_buffer(pool,0,width,height,width*4,WL_SHM_FORMAT_ARGB8888);
  close(fd);wl_shm_pool_destroy(pool);wl_buffer_add_listener(pixels->buffer,&buffer_listener,pixels);
  wl_surface_attach(visual_icon,pixels->buffer,visual_frames?0:-6,visual_frames?0:-8);
  wl_surface_damage_buffer(visual_icon,0,0,width,height);
  visual_callback=wl_surface_frame(visual_icon);wl_callback_add_listener(visual_callback,&visual_listener,NULL);
  wl_surface_commit(visual_icon);
}
static struct wl_surface *visual_start(void) {
  visual_cancel();visual_frames=0;visual_icon=wl_compositor_create_surface(compositor);visual_draw();return visual_icon;
}
