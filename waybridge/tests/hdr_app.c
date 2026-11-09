// Real FP16 Wayland client with explicit Windows-scRGB color description.
#define main base_main
#include "test_app.c"
#undef main
#include "color-management-client-protocol.h"
#include <poll.h>
static struct wp_color_manager_v1 *color_manager;
static struct wp_color_management_surface_v1 *color_surface;
static struct wp_image_description_v1 *image_description;
static struct wl_subcompositor *subcompositor;
static struct wl_surface *overlay;
static struct wl_subsurface *subsurface;
static int scrgb_supported,description_ready,hdr_mode=1,mixed;
static void number(void *d,struct wp_color_manager_v1 *m,uint32_t value) {(void)d;(void)m;(void)value;}
static void feature(void *d,struct wp_color_manager_v1 *m,uint32_t value) {(void)d;(void)m;if(value==WP_COLOR_MANAGER_V1_FEATURE_WINDOWS_SCRGB)scrgb_supported=1;}
static void manager_done(void *d,struct wp_color_manager_v1 *m) {(void)d;(void)m;}
static const struct wp_color_manager_v1_listener manager_listener={.supported_intent=number,.supported_feature=feature,.supported_tf_named=number,.supported_primaries_named=number,.done=manager_done};
static void failed(void *d,struct wp_image_description_v1 *i,uint32_t cause,const char *message) {(void)d;(void)i;fprintf(stderr,"DESCRIPTION_FAILED %u %s\n",cause,message);exit(3);}
static void ready(void *d,struct wp_image_description_v1 *i,uint32_t identity) {(void)d;(void)i;(void)identity;description_ready=1;}
static const struct wp_image_description_v1_listener description_listener={.failed=failed,.ready=ready};
static void bind_color(void *d,struct wl_registry *r,uint32_t name,const char *interface,uint32_t version) {
    global(d,r,name,interface,version);
    if (!strcmp(interface,"wp_color_manager_v1")) {
        color_manager=wl_registry_bind(r,name,&wp_color_manager_v1_interface,1);
        wp_color_manager_v1_add_listener(color_manager,&manager_listener,NULL);
    }
    if (!strcmp(interface,"wl_subcompositor"))subcompositor=wl_registry_bind(r,name,&wl_subcompositor_interface,1);
}
static const struct wl_registry_listener color_registry={.global=bind_color,.global_remove=removed};
static uint16_t half(float value) {
    uint32_t bits;memcpy(&bits,&value,4);
    uint32_t exponent=(bits>>23)&255;
    if (!exponent)return 0;
    return (uint16_t)(((exponent-112)<<10)|((bits>>13)&1023));
}
static void draw_overlay(void) {
    if (!mixed || !overlay)return;
    int width=96,height=64;size_t size=(size_t)width*height*4;
    int fd=memfd_create("hdr-sdr-overlay",MFD_CLOEXEC);if(fd<0||ftruncate(fd,size))exit(2);
    struct pixels *pixels=calloc(1,sizeof(*pixels));pixels->size=size;
    pixels->data=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    for(int i=0;i<width*height;++i)((uint32_t *)pixels->data)[i]=0x80008000;
    struct wl_shm_pool *pool=wl_shm_create_pool(shm,fd,size);
    pixels->buffer=wl_shm_pool_create_buffer(pool,0,width,height,width*4,WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);close(fd);wl_buffer_add_listener(pixels->buffer,&buffer_listener,pixels);
    wl_surface_attach(overlay,pixels->buffer,0,0);wl_surface_damage_buffer(overlay,0,0,width,height);wl_surface_commit(overlay);
}
static void draw_hdr(struct window *w) {
    if(!hdr_mode){draw(w);return;}
    size_t size=(size_t)w->width*w->height*8;
    int fd=memfd_create("hdr-fp16-fixture",MFD_CLOEXEC);if(fd<0||ftruncate(fd,size))exit(2);
    struct pixels *pixels=calloc(1,sizeof(*pixels));pixels->size=size;
    pixels->data=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    for(int y=0;y<w->height;++y)for(int x=0;x<w->width;++x){
        float luminance=getenv("WAYBRIDGE_HDR_RAMP")?100.f*x/(w->width-1):.25f+4.f*x/(w->width-1);
        float alpha=x<w->width/3?1.f:x<w->width*2/3?.5f:0.f;
        uint16_t *p=(uint16_t *)pixels->data+((size_t)y*w->width+x)*4;
        p[0]=p[1]=p[2]=half(luminance*alpha);p[3]=half(alpha);
    }
    struct wl_shm_pool *pool=wl_shm_create_pool(shm,fd,size);
    pixels->buffer=wl_shm_pool_create_buffer(pool,0,w->width,w->height,w->width*8,WL_SHM_FORMAT_ABGR16161616F);
    wl_shm_pool_destroy(pool);close(fd);wl_buffer_add_listener(pixels->buffer,&buffer_listener,pixels);
    draw_overlay();
    wl_surface_attach(w->surface,pixels->buffer,0,0);
    xdg_surface_set_window_geometry(w->xdg,0,0,w->width,w->height);
    wl_surface_damage_buffer(w->surface,0,0,w->width,w->height);wl_surface_commit(w->surface);
    printf("HDR_CONFIGURE %d %d\n",w->width,w->height);fflush(stdout);
}
static void hdr_configured(void *data,struct xdg_surface *surface,uint32_t serial){xdg_surface_ack_configure(surface,serial);struct window *w=data;if(getenv("WAYBRIDGE_HDR_SMALL")){w->width=256;w->height=256;}draw_hdr(w);}
static const struct xdg_surface_listener hdr_surface_listener={.configure=hdr_configured};
int main(int argc,char **argv){
    mixed=getenv("WAYBRIDGE_HDR_MIXED")!=NULL;
    display=wl_display_connect(NULL);if(!display)return 1;
    wl_registry_add_listener(wl_display_get_registry(display),&color_registry,NULL);
    wl_display_roundtrip(display);wl_display_roundtrip(display);
    if(argc>1 && !strcmp(argv[1],"icc")){
        if(!color_manager)return 4;
        wp_color_manager_v1_create_icc_creator(color_manager);wl_display_roundtrip(display);
        return wl_display_get_error(display)?0:5;
    }
    if(argc>1 && !strcmp(argv[1],"lifecycle")){
        struct wl_surface *surface=wl_compositor_create_surface(compositor);
        struct wp_color_management_surface_v1 *color=wp_color_manager_v1_get_surface(color_manager,surface);
        wl_surface_destroy(surface);wp_color_management_surface_v1_destroy(color);
        wl_display_roundtrip(display);return wl_display_get_error(display)?6:0;
    }
    hdr_mode=color_manager && scrgb_supported;
    if(hdr_mode){
        image_description=wp_color_manager_v1_create_windows_scrgb(color_manager);
        wp_image_description_v1_add_listener(image_description,&description_listener,NULL);
        while(!description_ready && wl_display_dispatch(display)>=0){}
    }
    struct window *w=&windows[0];w->id=1;w->width=1024;w->height=256;
    w->surface=wl_compositor_create_surface(compositor);
    w->xdg=xdg_wm_base_get_xdg_surface(shell,w->surface);
    xdg_surface_add_listener(w->xdg,&hdr_surface_listener,w);
    w->top=xdg_surface_get_toplevel(w->xdg);xdg_toplevel_add_listener(w->top,&top_listener,w);
    xdg_toplevel_set_title(w->top,"Waybridge HDR scRGB fixture");
    if(hdr_mode){
        color_surface=wp_color_manager_v1_get_surface(color_manager,w->surface);
        wp_color_management_surface_v1_set_image_description(color_surface,image_description,WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    }
    if(mixed && subcompositor){
        overlay=wl_compositor_create_surface(compositor);
        subsurface=wl_subcompositor_get_subsurface(subcompositor,overlay,w->surface);
        wl_subsurface_set_position(subsurface,16,16);
        draw_overlay();
    }
    wl_surface_commit(w->surface);wl_display_flush(display);
    puts(hdr_mode?"HDR_READY":"SDR_READY");fflush(stdout);
    struct pollfd fds[]={{wl_display_get_fd(display),POLLIN,0},{STDIN_FILENO,POLLIN,0}};
    while(1){
        if(wl_display_dispatch_pending(display)<0 || wl_display_flush(display)<0)break;
        if(poll(fds,2,-1)<0)break;
        if(fds[0].revents&POLLIN)if(wl_display_dispatch(display)<0)break;
        if(fds[1].revents&POLLIN){char command[128];if(!fgets(command,sizeof(command),stdin))break;
            if(!strncmp(command,"pending",7) && color_surface)wp_color_management_surface_v1_unset_image_description(color_surface);
            else if(!strncmp(command,"sdr",3)){hdr_mode=0;if(color_surface)wp_color_management_surface_v1_unset_image_description(color_surface);draw_hdr(w);}
            else if(!strncmp(command,"scrgb",5) && image_description){hdr_mode=1;wp_color_management_surface_v1_set_image_description(color_surface,image_description,WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);draw_hdr(w);}
            else if(!strncmp(command,"size ",5)){int x,y;if(sscanf(command+5,"%d %d",&x,&y)==2 && x>=48 && x<=4096 && y>=48 && y<=2304){w->width=x;w->height=y;draw_hdr(w);}}
            else if(!strncmp(command,"frame",5))draw_hdr(w);
            else if(!strncmp(command,"quit",4))break;
            puts("COMMAND");fflush(stdout);
        }
    }
    return 0;
}
