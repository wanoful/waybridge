#define _POSIX_C_SOURCE 200809L
#include <wayland-client.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
static struct wl_display *display;
static struct wl_output *late_output;
static int outputs, pending;
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    (void)data;
    if(strcmp(interface,"wl_output"))return;
    ++outputs;
    if(outputs>1 && !late_output){
        late_output=wl_registry_bind(registry,name,&wl_output_interface,version<4?version:4);
        pending=1;
        printf("QUEUED %u\n",name);fflush(stdout);
    }
}
static void removed(void *data,struct wl_registry *registry,uint32_t name){(void)data;(void)registry;(void)name;}
static const struct wl_registry_listener listener={global,removed};
int main(int argc,char **argv){
    display=wl_display_connect(NULL);if(!display)return 2;
    struct wl_registry *registry=wl_display_get_registry(display);
    wl_registry_add_listener(registry,&listener,NULL);
    if(wl_display_roundtrip(display)<0)return 3;
    if(argc==2 && !strcmp(argv[1],"--count")){printf("%d\n",outputs);wl_display_disconnect(display);return 0;}
    puts("READY");fflush(stdout);
    while(!pending){if(wl_display_dispatch(display)<0)return 4;}
    /* bind is queued before removal, but intentionally flushed only after the
       other client closes its window. This models a stalled toolkit event loop. */
    char command;
    if(read(STDIN_FILENO,&command,1)!=1)return 5;
    if(wl_display_roundtrip(display)<0){fprintf(stderr,"LATE_BIND_ERROR %d\n",wl_display_get_error(display));return 6;}
    puts("LATE_BIND_OK");fflush(stdout);
    if(read(STDIN_FILENO,&command,1)!=1)return 8;
    if(late_output)wl_output_release(late_output);
    if(wl_display_roundtrip(display)<0)return 7;
    puts("RELEASE_OK");fflush(stdout);
    wl_registry_destroy(registry);wl_display_disconnect(display);return 0;
}
