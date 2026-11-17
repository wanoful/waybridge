#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../native/gpu_interop.h"
#include <stdatomic.h>
#include <pthread.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
struct wb_encoder;
struct wb_encoder *wb_encoder_new_gpu_codec(int,int,int,int,int,const void *,char *,size_t);
int wb_encoder_encode_gpu(struct wb_encoder *,void *,int64_t,int,const uint8_t **,int64_t *,int *);
void wb_encoder_free(struct wb_encoder *);
static void check(int okay,const char *what) { if (!okay) { fprintf(stderr,"FAIL %s EGL=%x GL=%x\n",what,eglGetError(),glGetError()); exit(1); } }
struct lifetime { atomic_int refs, retained, released; };
static void retain_owner(void *opaque) {
    struct lifetime *owner=opaque;
    atomic_fetch_add(&owner->refs,1); atomic_fetch_add(&owner->retained,1);
}
static void release_owner(void *opaque) {
    struct lifetime *owner=opaque;
    atomic_fetch_sub(&owner->refs,1); atomic_fetch_add(&owner->released,1);
}
static void *release_encoder(void *opaque) { wb_encoder_free(opaque); return NULL; }
int main(void) {
    PFNEGLQUERYDEVICESEXTPROC devices_fn=(PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC platform_fn=(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    PFNEGLQUERYDEVICESTRINGEXTPROC string_fn=(PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
    EGLDeviceEXT devices[16]; EGLint count=0; check(devices_fn && platform_fn && devices_fn(16,devices,&count),"enumerate EGL devices");
    EGLDisplay display=EGL_NO_DISPLAY; EGLContext context=EGL_NO_CONTEXT;
    const char *node=getenv("WAYBRIDGE_GPU_RENDER_NODE"); if (!node) node="/dev/dri/renderD129";
    for (int i=0;i<count;++i) {
        const char *path=string_fn(devices[i],EGL_DRM_RENDER_NODE_FILE_EXT);
        if (!path || strcmp(path,node)) continue;
        display=platform_fn(EGL_PLATFORM_DEVICE_EXT,devices[i],NULL);
        if (!eglInitialize(display,NULL,NULL) || !eglBindAPI(EGL_OPENGL_ES_API)) continue;
        EGLConfig config; EGLint found=0;
        EGLint attributes[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
        check(eglChooseConfig(display,attributes,&config,1,&found) && found,"EGL config");
        EGLint ctx[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};
        context=eglCreateContext(display,config,EGL_NO_CONTEXT,ctx);
        if (context!=EGL_NO_CONTEXT && eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context)) break;
    }
    check(context!=EGL_NO_CONTEXT,"private EGL context");
    printf("GL vendor: %s\n",glGetString(GL_VENDOR));
    int width=641,height=481;
    GLuint renderbuffer,fbo,texture,upload;
    glGenRenderbuffers(1,&renderbuffer); glBindRenderbuffer(GL_RENDERBUFFER,renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA8,width,height);
    glGenFramebuffers(1,&fbo); glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,renderbuffer);
    check(glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"RBO framebuffer");
    glGenTextures(1,&texture); glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glGenFramebuffers(1,&upload); glBindFramebuffer(GL_FRAMEBUFFER,upload);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
    char error[512]={0};
    struct wb_gpu_device *device=wb_gpu_device_new(error,sizeof(error));
    if (!device) { fprintf(stderr,"%s\n",error); return 1; }
    struct lifetime owner={0};
    struct wb_gpu_owner guard={&owner,retain_owner,release_owner};
    struct wb_gpu_source *source=wb_gpu_source_new_mode_owned(device,renderbuffer,width,height,0,&guard,error,sizeof(error));
    check(atomic_load(&owner.refs)==1,"SDR primary pool retains one owner");
    if (!source) { fprintf(stderr,"%s\n",error); return 1; }
    size_t length=(size_t)width*height*4;
    uint8_t *input=malloc(length),*output=malloc(length);
    for (int opaque=0;opaque<=1;++opaque) for (int flip=0;flip<=1;++flip) {
        for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
            uint8_t *p=input+((size_t)y*width+x)*4;
            p[3]=opaque?255:(x%3==0?0:x%3==1?128:255);
            p[0]=((x*7+y*3)%256)*p[3]/255; p[1]=((x*3+y*5)%256)*p[3]/255; p[2]=((x+y*9)%256)*p[3]/255;
        }
        glBindTexture(GL_TEXTURE_2D,texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,input);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,upload); glBindFramebuffer(GL_DRAW_FRAMEBUFFER,fbo);
        glBlitFramebuffer(0,0,width,height,0,0,width,height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        glFinish(); check(glGetError()==GL_NO_ERROR,"upload premultiplied fixture");
        struct wb_gpu_frame *frame=wb_gpu_capture(source,flip,error,sizeof(error));
        if (!frame) { fprintf(stderr,"%s\n",error); return 1; }
        check(wb_gpu_frame_download(frame,output,length)>=0,"download direct CUDA fixture for verification");
        size_t alpha_length=0;
        const uint8_t *alpha=wb_gpu_frame_alpha(frame,&alpha_length);
        check(alpha_length==(opaque?0:(size_t)width*height),"opaque fastpath / alpha bytes");
        for (int y=0;y<height;++y) for (int x=0;x<width;++x) {
            const uint8_t *p=input+((size_t)(flip?height-1-y:y)*width+x)*4;
            const uint8_t *actual=output+((size_t)y*width+x)*4;
            if (alpha_length) check(alpha[(size_t)y*width+x]==p[3],"exact alpha");
            for (int channel=0;channel<4;++channel) {
                unsigned int expected=p[channel];
                if (channel!=3 && p[3]!=255) { expected=p[3]?(expected*255+p[3]/2)/p[3]:0; if(expected>255)expected=255; }
                if (actual[channel]!=expected) { fprintf(stderr,"pixel mismatch x=%d y=%d channel=%d expected=%u actual=%u\n",x,y,channel,expected,actual[channel]); return 1; }
            }
        }
        if (!opaque && flip) for (int kind=2;kind<=5;++kind) {
            if (kind==3) continue;
            struct wb_encoder *encoder=wb_encoder_new_gpu_codec(kind,width,height,60,30000,wb_gpu_frame_avframe(frame),error,sizeof(error));
            if (!encoder) { fprintf(stderr,"GPU encoder %d: %s\n",kind,error); return 1; }
            char filename[128]; snprintf(filename,sizeof(filename),"build/tests/native-gpu.%s",kind==2?"h264":kind==4?"hevc":"obu");
            FILE *bitstream=fopen(filename,"wb"); check(bitstream!=NULL,"open GPU bitstream");
            for (int i=0;i<4;++i) {
                const uint8_t *data=NULL; int64_t pts=0; int key=0;
                int length=wb_encoder_encode_gpu(encoder,wb_gpu_frame_avframe(frame),i+1,i==0,&data,&pts,&key);
                check(length>0 && pts==i+1 && key==(i==0),"actual CUDA-frame NVENC IDR/P chain");
                check(fwrite(data,1,length,bitstream)==(size_t)length,"write GPU bitstream");
            }
            fclose(bitstream); wb_encoder_free(encoder);
        }
        wb_gpu_frame_free(frame);
    }
    /* Both the raw frame and the encoder independently keep the original pool alive. */
    struct wb_gpu_frame *held=wb_gpu_capture(source,0,error,sizeof(error));
    check(held!=NULL,"capture held owner frame");
    struct wb_encoder *held_encoder=wb_encoder_new_gpu_codec(2,width,height,60,30000,wb_gpu_frame_avframe(held),error,sizeof(error));
    check(held_encoder!=NULL,"create held owner encoder");
    wb_gpu_source_free(source);
    check(atomic_load(&owner.refs)==1,"source destruction does not release retained native pool owner");
    wb_gpu_frame_free(held);
    check(atomic_load(&owner.refs)==1,"encoder keeps native owner after final raw frame destruction");
    pthread_t worker; check(pthread_create(&worker,NULL,release_encoder,held_encoder)==0,"start encoder teardown thread");
    check(pthread_join(worker,NULL)==0,"join encoder teardown thread");
    check(atomic_load(&owner.refs)==0 && atomic_load(&owner.retained)==1 && atomic_load(&owner.released)==1,"encoder-thread final release occurs once");

    /* Failure after context setup must release all owner references. */
    source=wb_gpu_source_new_mode_owned(device,0x7ffffffeU,width,height,0,&guard,error,sizeof(error));
    check(source==NULL && atomic_load(&owner.refs)==0,"failed GL registration releases primary owner");
    source=wb_gpu_source_new_mode_owned(device,0x7ffffffeU,width,height,1,&guard,error,sizeof(error));
    check(source==NULL && atomic_load(&owner.refs)==0,"failed HDR registration releases both owners");

    /* HDR fallback may die before the primary pool retained by an encoder. */
    glBindRenderbuffer(GL_RENDERBUFFER,renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA16F,width,height);
    glBindFramebuffer(GL_FRAMEBUFFER,fbo); glClearColor(.5f,.5f,.5f,.5f); glClear(GL_COLOR_BUFFER_BIT); glFinish();
    source=wb_gpu_source_new_mode_owned(device,renderbuffer,width,height,1,&guard,error,sizeof(error));
    if (!source) fprintf(stderr,"HDR source: %s\n",error);
    check(source!=NULL && atomic_load(&owner.refs)==2,"HDR source owns both pool guards");
    held=wb_gpu_capture(source,0,error,sizeof(error));
    if (!held) fprintf(stderr,"HDR frame: %s\n",error);
    check(held!=NULL,"HDR owner capture");
    AVFrame *avframe=wb_gpu_frame_avframe(held);
    AVBufferRef *retained=av_buffer_ref(avframe->hw_frames_ctx);
    check(retained!=NULL,"hold primary context independently of fallback");
    wb_gpu_source_free(source);
    check(atomic_load(&owner.refs)==2,"HDR frame keeps both owners after source release");
    wb_gpu_frame_free(held);
    check(atomic_load(&owner.refs)==1,"HDR fallback owner releases independently");
    av_buffer_unref(&retained);
    check(atomic_load(&owner.refs)==0 && atomic_load(&owner.retained)==atomic_load(&owner.released),"all HDR pool owners release exactly once");
    wb_gpu_device_free(device);
    puts("PASS: native owner survives source/frame destruction, releases on encoder thread, failed registration rolls back, HDR main/fallback pools release independently");
    glDeleteFramebuffers(1,&fbo); glDeleteFramebuffers(1,&upload); glDeleteRenderbuffers(1,&renderbuffer); glDeleteTextures(1,&texture);
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT); eglDestroyContext(display,context); eglTerminate(display);
    free(input); free(output);
    puts("PASS: real EGL RBO/CUDA registration; every odd-size RGBA pixel matches GPU unpremultiply, flip and exact alpha; opaque captures omit alpha readback");
    return 0;
}
