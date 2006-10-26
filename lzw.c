/*
 * LZW decoder
 * Copyright (c) 2003 Fabrice Bellard.
 * Copyright (c) 2006 Konstantin Shishkov.
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
 * @file lzw.c
 * @brief LZW decoding routines
 * @author Fabrice Bellard
 * Modified for use in TIFF by Konstantin Shishkov
 */

#include "avcodec.h"
#include "lzw.h"

#define LZW_MAXBITS                 12
#define LZW_SIZTABLE                (1<<LZW_MAXBITS)

static const uint16_t mask[17] =
{
    0x0000, 0x0001, 0x0003, 0x0007,
    0x000F, 0x001F, 0x003F, 0x007F,
    0x00FF, 0x01FF, 0x03FF, 0x07FF,
    0x0FFF, 0x1FFF, 0x3FFF, 0x7FFF, 0xFFFF
};

struct LZWState {
    int eob_reached;
    uint8_t *pbuf, *ebuf;
    int bbits;
    unsigned int bbuf;

    int mode;                   ///< Decoder mode
    int cursize;                ///< The current code size
    int curmask;
    int codesize;
    int clear_code;
    int end_code;
    int newcodes;               ///< First available code
    int top_slot;               ///< Highest code for current size
    int top_slot2;              ///< Highest possible code for current size (<=top_slot)
    int slot;                   ///< Last read code
    int fc, oc;
    uint8_t *sp;
    uint8_t stack[LZW_SIZTABLE];
    uint8_t suffix[LZW_SIZTABLE];
    uint16_t prefix[LZW_SIZTABLE];
    int bs;                     ///< current buffer size for GIF
};

/* get one code from stream */
static int lzw_get_code(struct LZWState * s)
{
    int c, sizbuf;

    if(s->mode == FF_LZW_GIF) {
        while (s->bbits < s->cursize) {
            if (!s->bs) {
                sizbuf = *s->pbuf++;
                s->bs = sizbuf;
                if(!sizbuf) {
                    s->eob_reached = 1;
                }
            }
            s->bbuf |= (*s->pbuf++) << s->bbits;
            s->bbits += 8;
            s->bs--;
        }
        c = s->bbuf & s->curmask;
       s->bbuf >>= s->cursize;
    } else { // TIFF
        while (s->bbits < s->cursize) {
            if (s->pbuf >= s->ebuf) {
                s->eob_reached = 1;
            }
            s->bbuf = (s->bbuf << 8) | (*s->pbuf++);
            s->bbits += 8;
        }
        c = (s->bbuf >> (s->bbits - s->cursize)) & s->curmask;
    }
    s->bbits -= s->cursize;
    return c;
}

uint8_t* ff_lzw_cur_ptr(LZWState *p)
{
    return ((struct LZWState*)p)->pbuf;
}

void ff_lzw_decode_tail(LZWState *p)
{
    struct LZWState *s = (struct LZWState *)p;
    while(!s->eob_reached)
        lzw_get_code(s);
}

void ff_lzw_decode_open(LZWState **p)
{
    *p = av_mallocz(sizeof(struct LZWState));
}

void ff_lzw_decode_close(LZWState **p)
{
    av_freep(p);
}

/**
 * Initialize LZW decoder
 * @param s LZW context
 * @param csize initial code size in bits
 * @param buf input data
 * @param buf_size input data size
 * @param mode decoder working mode - either GIF or TIFF
 */
int ff_lzw_decode_init(LZWState *p, int csize, uint8_t *buf, int buf_size, int mode)
{
    struct LZWState *s = (struct LZWState *)p;

    if(csize < 1 || csize > LZW_MAXBITS)
        return -1;
    /* read buffer */
    s->eob_reached = 0;
    s->pbuf = buf;
    s->ebuf = s->pbuf + buf_size;
    s->bbuf = 0;
    s->bbits = 0;
    s->bs = 0;

    /* decoder */
    s->codesize = csize;
    s->cursize = s->codesize + 1;
    s->curmask = mask[s->cursize];
    s->top_slot = 1 << s->cursize;
    s->clear_code = 1 << s->codesize;
    s->end_code = s->clear_code + 1;
    s->slot = s->newcodes = s->clear_code + 2;
    s->oc = s->fc = 0;
    s->sp = s->stack;

    s->mode = mode;
    switch(s->mode){
    case FF_LZW_GIF:
        s->top_slot2 = s->top_slot;
        break;
    case FF_LZW_TIFF:
        s->top_slot2 = s->top_slot - 1;
        break;
    default:
        return -1;
    }
    return 0;
}

/**
 * Decode given number of bytes
 * NOTE: the algorithm here is inspired from the LZW GIF decoder
 *  written by Steven A. Bennett in 1987.
 *
 * @param s LZW context
 * @param buf output buffer
 * @param len number of bytes to decode
 * @return number of bytes decoded
 */
int ff_lzw_decode(LZWState *p, uint8_t *buf, int len){
    int l, c, code, oc, fc;
    uint8_t *sp;
    struct LZWState *s = (struct LZWState *)p;

    if (s->end_code < 0)
        return 0;

    l = len;
    sp = s->sp;
    oc = s->oc;
    fc = s->fc;

    while (sp > s->stack) {
        *buf++ = *(--sp);
        if ((--l) == 0)
            goto the_end;
    }

    for (;;) {
        c = lzw_get_code(s);
        if (c == s->end_code) {
            s->end_code = -1;
            break;
        } else if (c == s->clear_code) {
            s->cursize = s->codesize + 1;
            s->curmask = mask[s->cursize];
            s->slot = s->newcodes;
            s->top_slot = 1 << s->cursize;
            s->top_slot2 = s->top_slot;
            if(s->mode == FF_LZW_TIFF)
                s->top_slot2--;
            while ((c = lzw_get_code(s)) == s->clear_code);
            if (c == s->end_code) {
                s->end_code = -1;
                break;
            }
            /* test error */
            if (c >= s->slot)
                c = 0;
            fc = oc = c;
            *buf++ = c;
            if ((--l) == 0)
                break;
        } else {
            code = c;
            if (code >= s->slot) {
                *sp++ = fc;
                code = oc;
            }
            while (code >= s->newcodes) {
                *sp++ = s->suffix[code];
                code = s->prefix[code];
            }
            *sp++ = code;
            if (s->slot < s->top_slot) {
                s->suffix[s->slot] = fc = code;
                s->prefix[s->slot++] = oc;
                oc = c;
            }
            if (s->slot >= s->top_slot2) {
                if (s->cursize < LZW_MAXBITS) {
                    s->top_slot <<= 1;
                    s->top_slot2 = s->top_slot;
                    if(s->mode == FF_LZW_TIFF)
                        s->top_slot2--;
                    s->curmask = mask[++s->cursize];
                }
            }
            while (sp > s->stack) {
                *buf++ = *(--sp);
                if ((--l) == 0)
                    goto the_end;
            }
        }
    }
  the_end:
    s->sp = sp;
    s->oc = oc;
    s->fc = fc;
    return len - l;
}
