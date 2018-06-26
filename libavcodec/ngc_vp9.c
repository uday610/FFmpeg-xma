#include "libavutil/internal.h"
#include "libavutil/common.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "libavutil/imgutils.h"
#include "avcodec.h"
#include "internal.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <memory.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <xma.h>
#include <pthread.h>

#define SDXL


typedef struct ngcvp9_encoder
{

  unsigned int      m_nFrameNum;
  unsigned int      m_nOutFrameNum;
  XmaEncoderSession *m_pEnc_session;
}ngcvp9_encoder;


typedef struct ngcvp9Context {
    const AVClass *class;

    ngcvp9_encoder encoder;
    AVFrame        *tmp_frame;
    //ngcvp9_param   *params;
    int paramSet;
    int nFrameNum;
    unsigned int FixedQP;
    unsigned int Intra_Period;
    unsigned int fps;
    unsigned int bitrateKbps;
    unsigned int rc_lookahead;
    unsigned int aq_mode;
    unsigned int idr_period;
} ngcvp9Context;

#define OFFSET(x) offsetof(ngcvp9Context, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "aq-mode",       "AQ method",                                OFFSET(aq_mode),       AV_OPT_TYPE_INT,    { .i64 = 1}, 0, 1, VE, "aq_mode"},
    { "rc-lookahead",  "Number of frames to look ahead for frametype and ratecontrol", OFFSET(rc_lookahead), AV_OPT_TYPE_INT, { .i64 = 8 }, 8, 64, VE },
    { "idr-period",       "IDR Period",                                OFFSET(idr_period),       AV_OPT_TYPE_INT,    { .i64 = 0 }, 0, INT_MAX, VE, "idr_period"},
    { NULL },
};



static av_cold int ngcvp9_encode_close(AVCodecContext *avctx)
{
    printf("ngc close\n");

    ngcvp9Context *ctx = avctx->priv_data;

    av_frame_free(&ctx->tmp_frame);

    //ngcvp9_param_free(ctx->params);


    return 0;
}

#define NUM_REF_FRAMES_NGC 46
static av_cold int ngcvp9_encode_init(AVCodecContext *avctx)
{

    ngcvp9Context *ctx = avctx->priv_data;
    int ret =0,temp =0;
    unsigned int ptr =0;
    avctx->coded_frame = av_frame_alloc();
    printf("ngcvp9_encode_init avctx->encoder= 0x%x\n",(unsigned int)&ctx->encoder);
    ctx->encoder.m_nFrameNum = 0;
    ctx->encoder.m_nOutFrameNum = 0;

    ctx->paramSet = 0;
    ctx->nFrameNum = 0;
    ctx->Intra_Period = 0;
    ctx->fps = 25;
    ctx->bitrateKbps = 0;
    ctx->FixedQP = 35;
    if (avctx->bit_rate > 0) {
        printf("bitrate set as %d\n",avctx->bit_rate);
        ctx->bitrateKbps = avctx->bit_rate/1000;
    }
    if (avctx->gop_size > 0) {
        printf("gop set as %d\n",avctx->gop_size);
        ctx->Intra_Period = avctx->gop_size;
        //printf("gop set as %d\n",ctx->Intra_Period);
    }
    if (avctx->global_quality > 0){
      	printf("qp set as %d\n",avctx->global_quality/FF_QP2LAMBDA);
      	ctx->FixedQP = avctx->global_quality/FF_QP2LAMBDA;
    }
    //printf("fps set as %d/%d\n",avctx->framerate.num,avctx->framerate.den);
    if (avctx->time_base.den > 0) {
        int fpsNum = avctx->time_base.den;
        int fpsDenom = avctx->time_base.num * avctx->ticks_per_frame;
        int fps = fpsNum/fpsDenom;
        printf("fps set as %d/%d=%d\n",avctx->framerate.num,avctx->framerate.den,fps);
        ctx->fps = fps;
        //printf("fps set\n");
    }
    printf("aq=%d\n",ctx->aq_mode);
#if 0
    if(avctx->max_b_frames == 0)
    {
        printf("No B- frames use MrfMode \n");
    }
    if(ctx->rc_lookahead)
      printf("la=%d\n",ctx->rc_lookahead);
    if(ctx->idr_period)
      printf("idr=%d\n",ctx->idr_period);
#endif
    ctx->paramSet = 1;
    ctx->tmp_frame = av_frame_alloc();
    if (!ctx->tmp_frame)
        printf("temp frame alloc failed\n");
    else
    {
      ctx->tmp_frame->width  = avctx->width;
      ctx->tmp_frame->height = avctx->height;
      ctx->tmp_frame->format = AV_PIX_FMT_YUV420P;

      ret = av_frame_get_buffer(ctx->tmp_frame, 32);
      if (ret < 0)
        printf("tmp frame get buffer failed\n");
      else
      {
        printf("tmp frame get buffer passed linesize = %d,%d and %d\n",ctx->tmp_frame->linesize[0],ctx->tmp_frame->linesize[1],ctx->tmp_frame->linesize[2]);
      }

    }
    printf("width=%d height = %d rc=%d\n",avctx->width,avctx->height,avctx->bit_rate);
    XmaEncoderProperties enc_props;
    enc_props.hwencoder_type = XMA_VP9_ENCODER_TYPE;

    strcpy(enc_props.hwvendor_string, "NGCodec");

    enc_props.format = XMA_YUV420_FMT_TYPE;
    enc_props.bits_per_pixel = 8;
    enc_props.width = avctx->width;
    enc_props.height = avctx->height;
    enc_props.framerate.numerator = ctx->fps;
    enc_props.framerate.denominator = 1;
    enc_props.bitrate = avctx->bit_rate;
    enc_props.qp = ctx->FixedQP;
    enc_props.gop_size = ctx->Intra_Period;
    enc_props.idr_interval = ctx->idr_period;
    enc_props.lookahead_depth = ctx->rc_lookahead;
    enc_props.aq_mode = ctx->aq_mode;
    printf("creating enc session\n");
    ctx->encoder.m_pEnc_session = xma_enc_session_create(&enc_props);
    printf("session : %x \n",ctx->encoder.m_pEnc_session);
    return 0;
}


static int ngcvp9_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
                                const AVFrame *pic, int *got_packet)
{

    ngcvp9Context *ctx = avctx->priv_data;
    unsigned long out_size;
    XmaFrameProperties fprops;
    XmaFrameData       frame_data;
    printf("framenum = %d\n",ctx->encoder.m_nFrameNum);

    fprops.format = XMA_YUV420_FMT_TYPE;
    fprops.width = avctx->width;
    fprops.height = avctx->height;
    fprops.bits_per_pixel = 8;

    frame_data.data[0] = NULL;
    frame_data.data[1] = NULL;
    frame_data.data[2] = NULL;

    if(!pic)
    {

      printf("got enc EOF\n");
      if(ctx->encoder.m_nOutFrameNum >= ctx->encoder.m_nFrameNum)
      {
          printf("returning EOF\n");
	        *got_packet = 0;
          return AVERROR_EOF;
      }
      XmaFrame *frame = xma_frame_from_buffers_clone(&fprops,&frame_data);
      frame->is_idr=0;
      frame->do_not_encode = 1;
      frame->is_last_frame = 1;
      char *temp = malloc(avctx->width * avctx->height * (ctx->encoder.m_nOutFrameNum - ctx->encoder.m_nFrameNum));
      unsigned long nSize = 0;
      unsigned char bIsLastOutput = 0;
      while(bIsLastOutput == 0)
      {
         printf("out num %d in num %d\n",ctx->encoder.m_nOutFrameNum,ctx->encoder.m_nFrameNum);
         out_size = 0;
         if(nSize>0)
           frame->is_last_frame = 0;        
         xma_enc_session_send_frame(ctx->encoder.m_pEnc_session, frame);

         XmaDataBuffer *out_buffer = xma_data_from_buffer_clone(temp+nSize, avctx->width * avctx->height);
         do{
              xma_enc_session_recv_data(ctx->encoder.m_pEnc_session, out_buffer, &out_size);
              nSize += out_size;

          }while(out_size == 0);
          bIsLastOutput = out_buffer->is_eof;
          ctx->encoder.m_nOutFrameNum++;
      }
      if(nSize > 0)
      {
        printf("nsize=%d\n",nSize);
        int rc = ff_alloc_packet(pkt, nSize);
        memcpy(pkt->data,temp,nSize);
        free(temp);
        if (rc < 0) 
        {
          av_log(avctx, AV_LOG_ERROR, "Error getting output packet.\n");
          return rc;
        }
    	*got_packet = 1;
    	return 0;
      }
      else
      {
        *got_packet = 0;
        return AVERROR_EOF;
      }
    }

    *got_packet = 0;
    out_size = 0;
    // Create a frame

    frame_data.data[0] = pic->data[0];
    frame_data.data[1] = pic->data[1];
    frame_data.data[2] = pic->data[2];

    XmaFrame *frame = xma_frame_from_buffers_clone(&fprops,&frame_data);
    frame->is_idr=0;
    frame->do_not_encode = 0;
    frame->is_last_frame = 0;
    //printf("linesize %d %d w %d h %d\n",pic->linesize[0],pic->linesize[1],pic->width,pic->height);
    int rc = xma_enc_session_send_frame(ctx->encoder.m_pEnc_session, frame);
    //usleep(100*1000);
    out_size = 0;
    rc = ff_alloc_packet(pkt, fprops.width * fprops.height);
    XmaDataBuffer *out_buffer = xma_data_from_buffer_clone(pkt->data, fprops.width * fprops.height);
    int nCount=0;
#if 1
    if(ctx->encoder.m_nFrameNum < (ctx->rc_lookahead+23))
    {
        /*ctx->nFrameNum++;
        ctx->encoder.m_nFrameNum++;
        *got_packet = 0;
          return 0;*/
        rc = xma_enc_session_recv_data(ctx->encoder.m_pEnc_session, out_buffer, &out_size);
    }
    else {
    
#endif
    do{
	//usleep(5*1000);
        rc = xma_enc_session_recv_data(ctx->encoder.m_pEnc_session, out_buffer, &out_size);
        //printf("out_size=%d\n",out_size);
	nCount++;
    }while(out_size == 0);
    }
    //printf("out_size=%d\n",out_size);
    if(out_size != 0)
    {
      int rc = ff_alloc_packet(pkt, out_size);
      if (rc < 0) {
       av_log(avctx, AV_LOG_ERROR, "Error getting output packet.\n");
       return rc;
      }

      pkt->pts = pic->pts;
      pkt->dts = pic->pts;
      *got_packet = 1;
      ctx->encoder.m_nOutFrameNum++;
    }
    else
    {
	*got_packet = 0;
    }
    if(frame)
	xma_frame_free(frame);
    if(out_buffer)
	xma_data_buffer_free(out_buffer);
    ctx->nFrameNum++;
    ctx->encoder.m_nFrameNum++;

    return 0;
}


static const enum AVPixelFormat ngcvp9_csp_eight[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_NONE
};


static av_cold void ngcvp9_encode_init_csp(AVCodec *codec)
{
        codec->pix_fmts = ngcvp9_csp_eight;
}




static const AVClass class = {
    .class_name = "ngcvp9",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const AVCodecDefault ngcvp9_defaults[] = {
    { "b", "0" },
    { NULL },
};

AVCodec ff_ngc_vp9_encoder = {
    .name             = "NGCVP9",
    .long_name        = NULL_IF_CONFIG_SMALL("NGCodec vp9 "),
    .type             = AVMEDIA_TYPE_VIDEO,
    .id               = AV_CODEC_ID_VP9,
    .init             = ngcvp9_encode_init,
    .init_static_data = ngcvp9_encode_init_csp,
    .encode2          = ngcvp9_encode_frame,
    .close            = ngcvp9_encode_close,
    .priv_data_size   = sizeof(ngcvp9Context),
    .priv_class       = &class,
    .defaults         = ngcvp9_defaults,
    .capabilities     = CODEC_CAP_DELAY | CODEC_CAP_AUTO_THREADS,
};
