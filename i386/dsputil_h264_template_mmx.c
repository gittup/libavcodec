/*
 * Copyright (c) 2005 Zoltan Hidvegi <hzoli -a- hzoli -d- com>,
 *                    Loren Merritt
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
 * MMX optimized version of (put|avg)_h264_chroma_mc8.
 * H264_CHROMA_MC8_TMPL must be defined to the desired function name
 * H264_CHROMA_OP must be defined to empty for put and pavgb/pavgusb for avg
 * H264_CHROMA_MC8_MV0 must be defined to a (put|avg)_pixels8 function
 */
static void H264_CHROMA_MC8_TMPL(uint8_t *dst/*align 8*/, uint8_t *src/*align 1*/, int stride, int h, int x, int y)
{
    DECLARE_ALIGNED_8(uint64_t, AA);
    DECLARE_ALIGNED_8(uint64_t, DD);
    int i;

    if(y==0 && x==0) {
        /* no filter needed */
        H264_CHROMA_MC8_MV0(dst, src, stride, h);
        return;
    }

    assert(x<8 && y<8 && x>=0 && y>=0);

    if(y==0 || x==0)
    {
        /* 1 dimensional filter only */
        const int dxy = x ? 1 : stride;

        asm volatile(
            "movd %0, %%mm5\n\t"
            "movq %1, %%mm4\n\t"
            "punpcklwd %%mm5, %%mm5\n\t"
            "punpckldq %%mm5, %%mm5\n\t" /* mm5 = B = x */
            "movq %%mm4, %%mm6\n\t"
            "pxor %%mm7, %%mm7\n\t"
            "psubw %%mm5, %%mm4\n\t"     /* mm4 = A = 8-x */
            "psrlw $1, %%mm6\n\t"        /* mm6 = 4 */
            :: "rm"(x+y), "m"(ff_pw_8));

        for(i=0; i<h; i++) {
            asm volatile(
                /* mm0 = src[0..7], mm1 = src[1..8] */
                "movq %0, %%mm0\n\t"
                "movq %1, %%mm2\n\t"
                :: "m"(src[0]), "m"(src[dxy]));

            asm volatile(
                /* [mm0,mm1] = A * src[0..7] */
                /* [mm2,mm3] = B * src[1..8] */
                "movq %%mm0, %%mm1\n\t"
                "movq %%mm2, %%mm3\n\t"
                "punpcklbw %%mm7, %%mm0\n\t"
                "punpckhbw %%mm7, %%mm1\n\t"
                "punpcklbw %%mm7, %%mm2\n\t"
                "punpckhbw %%mm7, %%mm3\n\t"
                "pmullw %%mm4, %%mm0\n\t"
                "pmullw %%mm4, %%mm1\n\t"
                "pmullw %%mm5, %%mm2\n\t"
                "pmullw %%mm5, %%mm3\n\t"

                /* dst[0..7] = (A * src[0..7] + B * src[1..8] + 4) >> 3 */
                "paddw %%mm6, %%mm0\n\t"
                "paddw %%mm6, %%mm1\n\t"
                "paddw %%mm2, %%mm0\n\t"
                "paddw %%mm3, %%mm1\n\t"
                "psrlw $3, %%mm0\n\t"
                "psrlw $3, %%mm1\n\t"
                "packuswb %%mm1, %%mm0\n\t"
                H264_CHROMA_OP(%0, %%mm0)
                "movq %%mm0, %0\n\t"
                : "=m" (dst[0]));

            src += stride;
            dst += stride;
        }
        return;
    }

    /* general case, bilinear */
    asm volatile("movd %2, %%mm4\n\t"
                 "movd %3, %%mm6\n\t"
                 "punpcklwd %%mm4, %%mm4\n\t"
                 "punpcklwd %%mm6, %%mm6\n\t"
                 "punpckldq %%mm4, %%mm4\n\t" /* mm4 = x words */
                 "punpckldq %%mm6, %%mm6\n\t" /* mm6 = y words */
                 "movq %%mm4, %%mm5\n\t"
                 "pmullw %%mm6, %%mm4\n\t"    /* mm4 = x * y */
                 "psllw $3, %%mm5\n\t"
                 "psllw $3, %%mm6\n\t"
                 "movq %%mm5, %%mm7\n\t"
                 "paddw %%mm6, %%mm7\n\t"
                 "movq %%mm4, %1\n\t"         /* DD = x * y */
                 "psubw %%mm4, %%mm5\n\t"     /* mm5 = B = 8x - xy */
                 "psubw %%mm4, %%mm6\n\t"     /* mm6 = C = 8y - xy */
                 "paddw %4, %%mm4\n\t"
                 "psubw %%mm7, %%mm4\n\t"     /* mm4 = A = xy - (8x+8y) + 64 */
                 "pxor %%mm7, %%mm7\n\t"
                 "movq %%mm4, %0\n\t"
                 : "=m" (AA), "=m" (DD) : "rm" (x), "rm" (y), "m" (ff_pw_64));

    asm volatile(
        /* mm0 = src[0..7], mm1 = src[1..8] */
        "movq %0, %%mm0\n\t"
        "movq %1, %%mm1\n\t"
        : : "m" (src[0]), "m" (src[1]));

    for(i=0; i<h; i++) {
        src += stride;

        asm volatile(
            /* mm2 = A * src[0..3] + B * src[1..4] */
            /* mm3 = A * src[4..7] + B * src[5..8] */
            "movq %%mm0, %%mm2\n\t"
            "movq %%mm1, %%mm3\n\t"
            "punpckhbw %%mm7, %%mm0\n\t"
            "punpcklbw %%mm7, %%mm1\n\t"
            "punpcklbw %%mm7, %%mm2\n\t"
            "punpckhbw %%mm7, %%mm3\n\t"
            "pmullw %0, %%mm0\n\t"
            "pmullw %0, %%mm2\n\t"
            "pmullw %%mm5, %%mm1\n\t"
            "pmullw %%mm5, %%mm3\n\t"
            "paddw %%mm1, %%mm2\n\t"
            "paddw %%mm0, %%mm3\n\t"
            : : "m" (AA));

        asm volatile(
            /* [mm2,mm3] += C * src[0..7] */
            "movq %0, %%mm0\n\t"
            "movq %%mm0, %%mm1\n\t"
            "punpcklbw %%mm7, %%mm0\n\t"
            "punpckhbw %%mm7, %%mm1\n\t"
            "pmullw %%mm6, %%mm0\n\t"
            "pmullw %%mm6, %%mm1\n\t"
            "paddw %%mm0, %%mm2\n\t"
            "paddw %%mm1, %%mm3\n\t"
            : : "m" (src[0]));

        asm volatile(
            /* [mm2,mm3] += D * src[1..8] */
            "movq %1, %%mm1\n\t"
            "movq %%mm1, %%mm0\n\t"
            "movq %%mm1, %%mm4\n\t"
            "punpcklbw %%mm7, %%mm0\n\t"
            "punpckhbw %%mm7, %%mm4\n\t"
            "pmullw %2, %%mm0\n\t"
            "pmullw %2, %%mm4\n\t"
            "paddw %%mm0, %%mm2\n\t"
            "paddw %%mm4, %%mm3\n\t"
            "movq %0, %%mm0\n\t"
            : : "m" (src[0]), "m" (src[1]), "m" (DD));

        asm volatile(
            /* dst[0..7] = ([mm2,mm3] + 32) >> 6 */
            "paddw %1, %%mm2\n\t"
            "paddw %1, %%mm3\n\t"
            "psrlw $6, %%mm2\n\t"
            "psrlw $6, %%mm3\n\t"
            "packuswb %%mm3, %%mm2\n\t"
            H264_CHROMA_OP(%0, %%mm2)
            "movq %%mm2, %0\n\t"
            : "=m" (dst[0]) : "m" (ff_pw_32));
        dst+= stride;
    }
}

static void H264_CHROMA_MC4_TMPL(uint8_t *dst/*align 4*/, uint8_t *src/*align 1*/, int stride, int h, int x, int y)
{
    DECLARE_ALIGNED_8(uint64_t, AA);
    DECLARE_ALIGNED_8(uint64_t, DD);
    int i;

    /* no special case for mv=(0,0) in 4x*, since it's much less common than in 8x*.
     * could still save a few cycles, but maybe not worth the complexity. */

    assert(x<8 && y<8 && x>=0 && y>=0);

    asm volatile("movd %2, %%mm4\n\t"
                 "movd %3, %%mm6\n\t"
                 "punpcklwd %%mm4, %%mm4\n\t"
                 "punpcklwd %%mm6, %%mm6\n\t"
                 "punpckldq %%mm4, %%mm4\n\t" /* mm4 = x words */
                 "punpckldq %%mm6, %%mm6\n\t" /* mm6 = y words */
                 "movq %%mm4, %%mm5\n\t"
                 "pmullw %%mm6, %%mm4\n\t"    /* mm4 = x * y */
                 "psllw $3, %%mm5\n\t"
                 "psllw $3, %%mm6\n\t"
                 "movq %%mm5, %%mm7\n\t"
                 "paddw %%mm6, %%mm7\n\t"
                 "movq %%mm4, %1\n\t"         /* DD = x * y */
                 "psubw %%mm4, %%mm5\n\t"     /* mm5 = B = 8x - xy */
                 "psubw %%mm4, %%mm6\n\t"     /* mm6 = C = 8y - xy */
                 "paddw %4, %%mm4\n\t"
                 "psubw %%mm7, %%mm4\n\t"     /* mm4 = A = xy - (8x+8y) + 64 */
                 "pxor %%mm7, %%mm7\n\t"
                 "movq %%mm4, %0\n\t"
                 : "=m" (AA), "=m" (DD) : "rm" (x), "rm" (y), "m" (ff_pw_64));

    asm volatile(
        /* mm0 = src[0..3], mm1 = src[1..4] */
        "movd %0, %%mm0\n\t"
        "movd %1, %%mm1\n\t"
        "punpcklbw %%mm7, %%mm0\n\t"
        "punpcklbw %%mm7, %%mm1\n\t"
        : : "m" (src[0]), "m" (src[1]));

    for(i=0; i<h; i++) {
        asm volatile(
            /* mm2 = A * src[0..3] + B * src[1..4] */
            "movq %%mm0, %%mm2\n\t"
            "pmullw %0, %%mm2\n\t"
            "pmullw %%mm5, %%mm1\n\t"
            "paddw %%mm1, %%mm2\n\t"
            : : "m" (AA));

        src += stride;
        asm volatile(
            /* mm0 = src[0..3], mm1 = src[1..4] */
            "movd %0, %%mm0\n\t"
            "movd %1, %%mm1\n\t"
            "punpcklbw %%mm7, %%mm0\n\t"
            "punpcklbw %%mm7, %%mm1\n\t"
            : : "m" (src[0]), "m" (src[1]));

        asm volatile(
            /* mm2 += C * src[0..3] + D * src[1..4] */
            "movq %%mm0, %%mm3\n\t"
            "movq %%mm1, %%mm4\n\t"
            "pmullw %%mm6, %%mm3\n\t"
            "pmullw %0, %%mm4\n\t"
            "paddw %%mm3, %%mm2\n\t"
            "paddw %%mm4, %%mm2\n\t"
            : : "m" (DD));

        asm volatile(
            /* dst[0..3] = pack((mm2 + 32) >> 6) */
            "paddw %1, %%mm2\n\t"
            "psrlw $6, %%mm2\n\t"
            "packuswb %%mm7, %%mm2\n\t"
            H264_CHROMA_OP4(%0, %%mm2, %%mm3)
            "movd %%mm2, %0\n\t"
            : "=m" (dst[0]) : "m" (ff_pw_32));
        dst += stride;
    }
}

#ifdef H264_CHROMA_MC2_TMPL
static void H264_CHROMA_MC2_TMPL(uint8_t *dst/*align 2*/, uint8_t *src/*align 1*/, int stride, int h, int x, int y)
{
    int CD=((1<<16)-1)*x*y + 8*y;
    int AB=((8<<16)-8)*x + 64 - CD;
    int i;

    asm volatile(
        /* mm5 = {A,B,A,B} */
        /* mm6 = {C,D,C,D} */
        "movd %0, %%mm5\n\t"
        "movd %1, %%mm6\n\t"
        "punpckldq %%mm5, %%mm5\n\t"
        "punpckldq %%mm6, %%mm6\n\t"
        "pxor %%mm7, %%mm7\n\t"
        :: "r"(AB), "r"(CD));

    asm volatile(
        /* mm0 = src[0,1,1,2] */
        "movd %0, %%mm0\n\t"
        "punpcklbw %%mm7, %%mm0\n\t"
        "pshufw $0x94, %%mm0, %%mm0\n\t"
        :: "m"(src[0]));

    for(i=0; i<h; i++) {
        asm volatile(
            /* mm1 = A * src[0,1] + B * src[1,2] */
            "movq    %%mm0, %%mm1\n\t"
            "pmaddwd %%mm5, %%mm1\n\t"
            ::);

        src += stride;
        asm volatile(
            /* mm0 = src[0,1,1,2] */
            "movd %0, %%mm0\n\t"
            "punpcklbw %%mm7, %%mm0\n\t"
            "pshufw $0x94, %%mm0, %%mm0\n\t"
            :: "m"(src[0]));

        asm volatile(
            /* mm1 += C * src[0,1] + D * src[1,2] */
            "movq    %%mm0, %%mm2\n\t"
            "pmaddwd %%mm6, %%mm2\n\t"
            "paddw   %%mm2, %%mm1\n\t"
            ::);

        asm volatile(
            /* dst[0,1] = pack((mm1 + 32) >> 6) */
            "paddw %1, %%mm1\n\t"
            "psrlw $6, %%mm1\n\t"
            "packssdw %%mm7, %%mm1\n\t"
            "packuswb %%mm7, %%mm1\n\t"
            /* writes garbage to the right of dst.
             * ok because partitions are processed from left to right. */
            H264_CHROMA_OP4(%0, %%mm1, %%mm3)
            "movd %%mm1, %0\n\t"
            : "=m" (dst[0]) : "m" (ff_pw_32));
        dst += stride;
    }
}
#endif

