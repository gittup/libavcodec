/*
 * utils for libavcodec
 * Copyright (c) 2001 Gerard Lantau.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "common.h"
#include "dsputil.h"
#include "avcodec.h"
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#else
#include <stdlib.h>
#endif

/* memory alloc */
void *av_mallocz(int size)
{
    void *ptr;
#if defined ( ARCH_X86 ) && defined ( HAVE_MEMALIGN )
    ptr = memalign(64,size);
    /* Why 64? 
       Indeed, we should align it:
         on 4 for 386
         on 16 for 486
	 on 32 for 586, PPro - k6-III
	 on 64 for K7 (maybe for P3 too).
       Because L1 and L2 caches are aligned on those values.
       But I don't want to code such logic here!
     */
#else
    ptr = malloc(size);
#endif
    if (!ptr)
        return NULL;
    memset(ptr, 0, size);
    return ptr;
}

/* encoder management */
AVCodec *first_avcodec;

void register_avcodec(AVCodec *format)
{
    AVCodec **p;
    p = &first_avcodec;
    while (*p != NULL) p = &(*p)->next;
    *p = format;
    format->next = NULL;
}

int avcodec_open(AVCodecContext *avctx, AVCodec *codec)
{
    int ret;

    avctx->codec = codec;
    avctx->frame_number = 0;
    avctx->priv_data = av_mallocz(codec->priv_data_size);
    if (!avctx->priv_data) 
        return -ENOMEM;
    ret = avctx->codec->init(avctx);
    if (ret < 0) {
        free(avctx->priv_data);
        avctx->priv_data = NULL;
        return ret;
    }
    return 0;
}

int avcodec_encode_audio(AVCodecContext *avctx, UINT8 *buf, int buf_size, 
                         const short *samples)
{
    int ret;

    ret = avctx->codec->encode(avctx, buf, buf_size, (void *)samples);
    avctx->frame_number++;
    return ret;
}

int avcodec_encode_video(AVCodecContext *avctx, UINT8 *buf, int buf_size, 
                         const AVPicture *pict)
{
    int ret;

    ret = avctx->codec->encode(avctx, buf, buf_size, (void *)pict);
    avctx->frame_number++;
    return ret;
}

/* decode a frame. return -1 if error, otherwise return the number of
   bytes used. If no frame could be decompressed, *got_picture_ptr is
   zero. Otherwise, it is non zero */
int avcodec_decode_video(AVCodecContext *avctx, AVPicture *picture, 
                         int *got_picture_ptr,
                         UINT8 *buf, int buf_size)
{
    int ret;

    ret = avctx->codec->decode(avctx, picture, got_picture_ptr, 
                               buf, buf_size);
    avctx->frame_number++;
    return ret;
}

/* decode an audio frame. return -1 if error, otherwise return the
   *number of bytes used. If no frame could be decompressed,
   *frame_size_ptr is zero. Otherwise, it is the decompressed frame
   *size in BYTES. */
int avcodec_decode_audio(AVCodecContext *avctx, INT16 *samples, 
                         int *frame_size_ptr,
                         UINT8 *buf, int buf_size)
{
    int ret;

    ret = avctx->codec->decode(avctx, samples, frame_size_ptr, 
                               buf, buf_size);
    avctx->frame_number++;
    return ret;
}

int avcodec_close(AVCodecContext *avctx)
{
    if (avctx->codec->close)
        avctx->codec->close(avctx);
    free(avctx->priv_data);
    avctx->priv_data = NULL;
    avctx->codec = NULL;
    return 0;
}

AVCodec *avcodec_find_encoder(enum CodecID id)
{
    AVCodec *p;
    p = first_avcodec;
    while (p) {
        if (p->encode != NULL && p->id == id)
            return p;
        p = p->next;
    }
    return NULL;
}

AVCodec *avcodec_find_decoder(enum CodecID id)
{
    AVCodec *p;
    p = first_avcodec;
    while (p) {
        if (p->decode != NULL && p->id == id)
            return p;
        p = p->next;
    }
    return NULL;
}

AVCodec *avcodec_find_decoder_by_name(const char *name)
{
    AVCodec *p;
    p = first_avcodec;
    while (p) {
        if (p->decode != NULL && strcmp(name,p->name) == 0)
            return p;
        p = p->next;
    }
    return NULL;
}

AVCodec *avcodec_find(enum CodecID id)
{
    AVCodec *p;
    p = first_avcodec;
    while (p) {
        if (p->id == id)
            return p;
        p = p->next;
    }
    return NULL;
}

const char *pix_fmt_str[] = {
    "yuv420p",
    "yuv422",
    "rgb24",
    "bgr24",
    "yuv422p",
    "yuv444p",
};
    
void avcodec_string(char *buf, int buf_size, AVCodecContext *enc, int encode)
{
    const char *codec_name;
    AVCodec *p;
    char buf1[32];
    int bitrate;

    if (encode)
        p = avcodec_find_encoder(enc->codec_id);
    else
        p = avcodec_find_decoder(enc->codec_id);

    if (p) {
        codec_name = p->name;
    } else if (enc->codec_name[0] != '\0') {
        codec_name = enc->codec_name;
    } else {
        /* output avi tags */
        if (enc->codec_type == CODEC_TYPE_VIDEO) {
            snprintf(buf1, sizeof(buf1), "%c%c%c%c", 
                     enc->codec_tag & 0xff,
                     (enc->codec_tag >> 8) & 0xff,
                     (enc->codec_tag >> 16) & 0xff,
                     (enc->codec_tag >> 24) & 0xff);
        } else {
            snprintf(buf1, sizeof(buf1), "0x%04x", enc->codec_tag);
        }
        codec_name = buf1;
    }

    switch(enc->codec_type) {
    case CODEC_TYPE_VIDEO:
        snprintf(buf, buf_size,
                 "Video: %s%s",
                 codec_name, enc->flags & CODEC_FLAG_HQ ? " (hq)" : "");
        if (enc->codec_id == CODEC_ID_RAWVIDEO) {
            snprintf(buf + strlen(buf), buf_size - strlen(buf),
                     ", %s",
                     pix_fmt_str[enc->pix_fmt]);
        }
        if (enc->width) {
            snprintf(buf + strlen(buf), buf_size - strlen(buf),
                     ", %dx%d, %0.2f fps",
                     enc->width, enc->height, 
                     (float)enc->frame_rate / FRAME_RATE_BASE);
        }
        bitrate = enc->bit_rate;
        break;
    case CODEC_TYPE_AUDIO:
        snprintf(buf, buf_size,
                 "Audio: %s",
                 codec_name);
        if (enc->sample_rate) {
            snprintf(buf + strlen(buf), buf_size - strlen(buf),
                     ", %d Hz, %s",
                     enc->sample_rate,
                     enc->channels == 2 ? "stereo" : "mono");
        }
        /* for PCM codecs, compute bitrate directly */
        switch(enc->codec_id) {
        case CODEC_ID_PCM_S16LE:
        case CODEC_ID_PCM_S16BE:
        case CODEC_ID_PCM_U16LE:
        case CODEC_ID_PCM_U16BE:
            bitrate = enc->sample_rate * 16;
            break;
        case CODEC_ID_PCM_S8:
        case CODEC_ID_PCM_U8:
        case CODEC_ID_PCM_ALAW:
        case CODEC_ID_PCM_MULAW:
            bitrate = enc->sample_rate * 8;
            break;
        default:
            bitrate = enc->bit_rate;
            break;
        }
        break;
    default:
        abort();
    }
    if (bitrate != 0) {
        snprintf(buf + strlen(buf), buf_size - strlen(buf), 
                 ", %d kb/s", bitrate / 1000);
    }
}

/* Picture field are filled with 'ptr' addresses */
void avpicture_fill(AVPicture *picture, UINT8 *ptr,
                    int pix_fmt, int width, int height)
{
    int size;

    size = width * height;
    switch(pix_fmt) {
    case PIX_FMT_YUV420P:
        picture->data[0] = ptr;
        picture->data[1] = picture->data[0] + size;
        picture->data[2] = picture->data[1] + size / 4;
        picture->linesize[0] = width;
        picture->linesize[1] = width / 2;
        picture->linesize[2] = width / 2;
        break;
    case PIX_FMT_YUV422P:
        picture->data[0] = ptr;
        picture->data[1] = picture->data[0] + size;
        picture->data[2] = picture->data[1] + size / 2;
        picture->linesize[0] = width;
        picture->linesize[1] = width / 2;
        picture->linesize[2] = width / 2;
        break;
    case PIX_FMT_YUV444P:
        picture->data[0] = ptr;
        picture->data[1] = picture->data[0] + size;
        picture->data[2] = picture->data[1] + size;
        picture->linesize[0] = width;
        picture->linesize[1] = width;
        picture->linesize[2] = width;
        break;
    case PIX_FMT_RGB24:
    case PIX_FMT_BGR24:
        picture->data[0] = ptr;
        picture->data[1] = NULL;
        picture->data[2] = NULL;
        picture->linesize[0] = width * 3;
        break;
    case PIX_FMT_YUV422:
        picture->data[0] = ptr;
        picture->data[1] = NULL;
        picture->data[2] = NULL;
        picture->linesize[0] = width * 2;
        break;
    default:
        picture->data[0] = NULL;
        picture->data[1] = NULL;
        picture->data[2] = NULL;
        break;
    }
}

int avpicture_get_size(int pix_fmt, int width, int height)
{
    int size;

    size = width * height;
    switch(pix_fmt) {
    case PIX_FMT_YUV420P:
        size = (size * 3) / 2;
        break;
    case PIX_FMT_YUV422P:
        size = (size * 2);
        break;
    case PIX_FMT_YUV444P:
        size = (size * 3);
        break;
    case PIX_FMT_RGB24:
    case PIX_FMT_BGR24:
        size = (size * 3);
        break;
    case PIX_FMT_YUV422:
        size = (size * 2);
        break;
    default:
        size = -1;
        break;
    }
    return size;
}


/* must be called before any other functions */
void avcodec_init(void)
{
    dsputil_init();
}

/* simple call to use all the codecs */
void avcodec_register_all(void)
{
    /* encoders */
#ifdef CONFIG_ENCODERS
    register_avcodec(&ac3_encoder);
    register_avcodec(&mp2_encoder);
    register_avcodec(&mpeg1video_encoder);
    register_avcodec(&h263_encoder);
    register_avcodec(&h263p_encoder);
    register_avcodec(&rv10_encoder);
    register_avcodec(&mjpeg_encoder);
    register_avcodec(&mpeg4_encoder);
    register_avcodec(&msmpeg4_encoder);
#endif /* CONFIG_ENCODERS */
    register_avcodec(&rawvideo_codec);

    /* decoders */
#ifdef CONFIG_DECODERS
    register_avcodec(&h263_decoder);
    register_avcodec(&mpeg4_decoder);
    register_avcodec(&msmpeg4_decoder);
    register_avcodec(&mpeg_decoder);
    register_avcodec(&h263i_decoder);
    register_avcodec(&rv10_decoder);
    register_avcodec(&mjpeg_decoder);
    register_avcodec(&mp3_decoder);
#ifdef CONFIG_AC3
    register_avcodec(&ac3_decoder);
#endif
#endif /* CONFIG_DECODERS */

    /* pcm codecs */

#define PCM_CODEC(id, name) \
    register_avcodec(& name ## _encoder); \
    register_avcodec(& name ## _decoder); \

PCM_CODEC(CODEC_ID_PCM_S16LE, pcm_s16le);
PCM_CODEC(CODEC_ID_PCM_S16BE, pcm_s16be);
PCM_CODEC(CODEC_ID_PCM_U16LE, pcm_u16le);
PCM_CODEC(CODEC_ID_PCM_U16BE, pcm_u16be);
PCM_CODEC(CODEC_ID_PCM_S8, pcm_s8);
PCM_CODEC(CODEC_ID_PCM_U8, pcm_u8);
PCM_CODEC(CODEC_ID_PCM_ALAW, pcm_alaw);
PCM_CODEC(CODEC_ID_PCM_MULAW, pcm_mulaw);

#undef PCM_CODEC
}

static int encode_init(AVCodecContext *s)
{
    return 0;
}

static int decode_frame(AVCodecContext *avctx, 
                        void *data, int *data_size,
                        UINT8 *buf, int buf_size)
{
    return -1;
}

static int encode_frame(AVCodecContext *avctx,
                        unsigned char *frame, int buf_size, void *data)
{
    return -1;
}

AVCodec rawvideo_codec = {
    "rawvideo",
    CODEC_TYPE_VIDEO,
    CODEC_ID_RAWVIDEO,
    0,
    encode_init,
    encode_frame,
    NULL,
    decode_frame,
};
