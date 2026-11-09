// Real 10-bit NVENC diagnostic. This does not enable HDR streaming.
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/error.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
int wb_hdr_probe_10bit(int kind,int *depth,int *distinct,int *peak,char *error,size_t size) {
    const AVCodec *encoder=avcodec_find_encoder_by_name(kind==4?"hevc_nvenc":kind==5?"av1_nvenc":"");
    AVCodecContext *encode=NULL,*decode=NULL;
    AVFrame *input=NULL,*output=NULL;
    AVPacket *packet=NULL;
    AVDictionary *options=NULL;
    int status=AVERROR_ENCODER_NOT_FOUND;
    *depth=*distinct=*peak=0;
    if (!encoder) goto failed;
    encode=avcodec_alloc_context3(encoder);
    input=av_frame_alloc(); output=av_frame_alloc(); packet=av_packet_alloc();
    if (!encode || !input || !output || !packet) { status=AVERROR(ENOMEM); goto failed; }
    encode->width=1024;encode->height=256;encode->pix_fmt=AV_PIX_FMT_P010LE;
    encode->time_base=(AVRational){1,60};encode->framerate=(AVRational){60,1};
    encode->max_b_frames=0;encode->gop_size=120;
    encode->color_range=AVCOL_RANGE_MPEG;encode->colorspace=AVCOL_SPC_BT2020_NCL;
    encode->color_primaries=AVCOL_PRI_BT2020;encode->color_trc=AVCOL_TRC_SMPTE2084;
    av_dict_set(&options,"preset","p1",0);av_dict_set(&options,"tune","ull",0);
    av_dict_set(&options,"rc","constqp",0);av_dict_set(&options,"qp",kind==5?"1":"0",0);
    av_dict_set(&options,"delay","0",0);av_dict_set(&options,"zerolatency","1",0);
    if (kind==4) av_dict_set(&options,"profile","main10",0);
    status=avcodec_open2(encode,encoder,&options);
    av_dict_free(&options);if (status<0) goto failed;
    input->format=encode->pix_fmt;input->width=encode->width;input->height=encode->height;input->pts=1;
    status=av_frame_get_buffer(input,64);if (status<0) goto failed;
    for (int y=0;y<input->height;++y) {
        uint16_t *row=(uint16_t *)(input->data[0]+(size_t)y*input->linesize[0]);
        for (int x=0;x<input->width;++x) row[x]=(uint16_t)((64+x*876/1023)<<6);
    }
    for (int y=0;y<input->height/2;++y) {
        uint16_t *row=(uint16_t *)(input->data[1]+(size_t)y*input->linesize[1]);
        for (int x=0;x<input->width;++x) row[x]=512<<6;
    }
    status=avcodec_send_frame(encode,input);if (status<0) goto failed;
    status=avcodec_receive_packet(encode,packet);if (status<0) goto failed;
    const AVCodec *decoder=avcodec_find_decoder(kind==4?AV_CODEC_ID_HEVC:AV_CODEC_ID_AV1);
    if (!decoder) { status=AVERROR_DECODER_NOT_FOUND; goto failed; }
    decode=avcodec_alloc_context3(decoder);if (!decode) { status=AVERROR(ENOMEM); goto failed; }
    decode->thread_count=1;
    status=avcodec_open2(decode,decoder,NULL);if (status<0) goto failed;
    status=avcodec_send_packet(decode,packet);if (status<0) goto failed;
    status=avcodec_receive_frame(decode,output);
    if (status==AVERROR(EAGAIN)) { avcodec_send_packet(decode,NULL);status=avcodec_receive_frame(decode,output); }
    if (status<0) goto failed;
    const AVPixFmtDescriptor *descriptor=av_pix_fmt_desc_get(output->format);
    if (!descriptor || descriptor->comp[0].depth!=10 || output->width!=1024 || output->height!=256) { status=AVERROR_INVALIDDATA; goto failed; }
    *depth=descriptor->comp[0].depth;
    unsigned char seen[1024]={0};
    const AVComponentDescriptor component=descriptor->comp[0];
    for (int x=0;x<output->width;++x) {
        uint16_t encoded=0;
        memcpy(&encoded,output->data[component.plane]+(size_t)(output->height/2)*output->linesize[component.plane]+(size_t)x*component.step+component.offset,2);
        unsigned int value=(encoded>>component.shift)&1023;
        seen[value]=1;if ((int)value>*peak) *peak=(int)value;
    }
    for (int value=0;value<1024;++value) *distinct+=seen[value];
    status=*distinct>256?0:AVERROR_INVALIDDATA;
failed:
    if (status<0) av_strerror(status,error,size);
    av_dict_free(&options);av_packet_free(&packet);av_frame_free(&input);av_frame_free(&output);
    avcodec_free_context(&encode);avcodec_free_context(&decode);
    return status;
}
