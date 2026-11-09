// Test-only FFmpeg API rejection inside the isolated server; never a driver fault.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <libavcodec/avcodec.h>
int avcodec_send_frame(AVCodecContext *context, const AVFrame *frame) {
    static int gpu_frames=0;
    if (frame && context->pix_fmt==AV_PIX_FMT_CUDA && ++gpu_frames==4) {
        fprintf(stderr,"TEST injected fourth CUDA-frame rejection\n");
        return AVERROR(EIO);
    }
    int (*next)(AVCodecContext *,const AVFrame *)=NULL;
    void *symbol=dlsym(RTLD_NEXT,"avcodec_send_frame");
    _Static_assert(sizeof(next)==sizeof(symbol),"function pointer size");
    memcpy(&next,&symbol,sizeof(symbol));
    return next?next(context,frame):AVERROR(ENOSYS);
}
