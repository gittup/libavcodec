/*
 * H.26L/H.264/AVC/JVT/14496-10/... loop filter
 * Copyright (c) 2003 Michael Niedermayer <michaelni@gmx.at>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file libavcodec/h264_loopfilter.c
 * H.264 / AVC / MPEG4 part10 loop filter.
 * @author Michael Niedermayer <michaelni@gmx.at>
 */

#include "internal.h"
#include "dsputil.h"
#include "avcodec.h"
#include "mpegvideo.h"
#include "h264.h"
#include "mathops.h"
#include "rectangle.h"

#if ARCH_X86
#include "x86/h264_i386.h"
#endif

//#undef NDEBUG
#include <assert.h>

/* Deblocking filter (p153) */
static const uint8_t alpha_table[52*3] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  4,  4,  5,  6,
     7,  8,  9, 10, 12, 13, 15, 17, 20, 22,
    25, 28, 32, 36, 40, 45, 50, 56, 63, 71,
    80, 90,101,113,127,144,162,182,203,226,
   255,255,
   255,255,255,255,255,255,255,255,255,255,255,255,255,
   255,255,255,255,255,255,255,255,255,255,255,255,255,
   255,255,255,255,255,255,255,255,255,255,255,255,255,
   255,255,255,255,255,255,255,255,255,255,255,255,255,
};
static const uint8_t beta_table[52*3] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  2,  2,  2,  3,
     3,  3,  3,  4,  4,  4,  6,  6,  7,  7,
     8,  8,  9,  9, 10, 10, 11, 11, 12, 12,
    13, 13, 14, 14, 15, 15, 16, 16, 17, 17,
    18, 18,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,
    18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18, 18,
};
static const uint8_t tc0_table[52*3][4] = {
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 },
    {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 0 }, {-1, 0, 0, 1 },
    {-1, 0, 0, 1 }, {-1, 0, 0, 1 }, {-1, 0, 0, 1 }, {-1, 0, 1, 1 }, {-1, 0, 1, 1 }, {-1, 1, 1, 1 },
    {-1, 1, 1, 1 }, {-1, 1, 1, 1 }, {-1, 1, 1, 1 }, {-1, 1, 1, 2 }, {-1, 1, 1, 2 }, {-1, 1, 1, 2 },
    {-1, 1, 1, 2 }, {-1, 1, 2, 3 }, {-1, 1, 2, 3 }, {-1, 2, 2, 3 }, {-1, 2, 2, 4 }, {-1, 2, 3, 4 },
    {-1, 2, 3, 4 }, {-1, 3, 3, 5 }, {-1, 3, 4, 6 }, {-1, 3, 4, 6 }, {-1, 4, 5, 7 }, {-1, 4, 5, 8 },
    {-1, 4, 6, 9 }, {-1, 5, 7,10 }, {-1, 6, 8,11 }, {-1, 6, 8,13 }, {-1, 7,10,14 }, {-1, 8,11,16 },
    {-1, 9,12,18 }, {-1,10,13,20 }, {-1,11,15,23 }, {-1,13,17,25 },
    {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 },
    {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 },
    {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 },
    {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 },
    {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 },
    {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 },
    {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 },
    {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 },
    {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 }, {-1,13,17,25 },
};

static void av_noinline filter_mb_edgev( H264Context *h, uint8_t *pix, int stride, int16_t bS[4], int qp ) {
    const int index_a = qp + h->slice_alpha_c0_offset;
    const int alpha = (alpha_table+52)[index_a];
    const int beta  = (beta_table+52)[qp + h->slice_beta_offset];
    if (alpha ==0 || beta == 0) return;

    if( bS[0] < 4 ) {
        int8_t tc[4];
        tc[0] = (tc0_table+52)[index_a][bS[0]];
        tc[1] = (tc0_table+52)[index_a][bS[1]];
        tc[2] = (tc0_table+52)[index_a][bS[2]];
        tc[3] = (tc0_table+52)[index_a][bS[3]];
        h->s.dsp.h264_h_loop_filter_luma(pix, stride, alpha, beta, tc);
    } else {
        h->s.dsp.h264_h_loop_filter_luma_intra(pix, stride, alpha, beta);
    }
}
static void av_noinline filter_mb_edgecv( H264Context *h, uint8_t *pix, int stride, int16_t bS[4], int qp ) {
    const int index_a = qp + h->slice_alpha_c0_offset;
    const int alpha = (alpha_table+52)[index_a];
    const int beta  = (beta_table+52)[qp + h->slice_beta_offset];
    if (alpha ==0 || beta == 0) return;

    if( bS[0] < 4 ) {
        int8_t tc[4];
        tc[0] = (tc0_table+52)[index_a][bS[0]]+1;
        tc[1] = (tc0_table+52)[index_a][bS[1]]+1;
        tc[2] = (tc0_table+52)[index_a][bS[2]]+1;
        tc[3] = (tc0_table+52)[index_a][bS[3]]+1;
        h->s.dsp.h264_h_loop_filter_chroma(pix, stride, alpha, beta, tc);
    } else {
        h->s.dsp.h264_h_loop_filter_chroma_intra(pix, stride, alpha, beta);
    }
}

static void filter_mb_mbaff_edgev( H264Context *h, uint8_t *pix, int stride, int16_t bS[8], int qp[2] ) {
    int i;
    for( i = 0; i < 16; i++, pix += stride) {
        int index_a;
        int alpha;
        int beta;

        int qp_index;
        int bS_index = (i >> 1);
        if (!MB_FIELD) {
            bS_index &= ~1;
            bS_index |= (i & 1);
        }

        if( bS[bS_index] == 0 ) {
            continue;
        }

        qp_index = MB_FIELD ? (i >> 3) : (i & 1);
        index_a = qp[qp_index] + h->slice_alpha_c0_offset;
        alpha = (alpha_table+52)[index_a];
        beta  = (beta_table+52)[qp[qp_index] + h->slice_beta_offset];

        if( bS[bS_index] < 4 ) {
            const int tc0 = (tc0_table+52)[index_a][bS[bS_index]];
            const int p0 = pix[-1];
            const int p1 = pix[-2];
            const int p2 = pix[-3];
            const int q0 = pix[0];
            const int q1 = pix[1];
            const int q2 = pix[2];

            if( FFABS( p0 - q0 ) < alpha &&
                FFABS( p1 - p0 ) < beta &&
                FFABS( q1 - q0 ) < beta ) {
                int tc = tc0;
                int i_delta;

                if( FFABS( p2 - p0 ) < beta ) {
                    pix[-2] = p1 + av_clip( ( p2 + ( ( p0 + q0 + 1 ) >> 1 ) - ( p1 << 1 ) ) >> 1, -tc0, tc0 );
                    tc++;
                }
                if( FFABS( q2 - q0 ) < beta ) {
                    pix[1] = q1 + av_clip( ( q2 + ( ( p0 + q0 + 1 ) >> 1 ) - ( q1 << 1 ) ) >> 1, -tc0, tc0 );
                    tc++;
                }

                i_delta = av_clip( (((q0 - p0 ) << 2) + (p1 - q1) + 4) >> 3, -tc, tc );
                pix[-1] = av_clip_uint8( p0 + i_delta );    /* p0' */
                pix[0]  = av_clip_uint8( q0 - i_delta );    /* q0' */
                tprintf(h->s.avctx, "filter_mb_mbaff_edgev i:%d, qp:%d, indexA:%d, alpha:%d, beta:%d, tc:%d\n# bS:%d -> [%02x, %02x, %02x, %02x, %02x, %02x] =>[%02x, %02x, %02x, %02x]\n", i, qp[qp_index], index_a, alpha, beta, tc, bS[bS_index], pix[-3], p1, p0, q0, q1, pix[2], p1, pix[-1], pix[0], q1);
            }
        }else{
            const int p0 = pix[-1];
            const int p1 = pix[-2];
            const int p2 = pix[-3];

            const int q0 = pix[0];
            const int q1 = pix[1];
            const int q2 = pix[2];

            if( FFABS( p0 - q0 ) < alpha &&
                FFABS( p1 - p0 ) < beta &&
                FFABS( q1 - q0 ) < beta ) {

                if(FFABS( p0 - q0 ) < (( alpha >> 2 ) + 2 )){
                    if( FFABS( p2 - p0 ) < beta)
                    {
                        const int p3 = pix[-4];
                        /* p0', p1', p2' */
                        pix[-1] = ( p2 + 2*p1 + 2*p0 + 2*q0 + q1 + 4 ) >> 3;
                        pix[-2] = ( p2 + p1 + p0 + q0 + 2 ) >> 2;
                        pix[-3] = ( 2*p3 + 3*p2 + p1 + p0 + q0 + 4 ) >> 3;
                    } else {
                        /* p0' */
                        pix[-1] = ( 2*p1 + p0 + q1 + 2 ) >> 2;
                    }
                    if( FFABS( q2 - q0 ) < beta)
                    {
                        const int q3 = pix[3];
                        /* q0', q1', q2' */
                        pix[0] = ( p1 + 2*p0 + 2*q0 + 2*q1 + q2 + 4 ) >> 3;
                        pix[1] = ( p0 + q0 + q1 + q2 + 2 ) >> 2;
                        pix[2] = ( 2*q3 + 3*q2 + q1 + q0 + p0 + 4 ) >> 3;
                    } else {
                        /* q0' */
                        pix[0] = ( 2*q1 + q0 + p1 + 2 ) >> 2;
                    }
                }else{
                    /* p0', q0' */
                    pix[-1] = ( 2*p1 + p0 + q1 + 2 ) >> 2;
                    pix[ 0] = ( 2*q1 + q0 + p1 + 2 ) >> 2;
                }
                tprintf(h->s.avctx, "filter_mb_mbaff_edgev i:%d, qp:%d, indexA:%d, alpha:%d, beta:%d\n# bS:4 -> [%02x, %02x, %02x, %02x, %02x, %02x] =>[%02x, %02x, %02x, %02x, %02x, %02x]\n", i, qp[qp_index], index_a, alpha, beta, p2, p1, p0, q0, q1, q2, pix[-3], pix[-2], pix[-1], pix[0], pix[1], pix[2]);
            }
        }
    }
}
static void filter_mb_mbaff_edgecv( H264Context *h, uint8_t *pix, int stride, int16_t bS[8], int qp[2] ) {
    int i;
    for( i = 0; i < 8; i++, pix += stride) {
        int index_a;
        int alpha;
        int beta;

        int qp_index;
        int bS_index = i;

        if( bS[bS_index] == 0 ) {
            continue;
        }

        qp_index = MB_FIELD ? (i >> 2) : (i & 1);
        index_a = qp[qp_index] + h->slice_alpha_c0_offset;
        alpha = (alpha_table+52)[index_a];
        beta  = (beta_table+52)[qp[qp_index] + h->slice_beta_offset];

        if( bS[bS_index] < 4 ) {
            const int tc = (tc0_table+52)[index_a][bS[bS_index]] + 1;
            const int p0 = pix[-1];
            const int p1 = pix[-2];
            const int q0 = pix[0];
            const int q1 = pix[1];

            if( FFABS( p0 - q0 ) < alpha &&
                FFABS( p1 - p0 ) < beta &&
                FFABS( q1 - q0 ) < beta ) {
                const int i_delta = av_clip( (((q0 - p0 ) << 2) + (p1 - q1) + 4) >> 3, -tc, tc );

                pix[-1] = av_clip_uint8( p0 + i_delta );    /* p0' */
                pix[0]  = av_clip_uint8( q0 - i_delta );    /* q0' */
                tprintf(h->s.avctx, "filter_mb_mbaff_edgecv i:%d, qp:%d, indexA:%d, alpha:%d, beta:%d, tc:%d\n# bS:%d -> [%02x, %02x, %02x, %02x, %02x, %02x] =>[%02x, %02x, %02x, %02x]\n", i, qp[qp_index], index_a, alpha, beta, tc, bS[bS_index], pix[-3], p1, p0, q0, q1, pix[2], p1, pix[-1], pix[0], q1);
            }
        }else{
            const int p0 = pix[-1];
            const int p1 = pix[-2];
            const int q0 = pix[0];
            const int q1 = pix[1];

            if( FFABS( p0 - q0 ) < alpha &&
                FFABS( p1 - p0 ) < beta &&
                FFABS( q1 - q0 ) < beta ) {

                pix[-1] = ( 2*p1 + p0 + q1 + 2 ) >> 2;   /* p0' */
                pix[0]  = ( 2*q1 + q0 + p1 + 2 ) >> 2;   /* q0' */
                tprintf(h->s.avctx, "filter_mb_mbaff_edgecv i:%d\n# bS:4 -> [%02x, %02x, %02x, %02x, %02x, %02x] =>[%02x, %02x, %02x, %02x, %02x, %02x]\n", i, pix[-3], p1, p0, q0, q1, pix[2], pix[-3], pix[-2], pix[-1], pix[0], pix[1], pix[2]);
            }
        }
    }
}

static void av_noinline filter_mb_edgeh( H264Context *h, uint8_t *pix, int stride, int16_t bS[4], int qp ) {
    const int index_a = qp + h->slice_alpha_c0_offset;
    const int alpha = (alpha_table+52)[index_a];
    const int beta  = (beta_table+52)[qp + h->slice_beta_offset];
    if (alpha ==0 || beta == 0) return;

    if( bS[0] < 4 ) {
        int8_t tc[4];
        tc[0] = (tc0_table+52)[index_a][bS[0]];
        tc[1] = (tc0_table+52)[index_a][bS[1]];
        tc[2] = (tc0_table+52)[index_a][bS[2]];
        tc[3] = (tc0_table+52)[index_a][bS[3]];
        h->s.dsp.h264_v_loop_filter_luma(pix, stride, alpha, beta, tc);
    } else {
        h->s.dsp.h264_v_loop_filter_luma_intra(pix, stride, alpha, beta);
    }
}

static void av_noinline filter_mb_edgech( H264Context *h, uint8_t *pix, int stride, int16_t bS[4], int qp ) {
    const int index_a = qp + h->slice_alpha_c0_offset;
    const int alpha = (alpha_table+52)[index_a];
    const int beta  = (beta_table+52)[qp + h->slice_beta_offset];
    if (alpha ==0 || beta == 0) return;

    if( bS[0] < 4 ) {
        int8_t tc[4];
        tc[0] = (tc0_table+52)[index_a][bS[0]]+1;
        tc[1] = (tc0_table+52)[index_a][bS[1]]+1;
        tc[2] = (tc0_table+52)[index_a][bS[2]]+1;
        tc[3] = (tc0_table+52)[index_a][bS[3]]+1;
        h->s.dsp.h264_v_loop_filter_chroma(pix, stride, alpha, beta, tc);
    } else {
        h->s.dsp.h264_v_loop_filter_chroma_intra(pix, stride, alpha, beta);
    }
}

void ff_h264_filter_mb_fast( H264Context *h, int mb_x, int mb_y, uint8_t *img_y, uint8_t *img_cb, uint8_t *img_cr, unsigned int linesize, unsigned int uvlinesize) {
    MpegEncContext * const s = &h->s;
    int mb_y_firstrow = s->picture_structure == PICT_BOTTOM_FIELD;
    int mb_xy, mb_type;
    int qp, qp0, qp1, qpc, qpc0, qpc1, qp_thresh;

    mb_xy = h->mb_xy;

    if(mb_x==0 || mb_y==mb_y_firstrow || !s->dsp.h264_loop_filter_strength || h->pps.chroma_qp_diff ||
        !(s->flags2 & CODEC_FLAG2_FAST) || //FIXME filter_mb_fast is broken, thus hasto be, but should not under CODEC_FLAG2_FAST
       (h->deblocking_filter == 2 && (h->slice_table[mb_xy] != h->slice_table[h->top_mb_xy] ||
                                      h->slice_table[mb_xy] != h->slice_table[mb_xy - 1]))) {
        ff_h264_filter_mb(h, mb_x, mb_y, img_y, img_cb, img_cr, linesize, uvlinesize);
        return;
    }
    assert(!FRAME_MBAFF);

    mb_type = s->current_picture.mb_type[mb_xy];
    qp = s->current_picture.qscale_table[mb_xy];
    qp0 = s->current_picture.qscale_table[mb_xy-1];
    qp1 = s->current_picture.qscale_table[h->top_mb_xy];
    qpc = get_chroma_qp( h, 0, qp );
    qpc0 = get_chroma_qp( h, 0, qp0 );
    qpc1 = get_chroma_qp( h, 0, qp1 );
    qp0 = (qp + qp0 + 1) >> 1;
    qp1 = (qp + qp1 + 1) >> 1;
    qpc0 = (qpc + qpc0 + 1) >> 1;
    qpc1 = (qpc + qpc1 + 1) >> 1;
    qp_thresh = 15 - h->slice_alpha_c0_offset;
    if(qp <= qp_thresh && qp0 <= qp_thresh && qp1 <= qp_thresh &&
       qpc <= qp_thresh && qpc0 <= qp_thresh && qpc1 <= qp_thresh)
        return;

    if( IS_INTRA(mb_type) ) {
        int16_t bS4[4] = {4,4,4,4};
        int16_t bS3[4] = {3,3,3,3};
        int16_t *bSH = FIELD_PICTURE ? bS3 : bS4;
        if( IS_8x8DCT(mb_type) ) {
            filter_mb_edgev( h, &img_y[4*0], linesize, bS4, qp0 );
            filter_mb_edgev( h, &img_y[4*2], linesize, bS3, qp );
            filter_mb_edgeh( h, &img_y[4*0*linesize], linesize, bSH, qp1 );
            filter_mb_edgeh( h, &img_y[4*2*linesize], linesize, bS3, qp );
        } else {
            filter_mb_edgev( h, &img_y[4*0], linesize, bS4, qp0 );
            filter_mb_edgev( h, &img_y[4*1], linesize, bS3, qp );
            filter_mb_edgev( h, &img_y[4*2], linesize, bS3, qp );
            filter_mb_edgev( h, &img_y[4*3], linesize, bS3, qp );
            filter_mb_edgeh( h, &img_y[4*0*linesize], linesize, bSH, qp1 );
            filter_mb_edgeh( h, &img_y[4*1*linesize], linesize, bS3, qp );
            filter_mb_edgeh( h, &img_y[4*2*linesize], linesize, bS3, qp );
            filter_mb_edgeh( h, &img_y[4*3*linesize], linesize, bS3, qp );
        }
        filter_mb_edgecv( h, &img_cb[2*0], uvlinesize, bS4, qpc0 );
        filter_mb_edgecv( h, &img_cb[2*2], uvlinesize, bS3, qpc );
        filter_mb_edgecv( h, &img_cr[2*0], uvlinesize, bS4, qpc0 );
        filter_mb_edgecv( h, &img_cr[2*2], uvlinesize, bS3, qpc );
        filter_mb_edgech( h, &img_cb[2*0*uvlinesize], uvlinesize, bSH, qpc1 );
        filter_mb_edgech( h, &img_cb[2*2*uvlinesize], uvlinesize, bS3, qpc );
        filter_mb_edgech( h, &img_cr[2*0*uvlinesize], uvlinesize, bSH, qpc1 );
        filter_mb_edgech( h, &img_cr[2*2*uvlinesize], uvlinesize, bS3, qpc );
        return;
    } else {
        DECLARE_ALIGNED_8(int16_t, bS[2][4][4]);
        uint64_t (*bSv)[4] = (uint64_t(*)[4])bS;
        int edges;
        if( IS_8x8DCT(mb_type) && (h->cbp&7) == 7 ) {
            edges = 4;
            bSv[0][0] = bSv[0][2] = bSv[1][0] = bSv[1][2] = 0x0002000200020002ULL;
        } else {
            int mask_edge1 = (mb_type & (MB_TYPE_16x16 | MB_TYPE_8x16)) ? 3 :
                             (mb_type & MB_TYPE_16x8) ? 1 : 0;
            int mask_edge0 = (mb_type & (MB_TYPE_16x16 | MB_TYPE_8x16))
                             && (s->current_picture.mb_type[mb_xy-1] & (MB_TYPE_16x16 | MB_TYPE_8x16))
                             ? 3 : 0;
            int step = IS_8x8DCT(mb_type) ? 2 : 1;
            edges = (mb_type & MB_TYPE_16x16) && !(h->cbp & 15) ? 1 : 4;
            s->dsp.h264_loop_filter_strength( bS, h->non_zero_count_cache, h->ref_cache, h->mv_cache,
                                              (h->slice_type_nos == FF_B_TYPE), edges, step, mask_edge0, mask_edge1, FIELD_PICTURE);
        }
        if( IS_INTRA(s->current_picture.mb_type[mb_xy-1]) )
            bSv[0][0] = 0x0004000400040004ULL;
        if( IS_INTRA(s->current_picture.mb_type[h->top_mb_xy]) )
            bSv[1][0] = FIELD_PICTURE ? 0x0003000300030003ULL : 0x0004000400040004ULL;

#define FILTER(hv,dir,edge)\
        if(bSv[dir][edge]) {\
            filter_mb_edge##hv( h, &img_y[4*edge*(dir?linesize:1)], linesize, bS[dir][edge], edge ? qp : qp##dir );\
            if(!(edge&1)) {\
                filter_mb_edgec##hv( h, &img_cb[2*edge*(dir?uvlinesize:1)], uvlinesize, bS[dir][edge], edge ? qpc : qpc##dir );\
                filter_mb_edgec##hv( h, &img_cr[2*edge*(dir?uvlinesize:1)], uvlinesize, bS[dir][edge], edge ? qpc : qpc##dir );\
            }\
        }
        if( edges == 1 ) {
            FILTER(v,0,0);
            FILTER(h,1,0);
        } else if( IS_8x8DCT(mb_type) ) {
            FILTER(v,0,0);
            FILTER(v,0,2);
            FILTER(h,1,0);
            FILTER(h,1,2);
        } else {
            FILTER(v,0,0);
            FILTER(v,0,1);
            FILTER(v,0,2);
            FILTER(v,0,3);
            FILTER(h,1,0);
            FILTER(h,1,1);
            FILTER(h,1,2);
            FILTER(h,1,3);
        }
#undef FILTER
    }
}


static av_always_inline void filter_mb_dir(H264Context *h, int mb_x, int mb_y, uint8_t *img_y, uint8_t *img_cb, uint8_t *img_cr, unsigned int linesize, unsigned int uvlinesize, int mb_xy, int mb_type, int mvy_limit, int first_vertical_edge_done, int dir) {
    MpegEncContext * const s = &h->s;
    int edge;
    const int mbm_xy = dir == 0 ? mb_xy -1 : h->top_mb_xy;
    const int mbm_type = s->current_picture.mb_type[mbm_xy];
    int (*ref2frm) [64] = h->ref2frm[ h->slice_num          &(MAX_SLICES-1) ][0] + (MB_MBAFF ? 20 : 2);
    int (*ref2frmm)[64] = h->ref2frm[ h->slice_table[mbm_xy]&(MAX_SLICES-1) ][0] + (MB_MBAFF ? 20 : 2);
    int start = h->slice_table[mbm_xy] == 0xFFFF ? 1 : 0;

    const int edges = (mb_type & (MB_TYPE_16x16|MB_TYPE_SKIP))
                              == (MB_TYPE_16x16|MB_TYPE_SKIP) ? 1 : 4;
    // how often to recheck mv-based bS when iterating between edges
    const int mask_edge = (mb_type & (MB_TYPE_16x16 | (MB_TYPE_16x8 << dir))) ? 3 :
                          (mb_type & (MB_TYPE_8x16 >> dir)) ? 1 : 0;
    // how often to recheck mv-based bS when iterating along each edge
    const int mask_par0 = mb_type & (MB_TYPE_16x16 | (MB_TYPE_8x16 >> dir));

    if (first_vertical_edge_done) {
        start = 1;
    }

    if (h->deblocking_filter==2 && h->slice_table[mbm_xy] != h->slice_table[mb_xy])
        start = 1;

    if (FRAME_MBAFF && (dir == 1) && ((mb_y&1) == 0) && start == 0
        && !IS_INTERLACED(mb_type)
        && IS_INTERLACED(mbm_type)
        ) {
        // This is a special case in the norm where the filtering must
        // be done twice (one each of the field) even if we are in a
        // frame macroblock.
        //
        unsigned int tmp_linesize   = 2 *   linesize;
        unsigned int tmp_uvlinesize = 2 * uvlinesize;
        int mbn_xy = mb_xy - 2 * s->mb_stride;
        int qp;
        int i, j;
        int16_t bS[4];

        for(j=0; j<2; j++, mbn_xy += s->mb_stride){
            if( IS_INTRA(mb_type) ||
                IS_INTRA(s->current_picture.mb_type[mbn_xy]) ) {
                bS[0] = bS[1] = bS[2] = bS[3] = 3;
            } else {
                const uint8_t *mbn_nnz = h->non_zero_count[mbn_xy];
                for( i = 0; i < 4; i++ ) {
                    if( h->non_zero_count_cache[scan8[0]+i] != 0 ||
                        mbn_nnz[i+4+3*8] != 0 )
                        bS[i] = 2;
                    else
                        bS[i] = 1;
                }
            }
            // Do not use s->qscale as luma quantizer because it has not the same
            // value in IPCM macroblocks.
            qp = ( s->current_picture.qscale_table[mb_xy] + s->current_picture.qscale_table[mbn_xy] + 1 ) >> 1;
            tprintf(s->avctx, "filter mb:%d/%d dir:%d edge:%d, QPy:%d ls:%d uvls:%d", mb_x, mb_y, dir, edge, qp, tmp_linesize, tmp_uvlinesize);
            { int i; for (i = 0; i < 4; i++) tprintf(s->avctx, " bS[%d]:%d", i, bS[i]); tprintf(s->avctx, "\n"); }
            filter_mb_edgeh( h, &img_y[j*linesize], tmp_linesize, bS, qp );
            filter_mb_edgech( h, &img_cb[j*uvlinesize], tmp_uvlinesize, bS,
                              ( h->chroma_qp[0] + get_chroma_qp( h, 0, s->current_picture.qscale_table[mbn_xy] ) + 1 ) >> 1);
            filter_mb_edgech( h, &img_cr[j*uvlinesize], tmp_uvlinesize, bS,
                              ( h->chroma_qp[1] + get_chroma_qp( h, 1, s->current_picture.qscale_table[mbn_xy] ) + 1 ) >> 1);
        }

        start = 1;
    }

    /* Calculate bS */
    for( edge = start; edge < edges; edge++ ) {
        /* mbn_xy: neighbor macroblock */
        const int mbn_xy = edge > 0 ? mb_xy : mbm_xy;
        const int mbn_type = s->current_picture.mb_type[mbn_xy];
        int (*ref2frmn)[64] = edge > 0 ? ref2frm : ref2frmm;
        int16_t bS[4];
        int qp;

        if( (edge&1) && IS_8x8DCT(mb_type) )
            continue;

        if( IS_INTRA(mb_type) ||
            IS_INTRA(mbn_type) ) {
            int value;
            if (edge == 0) {
                if (   (!IS_INTERLACED(mb_type) && !IS_INTERLACED(mbm_type))
                    || ((FRAME_MBAFF || (s->picture_structure != PICT_FRAME)) && (dir == 0))
                ) {
                    value = 4;
                } else {
                    value = 3;
                }
            } else {
                value = 3;
            }
            bS[0] = bS[1] = bS[2] = bS[3] = value;
        } else {
            int i, l;
            int mv_done;

            if( edge & mask_edge ) {
                bS[0] = bS[1] = bS[2] = bS[3] = 0;
                mv_done = 1;
            }
            else if( FRAME_MBAFF && IS_INTERLACED(mb_type ^ mbn_type)) {
                bS[0] = bS[1] = bS[2] = bS[3] = 1;
                mv_done = 1;
            }
            else if( mask_par0 && (edge || (mbn_type & (MB_TYPE_16x16 | (MB_TYPE_8x16 >> dir)))) ) {
                int b_idx= 8 + 4 + edge * (dir ? 8:1);
                int bn_idx= b_idx - (dir ? 8:1);
                int v = 0;

                for( l = 0; !v && l < 1 + (h->slice_type_nos == FF_B_TYPE); l++ ) {
                    v |= ref2frm[l][h->ref_cache[l][b_idx]] != ref2frmn[l][h->ref_cache[l][bn_idx]] |
                         h->mv_cache[l][b_idx][0] - h->mv_cache[l][bn_idx][0] + 3 >= 7U |
                         FFABS( h->mv_cache[l][b_idx][1] - h->mv_cache[l][bn_idx][1] ) >= mvy_limit;
                }

                if(h->slice_type_nos == FF_B_TYPE && v){
                    v=0;
                    for( l = 0; !v && l < 2; l++ ) {
                        int ln= 1-l;
                        v |= ref2frm[l][h->ref_cache[l][b_idx]] != ref2frmn[ln][h->ref_cache[ln][bn_idx]] |
                            h->mv_cache[l][b_idx][0] - h->mv_cache[ln][bn_idx][0] + 3 >= 7U |
                            FFABS( h->mv_cache[l][b_idx][1] - h->mv_cache[ln][bn_idx][1] ) >= mvy_limit;
                    }
                }

                bS[0] = bS[1] = bS[2] = bS[3] = v;
                mv_done = 1;
            }
            else
                mv_done = 0;

            for( i = 0; i < 4; i++ ) {
                int x = dir == 0 ? edge : i;
                int y = dir == 0 ? i    : edge;
                int b_idx= 8 + 4 + x + 8*y;
                int bn_idx= b_idx - (dir ? 8:1);

                if( h->non_zero_count_cache[b_idx] |
                    h->non_zero_count_cache[bn_idx] ) {
                    bS[i] = 2;
                }
                else if(!mv_done)
                {
                    bS[i] = 0;
                    for( l = 0; l < 1 + (h->slice_type_nos == FF_B_TYPE); l++ ) {
                        if( ref2frm[l][h->ref_cache[l][b_idx]] != ref2frmn[l][h->ref_cache[l][bn_idx]] |
                            h->mv_cache[l][b_idx][0] - h->mv_cache[l][bn_idx][0] + 3 >= 7U |
                            FFABS( h->mv_cache[l][b_idx][1] - h->mv_cache[l][bn_idx][1] ) >= mvy_limit ) {
                            bS[i] = 1;
                            break;
                        }
                    }

                    if(h->slice_type_nos == FF_B_TYPE && bS[i]){
                        bS[i] = 0;
                        for( l = 0; l < 2; l++ ) {
                            int ln= 1-l;
                            if( ref2frm[l][h->ref_cache[l][b_idx]] != ref2frmn[ln][h->ref_cache[ln][bn_idx]] |
                                h->mv_cache[l][b_idx][0] - h->mv_cache[ln][bn_idx][0] + 3 >= 7U |
                                FFABS( h->mv_cache[l][b_idx][1] - h->mv_cache[ln][bn_idx][1] ) >= mvy_limit ) {
                                bS[i] = 1;
                                break;
                            }
                        }
                    }
                }
            }

            if(bS[0]+bS[1]+bS[2]+bS[3] == 0)
                continue;
        }

        /* Filter edge */
        // Do not use s->qscale as luma quantizer because it has not the same
        // value in IPCM macroblocks.
        qp = ( s->current_picture.qscale_table[mb_xy] + s->current_picture.qscale_table[mbn_xy] + 1 ) >> 1;
        //tprintf(s->avctx, "filter mb:%d/%d dir:%d edge:%d, QPy:%d, QPc:%d, QPcn:%d\n", mb_x, mb_y, dir, edge, qp, h->chroma_qp[0], s->current_picture.qscale_table[mbn_xy]);
        tprintf(s->avctx, "filter mb:%d/%d dir:%d edge:%d, QPy:%d ls:%d uvls:%d", mb_x, mb_y, dir, edge, qp, linesize, uvlinesize);
        //{ int i; for (i = 0; i < 4; i++) tprintf(s->avctx, " bS[%d]:%d", i, bS[i]); tprintf(s->avctx, "\n"); }
        if( dir == 0 ) {
            filter_mb_edgev( h, &img_y[4*edge], linesize, bS, qp );
            if( (edge&1) == 0 ) {
                filter_mb_edgecv( h, &img_cb[2*edge], uvlinesize, bS,
                                  ( h->chroma_qp[0] + get_chroma_qp( h, 0, s->current_picture.qscale_table[mbn_xy] ) + 1 ) >> 1);
                filter_mb_edgecv( h, &img_cr[2*edge], uvlinesize, bS,
                                  ( h->chroma_qp[1] + get_chroma_qp( h, 1, s->current_picture.qscale_table[mbn_xy] ) + 1 ) >> 1);
            }
        } else {
            filter_mb_edgeh( h, &img_y[4*edge*linesize], linesize, bS, qp );
            if( (edge&1) == 0 ) {
                filter_mb_edgech( h, &img_cb[2*edge*uvlinesize], uvlinesize, bS,
                                  ( h->chroma_qp[0] + get_chroma_qp( h, 0, s->current_picture.qscale_table[mbn_xy] ) + 1 ) >> 1);
                filter_mb_edgech( h, &img_cr[2*edge*uvlinesize], uvlinesize, bS,
                                  ( h->chroma_qp[1] + get_chroma_qp( h, 1, s->current_picture.qscale_table[mbn_xy] ) + 1 ) >> 1);
            }
        }
    }
}

void ff_h264_filter_mb( H264Context *h, int mb_x, int mb_y, uint8_t *img_y, uint8_t *img_cb, uint8_t *img_cr, unsigned int linesize, unsigned int uvlinesize) {
    MpegEncContext * const s = &h->s;
    const int mb_xy= mb_x + mb_y*s->mb_stride;
    const int mb_type = s->current_picture.mb_type[mb_xy];
    const int mvy_limit = IS_INTERLACED(mb_type) ? 2 : 4;
    int first_vertical_edge_done = 0;
    av_unused int dir;
    int list;

    // CAVLC 8x8dct requires NNZ values for residual decoding that differ from what the loop filter needs
    if(!h->pps.cabac && h->pps.transform_8x8_mode){
        int top_type, left_type[2];
        top_type     = s->current_picture.mb_type[h->top_mb_xy]    ;
        left_type[0] = s->current_picture.mb_type[h->left_mb_xy[0]];
        left_type[1] = s->current_picture.mb_type[h->left_mb_xy[1]];

        if(IS_8x8DCT(top_type)){
            h->non_zero_count_cache[4+8*0]=
            h->non_zero_count_cache[5+8*0]= h->cbp_table[h->top_mb_xy] & 4;
            h->non_zero_count_cache[6+8*0]=
            h->non_zero_count_cache[7+8*0]= h->cbp_table[h->top_mb_xy] & 8;
        }
        if(IS_8x8DCT(left_type[0])){
            h->non_zero_count_cache[3+8*1]=
            h->non_zero_count_cache[3+8*2]= h->cbp_table[h->left_mb_xy[0]]&2; //FIXME check MBAFF
        }
        if(IS_8x8DCT(left_type[1])){
            h->non_zero_count_cache[3+8*3]=
            h->non_zero_count_cache[3+8*4]= h->cbp_table[h->left_mb_xy[1]]&8; //FIXME check MBAFF
        }

        if(IS_8x8DCT(mb_type)){
            h->non_zero_count_cache[scan8[0   ]]= h->non_zero_count_cache[scan8[1   ]]=
            h->non_zero_count_cache[scan8[2   ]]= h->non_zero_count_cache[scan8[3   ]]= h->cbp_table[mb_xy] & 1;

            h->non_zero_count_cache[scan8[0+ 4]]= h->non_zero_count_cache[scan8[1+ 4]]=
            h->non_zero_count_cache[scan8[2+ 4]]= h->non_zero_count_cache[scan8[3+ 4]]= h->cbp_table[mb_xy] & 2;

            h->non_zero_count_cache[scan8[0+ 8]]= h->non_zero_count_cache[scan8[1+ 8]]=
            h->non_zero_count_cache[scan8[2+ 8]]= h->non_zero_count_cache[scan8[3+ 8]]= h->cbp_table[mb_xy] & 4;

            h->non_zero_count_cache[scan8[0+12]]= h->non_zero_count_cache[scan8[1+12]]=
            h->non_zero_count_cache[scan8[2+12]]= h->non_zero_count_cache[scan8[3+12]]= h->cbp_table[mb_xy] & 8;
        }
    }

    if (FRAME_MBAFF
            // left mb is in picture
            && h->slice_table[mb_xy-1] != 0xFFFF
            // and current and left pair do not have the same interlaced type
            && (IS_INTERLACED(mb_type) != IS_INTERLACED(s->current_picture.mb_type[mb_xy-1]))
            // and left mb is in the same slice if deblocking_filter == 2
            && (h->deblocking_filter!=2 || h->slice_table[mb_xy-1] == h->slice_table[mb_xy])) {
        /* First vertical edge is different in MBAFF frames
         * There are 8 different bS to compute and 2 different Qp
         */
        const int pair_xy = mb_x + (mb_y&~1)*s->mb_stride;
        const int left_mb_xy[2] = { pair_xy-1, pair_xy-1+s->mb_stride };
        int16_t bS[8];
        int qp[2];
        int bqp[2];
        int rqp[2];
        int mb_qp, mbn0_qp, mbn1_qp;
        int i;
        first_vertical_edge_done = 1;

        if( IS_INTRA(mb_type) )
            bS[0] = bS[1] = bS[2] = bS[3] = bS[4] = bS[5] = bS[6] = bS[7] = 4;
        else {
            for( i = 0; i < 8; i++ ) {
                int mbn_xy = MB_FIELD ? left_mb_xy[i>>2] : left_mb_xy[i&1];

                if( IS_INTRA( s->current_picture.mb_type[mbn_xy] ) )
                    bS[i] = 4;
                else if( h->non_zero_count_cache[12+8*(i>>1)] != 0 ||
                         ((!h->pps.cabac && IS_8x8DCT(s->current_picture.mb_type[mbn_xy])) ?
                            (h->cbp_table[mbn_xy] & ((MB_FIELD ? (i&2) : (mb_y&1)) ? 8 : 2))
                                                                       :
                            h->non_zero_count[mbn_xy][7+(MB_FIELD ? (i&3) : (i>>2)+(mb_y&1)*2)*8]))
                    bS[i] = 2;
                else
                    bS[i] = 1;
            }
        }

        mb_qp = s->current_picture.qscale_table[mb_xy];
        mbn0_qp = s->current_picture.qscale_table[left_mb_xy[0]];
        mbn1_qp = s->current_picture.qscale_table[left_mb_xy[1]];
        qp[0] = ( mb_qp + mbn0_qp + 1 ) >> 1;
        bqp[0] = ( get_chroma_qp( h, 0, mb_qp ) +
                   get_chroma_qp( h, 0, mbn0_qp ) + 1 ) >> 1;
        rqp[0] = ( get_chroma_qp( h, 1, mb_qp ) +
                   get_chroma_qp( h, 1, mbn0_qp ) + 1 ) >> 1;
        qp[1] = ( mb_qp + mbn1_qp + 1 ) >> 1;
        bqp[1] = ( get_chroma_qp( h, 0, mb_qp ) +
                   get_chroma_qp( h, 0, mbn1_qp ) + 1 ) >> 1;
        rqp[1] = ( get_chroma_qp( h, 1, mb_qp ) +
                   get_chroma_qp( h, 1, mbn1_qp ) + 1 ) >> 1;

        /* Filter edge */
        tprintf(s->avctx, "filter mb:%d/%d MBAFF, QPy:%d/%d, QPb:%d/%d QPr:%d/%d ls:%d uvls:%d", mb_x, mb_y, qp[0], qp[1], bqp[0], bqp[1], rqp[0], rqp[1], linesize, uvlinesize);
        { int i; for (i = 0; i < 8; i++) tprintf(s->avctx, " bS[%d]:%d", i, bS[i]); tprintf(s->avctx, "\n"); }
        filter_mb_mbaff_edgev ( h, &img_y [0], linesize,   bS, qp );
        filter_mb_mbaff_edgecv( h, &img_cb[0], uvlinesize, bS, bqp );
        filter_mb_mbaff_edgecv( h, &img_cr[0], uvlinesize, bS, rqp );
    }

#if CONFIG_SMALL
    for( dir = 0; dir < 2; dir++ )
        filter_mb_dir(h, mb_x, mb_y, img_y, img_cb, img_cr, linesize, uvlinesize, mb_xy, mb_type, mvy_limit, dir ? 0 : first_vertical_edge_done, dir);
#else
    filter_mb_dir(h, mb_x, mb_y, img_y, img_cb, img_cr, linesize, uvlinesize, mb_xy, mb_type, mvy_limit, first_vertical_edge_done, 0);
    filter_mb_dir(h, mb_x, mb_y, img_y, img_cb, img_cr, linesize, uvlinesize, mb_xy, mb_type, mvy_limit, 0, 1);
#endif
}
