// Optional CUDA/OpenGL color interop. No Smithay private objects or layouts.
#include "gpu_interop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/error.h>
#ifdef WB_CUDA_INTEROP
#include <dlfcn.h>
#include <cuda.h>
#include <cudaGL.h>
#include <libavutil/hwcontext_cuda.h>
#include "gpu_kernel_ptx.h"
#define DRIVER_FUNCTIONS(F) \
 F(cuInit) F(cuDeviceGetCount) F(cuDeviceGet) F(cuGLGetDevices) F(cuCtxPushCurrent) F(cuCtxPopCurrent) \
 F(cuModuleLoadDataEx) F(cuModuleUnload) F(cuModuleGetFunction) \
 F(cuGraphicsGLRegisterImage) F(cuGraphicsUnregisterResource) \
 F(cuGraphicsMapResources) F(cuGraphicsUnmapResources) F(cuGraphicsSubResourceGetMappedArray) \
 F(cuArrayGetDescriptor) F(cuTexObjectCreate) F(cuTexObjectDestroy) \
 F(cuMemAlloc) F(cuMemFree) F(cuMemsetD32Async) F(cuLaunchKernel) \
 F(cuMemcpyDtoH) F(cuStreamSynchronize) F(cuGetErrorName)
struct driver {
    void *library;
#define FIELD(name) __typeof__(&name) name;
    DRIVER_FUNCTIONS(FIELD)
#undef FIELD
};
struct wb_gpu_device {
    struct driver api;
    AVBufferRef *device;
    CUcontext context;
    CUstream stream;
    CUmodule module;
    CUfunction kernel;
};
struct wb_gpu_source {
    struct wb_gpu_device *device;
    AVBufferRef *frames;
    CUgraphicsResource resource;
    CUdeviceptr alpha, opaque;
    int width, height;
};
struct wb_gpu_frame {
    AVFrame *frame;
    uint8_t *alpha;
    size_t alpha_length;
    int width, height;
};
#define STRINGIFY_INNER(value) #value
#define STRINGIFY(value) STRINGIFY_INNER(value)
static int load_driver(struct driver *api) {
    api->library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!api->library) return 0;
#define LOAD(name) do { void *symbol = dlsym(api->library, STRINGIFY(name)); \
    _Static_assert(sizeof(api->name) == sizeof(symbol), "function pointer size"); \
    memcpy(&api->name, &symbol, sizeof(symbol)); if (!api->name) return 0; } while(0);
    DRIVER_FUNCTIONS(LOAD)
#undef LOAD
    return 1;
}
static void cuda_error(struct wb_gpu_device *device, CUresult result, char *error, size_t size) {
    const char *name = "unknown CUDA error";
    if (device->api.cuGetErrorName) device->api.cuGetErrorName(result, &name);
    snprintf(error, size, "CUDA GL interop: %s (%d)", name, (int)result);
}
static int push(struct wb_gpu_device *device) {
    return device->api.cuCtxPushCurrent(device->context) == CUDA_SUCCESS;
}
static void pop(struct wb_gpu_device *device) {
    CUcontext previous = NULL;
    device->api.cuCtxPopCurrent(&previous);
}
void wb_gpu_device_free(struct wb_gpu_device *device) {
    if (!device) return;
    if (device->module && push(device)) {
        device->api.cuModuleUnload(device->module);
        pop(device);
    }
    av_buffer_unref(&device->device);
    if (device->api.library) dlclose(device->api.library);
    free(device);
}
struct wb_gpu_device *wb_gpu_device_new(char *error, size_t size) {
    struct wb_gpu_device *device = calloc(1, sizeof(*device));
    if (!device) return NULL;
    if (!load_driver(&device->api)) {
        snprintf(error,size,"CUDA driver API is unavailable"); goto failed;
    }
    CUresult result = device->api.cuInit(0);
    if (result != CUDA_SUCCESS) goto cuda_failed;
    CUdevice gl_device;
    unsigned int count = 0;
    result = device->api.cuGLGetDevices(&count, &gl_device, 1, CU_GL_DEVICE_LIST_ALL);
    if (result != CUDA_SUCCESS) goto cuda_failed;
    if (count != 1) { snprintf(error,size,"GL context must belong to exactly one CUDA GPU"); goto failed; }
    int devices=0,selected=-1;
    result=device->api.cuDeviceGetCount(&devices);
    if (result!=CUDA_SUCCESS) goto cuda_failed;
    for (int i=0;i<devices;++i) {
        CUdevice candidate;
        result=device->api.cuDeviceGet(&candidate,i);
        if (result!=CUDA_SUCCESS) goto cuda_failed;
        if (candidate==gl_device) { selected=i; break; }
    }
    if (selected<0) { snprintf(error,size,"GL GPU has no matching CUDA device ordinal"); goto failed; }
    char ordinal[32]; snprintf(ordinal,sizeof(ordinal),"%d",selected);
    int status = av_hwdevice_ctx_create(&device->device,AV_HWDEVICE_TYPE_CUDA,ordinal,NULL,0);
    if (status < 0) { av_strerror(status,error,size); goto failed; }
    AVCUDADeviceContext *cuda = ((AVHWDeviceContext *)device->device->data)->hwctx;
    device->context = cuda->cuda_ctx; device->stream = cuda->stream;
    if (!push(device)) { snprintf(error,size,"cannot make shared CUDA context current"); goto failed; }
    result = device->api.cuModuleLoadDataEx(&device->module,wb_gpu_kernel_ptx,0,NULL,NULL);
    if (result == CUDA_SUCCESS) result = device->api.cuModuleGetFunction(&device->kernel,device->module,"wb_straight_rgba");
    pop(device);
    if (result != CUDA_SUCCESS) goto cuda_failed;
    return device;
cuda_failed:
    cuda_error(device,result,error,size);
failed:
    wb_gpu_device_free(device); return NULL;
}
void wb_gpu_source_free(struct wb_gpu_source *source) {
    if (!source) return;
    struct wb_gpu_device *device = source->device;
    if (device && push(device)) {
        if (source->resource) device->api.cuGraphicsUnregisterResource(source->resource);
        if (source->alpha) device->api.cuMemFree(source->alpha);
        if (source->opaque) device->api.cuMemFree(source->opaque);
        pop(device);
    }
    av_buffer_unref(&source->frames);
    free(source);
}
struct wb_gpu_source *wb_gpu_source_new(struct wb_gpu_device *device,unsigned int renderbuffer,
    int width,int height,char *error,size_t size) {
    if (!device || !renderbuffer || width<48 || width>4096 || height<48 || height>2304) {
        snprintf(error,size,"invalid GPU source or dimensions"); return NULL;
    }
    struct wb_gpu_source *source = calloc(1,sizeof(*source));
    if (!source) return NULL;
    source->device=device; source->width=width; source->height=height;
    source->frames=av_hwframe_ctx_alloc(device->device);
    if (!source->frames) goto failed;
    AVHWFramesContext *frames=(AVHWFramesContext *)source->frames->data;
    frames->format=AV_PIX_FMT_CUDA; frames->sw_format=AV_PIX_FMT_RGBA;
    frames->width=(width+1)&~1; frames->height=(height+1)&~1;
    int status=av_hwframe_ctx_init(source->frames);
    if (status<0) { av_strerror(status,error,size); goto failed; }
    if (!push(device)) { snprintf(error,size,"cannot push CUDA source context"); goto failed; }
    CUresult result=device->api.cuGraphicsGLRegisterImage(&source->resource,renderbuffer,GL_RENDERBUFFER,CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY);
    if (result==CUDA_SUCCESS) result=device->api.cuMemAlloc(&source->alpha,(size_t)width*height);
    if (result==CUDA_SUCCESS) result=device->api.cuMemAlloc(&source->opaque,sizeof(unsigned int));
    pop(device);
    if (result!=CUDA_SUCCESS) { cuda_error(device,result,error,size); goto failed; }
    return source;
failed:
    wb_gpu_source_free(source); return NULL;
}
void wb_gpu_frame_free(struct wb_gpu_frame *frame) {
    if (!frame) return;
    av_frame_free(&frame->frame); free(frame->alpha); free(frame);
}
struct wb_gpu_frame *wb_gpu_capture(struct wb_gpu_source *source,int flip,char *error,size_t size) {
    struct wb_gpu_device *device=source->device;
    struct wb_gpu_frame *frame=calloc(1,sizeof(*frame));
    if (!frame) return NULL;
    frame->width=source->width; frame->height=source->height;
    frame->frame=av_frame_alloc();
    if (!frame->frame) goto failed;
    int status=av_hwframe_get_buffer(source->frames,frame->frame,0);
    if (status<0) { av_strerror(status,error,size); goto failed; }
    if (!push(device)) { snprintf(error,size,"cannot push CUDA capture context"); goto failed; }
    int mapped=0;
    CUtexObject texture=0;
    CUarray array=NULL;
    CUresult result=device->api.cuGraphicsMapResources(1,&source->resource,device->stream);
    if (result==CUDA_SUCCESS) { mapped=1; result=device->api.cuGraphicsSubResourceGetMappedArray(&array,source->resource,0,0); }
    CUDA_ARRAY_DESCRIPTOR description;
    if (result==CUDA_SUCCESS) result=device->api.cuArrayGetDescriptor(&description,array);
    if (result==CUDA_SUCCESS && (description.Format!=CU_AD_FORMAT_UNSIGNED_INT8 || description.NumChannels!=4 ||
        description.Width!=(size_t)source->width || description.Height!=(size_t)source->height)) result=CUDA_ERROR_NOT_SUPPORTED;
    CUDA_RESOURCE_DESC resource={0}; resource.resType=CU_RESOURCE_TYPE_ARRAY; resource.res.array.hArray=array;
    CUDA_TEXTURE_DESC sampler={0}; sampler.addressMode[0]=sampler.addressMode[1]=CU_TR_ADDRESS_MODE_CLAMP;
    sampler.filterMode=CU_TR_FILTER_MODE_POINT; sampler.flags=CU_TRSF_READ_AS_INTEGER;
    if (result==CUDA_SUCCESS) result=device->api.cuTexObjectCreate(&texture,&resource,&sampler,NULL);
    if (result==CUDA_SUCCESS) result=device->api.cuMemsetD32Async(source->opaque,1,1,device->stream);
    CUdeviceptr output=(CUdeviceptr)frame->frame->data[0];
    unsigned long long pitch=(unsigned int)frame->frame->linesize[0];
    int coded_width=frame->frame->width,coded_height=frame->frame->height;
    void *arguments[]={&texture,&output,&pitch,&source->alpha,&source->opaque,&source->width,&source->height,&coded_width,&coded_height,&flip};
    if (result==CUDA_SUCCESS) result=device->api.cuLaunchKernel(device->kernel,(coded_width+31)/32,(coded_height+7)/8,1,32,8,1,0,device->stream,arguments,NULL);
    unsigned int opaque=0;
    if (result==CUDA_SUCCESS) result=device->api.cuMemcpyDtoH(&opaque,source->opaque,sizeof(opaque));
    if (result==CUDA_SUCCESS && !opaque) {
        frame->alpha_length=(size_t)source->width*source->height;
        frame->alpha=malloc(frame->alpha_length);
        if (!frame->alpha) result=CUDA_ERROR_OUT_OF_MEMORY;
        else result=device->api.cuMemcpyDtoH(frame->alpha,source->alpha,frame->alpha_length);
    }
    if (mapped) {
        CUresult sync=device->api.cuStreamSynchronize(device->stream);
        if (result==CUDA_SUCCESS) result=sync;
    }
    if (texture) device->api.cuTexObjectDestroy(texture);
    if (mapped) {
        CUresult unmap=device->api.cuGraphicsUnmapResources(1,&source->resource,device->stream);
        if (result==CUDA_SUCCESS) result=unmap;
        device->api.cuStreamSynchronize(device->stream);
    }
    pop(device);
    if (result!=CUDA_SUCCESS) { cuda_error(device,result,error,size); goto failed; }
    return frame;
failed:
    wb_gpu_frame_free(frame); return NULL;
}
void *wb_gpu_frame_avframe(const struct wb_gpu_frame *frame) { return frame->frame; }
const uint8_t *wb_gpu_frame_alpha(const struct wb_gpu_frame *frame,size_t *length) { *length=frame->alpha_length; return frame->alpha; }
size_t wb_gpu_frame_bytes(const struct wb_gpu_frame *frame) { return (size_t)frame->frame->linesize[0]*frame->frame->height+frame->alpha_length; }
uintptr_t wb_gpu_frame_device(const struct wb_gpu_frame *frame) {
    AVHWFramesContext *frames=(AVHWFramesContext *)frame->frame->hw_frames_ctx->data;
    AVCUDADeviceContext *cuda=frames->device_ctx->hwctx;
    return (uintptr_t)cuda->cuda_ctx;
}
int wb_gpu_frame_download(const struct wb_gpu_frame *frame,uint8_t *rgba,size_t length) {
    if (length!=(size_t)frame->width*frame->height*4) return AVERROR(EINVAL);
    AVFrame *cpu=av_frame_alloc(); if (!cpu) return AVERROR(ENOMEM);
    cpu->format=AV_PIX_FMT_RGBA;
    int status=av_hwframe_transfer_data(cpu,frame->frame,0);
    if (status>=0) for (int y=0;y<frame->height;++y)
        memcpy(rgba+(size_t)y*frame->width*4,cpu->data[0]+(size_t)y*cpu->linesize[0],(size_t)frame->width*4);
    av_frame_free(&cpu); return status;
}
#else
struct wb_gpu_device *wb_gpu_device_new(char *error,size_t size) { snprintf(error,size,"built without CUDA interop headers"); return NULL; }
void wb_gpu_device_free(struct wb_gpu_device *device) { (void)device; }
struct wb_gpu_source *wb_gpu_source_new(struct wb_gpu_device *d,unsigned int r,int w,int h,char *e,size_t s) { (void)d;(void)r;(void)w;(void)h;snprintf(e,s,"CUDA interop is unavailable");return NULL; }
void wb_gpu_source_free(struct wb_gpu_source *source) { (void)source; }
struct wb_gpu_frame *wb_gpu_capture(struct wb_gpu_source *source,int flip,char *error,size_t size) { (void)source;(void)flip;snprintf(error,size,"CUDA interop is unavailable");return NULL; }
void wb_gpu_frame_free(struct wb_gpu_frame *frame) { (void)frame; }
void *wb_gpu_frame_avframe(const struct wb_gpu_frame *frame) { (void)frame;return NULL; }
const uint8_t *wb_gpu_frame_alpha(const struct wb_gpu_frame *frame,size_t *length) { (void)frame;*length=0;return NULL; }
size_t wb_gpu_frame_bytes(const struct wb_gpu_frame *frame) { (void)frame;return 0; }
uintptr_t wb_gpu_frame_device(const struct wb_gpu_frame *frame) { (void)frame;return 0; }
int wb_gpu_frame_download(const struct wb_gpu_frame *frame,uint8_t *rgba,size_t length) { (void)frame;(void)rgba;(void)length;return AVERROR(ENOSYS); }
#endif
