// Small FFmpeg/NVENC adapter. The system FFmpeg library owns the codec implementation.
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct wb_encoder { AVCodecContext *codec; AVBSFContext *filter; AVFrame *frame; AVPacket *packet; int width, height; int gpu; };

void wb_encoder_free(struct wb_encoder *encoder) {
    if (!encoder) return;
    av_bsf_free(&encoder->filter);
    avcodec_free_context(&encoder->codec);
    av_frame_free(&encoder->frame);
    av_packet_free(&encoder->packet);
    free(encoder);
}

static struct wb_encoder *wb_encoder_new_input(int kind, int width, int height, int fps, int bitrate, const AVFrame *gpu, char *error, size_t size) {
    struct wb_encoder *encoder=calloc(1,sizeof(*encoder));
    if (!encoder) return NULL;
    int result=AVERROR(ENOMEM);
    const char *name=kind==2?"h264_nvenc":kind==4?"hevc_nvenc":kind==5?"av1_nvenc":NULL;
    if (!name || width<48 || width>4096 || height<48 || height>2304 || fps<1 || fps>120 || bitrate<1000 || bitrate>200000) {
        snprintf(error,size,"invalid codec, dimensions or rate"); goto failed;
    }
    const AVCodec *codec=avcodec_find_encoder_by_name(name);
    if (!codec) { snprintf(error,size,"%s is unavailable in the system FFmpeg",name); goto failed; }
    encoder->codec=avcodec_alloc_context3(codec);
    if (!gpu) encoder->frame=av_frame_alloc();
    encoder->packet=av_packet_alloc();
    if (!encoder->codec || (!gpu && !encoder->frame) || !encoder->packet) goto failed_error;
    AVCodecContext *ctx=encoder->codec;
    ctx->width=(width+1)&~1; ctx->height=(height+1)&~1;
    ctx->pix_fmt=gpu?AV_PIX_FMT_CUDA:AV_PIX_FMT_RGBA;
    if (gpu) {
        if (gpu->format!=AV_PIX_FMT_CUDA || !gpu->hw_frames_ctx || gpu->width!=ctx->width || gpu->height!=ctx->height ||
            ((AVHWFramesContext *)gpu->hw_frames_ctx->data)->sw_format!=AV_PIX_FMT_RGBA) {
            result=AVERROR(EINVAL); goto failed_error;
        }
        ctx->hw_frames_ctx=av_buffer_ref(gpu->hw_frames_ctx);
        if (!ctx->hw_frames_ctx) goto failed_error;
        encoder->gpu=1;
    }
    ctx->time_base=(AVRational){1,fps}; ctx->framerate=(AVRational){fps,1};
    ctx->bit_rate=(int64_t)bitrate*1000; ctx->rc_max_rate=ctx->bit_rate;
    ctx->rc_buffer_size=(int)(ctx->bit_rate/fps);
    ctx->gop_size=fps*2; ctx->max_b_frames=0;
    ctx->flags|=AV_CODEC_FLAG_LOW_DELAY;
    ctx->color_range=AVCOL_RANGE_MPEG; ctx->colorspace=AVCOL_SPC_BT709;
    ctx->color_primaries=AVCOL_PRI_BT709; ctx->color_trc=AVCOL_TRC_BT709;
    AVDictionary *options=NULL;
    av_dict_set(&options,"preset","p1",0);
    av_dict_set(&options,"tune","ull",0);
    av_dict_set(&options,"rc","cbr",0);
    if (kind==2) av_dict_set(&options,"cbr_padding","0",0);
    av_dict_set(&options,"rc-lookahead","0",0);
    av_dict_set(&options,"zerolatency","1",0);
    av_dict_set(&options,"delay","0",0);
    av_dict_set(&options,"forced-idr","1",0);
    // Give streaming decoders an explicit access-unit boundary, including when updates go idle.
    if (kind!=5) av_dict_set(&options,"aud","1",0);
    if (kind!=5) av_dict_set(&options,"profile",kind==2?"high":"main",0);
    result=avcodec_open2(ctx,codec,&options);
    av_dict_free(&options);
    if (result<0) goto failed_error;
    // Codec-specific framing: never interpret AV1 OBU bytes as H.264 NALs.
    // Strip non-picture filler without touching picture/reference payloads.
    const AVBitStreamFilter *filter=av_bsf_get_by_name(kind==5?"av1_metadata":"filter_units");
    if (!filter) { result=AVERROR_BSF_NOT_FOUND; goto failed_error; }
    result=av_bsf_alloc(filter,&encoder->filter);
    if (result<0) goto failed_error;
    result=avcodec_parameters_from_context(encoder->filter->par_in,ctx);
    if (result<0) goto failed_error;
    encoder->filter->time_base_in=ctx->time_base;
    if (kind==5) {
        result=av_opt_set(encoder->filter->priv_data,"td","insert",0);
        if (result<0) goto failed_error;
        result=av_opt_set(encoder->filter->priv_data,"delete_padding","1",0);
    } else {
        result=av_opt_set(encoder->filter->priv_data,"remove_types",kind==2?"12":"38",0);
    }
    if (result<0) goto failed_error;
    result=av_bsf_init(encoder->filter);
    if (result<0) goto failed_error;
    if (!gpu) {
        encoder->frame->format=ctx->pix_fmt;encoder->frame->width=ctx->width;encoder->frame->height=ctx->height;
        result=av_frame_get_buffer(encoder->frame,64);
        if (result<0) goto failed_error;
    }
    encoder->width=width;encoder->height=height;
    return encoder;
failed_error:
    av_strerror(result,error,size);
failed:
    wb_encoder_free(encoder);return NULL;
}

struct wb_encoder *wb_encoder_new_codec(int kind, int width, int height, int fps, int bitrate, char *error, size_t size) {
    return wb_encoder_new_input(kind,width,height,fps,bitrate,NULL,error,size);
}
struct wb_encoder *wb_encoder_new_gpu_codec(int kind, int width, int height, int fps, int bitrate, const void *frame, char *error, size_t size) {
    if (!frame) { snprintf(error,size,"missing CUDA input frame"); return NULL; }
    return wb_encoder_new_input(kind,width,height,fps,bitrate,frame,error,size);
}

// Retain the original H.264 ABI for existing native fixtures and integrations.
struct wb_encoder *wb_encoder_new(int width, int height, int fps, int bitrate, char *error, size_t size) {
    return wb_encoder_new_codec(2,width,height,fps,bitrate,error,size);
}

static int wb_encoder_encode_frame(struct wb_encoder *encoder, AVFrame *frame, int64_t sequence,
                                   int force_idr, const uint8_t **output, int64_t *pts, int *key) {
    frame->pts=sequence;frame->pict_type=force_idr?AV_PICTURE_TYPE_I:AV_PICTURE_TYPE_NONE;
    int result=avcodec_send_frame(encoder->codec,frame);
    if (result<0) return result;
    av_packet_unref(encoder->packet);
    result=avcodec_receive_packet(encoder->codec,encoder->packet);
    if (result<0) return result;
    result=av_bsf_send_packet(encoder->filter,encoder->packet);
    if (result<0) return result;
    result=av_bsf_receive_packet(encoder->filter,encoder->packet);
    if (result<0) return result;
    *output=encoder->packet->data;*pts=encoder->packet->pts;*key=!!(encoder->packet->flags&AV_PKT_FLAG_KEY);
    return encoder->packet->size;
}

// Output storage remains owned by the encoder until the next call.
int wb_encoder_encode(struct wb_encoder *encoder, const uint8_t *rgba, int64_t sequence,
                      int force_idr, const uint8_t **output, int64_t *pts, int *key) {
    if (encoder->gpu || !rgba) return AVERROR(EINVAL);
    int result=av_frame_make_writable(encoder->frame);
    if (result<0) return result;
    AVFrame *frame=encoder->frame;
    for (int y=0;y<frame->height;++y) {
        const uint8_t *source=rgba+(size_t)(y<encoder->height?y:encoder->height-1)*encoder->width*4;
        uint8_t *dest=frame->data[0]+(size_t)y*frame->linesize[0];
        memcpy(dest,source,(size_t)encoder->width*4);
        if (frame->width>encoder->width) memcpy(dest+encoder->width*4,source+(encoder->width-1)*4,4);
    }
    return wb_encoder_encode_frame(encoder,frame,sequence,force_idr,output,pts,key);
}

int wb_encoder_encode_gpu(struct wb_encoder *encoder, void *input, int64_t sequence,
                         int force_idr, const uint8_t **output, int64_t *pts, int *key) {
    AVFrame *frame=input;
    if (!encoder->gpu || !frame || frame->format!=AV_PIX_FMT_CUDA || !frame->hw_frames_ctx ||
        frame->width!=encoder->codec->width || frame->height!=encoder->codec->height) return AVERROR(EINVAL);
    AVHWFramesContext *source=(AVHWFramesContext *)frame->hw_frames_ctx->data;
    AVHWFramesContext *expected=(AVHWFramesContext *)encoder->codec->hw_frames_ctx->data;
    if (source->device_ctx!=expected->device_ctx || source->sw_format!=expected->sw_format) return AVERROR(EINVAL);
    return wb_encoder_encode_frame(encoder,frame,sequence,force_idr,output,pts,key);
}
