/*
 * RealAudio 2.0 (28.8K)
 * Copyright (c) 2003 the ffmpeg project
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

#include "avcodec.h"
#define ALT_BITSTREAM_READER_LE
#include "bitstream.h"
#include "ra288.h"

typedef struct {
    float sp_lpc[36];      ///< LPC coefficients for speech data (spec: A)
    float gain_lpc[10];    ///< LPC coefficients for gain (spec: GB)

    float sp_hist[111];    ///< Speech data history (spec: SB)

    /** Speech part of the gain autocorrelation (spec: REXP) */
    float sp_rec[37];

    float gain_hist[38];   ///< Log-gain history (spec: SBLG)

    /** Recursive part of the gain autocorrelation (spec: REXPLG) */
    float gain_rec[11];

    float sp_block[41];    ///< Speech data of four blocks (spec: STTMP)
    float gain_block[10];  ///< Gain data of four blocks (spec: GSTATE)
} RA288Context;

static av_cold int ra288_decode_init(AVCodecContext *avctx)
{
    avctx->sample_fmt = SAMPLE_FMT_S16;
    return 0;
}

static inline float scalar_product_float(const float * v1, const float * v2,
                                         int size)
{
    float res = 0.;

    while (size--)
        res += *v1++ * *v2++;

    return res;
}

static void colmult(float *tgt, const float *m1, const float *m2, int n)
{
    while (n--)
        *tgt++ = *m1++ * *m2++;
}

static void decode(RA288Context *ractx, float gain, int cb_coef)
{
    int i, j;
    double sumsum;
    float sum, buffer[5];
    float *block = ractx->sp_block + 36; // Current block

    memmove(ractx->sp_block, ractx->sp_block + 5, 36*sizeof(*ractx->sp_block));

    for (i=0; i < 5; i++) {
        block[i] = 0.;
        for (j=0; j < 36; j++)
            block[i] -= block[i-1-j]*ractx->sp_lpc[j];
    }

    /* block 46 of G.728 spec */
    sum = 32.;
    for (i=0; i < 10; i++)
        sum -= ractx->gain_block[9-i] * ractx->gain_lpc[i];

    /* block 47 of G.728 spec */
    sum = av_clipf(sum, 0, 60);

    /* block 48 of G.728 spec */
    sumsum = exp(sum * 0.1151292546497) * gain; /* pow(10.0,sum/20)*gain */

    for (i=0; i < 5; i++)
        buffer[i] = codetable[cb_coef][i] * sumsum;

    sum = scalar_product_float(buffer, buffer, 5) / 5;

    sum = FFMAX(sum, 1);

    /* shift and store */
    memmove(ractx->gain_block, ractx->gain_block + 1,
            9 * sizeof(*ractx->gain_block));

    ractx->gain_block[9] = 10 * log10(sum) - 32;

    for (i=1; i < 5; i++)
        for (j=i-1; j >= 0; j--)
            buffer[i] -= ractx->sp_lpc[i-j-1] * buffer[j];

    /* output */
    for (i=0; i < 5; i++)
        block[i] = av_clipf(block[i] + buffer[i], -4095, 4095);
}

/**
 * Converts autocorrelation coefficients to LPC coefficients using the
 * Levinson-Durbin algorithm. See blocks 37 and 50 of the G.728 specification.
 *
 * @return 0 if success, -1 if fail
 */
static int eval_lpc_coeffs(const float *in, float *tgt, int n)
{
    int i, j;
    double f0, f1, f2;

    if (in[n] == 0)
        return -1;

    if ((f0 = *in) <= 0)
        return -1;

    in--; // To avoid a -1 subtraction in the inner loop

    for (i=1; i <= n; i++) {
        f1 = in[i+1];

        for (j=0; j < i - 1; j++)
            f1 += in[i-j]*tgt[j];

        tgt[i-1] = f2 = -f1/f0;
        for (j=0; j < i >> 1; j++) {
            float temp = tgt[j] + tgt[i-j-2]*f2;
            tgt[i-j-2] += tgt[j]*f2;
            tgt[j] = temp;
        }
        if ((f0 += f1*f2) < 0)
            return -1;
    }

    return 0;
}

static void convolve(float *tgt, const float *src, int len, int n)
{
    for (; n >= 0; n--)
        tgt[n] = scalar_product_float(src, src - n, len);

}

/**
 * Hybrid window filtering. See blocks 36 and 49 of the G.728 specification.
 *
 * @param order   the order of the filter
 * @param n       the length of the input
 * @param non_rec the number of non-recursive samples
 * @param out     the filter output
 * @param in      pointer to the input of the filter
 * @param hist    pointer to the input history of the filter. It is updated by
 *                this function.
 * @param out     pointer to the non-recursive part of the output
 * @param out2    pointer to the recursive part of the output
 * @param window  pointer to the windowing function table
 */
static void do_hybrid_window(int order, int n, int non_rec, const float *in,
                             float *out, float *hist, float *out2,
                             const float *window)
{
    int i;
    float buffer1[order + 1];
    float buffer2[order + 1];
    float work[order + n + non_rec];

    /* update history */
    memmove(hist                  , hist + n, (order + non_rec)*sizeof(*hist));
    memcpy (hist + order + non_rec, in      , n                *sizeof(*hist));

    colmult(work, window, hist, order + n + non_rec);

    convolve(buffer1, work + order    , n      , order);
    convolve(buffer2, work + order + n, non_rec, order);

    for (i=0; i <= order; i++) {
        out2[i] = out2[i] * 0.5625 + buffer1[i];
        out [i] = out2[i]          + buffer2[i];
    }

    /* Multiply by the white noise correcting factor (WNCF) */
    *out *= 257./256.;
}

/**
 * Backward synthesis filter. Find the LPC coefficients from past speech data.
 */
static void backward_filter(RA288Context *ractx)
{
    float temp1[37]; // RTMP in the spec
    float temp2[11]; // GPTPMP in the spec

    do_hybrid_window(36, 40, 35, ractx->sp_block+1, temp1, ractx->sp_hist,
                     ractx->sp_rec, syn_window);

    if (!eval_lpc_coeffs(temp1, ractx->sp_lpc, 36))
        colmult(ractx->sp_lpc, ractx->sp_lpc, syn_bw_tab, 36);

    do_hybrid_window(10, 8, 20, ractx->gain_block+2, temp2, ractx->gain_hist,
                     ractx->gain_rec, gain_window);

    if (!eval_lpc_coeffs(temp2, ractx->gain_lpc, 10))
        colmult(ractx->gain_lpc, ractx->gain_lpc, gain_bw_tab, 10);
}

static int ra288_decode_frame(AVCodecContext * avctx, void *data,
                              int *data_size, const uint8_t * buf,
                              int buf_size)
{
    int16_t *out = data;
    int i, j;
    RA288Context *ractx = avctx->priv_data;
    GetBitContext gb;

    if (buf_size < avctx->block_align) {
        av_log(avctx, AV_LOG_ERROR,
               "Error! Input buffer is too small [%d<%d]\n",
               buf_size, avctx->block_align);
        return 0;
    }

    init_get_bits(&gb, buf, avctx->block_align * 8);

    for (i=0; i < 32; i++) {
        float gain = amptable[get_bits(&gb, 3)];
        int cb_coef = get_bits(&gb, 6 + (i&1));

        decode(ractx, gain, cb_coef);

        for (j=0; j < 5; j++)
            *(out++) = 8 * ractx->sp_block[36 + j];

        if ((i & 7) == 3)
            backward_filter(ractx);
    }

    *data_size = (char *)out - (char *)data;
    return avctx->block_align;
}

AVCodec ra_288_decoder =
{
    "real_288",
    CODEC_TYPE_AUDIO,
    CODEC_ID_RA_288,
    sizeof(RA288Context),
    ra288_decode_init,
    NULL,
    NULL,
    ra288_decode_frame,
    .long_name = NULL_IF_CONFIG_SMALL("RealAudio 2.0 (28.8K)"),
};
