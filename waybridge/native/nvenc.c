// Small FFmpeg/NVENC adapter. The system FFmpeg library owns the codec implementation.
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct wb_encoder { AVCodecContext *codec; AVBSFContext *filter; AVFrame *frame; AVPacket *packet; int width, height; };

void wb_encoder_free(struct wb_encoder *encoder) {
    if (!encoder) return;
    av_bsf_free(&encoder->filter);
    avcodec_free_context(&encoder->codec);
    av_frame_free(&encoder->frame);
    av_packet_free(&encoder->packet);
    free(encoder);
}

struct wb_encoder *wb_encoder_new(int width, int height, int fps, int bitrate, char *error, size_t size) {
    struct wb_encoder *encoder=calloc(1,sizeof(*encoder));
    if (!encoder) return NULL;
    int result=AVERROR(ENOMEM);
    const AVCodec *codec=avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) { snprintf(error,size,"h264_nvenc is unavailable in the system FFmpeg"); goto failed; }
    encoder->codec=avcodec_alloc_context3(codec);
    encoder->frame=av_frame_alloc(); encoder->packet=av_packet_alloc();
    if (!encoder->codec || !encoder->frame || !encoder->packet) goto failed_error;
    AVCodecContext *ctx=encoder->codec;
    ctx->width=(width+1)&~1; ctx->height=(height+1)&~1;
    ctx->pix_fmt=AV_PIX_FMT_RGBA;
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
    av_dict_set(&options,"cbr_padding","0",0);
    av_dict_set(&options,"rc-lookahead","0",0);
    av_dict_set(&options,"zerolatency","1",0);
    av_dict_set(&options,"delay","0",0);
    av_dict_set(&options,"forced-idr","1",0);
    // Give streaming decoders an explicit access-unit boundary, including when updates go idle.
    av_dict_set(&options,"aud","1",0);
    av_dict_set(&options,"profile","high",0);
    result=avcodec_open2(ctx,codec,&options);
    av_dict_free(&options);
    if (result<0) goto failed_error;
    // Some NVENC/driver combinations emit CBR filler even with cbr_padding=0.
    // Trailing filler makes the Windows H.264 MFT hold the final picture until
    // another access unit arrives. Remove only non-picture filler NALs; retain
    // the rate-control configuration, timestamps and complete reference chain.
    const AVBitStreamFilter *filter=av_bsf_get_by_name("filter_units");
    if (!filter) { result=AVERROR_BSF_NOT_FOUND; goto failed_error; }
    result=av_bsf_alloc(filter,&encoder->filter);
    if (result<0) goto failed_error;
    result=avcodec_parameters_from_context(encoder->filter->par_in,ctx);
    if (result<0) goto failed_error;
    encoder->filter->time_base_in=ctx->time_base;
    result=av_opt_set(encoder->filter->priv_data,"remove_types","12",0);
    if (result<0) goto failed_error;
    result=av_bsf_init(encoder->filter);
    if (result<0) goto failed_error;
    encoder->frame->format=ctx->pix_fmt;encoder->frame->width=ctx->width;encoder->frame->height=ctx->height;
    result=av_frame_get_buffer(encoder->frame,64);
    if (result<0) goto failed_error;
    encoder->width=width;encoder->height=height;
    return encoder;
failed_error:
    av_strerror(result,error,size);
failed:
    wb_encoder_free(encoder);return NULL;
}

// Output storage remains owned by the encoder until the next call.
int wb_encoder_encode(struct wb_encoder *encoder, const uint8_t *rgba, int64_t sequence,
                      int force_idr, const uint8_t **output, int64_t *pts, int *key) {
    int result=av_frame_make_writable(encoder->frame);
    if (result<0) return result;
    AVFrame *frame=encoder->frame;
    for (int y=0;y<frame->height;++y) {
        const uint8_t *source=rgba+(size_t)(y<encoder->height?y:encoder->height-1)*encoder->width*4;
        uint8_t *dest=frame->data[0]+(size_t)y*frame->linesize[0];
        memcpy(dest,source,(size_t)encoder->width*4);
        if (frame->width>encoder->width) memcpy(dest+encoder->width*4,source+(encoder->width-1)*4,4);
    }
    frame->pts=sequence;frame->pict_type=force_idr?AV_PICTURE_TYPE_I:AV_PICTURE_TYPE_NONE;
    result=avcodec_send_frame(encoder->codec,frame);
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
