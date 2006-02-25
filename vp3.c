/*
 * Copyright (C) 2003-2004 the ffmpeg project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

/**
 * @file vp3.c
 * On2 VP3 Video Decoder
 *
 * VP3 Video Decoder by Mike Melanson (mike at multimedia.cx)
 * For more information about the VP3 coding process, visit:
 *   http://multimedia.cx/
 *
 * Theora decoder by Alex Beregszaszi
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "avcodec.h"
#include "dsputil.h"
#include "mpegvideo.h"

#include "vp3data.h"

#define FRAGMENT_PIXELS 8

/*
 * Debugging Variables
 *
 * Define one or more of the following compile-time variables to 1 to obtain
 * elaborate information about certain aspects of the decoding process.
 *
 * KEYFRAMES_ONLY: set this to 1 to only see keyframes (VP3 slideshow mode)
 * DEBUG_VP3: high-level decoding flow
 * DEBUG_INIT: initialization parameters
 * DEBUG_DEQUANTIZERS: display how the dequanization tables are built
 * DEBUG_BLOCK_CODING: unpacking the superblock/macroblock/fragment coding
 * DEBUG_MODES: unpacking the coding modes for individual fragments
 * DEBUG_VECTORS: display the motion vectors
 * DEBUG_TOKEN: display exhaustive information about each DCT token
 * DEBUG_VLC: display the VLCs as they are extracted from the stream
 * DEBUG_DC_PRED: display the process of reversing DC prediction
 * DEBUG_IDCT: show every detail of the IDCT process
 */

#define KEYFRAMES_ONLY 0

#define DEBUG_VP3 0
#define DEBUG_INIT 0
#define DEBUG_DEQUANTIZERS 0
#define DEBUG_BLOCK_CODING 0
#define DEBUG_MODES 0
#define DEBUG_VECTORS 0
#define DEBUG_TOKEN 0
#define DEBUG_VLC 0
#define DEBUG_DC_PRED 0
#define DEBUG_IDCT 0

#if DEBUG_VP3
#define debug_vp3(args...) av_log(NULL, AV_LOG_DEBUG, ## args)
#else
static inline void debug_vp3(const char *format, ...) { }
#endif

#if DEBUG_INIT
#define debug_init(args...) av_log(NULL, AV_LOG_DEBUG, ## args)
#else
static inline void debug_init(const char *format, ...) { }
#endif

#if DEBUG_DEQUANTIZERS
#define debug_dequantizers(args...) av_log(NULL, AV_LOG_DEBUG, ## args)
#else
static inline void debug_dequantizers(const char *format, ...) { }
#endif

#if DEBUG_BLOCK_CODING
#define debug_block_coding(args...) av_log(NULL, AV_LOG_DEBUG, ## args)
#else
static inline void debug_block_coding(const char *format, ...) { }
#endif

#if DEBUG_MODES
#define debug_modes(args...) av_log(NULL, AV_LOG_DEBUG, ## args)
#else
static inline void debug_modes(const char *format, ...) { }
#endif

#if DEBUG_VECTORS
#define debug_vectors(args...) av_log(NULL, AV_LOG_DEBUG, ## args)
#else
static inline void debug_vectors(const char *format, ...) { }
#endif

#if DEBUG_TOKEN
#define debug_token(args...) av_log(NULL, AV_LOG_DEBUG, ## args)
#else
static inline void debug_token(const char *format, ...) { }
#endif

#if DEBUG_VLC
#define debug_vlc(args...) av_log(NULL, AV_LOG_DEBUG, ## args)
#else
static inline void debug_vlc(const char *format, ...) { }
#endif

#if DEBUG_DC_PRED
#define debug_dc_pred(args...) av_log(NULL, AV_LOG_DEBUG, ## args)
#else
static inline void debug_dc_pred(const char *format, ...) { }
#endif

#if DEBUG_IDCT
#define debug_idct(args...) av_log(NULL, AV_LOG_DEBUG, ## args)
#else
static inline void debug_idct(const char *format, ...) { }
#endif

typedef struct Coeff {
    struct Coeff *next;
    DCTELEM coeff;
    uint8_t index;
} Coeff;

//FIXME split things out into their own arrays
typedef struct Vp3Fragment {
    Coeff *next_coeff;
    /* address of first pixel taking into account which plane the fragment
     * lives on as well as the plane stride */
    int first_pixel;
    /* this is the macroblock that the fragment belongs to */
    uint16_t macroblock;
    uint8_t coding_method;
    uint8_t coeff_count;
    int8_t motion_x;
    int8_t motion_y;
} Vp3Fragment;

#define SB_NOT_CODED        0
#define SB_PARTIALLY_CODED  1
#define SB_FULLY_CODED      2

#define MODE_INTER_NO_MV      0
#define MODE_INTRA            1
#define MODE_INTER_PLUS_MV    2
#define MODE_INTER_LAST_MV    3
#define MODE_INTER_PRIOR_LAST 4
#define MODE_USING_GOLDEN     5
#define MODE_GOLDEN_MV        6
#define MODE_INTER_FOURMV     7
#define CODING_MODE_COUNT     8

/* special internal mode */
#define MODE_COPY             8

/* There are 6 preset schemes, plus a free-form scheme */
static int ModeAlphabet[7][CODING_MODE_COUNT] =
{
    /* this is the custom scheme */
    { 0, 0, 0, 0, 0, 0, 0, 0 },

    /* scheme 1: Last motion vector dominates */
    {    MODE_INTER_LAST_MV,    MODE_INTER_PRIOR_LAST,
         MODE_INTER_PLUS_MV,    MODE_INTER_NO_MV,
         MODE_INTRA,            MODE_USING_GOLDEN,
         MODE_GOLDEN_MV,        MODE_INTER_FOURMV },

    /* scheme 2 */
    {    MODE_INTER_LAST_MV,    MODE_INTER_PRIOR_LAST,
         MODE_INTER_NO_MV,      MODE_INTER_PLUS_MV,
         MODE_INTRA,            MODE_USING_GOLDEN,
         MODE_GOLDEN_MV,        MODE_INTER_FOURMV },

    /* scheme 3 */
    {    MODE_INTER_LAST_MV,    MODE_INTER_PLUS_MV,
         MODE_INTER_PRIOR_LAST, MODE_INTER_NO_MV,
         MODE_INTRA,            MODE_USING_GOLDEN,
         MODE_GOLDEN_MV,        MODE_INTER_FOURMV },

    /* scheme 4 */
    {    MODE_INTER_LAST_MV,    MODE_INTER_PLUS_MV,
         MODE_INTER_NO_MV,      MODE_INTER_PRIOR_LAST,
         MODE_INTRA,            MODE_USING_GOLDEN,
         MODE_GOLDEN_MV,        MODE_INTER_FOURMV },

    /* scheme 5: No motion vector dominates */
    {    MODE_INTER_NO_MV,      MODE_INTER_LAST_MV,
         MODE_INTER_PRIOR_LAST, MODE_INTER_PLUS_MV,
         MODE_INTRA,            MODE_USING_GOLDEN,
         MODE_GOLDEN_MV,        MODE_INTER_FOURMV },

    /* scheme 6 */
    {    MODE_INTER_NO_MV,      MODE_USING_GOLDEN,
         MODE_INTER_LAST_MV,    MODE_INTER_PRIOR_LAST,
         MODE_INTER_PLUS_MV,    MODE_INTRA,
         MODE_GOLDEN_MV,        MODE_INTER_FOURMV },

};

#define MIN_DEQUANT_VAL 2

typedef struct Vp3DecodeContext {
    AVCodecContext *avctx;
    int theora, theora_tables;
    int version;
    int width, height;
    AVFrame golden_frame;
    AVFrame last_frame;
    AVFrame current_frame;
    int keyframe;
    DSPContext dsp;
    int flipped_image;

    int quality_index;
    int last_quality_index;

    int superblock_count;
    int superblock_width;
    int superblock_height;
    int y_superblock_width;
    int y_superblock_height;
    int c_superblock_width;
    int c_superblock_height;
    int u_superblock_start;
    int v_superblock_start;
    unsigned char *superblock_coding;

    int macroblock_count;
    int macroblock_width;
    int macroblock_height;

    int fragment_count;
    int fragment_width;
    int fragment_height;

    Vp3Fragment *all_fragments;
    Coeff *coeffs;
    Coeff *next_coeff;
    int u_fragment_start;
    int v_fragment_start;

    ScanTable scantable;

    /* tables */
    uint16_t coded_dc_scale_factor[64];
    uint32_t coded_ac_scale_factor[64];
    uint16_t coded_intra_y_dequant[64];
    uint16_t coded_intra_c_dequant[64];
    uint16_t coded_inter_dequant[64];

    /* this is a list of indices into the all_fragments array indicating
     * which of the fragments are coded */
    int *coded_fragment_list;
    int coded_fragment_list_index;
    int pixel_addresses_inited;

    VLC dc_vlc[16];
    VLC ac_vlc_1[16];
    VLC ac_vlc_2[16];
    VLC ac_vlc_3[16];
    VLC ac_vlc_4[16];

    VLC superblock_run_length_vlc;
    VLC fragment_run_length_vlc;
    VLC mode_code_vlc;
    VLC motion_vector_vlc;

    /* these arrays need to be on 16-byte boundaries since SSE2 operations
     * index into them */
    DECLARE_ALIGNED_16(int16_t, intra_y_dequant[64]);
    DECLARE_ALIGNED_16(int16_t, intra_c_dequant[64]);
    DECLARE_ALIGNED_16(int16_t, inter_dequant[64]);

    /* This table contains superblock_count * 16 entries. Each set of 16
     * numbers corresponds to the fragment indices 0..15 of the superblock.
     * An entry will be -1 to indicate that no entry corresponds to that
     * index. */
    int *superblock_fragments;

    /* This table contains superblock_count * 4 entries. Each set of 4
     * numbers corresponds to the macroblock indices 0..3 of the superblock.
     * An entry will be -1 to indicate that no entry corresponds to that
     * index. */
    int *superblock_macroblocks;

    /* This table contains macroblock_count * 6 entries. Each set of 6
     * numbers corresponds to the fragment indices 0..5 which comprise
     * the macroblock (4 Y fragments and 2 C fragments). */
    int *macroblock_fragments;
    /* This is an array that indicates how a particular macroblock
     * is coded. */
    unsigned char *macroblock_coding;

    int first_coded_y_fragment;
    int first_coded_c_fragment;
    int last_coded_y_fragment;
    int last_coded_c_fragment;

    uint8_t edge_emu_buffer[9*2048]; //FIXME dynamic alloc
    uint8_t qscale_table[2048]; //FIXME dynamic alloc (width+15)/16

    /* Huffman decode */
    int hti;
    unsigned int hbits;
    int entries;
    int huff_code_size;
    uint16_t huffman_table[80][32][2];

    uint32_t filter_limit_values[64];
    int bounding_values_array[256];
} Vp3DecodeContext;

static int theora_decode_tables(AVCodecContext *avctx, GetBitContext gb);

/************************************************************************
 * VP3 specific functions
 ************************************************************************/

/*
 * This function sets up all of the various blocks mappings:
 * superblocks <-> fragments, macroblocks <-> fragments,
 * superblocks <-> macroblocks
 *
 * Returns 0 is successful; returns 1 if *anything* went wrong.
 */
static int init_block_mapping(Vp3DecodeContext *s)
{
    int i, j;
    signed int hilbert_walk_y[16];
    signed int hilbert_walk_c[16];
    signed int hilbert_walk_mb[4];

    int current_fragment = 0;
    int current_width = 0;
    int current_height = 0;
    int right_edge = 0;
    int bottom_edge = 0;
    int superblock_row_inc = 0;
    int *hilbert = NULL;
    int mapping_index = 0;

    int current_macroblock;
    int c_fragment;

    signed char travel_width[16] = {
         1,  1,  0, -1,
         0,  0,  1,  0,
         1,  0,  1,  0,
         0, -1,  0,  1
    };

    signed char travel_height[16] = {
         0,  0,  1,  0,
         1,  1,  0, -1,
         0,  1,  0, -1,
        -1,  0, -1,  0
    };

    signed char travel_width_mb[4] = {
         1,  0,  1,  0
    };

    signed char travel_height_mb[4] = {
         0,  1,  0, -1
    };

    debug_vp3("  vp3: initialize block mapping tables\n");

    /* figure out hilbert pattern per these frame dimensions */
    hilbert_walk_y[0]  = 1;
    hilbert_walk_y[1]  = 1;
    hilbert_walk_y[2]  = s->fragment_width;
    hilbert_walk_y[3]  = -1;
    hilbert_walk_y[4]  = s->fragment_width;
    hilbert_walk_y[5]  = s->fragment_width;
    hilbert_walk_y[6]  = 1;
    hilbert_walk_y[7]  = -s->fragment_width;
    hilbert_walk_y[8]  = 1;
    hilbert_walk_y[9]  = s->fragment_width;
    hilbert_walk_y[10]  = 1;
    hilbert_walk_y[11] = -s->fragment_width;
    hilbert_walk_y[12] = -s->fragment_width;
    hilbert_walk_y[13] = -1;
    hilbert_walk_y[14] = -s->fragment_width;
    hilbert_walk_y[15] = 1;

    hilbert_walk_c[0]  = 1;
    hilbert_walk_c[1]  = 1;
    hilbert_walk_c[2]  = s->fragment_width / 2;
    hilbert_walk_c[3]  = -1;
    hilbert_walk_c[4]  = s->fragment_width / 2;
    hilbert_walk_c[5]  = s->fragment_width / 2;
    hilbert_walk_c[6]  = 1;
    hilbert_walk_c[7]  = -s->fragment_width / 2;
    hilbert_walk_c[8]  = 1;
    hilbert_walk_c[9]  = s->fragment_width / 2;
    hilbert_walk_c[10]  = 1;
    hilbert_walk_c[11] = -s->fragment_width / 2;
    hilbert_walk_c[12] = -s->fragment_width / 2;
    hilbert_walk_c[13] = -1;
    hilbert_walk_c[14] = -s->fragment_width / 2;
    hilbert_walk_c[15] = 1;

    hilbert_walk_mb[0] = 1;
    hilbert_walk_mb[1] = s->macroblock_width;
    hilbert_walk_mb[2] = 1;
    hilbert_walk_mb[3] = -s->macroblock_width;

    /* iterate through each superblock (all planes) and map the fragments */
    for (i = 0; i < s->superblock_count; i++) {
        debug_init("    superblock %d (u starts @ %d, v starts @ %d)\n",
            i, s->u_superblock_start, s->v_superblock_start);

        /* time to re-assign the limits? */
        if (i == 0) {

            /* start of Y superblocks */
            right_edge = s->fragment_width;
            bottom_edge = s->fragment_height;
            current_width = -1;
            current_height = 0;
            superblock_row_inc = 3 * s->fragment_width -
                (s->y_superblock_width * 4 - s->fragment_width);
            hilbert = hilbert_walk_y;

            /* the first operation for this variable is to advance by 1 */
            current_fragment = -1;

        } else if (i == s->u_superblock_start) {

            /* start of U superblocks */
            right_edge = s->fragment_width / 2;
            bottom_edge = s->fragment_height / 2;
            current_width = -1;
            current_height = 0;
            superblock_row_inc = 3 * (s->fragment_width / 2) -
                (s->c_superblock_width * 4 - s->fragment_width / 2);
            hilbert = hilbert_walk_c;

            /* the first operation for this variable is to advance by 1 */
            current_fragment = s->u_fragment_start - 1;

        } else if (i == s->v_superblock_start) {

            /* start of V superblocks */
            right_edge = s->fragment_width / 2;
            bottom_edge = s->fragment_height / 2;
            current_width = -1;
            current_height = 0;
            superblock_row_inc = 3 * (s->fragment_width / 2) -
                (s->c_superblock_width * 4 - s->fragment_width / 2);
            hilbert = hilbert_walk_c;

            /* the first operation for this variable is to advance by 1 */
            current_fragment = s->v_fragment_start - 1;

        }

        if (current_width >= right_edge - 1) {
            /* reset width and move to next superblock row */
            current_width = -1;
            current_height += 4;

            /* fragment is now at the start of a new superblock row */
            current_fragment += superblock_row_inc;
        }

        /* iterate through all 16 fragments in a superblock */
        for (j = 0; j < 16; j++) {
            current_fragment += hilbert[j];
            current_width += travel_width[j];
            current_height += travel_height[j];

            /* check if the fragment is in bounds */
            if ((current_width < right_edge) &&
                (current_height < bottom_edge)) {
                s->superblock_fragments[mapping_index] = current_fragment;
                debug_init("    mapping fragment %d to superblock %d, position %d (%d/%d x %d/%d)\n",
                    s->superblock_fragments[mapping_index], i, j,
                    current_width, right_edge, current_height, bottom_edge);
            } else {
                s->superblock_fragments[mapping_index] = -1;
                debug_init("    superblock %d, position %d has no fragment (%d/%d x %d/%d)\n",
                    i, j,
                    current_width, right_edge, current_height, bottom_edge);
            }

            mapping_index++;
        }
    }

    /* initialize the superblock <-> macroblock mapping; iterate through
     * all of the Y plane superblocks to build this mapping */
    right_edge = s->macroblock_width;
    bottom_edge = s->macroblock_height;
    current_width = -1;
    current_height = 0;
    superblock_row_inc = s->macroblock_width -
        (s->y_superblock_width * 2 - s->macroblock_width);;
    hilbert = hilbert_walk_mb;
    mapping_index = 0;
    current_macroblock = -1;
    for (i = 0; i < s->u_superblock_start; i++) {

        if (current_width >= right_edge - 1) {
            /* reset width and move to next superblock row */
            current_width = -1;
            current_height += 2;

            /* macroblock is now at the start of a new superblock row */
            current_macroblock += superblock_row_inc;
        }

        /* iterate through each potential macroblock in the superblock */
        for (j = 0; j < 4; j++) {
            current_macroblock += hilbert_walk_mb[j];
            current_width += travel_width_mb[j];
            current_height += travel_height_mb[j];

            /* check if the macroblock is in bounds */
            if ((current_width < right_edge) &&
                (current_height < bottom_edge)) {
                s->superblock_macroblocks[mapping_index] = current_macroblock;
                debug_init("    mapping macroblock %d to superblock %d, position %d (%d/%d x %d/%d)\n",
                    s->superblock_macroblocks[mapping_index], i, j,
                    current_width, right_edge, current_height, bottom_edge);
            } else {
                s->superblock_macroblocks[mapping_index] = -1;
                debug_init("    superblock %d, position %d has no macroblock (%d/%d x %d/%d)\n",
                    i, j,
                    current_width, right_edge, current_height, bottom_edge);
            }

            mapping_index++;
        }
    }

    /* initialize the macroblock <-> fragment mapping */
    current_fragment = 0;
    current_macroblock = 0;
    mapping_index = 0;
    for (i = 0; i < s->fragment_height; i += 2) {

        for (j = 0; j < s->fragment_width; j += 2) {

            debug_init("    macroblock %d contains fragments: ", current_macroblock);
            s->all_fragments[current_fragment].macroblock = current_macroblock;
            s->macroblock_fragments[mapping_index++] = current_fragment;
            debug_init("%d ", current_fragment);

            if (j + 1 < s->fragment_width) {
                s->all_fragments[current_fragment + 1].macroblock = current_macroblock;
                s->macroblock_fragments[mapping_index++] = current_fragment + 1;
                debug_init("%d ", current_fragment + 1);
            } else
                s->macroblock_fragments[mapping_index++] = -1;

            if (i + 1 < s->fragment_height) {
                s->all_fragments[current_fragment + s->fragment_width].macroblock =
                    current_macroblock;
                s->macroblock_fragments[mapping_index++] =
                    current_fragment + s->fragment_width;
                debug_init("%d ", current_fragment + s->fragment_width);
            } else
                s->macroblock_fragments[mapping_index++] = -1;

            if ((j + 1 < s->fragment_width) && (i + 1 < s->fragment_height)) {
                s->all_fragments[current_fragment + s->fragment_width + 1].macroblock =
                    current_macroblock;
                s->macroblock_fragments[mapping_index++] =
                    current_fragment + s->fragment_width + 1;
                debug_init("%d ", current_fragment + s->fragment_width + 1);
            } else
                s->macroblock_fragments[mapping_index++] = -1;

            /* C planes */
            c_fragment = s->u_fragment_start +
                (i * s->fragment_width / 4) + (j / 2);
            s->all_fragments[c_fragment].macroblock = s->macroblock_count;
            s->macroblock_fragments[mapping_index++] = c_fragment;
            debug_init("%d ", c_fragment);

            c_fragment = s->v_fragment_start +
                (i * s->fragment_width / 4) + (j / 2);
            s->all_fragments[c_fragment].macroblock = s->macroblock_count;
            s->macroblock_fragments[mapping_index++] = c_fragment;
            debug_init("%d ", c_fragment);

            debug_init("\n");

            if (j + 2 <= s->fragment_width)
                current_fragment += 2;
            else
                current_fragment++;
            current_macroblock++;
        }

        current_fragment += s->fragment_width;
    }

    return 0;  /* successful path out */
}

/*
 * This function wipes out all of the fragment data.
 */
static void init_frame(Vp3DecodeContext *s, GetBitContext *gb)
{
    int i;

    /* zero out all of the fragment information */
    s->coded_fragment_list_index = 0;
    for (i = 0; i < s->fragment_count; i++) {
        s->all_fragments[i].coeff_count = 0;
        s->all_fragments[i].motion_x = 127;
        s->all_fragments[i].motion_y = 127;
        s->all_fragments[i].next_coeff= NULL;
        s->coeffs[i].index=
        s->coeffs[i].coeff=0;
        s->coeffs[i].next= NULL;
    }
}

/*
 * This function sets up the dequantization tables used for a particular
 * frame.
 */
static void init_dequantizer(Vp3DecodeContext *s)
{

    int ac_scale_factor = s->coded_ac_scale_factor[s->quality_index];
    int dc_scale_factor = s->coded_dc_scale_factor[s->quality_index];
    int i, j;

    debug_vp3("  vp3: initializing dequantization tables\n");

    /*
     * Scale dequantizers:
     *
     *   quantizer * sf
     *   --------------
     *        100
     *
     * where sf = dc_scale_factor for DC quantizer
     *         or ac_scale_factor for AC quantizer
     *
     * Then, saturate the result to a lower limit of MIN_DEQUANT_VAL.
     */
#define SCALER 4

    /* scale DC quantizers */
    s->intra_y_dequant[0] = s->coded_intra_y_dequant[0] * dc_scale_factor / 100;
    if (s->intra_y_dequant[0] < MIN_DEQUANT_VAL * 2)
        s->intra_y_dequant[0] = MIN_DEQUANT_VAL * 2;
    s->intra_y_dequant[0] *= SCALER;

    s->intra_c_dequant[0] = s->coded_intra_c_dequant[0] * dc_scale_factor / 100;
    if (s->intra_c_dequant[0] < MIN_DEQUANT_VAL * 2)
        s->intra_c_dequant[0] = MIN_DEQUANT_VAL * 2;
    s->intra_c_dequant[0] *= SCALER;

    s->inter_dequant[0] = s->coded_inter_dequant[0] * dc_scale_factor / 100;
    if (s->inter_dequant[0] < MIN_DEQUANT_VAL * 4)
        s->inter_dequant[0] = MIN_DEQUANT_VAL * 4;
    s->inter_dequant[0] *= SCALER;

    /* scale AC quantizers, zigzag at the same time in preparation for
     * the dequantization phase */
    for (i = 1; i < 64; i++) {
        int k= s->scantable.scantable[i];
        j = s->scantable.permutated[i];

        s->intra_y_dequant[j] = s->coded_intra_y_dequant[k] * ac_scale_factor / 100;
        if (s->intra_y_dequant[j] < MIN_DEQUANT_VAL)
            s->intra_y_dequant[j] = MIN_DEQUANT_VAL;
        s->intra_y_dequant[j] *= SCALER;

        s->intra_c_dequant[j] = s->coded_intra_c_dequant[k] * ac_scale_factor / 100;
        if (s->intra_c_dequant[j] < MIN_DEQUANT_VAL)
            s->intra_c_dequant[j] = MIN_DEQUANT_VAL;
        s->intra_c_dequant[j] *= SCALER;

        s->inter_dequant[j] = s->coded_inter_dequant[k] * ac_scale_factor / 100;
        if (s->inter_dequant[j] < MIN_DEQUANT_VAL * 2)
            s->inter_dequant[j] = MIN_DEQUANT_VAL * 2;
        s->inter_dequant[j] *= SCALER;
    }

    memset(s->qscale_table, (FFMAX(s->intra_y_dequant[1], s->intra_c_dequant[1])+8)/16, 512); //FIXME finetune

    /* print debug information as requested */
    debug_dequantizers("intra Y dequantizers:\n");
    for (i = 0; i < 8; i++) {
      for (j = i * 8; j < i * 8 + 8; j++) {
        debug_dequantizers(" %4d,", s->intra_y_dequant[j]);
      }
      debug_dequantizers("\n");
    }
    debug_dequantizers("\n");

    debug_dequantizers("intra C dequantizers:\n");
    for (i = 0; i < 8; i++) {
      for (j = i * 8; j < i * 8 + 8; j++) {
        debug_dequantizers(" %4d,", s->intra_c_dequant[j]);
      }
      debug_dequantizers("\n");
    }
    debug_dequantizers("\n");

    debug_dequantizers("interframe dequantizers:\n");
    for (i = 0; i < 8; i++) {
      for (j = i * 8; j < i * 8 + 8; j++) {
        debug_dequantizers(" %4d,", s->inter_dequant[j]);
      }
      debug_dequantizers("\n");
    }
    debug_dequantizers("\n");
}

/*
 * This function initializes the loop filter boundary limits if the frame's
 * quality index is different from the previous frame's.
 */
static void init_loop_filter(Vp3DecodeContext *s)
{
    int *bounding_values= s->bounding_values_array+127;
    int filter_limit;
    int x;

    filter_limit = s->filter_limit_values[s->quality_index];

    /* set up the bounding values */
    memset(s->bounding_values_array, 0, 256 * sizeof(int));
    for (x = 0; x < filter_limit; x++) {
        bounding_values[-x - filter_limit] = -filter_limit + x;
        bounding_values[-x] = -x;
        bounding_values[x] = x;
        bounding_values[x + filter_limit] = filter_limit - x;
    }
}

/*
 * This function unpacks all of the superblock/macroblock/fragment coding
 * information from the bitstream.
 */
static int unpack_superblocks(Vp3DecodeContext *s, GetBitContext *gb)
{
    int bit = 0;
    int current_superblock = 0;
    int current_run = 0;
    int decode_fully_flags = 0;
    int decode_partial_blocks = 0;
    int first_c_fragment_seen;

    int i, j;
    int current_fragment;

    debug_vp3("  vp3: unpacking superblock coding\n");

    if (s->keyframe) {

        debug_vp3("    keyframe-- all superblocks are fully coded\n");
        memset(s->superblock_coding, SB_FULLY_CODED, s->superblock_count);

    } else {

        /* unpack the list of partially-coded superblocks */
        bit = get_bits(gb, 1);
        /* toggle the bit because as soon as the first run length is
         * fetched the bit will be toggled again */
        bit ^= 1;
        while (current_superblock < s->superblock_count) {
            if (current_run-- == 0) {
                bit ^= 1;
                current_run = get_vlc2(gb,
                    s->superblock_run_length_vlc.table, 6, 2);
                if (current_run == 33)
                    current_run += get_bits(gb, 12);
                debug_block_coding("      setting superblocks %d..%d to %s\n",
                    current_superblock,
                    current_superblock + current_run - 1,
                    (bit) ? "partially coded" : "not coded");

                /* if any of the superblocks are not partially coded, flag
                 * a boolean to decode the list of fully-coded superblocks */
                if (bit == 0) {
                    decode_fully_flags = 1;
                } else {

                    /* make a note of the fact that there are partially coded
                     * superblocks */
                    decode_partial_blocks = 1;
                }
            }
            s->superblock_coding[current_superblock++] = bit;
        }

        /* unpack the list of fully coded superblocks if any of the blocks were
         * not marked as partially coded in the previous step */
        if (decode_fully_flags) {

            current_superblock = 0;
            current_run = 0;
            bit = get_bits(gb, 1);
            /* toggle the bit because as soon as the first run length is
             * fetched the bit will be toggled again */
            bit ^= 1;
            while (current_superblock < s->superblock_count) {

                /* skip any superblocks already marked as partially coded */
                if (s->superblock_coding[current_superblock] == SB_NOT_CODED) {

                    if (current_run-- == 0) {
                        bit ^= 1;
                        current_run = get_vlc2(gb,
                            s->superblock_run_length_vlc.table, 6, 2);
                        if (current_run == 33)
                            current_run += get_bits(gb, 12);
                    }

                    debug_block_coding("      setting superblock %d to %s\n",
                        current_superblock,
                        (bit) ? "fully coded" : "not coded");
                    s->superblock_coding[current_superblock] = 2*bit;
                }
                current_superblock++;
            }
        }

        /* if there were partial blocks, initialize bitstream for
         * unpacking fragment codings */
        if (decode_partial_blocks) {

            current_run = 0;
            bit = get_bits(gb, 1);
            /* toggle the bit because as soon as the first run length is
             * fetched the bit will be toggled again */
            bit ^= 1;
        }
    }

    /* figure out which fragments are coded; iterate through each
     * superblock (all planes) */
    s->coded_fragment_list_index = 0;
    s->next_coeff= s->coeffs + s->fragment_count;
    s->first_coded_y_fragment = s->first_coded_c_fragment = 0;
    s->last_coded_y_fragment = s->last_coded_c_fragment = -1;
    first_c_fragment_seen = 0;
    memset(s->macroblock_coding, MODE_COPY, s->macroblock_count);
    for (i = 0; i < s->superblock_count; i++) {

        /* iterate through all 16 fragments in a superblock */
        for (j = 0; j < 16; j++) {

            /* if the fragment is in bounds, check its coding status */
            current_fragment = s->superblock_fragments[i * 16 + j];
            if (current_fragment >= s->fragment_count) {
                av_log(s->avctx, AV_LOG_ERROR, "  vp3:unpack_superblocks(): bad fragment number (%d >= %d)\n",
                    current_fragment, s->fragment_count);
                return 1;
            }
            if (current_fragment != -1) {
                if (s->superblock_coding[i] == SB_NOT_CODED) {

                    /* copy all the fragments from the prior frame */
                    s->all_fragments[current_fragment].coding_method =
                        MODE_COPY;

                } else if (s->superblock_coding[i] == SB_PARTIALLY_CODED) {

                    /* fragment may or may not be coded; this is the case
                     * that cares about the fragment coding runs */
                    if (current_run-- == 0) {
                        bit ^= 1;
                        current_run = get_vlc2(gb,
                            s->fragment_run_length_vlc.table, 5, 2);
                    }

                    if (bit) {
                        /* default mode; actual mode will be decoded in
                         * the next phase */
                        s->all_fragments[current_fragment].coding_method =
                            MODE_INTER_NO_MV;
                        s->all_fragments[current_fragment].next_coeff= s->coeffs + current_fragment;
                        s->coded_fragment_list[s->coded_fragment_list_index] =
                            current_fragment;
                        if ((current_fragment >= s->u_fragment_start) &&
                            (s->last_coded_y_fragment == -1) &&
                            (!first_c_fragment_seen)) {
                            s->first_coded_c_fragment = s->coded_fragment_list_index;
                            s->last_coded_y_fragment = s->first_coded_c_fragment - 1;
                            first_c_fragment_seen = 1;
                        }
                        s->coded_fragment_list_index++;
                        s->macroblock_coding[s->all_fragments[current_fragment].macroblock] = MODE_INTER_NO_MV;
                        debug_block_coding("      superblock %d is partially coded, fragment %d is coded\n",
                            i, current_fragment);
                    } else {
                        /* not coded; copy this fragment from the prior frame */
                        s->all_fragments[current_fragment].coding_method =
                            MODE_COPY;
                        debug_block_coding("      superblock %d is partially coded, fragment %d is not coded\n",
                            i, current_fragment);
                    }

                } else {

                    /* fragments are fully coded in this superblock; actual
                     * coding will be determined in next step */
                    s->all_fragments[current_fragment].coding_method =
                        MODE_INTER_NO_MV;
                    s->all_fragments[current_fragment].next_coeff= s->coeffs + current_fragment;
                    s->coded_fragment_list[s->coded_fragment_list_index] =
                        current_fragment;
                    if ((current_fragment >= s->u_fragment_start) &&
                        (s->last_coded_y_fragment == -1) &&
                        (!first_c_fragment_seen)) {
                        s->first_coded_c_fragment = s->coded_fragment_list_index;
                        s->last_coded_y_fragment = s->first_coded_c_fragment - 1;
                        first_c_fragment_seen = 1;
                    }
                    s->coded_fragment_list_index++;
                    s->macroblock_coding[s->all_fragments[current_fragment].macroblock] = MODE_INTER_NO_MV;
                    debug_block_coding("      superblock %d is fully coded, fragment %d is coded\n",
                        i, current_fragment);
                }
            }
        }
    }

    if (!first_c_fragment_seen)
        /* only Y fragments coded in this frame */
        s->last_coded_y_fragment = s->coded_fragment_list_index - 1;
    else
        /* end the list of coded C fragments */
        s->last_coded_c_fragment = s->coded_fragment_list_index - 1;

    debug_block_coding("    %d total coded fragments, y: %d -> %d, c: %d -> %d\n",
        s->coded_fragment_list_index,
        s->first_coded_y_fragment,
        s->last_coded_y_fragment,
        s->first_coded_c_fragment,
        s->last_coded_c_fragment);

    return 0;
}

/*
 * This function unpacks all the coding mode data for individual macroblocks
 * from the bitstream.
 */
static int unpack_modes(Vp3DecodeContext *s, GetBitContext *gb)
{
    int i, j, k;
    int scheme;
    int current_macroblock;
    int current_fragment;
    int coding_mode;

    debug_vp3("  vp3: unpacking encoding modes\n");

    if (s->keyframe) {
        debug_vp3("    keyframe-- all blocks are coded as INTRA\n");

        for (i = 0; i < s->fragment_count; i++)
            s->all_fragments[i].coding_method = MODE_INTRA;

    } else {

        /* fetch the mode coding scheme for this frame */
        scheme = get_bits(gb, 3);
        debug_modes("    using mode alphabet %d\n", scheme);

        /* is it a custom coding scheme? */
        if (scheme == 0) {
            debug_modes("    custom mode alphabet ahead:\n");
            for (i = 0; i < 8; i++)
                ModeAlphabet[scheme][get_bits(gb, 3)] = i;
        }

        for (i = 0; i < 8; i++)
            debug_modes("      mode[%d][%d] = %d\n", scheme, i,
                ModeAlphabet[scheme][i]);

        /* iterate through all of the macroblocks that contain 1 or more
         * coded fragments */
        for (i = 0; i < s->u_superblock_start; i++) {

            for (j = 0; j < 4; j++) {
                current_macroblock = s->superblock_macroblocks[i * 4 + j];
                if ((current_macroblock == -1) ||
                    (s->macroblock_coding[current_macroblock] == MODE_COPY))
                    continue;
                if (current_macroblock >= s->macroblock_count) {
                    av_log(s->avctx, AV_LOG_ERROR, "  vp3:unpack_modes(): bad macroblock number (%d >= %d)\n",
                        current_macroblock, s->macroblock_count);
                    return 1;
                }

                /* mode 7 means get 3 bits for each coding mode */
                if (scheme == 7)
                    coding_mode = get_bits(gb, 3);
                else
                    coding_mode = ModeAlphabet[scheme]
                        [get_vlc2(gb, s->mode_code_vlc.table, 3, 3)];

                s->macroblock_coding[current_macroblock] = coding_mode;
                for (k = 0; k < 6; k++) {
                    current_fragment =
                        s->macroblock_fragments[current_macroblock * 6 + k];
                    if (current_fragment == -1)
                        continue;
                    if (current_fragment >= s->fragment_count) {
                        av_log(s->avctx, AV_LOG_ERROR, "  vp3:unpack_modes(): bad fragment number (%d >= %d)\n",
                            current_fragment, s->fragment_count);
                        return 1;
                    }
                    if (s->all_fragments[current_fragment].coding_method !=
                        MODE_COPY)
                        s->all_fragments[current_fragment].coding_method =
                            coding_mode;
                }

                debug_modes("    coding method for macroblock starting @ fragment %d = %d\n",
                    s->macroblock_fragments[current_macroblock * 6], coding_mode);
            }
        }
    }

    return 0;
}

/*
 * This function unpacks all the motion vectors for the individual
 * macroblocks from the bitstream.
 */
static int unpack_vectors(Vp3DecodeContext *s, GetBitContext *gb)
{
    int i, j, k;
    int coding_mode;
    int motion_x[6];
    int motion_y[6];
    int last_motion_x = 0;
    int last_motion_y = 0;
    int prior_last_motion_x = 0;
    int prior_last_motion_y = 0;
    int current_macroblock;
    int current_fragment;

    debug_vp3("  vp3: unpacking motion vectors\n");
    if (s->keyframe) {

        debug_vp3("    keyframe-- there are no motion vectors\n");

    } else {

        memset(motion_x, 0, 6 * sizeof(int));
        memset(motion_y, 0, 6 * sizeof(int));

        /* coding mode 0 is the VLC scheme; 1 is the fixed code scheme */
        coding_mode = get_bits(gb, 1);
        debug_vectors("    using %s scheme for unpacking motion vectors\n",
            (coding_mode == 0) ? "VLC" : "fixed-length");

        /* iterate through all of the macroblocks that contain 1 or more
         * coded fragments */
        for (i = 0; i < s->u_superblock_start; i++) {

            for (j = 0; j < 4; j++) {
                current_macroblock = s->superblock_macroblocks[i * 4 + j];
                if ((current_macroblock == -1) ||
                    (s->macroblock_coding[current_macroblock] == MODE_COPY))
                    continue;
                if (current_macroblock >= s->macroblock_count) {
                    av_log(s->avctx, AV_LOG_ERROR, "  vp3:unpack_vectors(): bad macroblock number (%d >= %d)\n",
                        current_macroblock, s->macroblock_count);
                    return 1;
                }

                current_fragment = s->macroblock_fragments[current_macroblock * 6];
                if (current_fragment >= s->fragment_count) {
                    av_log(s->avctx, AV_LOG_ERROR, "  vp3:unpack_vectors(): bad fragment number (%d >= %d\n",
                        current_fragment, s->fragment_count);
                    return 1;
                }
                switch (s->macroblock_coding[current_macroblock]) {

                case MODE_INTER_PLUS_MV:
                case MODE_GOLDEN_MV:
                    /* all 6 fragments use the same motion vector */
                    if (coding_mode == 0) {
                        motion_x[0] = motion_vector_table[get_vlc2(gb, s->motion_vector_vlc.table, 6, 2)];
                        motion_y[0] = motion_vector_table[get_vlc2(gb, s->motion_vector_vlc.table, 6, 2)];
                    } else {
                        motion_x[0] = fixed_motion_vector_table[get_bits(gb, 6)];
                        motion_y[0] = fixed_motion_vector_table[get_bits(gb, 6)];
                    }

                    for (k = 1; k < 6; k++) {
                        motion_x[k] = motion_x[0];
                        motion_y[k] = motion_y[0];
                    }

                    /* vector maintenance, only on MODE_INTER_PLUS_MV */
                    if (s->macroblock_coding[current_macroblock] ==
                        MODE_INTER_PLUS_MV) {
                        prior_last_motion_x = last_motion_x;
                        prior_last_motion_y = last_motion_y;
                        last_motion_x = motion_x[0];
                        last_motion_y = motion_y[0];
                    }
                    break;

                case MODE_INTER_FOURMV:
                    /* fetch 4 vectors from the bitstream, one for each
                     * Y fragment, then average for the C fragment vectors */
                    motion_x[4] = motion_y[4] = 0;
                    for (k = 0; k < 4; k++) {
                        if (coding_mode == 0) {
                            motion_x[k] = motion_vector_table[get_vlc2(gb, s->motion_vector_vlc.table, 6, 2)];
                            motion_y[k] = motion_vector_table[get_vlc2(gb, s->motion_vector_vlc.table, 6, 2)];
                        } else {
                            motion_x[k] = fixed_motion_vector_table[get_bits(gb, 6)];
                            motion_y[k] = fixed_motion_vector_table[get_bits(gb, 6)];
                        }
                        motion_x[4] += motion_x[k];
                        motion_y[4] += motion_y[k];
                    }

                    if (motion_x[4] >= 0)
                        motion_x[4] = (motion_x[4] + 2) / 4;
                    else
                        motion_x[4] = (motion_x[4] - 2) / 4;
                    motion_x[5] = motion_x[4];

                    if (motion_y[4] >= 0)
                        motion_y[4] = (motion_y[4] + 2) / 4;
                    else
                        motion_y[4] = (motion_y[4] - 2) / 4;
                    motion_y[5] = motion_y[4];

                    /* vector maintenance; vector[3] is treated as the
                     * last vector in this case */
                    prior_last_motion_x = last_motion_x;
                    prior_last_motion_y = last_motion_y;
                    last_motion_x = motion_x[3];
                    last_motion_y = motion_y[3];
                    break;

                case MODE_INTER_LAST_MV:
                    /* all 6 fragments use the last motion vector */
                    motion_x[0] = last_motion_x;
                    motion_y[0] = last_motion_y;
                    for (k = 1; k < 6; k++) {
                        motion_x[k] = motion_x[0];
                        motion_y[k] = motion_y[0];
                    }

                    /* no vector maintenance (last vector remains the
                     * last vector) */
                    break;

                case MODE_INTER_PRIOR_LAST:
                    /* all 6 fragments use the motion vector prior to the
                     * last motion vector */
                    motion_x[0] = prior_last_motion_x;
                    motion_y[0] = prior_last_motion_y;
                    for (k = 1; k < 6; k++) {
                        motion_x[k] = motion_x[0];
                        motion_y[k] = motion_y[0];
                    }

                    /* vector maintenance */
                    prior_last_motion_x = last_motion_x;
                    prior_last_motion_y = last_motion_y;
                    last_motion_x = motion_x[0];
                    last_motion_y = motion_y[0];
                    break;

                default:
                    /* covers intra, inter without MV, golden without MV */
                    memset(motion_x, 0, 6 * sizeof(int));
                    memset(motion_y, 0, 6 * sizeof(int));

                    /* no vector maintenance */
                    break;
                }

                /* assign the motion vectors to the correct fragments */
                debug_vectors("    vectors for macroblock starting @ fragment %d (coding method %d):\n",
                    current_fragment,
                    s->macroblock_coding[current_macroblock]);
                for (k = 0; k < 6; k++) {
                    current_fragment =
                        s->macroblock_fragments[current_macroblock * 6 + k];
                    if (current_fragment == -1)
                        continue;
                    if (current_fragment >= s->fragment_count) {
                        av_log(s->avctx, AV_LOG_ERROR, "  vp3:unpack_vectors(): bad fragment number (%d >= %d)\n",
                            current_fragment, s->fragment_count);
                        return 1;
                    }
                    s->all_fragments[current_fragment].motion_x = motion_x[k];
                    s->all_fragments[current_fragment].motion_y = motion_y[k];
                    debug_vectors("    vector %d: fragment %d = (%d, %d)\n",
                        k, current_fragment, motion_x[k], motion_y[k]);
                }
            }
        }
    }

    return 0;
}

/*
 * This function is called by unpack_dct_coeffs() to extract the VLCs from
 * the bitstream. The VLCs encode tokens which are used to unpack DCT
 * data. This function unpacks all the VLCs for either the Y plane or both
 * C planes, and is called for DC coefficients or different AC coefficient
 * levels (since different coefficient types require different VLC tables.
 *
 * This function returns a residual eob run. E.g, if a particular token gave
 * instructions to EOB the next 5 fragments and there were only 2 fragments
 * left in the current fragment range, 3 would be returned so that it could
 * be passed into the next call to this same function.
 */
static int unpack_vlcs(Vp3DecodeContext *s, GetBitContext *gb,
                        VLC *table, int coeff_index,
                        int first_fragment, int last_fragment,
                        int eob_run)
{
    int i;
    int token;
    int zero_run = 0;
    DCTELEM coeff = 0;
    Vp3Fragment *fragment;
    uint8_t *perm= s->scantable.permutated;
    int bits_to_get;

    if ((first_fragment >= s->fragment_count) ||
        (last_fragment >= s->fragment_count)) {

        av_log(s->avctx, AV_LOG_ERROR, "  vp3:unpack_vlcs(): bad fragment number (%d -> %d ?)\n",
            first_fragment, last_fragment);
        return 0;
    }

    for (i = first_fragment; i <= last_fragment; i++) {

        fragment = &s->all_fragments[s->coded_fragment_list[i]];
        if (fragment->coeff_count > coeff_index)
            continue;

        if (!eob_run) {
            /* decode a VLC into a token */
            token = get_vlc2(gb, table->table, 5, 3);
            debug_vlc(" token = %2d, ", token);
            /* use the token to get a zero run, a coefficient, and an eob run */
            if (token <= 6) {
                eob_run = eob_run_base[token];
                if (eob_run_get_bits[token])
                    eob_run += get_bits(gb, eob_run_get_bits[token]);
                coeff = zero_run = 0;
            } else {
                bits_to_get = coeff_get_bits[token];
                if (!bits_to_get)
                    coeff = coeff_tables[token][0];
                else
                    coeff = coeff_tables[token][get_bits(gb, bits_to_get)];

                zero_run = zero_run_base[token];
                if (zero_run_get_bits[token])
                    zero_run += get_bits(gb, zero_run_get_bits[token]);
            }
        }

        if (!eob_run) {
            fragment->coeff_count += zero_run;
            if (fragment->coeff_count < 64){
                fragment->next_coeff->coeff= coeff;
                fragment->next_coeff->index= perm[fragment->coeff_count++]; //FIXME perm here already?
                fragment->next_coeff->next= s->next_coeff;
                s->next_coeff->next=NULL;
                fragment->next_coeff= s->next_coeff++;
            }
            debug_vlc(" fragment %d coeff = %d\n",
                s->coded_fragment_list[i], fragment->next_coeff[coeff_index]);
        } else {
            fragment->coeff_count |= 128;
            debug_vlc(" fragment %d eob with %d coefficients\n",
                s->coded_fragment_list[i], fragment->coeff_count&127);
            eob_run--;
        }
    }

    return eob_run;
}

/*
 * This function unpacks all of the DCT coefficient data from the
 * bitstream.
 */
static int unpack_dct_coeffs(Vp3DecodeContext *s, GetBitContext *gb)
{
    int i;
    int dc_y_table;
    int dc_c_table;
    int ac_y_table;
    int ac_c_table;
    int residual_eob_run = 0;

    /* fetch the DC table indices */
    dc_y_table = get_bits(gb, 4);
    dc_c_table = get_bits(gb, 4);

    /* unpack the Y plane DC coefficients */
    debug_vp3("  vp3: unpacking Y plane DC coefficients using table %d\n",
        dc_y_table);
    residual_eob_run = unpack_vlcs(s, gb, &s->dc_vlc[dc_y_table], 0,
        s->first_coded_y_fragment, s->last_coded_y_fragment, residual_eob_run);

    /* unpack the C plane DC coefficients */
    debug_vp3("  vp3: unpacking C plane DC coefficients using table %d\n",
        dc_c_table);
    residual_eob_run = unpack_vlcs(s, gb, &s->dc_vlc[dc_c_table], 0,
        s->first_coded_c_fragment, s->last_coded_c_fragment, residual_eob_run);

    /* fetch the AC table indices */
    ac_y_table = get_bits(gb, 4);
    ac_c_table = get_bits(gb, 4);

    /* unpack the group 1 AC coefficients (coeffs 1-5) */
    for (i = 1; i <= 5; i++) {

        debug_vp3("  vp3: unpacking level %d Y plane AC coefficients using table %d\n",
            i, ac_y_table);
        residual_eob_run = unpack_vlcs(s, gb, &s->ac_vlc_1[ac_y_table], i,
            s->first_coded_y_fragment, s->last_coded_y_fragment, residual_eob_run);

        debug_vp3("  vp3: unpacking level %d C plane AC coefficients using table %d\n",
            i, ac_c_table);
        residual_eob_run = unpack_vlcs(s, gb, &s->ac_vlc_1[ac_c_table], i,
            s->first_coded_c_fragment, s->last_coded_c_fragment, residual_eob_run);
    }

    /* unpack the group 2 AC coefficients (coeffs 6-14) */
    for (i = 6; i <= 14; i++) {

        debug_vp3("  vp3: unpacking level %d Y plane AC coefficients using table %d\n",
            i, ac_y_table);
        residual_eob_run = unpack_vlcs(s, gb, &s->ac_vlc_2[ac_y_table], i,
            s->first_coded_y_fragment, s->last_coded_y_fragment, residual_eob_run);

        debug_vp3("  vp3: unpacking level %d C plane AC coefficients using table %d\n",
            i, ac_c_table);
        residual_eob_run = unpack_vlcs(s, gb, &s->ac_vlc_2[ac_c_table], i,
            s->first_coded_c_fragment, s->last_coded_c_fragment, residual_eob_run);
    }

    /* unpack the group 3 AC coefficients (coeffs 15-27) */
    for (i = 15; i <= 27; i++) {

        debug_vp3("  vp3: unpacking level %d Y plane AC coefficients using table %d\n",
            i, ac_y_table);
        residual_eob_run = unpack_vlcs(s, gb, &s->ac_vlc_3[ac_y_table], i,
            s->first_coded_y_fragment, s->last_coded_y_fragment, residual_eob_run);

        debug_vp3("  vp3: unpacking level %d C plane AC coefficients using table %d\n",
            i, ac_c_table);
        residual_eob_run = unpack_vlcs(s, gb, &s->ac_vlc_3[ac_c_table], i,
            s->first_coded_c_fragment, s->last_coded_c_fragment, residual_eob_run);
    }

    /* unpack the group 4 AC coefficients (coeffs 28-63) */
    for (i = 28; i <= 63; i++) {

        debug_vp3("  vp3: unpacking level %d Y plane AC coefficients using table %d\n",
            i, ac_y_table);
        residual_eob_run = unpack_vlcs(s, gb, &s->ac_vlc_4[ac_y_table], i,
            s->first_coded_y_fragment, s->last_coded_y_fragment, residual_eob_run);

        debug_vp3("  vp3: unpacking level %d C plane AC coefficients using table %d\n",
            i, ac_c_table);
        residual_eob_run = unpack_vlcs(s, gb, &s->ac_vlc_4[ac_c_table], i,
            s->first_coded_c_fragment, s->last_coded_c_fragment, residual_eob_run);
    }

    return 0;
}

/*
 * This function reverses the DC prediction for each coded fragment in
 * the frame. Much of this function is adapted directly from the original
 * VP3 source code.
 */
#define COMPATIBLE_FRAME(x) \
  (compatible_frame[s->all_fragments[x].coding_method] == current_frame_type)
#define FRAME_CODED(x) (s->all_fragments[x].coding_method != MODE_COPY)
#define DC_COEFF(u) (s->coeffs[u].index ? 0 : s->coeffs[u].coeff) //FIXME do somethin to simplify this
static inline int iabs (int x) { return ((x < 0) ? -x : x); }

static void reverse_dc_prediction(Vp3DecodeContext *s,
                                  int first_fragment,
                                  int fragment_width,
                                  int fragment_height)
{

#define PUL 8
#define PU 4
#define PUR 2
#define PL 1

    int x, y;
    int i = first_fragment;

    /*
     * Fragment prediction groups:
     *
     * 32222222226
     * 10000000004
     * 10000000004
     * 10000000004
     * 10000000004
     *
     * Note: Groups 5 and 7 do not exist as it would mean that the
     * fragment's x coordinate is both 0 and (width - 1) at the same time.
     */
    int predictor_group;
    short predicted_dc;

    /* validity flags for the left, up-left, up, and up-right fragments */
    int fl, ful, fu, fur;

    /* DC values for the left, up-left, up, and up-right fragments */
    int vl, vul, vu, vur;

    /* indices for the left, up-left, up, and up-right fragments */
    int l, ul, u, ur;

    /*
     * The 6 fields mean:
     *   0: up-left multiplier
     *   1: up multiplier
     *   2: up-right multiplier
     *   3: left multiplier
     *   4: mask
     *   5: right bit shift divisor (e.g., 7 means >>=7, a.k.a. div by 128)
     */
    int predictor_transform[16][6] = {
        {  0,  0,  0,  0,   0,  0 },
        {  0,  0,  0,  1,   0,  0 },        // PL
        {  0,  0,  1,  0,   0,  0 },        // PUR
        {  0,  0, 53, 75, 127,  7 },        // PUR|PL
        {  0,  1,  0,  0,   0,  0 },        // PU
        {  0,  1,  0,  1,   1,  1 },        // PU|PL
        {  0,  1,  0,  0,   0,  0 },        // PU|PUR
        {  0,  0, 53, 75, 127,  7 },        // PU|PUR|PL
        {  1,  0,  0,  0,   0,  0 },        // PUL
        {  0,  0,  0,  1,   0,  0 },        // PUL|PL
        {  1,  0,  1,  0,   1,  1 },        // PUL|PUR
        {  0,  0, 53, 75, 127,  7 },        // PUL|PUR|PL
        {  0,  1,  0,  0,   0,  0 },        // PUL|PU
        {-26, 29,  0, 29,  31,  5 },        // PUL|PU|PL
        {  3, 10,  3,  0,  15,  4 },        // PUL|PU|PUR
        {-26, 29,  0, 29,  31,  5 }         // PUL|PU|PUR|PL
    };

    /* This table shows which types of blocks can use other blocks for
     * prediction. For example, INTRA is the only mode in this table to
     * have a frame number of 0. That means INTRA blocks can only predict
     * from other INTRA blocks. There are 2 golden frame coding types;
     * blocks encoding in these modes can only predict from other blocks
     * that were encoded with these 1 of these 2 modes. */
    unsigned char compatible_frame[8] = {
        1,    /* MODE_INTER_NO_MV */
        0,    /* MODE_INTRA */
        1,    /* MODE_INTER_PLUS_MV */
        1,    /* MODE_INTER_LAST_MV */
        1,    /* MODE_INTER_PRIOR_MV */
        2,    /* MODE_USING_GOLDEN */
        2,    /* MODE_GOLDEN_MV */
        1     /* MODE_INTER_FOUR_MV */
    };
    int current_frame_type;

    /* there is a last DC predictor for each of the 3 frame types */
    short last_dc[3];

    int transform = 0;

    debug_vp3("  vp3: reversing DC prediction\n");

    vul = vu = vur = vl = 0;
    last_dc[0] = last_dc[1] = last_dc[2] = 0;

    /* for each fragment row... */
    for (y = 0; y < fragment_height; y++) {

        /* for each fragment in a row... */
        for (x = 0; x < fragment_width; x++, i++) {

            /* reverse prediction if this block was coded */
            if (s->all_fragments[i].coding_method != MODE_COPY) {

                current_frame_type =
                    compatible_frame[s->all_fragments[i].coding_method];
                predictor_group = (x == 0) + ((y == 0) << 1) +
                    ((x + 1 == fragment_width) << 2);
                debug_dc_pred(" frag %d: group %d, orig DC = %d, ",
                    i, predictor_group, DC_COEFF(i));

                switch (predictor_group) {

                case 0:
                    /* main body of fragments; consider all 4 possible
                     * fragments for prediction */

                    /* calculate the indices of the predicting fragments */
                    ul = i - fragment_width - 1;
                    u = i - fragment_width;
                    ur = i - fragment_width + 1;
                    l = i - 1;

                    /* fetch the DC values for the predicting fragments */
                    vul = DC_COEFF(ul);
                    vu = DC_COEFF(u);
                    vur = DC_COEFF(ur);
                    vl = DC_COEFF(l);

                    /* figure out which fragments are valid */
                    ful = FRAME_CODED(ul) && COMPATIBLE_FRAME(ul);
                    fu = FRAME_CODED(u) && COMPATIBLE_FRAME(u);
                    fur = FRAME_CODED(ur) && COMPATIBLE_FRAME(ur);
                    fl = FRAME_CODED(l) && COMPATIBLE_FRAME(l);

                    /* decide which predictor transform to use */
                    transform = (fl*PL) | (fu*PU) | (ful*PUL) | (fur*PUR);

                    break;

                case 1:
                    /* left column of fragments, not including top corner;
                     * only consider up and up-right fragments */

                    /* calculate the indices of the predicting fragments */
                    u = i - fragment_width;
                    ur = i - fragment_width + 1;

                    /* fetch the DC values for the predicting fragments */
                    vu = DC_COEFF(u);
                    vur = DC_COEFF(ur);

                    /* figure out which fragments are valid */
                    fur = FRAME_CODED(ur) && COMPATIBLE_FRAME(ur);
                    fu = FRAME_CODED(u) && COMPATIBLE_FRAME(u);

                    /* decide which predictor transform to use */
                    transform = (fu*PU) | (fur*PUR);

                    break;

                case 2:
                case 6:
                    /* top row of fragments, not including top-left frag;
                     * only consider the left fragment for prediction */

                    /* calculate the indices of the predicting fragments */
                    l = i - 1;

                    /* fetch the DC values for the predicting fragments */
                    vl = DC_COEFF(l);

                    /* figure out which fragments are valid */
                    fl = FRAME_CODED(l) && COMPATIBLE_FRAME(l);

                    /* decide which predictor transform to use */
                    transform = (fl*PL);

                    break;

                case 3:
                    /* top-left fragment */

                    /* nothing to predict from in this case */
                    transform = 0;

                    break;

                case 4:
                    /* right column of fragments, not including top corner;
                     * consider up-left, up, and left fragments for
                     * prediction */

                    /* calculate the indices of the predicting fragments */
                    ul = i - fragment_width - 1;
                    u = i - fragment_width;
                    l = i - 1;

                    /* fetch the DC values for the predicting fragments */
                    vul = DC_COEFF(ul);
                    vu = DC_COEFF(u);
                    vl = DC_COEFF(l);

                    /* figure out which fragments are valid */
                    ful = FRAME_CODED(ul) && COMPATIBLE_FRAME(ul);
                    fu = FRAME_CODED(u) && COMPATIBLE_FRAME(u);
                    fl = FRAME_CODED(l) && COMPATIBLE_FRAME(l);

                    /* decide which predictor transform to use */
                    transform = (fl*PL) | (fu*PU) | (ful*PUL);

                    break;

                }

                debug_dc_pred("transform = %d, ", transform);

                if (transform == 0) {

                    /* if there were no fragments to predict from, use last
                     * DC saved */
                    predicted_dc = last_dc[current_frame_type];
                    debug_dc_pred("from last DC (%d) = %d\n",
                        current_frame_type, DC_COEFF(i));

                } else {

                    /* apply the appropriate predictor transform */
                    predicted_dc =
                        (predictor_transform[transform][0] * vul) +
                        (predictor_transform[transform][1] * vu) +
                        (predictor_transform[transform][2] * vur) +
                        (predictor_transform[transform][3] * vl);

                    /* if there is a shift value in the transform, add
                     * the sign bit before the shift */
                    if (predictor_transform[transform][5] != 0) {
                        predicted_dc += ((predicted_dc >> 15) &
                            predictor_transform[transform][4]);
                        predicted_dc >>= predictor_transform[transform][5];
                    }

                    /* check for outranging on the [ul u l] and
                     * [ul u ur l] predictors */
                    if ((transform == 13) || (transform == 15)) {
                        if (iabs(predicted_dc - vu) > 128)
                            predicted_dc = vu;
                        else if (iabs(predicted_dc - vl) > 128)
                            predicted_dc = vl;
                        else if (iabs(predicted_dc - vul) > 128)
                            predicted_dc = vul;
                    }

                    debug_dc_pred("from pred DC = %d\n",
                    DC_COEFF(i));
                }

                /* at long last, apply the predictor */
                if(s->coeffs[i].index){
                    *s->next_coeff= s->coeffs[i];
                    s->coeffs[i].index=0;
                    s->coeffs[i].coeff=0;
                    s->coeffs[i].next= s->next_coeff++;
                }
                s->coeffs[i].coeff += predicted_dc;
                /* save the DC */
                last_dc[current_frame_type] = DC_COEFF(i);
                if(DC_COEFF(i) && !(s->all_fragments[i].coeff_count&127)){
                    s->all_fragments[i].coeff_count= 129;
//                    s->all_fragments[i].next_coeff= s->next_coeff;
                    s->coeffs[i].next= s->next_coeff;
                    (s->next_coeff++)->next=NULL;
                }
            }
        }
    }
}


static void horizontal_filter(unsigned char *first_pixel, int stride,
    int *bounding_values);
static void vertical_filter(unsigned char *first_pixel, int stride,
    int *bounding_values);

/*
 * Perform the final rendering for a particular slice of data.
 * The slice number ranges from 0..(macroblock_height - 1).
 */
static void render_slice(Vp3DecodeContext *s, int slice)
{
    int x, y;
    int m, n;
    int i;  /* indicates current fragment */
    int16_t *dequantizer;
    DECLARE_ALIGNED_16(DCTELEM, block[64]);
    unsigned char *output_plane;
    unsigned char *last_plane;
    unsigned char *golden_plane;
    int stride;
    int motion_x = 0xdeadbeef, motion_y = 0xdeadbeef;
    int upper_motion_limit, lower_motion_limit;
    int motion_halfpel_index;
    uint8_t *motion_source;
    int plane;
    int plane_width;
    int plane_height;
    int slice_height;
    int current_macroblock_entry = slice * s->macroblock_width * 6;
    int fragment_width;

    if (slice >= s->macroblock_height)
        return;

    for (plane = 0; plane < 3; plane++) {

        /* set up plane-specific parameters */
        if (plane == 0) {
            output_plane = s->current_frame.data[0];
            last_plane = s->last_frame.data[0];
            golden_plane = s->golden_frame.data[0];
            stride = s->current_frame.linesize[0];
            if (!s->flipped_image) stride = -stride;
            upper_motion_limit = 7 * s->current_frame.linesize[0];
            lower_motion_limit = s->height * s->current_frame.linesize[0] + s->width - 8;
            y = slice * FRAGMENT_PIXELS * 2;
            plane_width = s->width;
            plane_height = s->height;
            slice_height = y + FRAGMENT_PIXELS * 2;
            i = s->macroblock_fragments[current_macroblock_entry + 0];
        } else if (plane == 1) {
            output_plane = s->current_frame.data[1];
            last_plane = s->last_frame.data[1];
            golden_plane = s->golden_frame.data[1];
            stride = s->current_frame.linesize[1];
            if (!s->flipped_image) stride = -stride;
            upper_motion_limit = 7 * s->current_frame.linesize[1];
            lower_motion_limit = (s->height / 2) * s->current_frame.linesize[1] + (s->width / 2) - 8;
            y = slice * FRAGMENT_PIXELS;
            plane_width = s->width / 2;
            plane_height = s->height / 2;
            slice_height = y + FRAGMENT_PIXELS;
            i = s->macroblock_fragments[current_macroblock_entry + 4];
        } else {
            output_plane = s->current_frame.data[2];
            last_plane = s->last_frame.data[2];
            golden_plane = s->golden_frame.data[2];
            stride = s->current_frame.linesize[2];
            if (!s->flipped_image) stride = -stride;
            upper_motion_limit = 7 * s->current_frame.linesize[2];
            lower_motion_limit = (s->height / 2) * s->current_frame.linesize[2] + (s->width / 2) - 8;
            y = slice * FRAGMENT_PIXELS;
            plane_width = s->width / 2;
            plane_height = s->height / 2;
            slice_height = y + FRAGMENT_PIXELS;
            i = s->macroblock_fragments[current_macroblock_entry + 5];
        }
        fragment_width = plane_width / FRAGMENT_PIXELS;

        if(ABS(stride) > 2048)
            return; //various tables are fixed size

        /* for each fragment row in the slice (both of them)... */
        for (; y < slice_height; y += 8) {

            /* for each fragment in a row... */
            for (x = 0; x < plane_width; x += 8, i++) {

                if ((i < 0) || (i >= s->fragment_count)) {
                    av_log(s->avctx, AV_LOG_ERROR, "  vp3:render_slice(): bad fragment number (%d)\n", i);
                    return;
                }

                /* transform if this block was coded */
                if ((s->all_fragments[i].coding_method != MODE_COPY) &&
                    !((s->avctx->flags & CODEC_FLAG_GRAY) && plane)) {

                    if ((s->all_fragments[i].coding_method == MODE_USING_GOLDEN) ||
                        (s->all_fragments[i].coding_method == MODE_GOLDEN_MV))
                        motion_source= golden_plane;
                    else
                        motion_source= last_plane;

                    motion_source += s->all_fragments[i].first_pixel;
                    motion_halfpel_index = 0;

                    /* sort out the motion vector if this fragment is coded
                     * using a motion vector method */
                    if ((s->all_fragments[i].coding_method > MODE_INTRA) &&
                        (s->all_fragments[i].coding_method != MODE_USING_GOLDEN)) {
                        int src_x, src_y;
                        motion_x = s->all_fragments[i].motion_x;
                        motion_y = s->all_fragments[i].motion_y;
                        if(plane){
                            motion_x= (motion_x>>1) | (motion_x&1);
                            motion_y= (motion_y>>1) | (motion_y&1);
                        }

                        src_x= (motion_x>>1) + x;
                        src_y= (motion_y>>1) + y;
                        if ((motion_x == 127) || (motion_y == 127))
                            av_log(s->avctx, AV_LOG_ERROR, " help! got invalid motion vector! (%X, %X)\n", motion_x, motion_y);

                        motion_halfpel_index = motion_x & 0x01;
                        motion_source += (motion_x >> 1);

                        motion_halfpel_index |= (motion_y & 0x01) << 1;
                        motion_source += ((motion_y >> 1) * stride);

                        if(src_x<0 || src_y<0 || src_x + 9 >= plane_width || src_y + 9 >= plane_height){
                            uint8_t *temp= s->edge_emu_buffer;
                            if(stride<0) temp -= 9*stride;
                            else temp += 9*stride;

                            ff_emulated_edge_mc(temp, motion_source, stride, 9, 9, src_x, src_y, plane_width, plane_height);
                            motion_source= temp;
                        }
                    }


                    /* first, take care of copying a block from either the
                     * previous or the golden frame */
                    if (s->all_fragments[i].coding_method != MODE_INTRA) {
                        /* Note, it is possible to implement all MC cases with
                           put_no_rnd_pixels_l2 which would look more like the
                           VP3 source but this would be slower as
                           put_no_rnd_pixels_tab is better optimzed */
                        if(motion_halfpel_index != 3){
                            s->dsp.put_no_rnd_pixels_tab[1][motion_halfpel_index](
                                output_plane + s->all_fragments[i].first_pixel,
                                motion_source, stride, 8);
                        }else{
                            int d= (motion_x ^ motion_y)>>31; // d is 0 if motion_x and _y have the same sign, else -1
                            s->dsp.put_no_rnd_pixels_l2[1](
                                output_plane + s->all_fragments[i].first_pixel,
                                motion_source - d,
                                motion_source + stride + 1 + d,
                                stride, 8);
                        }
                        dequantizer = s->inter_dequant;
                    }else{
                        if (plane == 0)
                            dequantizer = s->intra_y_dequant;
                        else
                            dequantizer = s->intra_c_dequant;
                    }

                    /* dequantize the DCT coefficients */
                    debug_idct("fragment %d, coding mode %d, DC = %d, dequant = %d:\n",
                        i, s->all_fragments[i].coding_method,
                        DC_COEFF(i), dequantizer[0]);

                    if(s->avctx->idct_algo==FF_IDCT_VP3){
                        Coeff *coeff= s->coeffs + i;
                        memset(block, 0, sizeof(block));
                        while(coeff->next){
                            block[coeff->index]= coeff->coeff * dequantizer[coeff->index];
                            coeff= coeff->next;
                        }
                    }else{
                        Coeff *coeff= s->coeffs + i;
                        memset(block, 0, sizeof(block));
                        while(coeff->next){
                            block[coeff->index]= (coeff->coeff * dequantizer[coeff->index] + 2)>>2;
                            coeff= coeff->next;
                        }
                    }

                    /* invert DCT and place (or add) in final output */

                    if (s->all_fragments[i].coding_method == MODE_INTRA) {
                        if(s->avctx->idct_algo!=FF_IDCT_VP3)
                            block[0] += 128<<3;
                        s->dsp.idct_put(
                            output_plane + s->all_fragments[i].first_pixel,
                            stride,
                            block);
                    } else {
                        s->dsp.idct_add(
                            output_plane + s->all_fragments[i].first_pixel,
                            stride,
                            block);
                    }

                    debug_idct("block after idct_%s():\n",
                        (s->all_fragments[i].coding_method == MODE_INTRA)?
                        "put" : "add");
                    for (m = 0; m < 8; m++) {
                        for (n = 0; n < 8; n++) {
                            debug_idct(" %3d", *(output_plane +
                                s->all_fragments[i].first_pixel + (m * stride + n)));
                        }
                        debug_idct("\n");
                    }
                    debug_idct("\n");

                } else {

                    /* copy directly from the previous frame */
                    s->dsp.put_pixels_tab[1][0](
                        output_plane + s->all_fragments[i].first_pixel,
                        last_plane + s->all_fragments[i].first_pixel,
                        stride, 8);

                }
#if 0
                /* perform the left edge filter if:
                 *   - the fragment is not on the left column
                 *   - the fragment is coded in this frame
                 *   - the fragment is not coded in this frame but the left
                 *     fragment is coded in this frame (this is done instead
                 *     of a right edge filter when rendering the left fragment
                 *     since this fragment is not available yet) */
                if ((x > 0) &&
                    ((s->all_fragments[i].coding_method != MODE_COPY) ||
                     ((s->all_fragments[i].coding_method == MODE_COPY) &&
                      (s->all_fragments[i - 1].coding_method != MODE_COPY)) )) {
                    horizontal_filter(
                        output_plane + s->all_fragments[i].first_pixel + 7*stride,
                        -stride, bounding_values);
                }

                /* perform the top edge filter if:
                 *   - the fragment is not on the top row
                 *   - the fragment is coded in this frame
                 *   - the fragment is not coded in this frame but the above
                 *     fragment is coded in this frame (this is done instead
                 *     of a bottom edge filter when rendering the above
                 *     fragment since this fragment is not available yet) */
                if ((y > 0) &&
                    ((s->all_fragments[i].coding_method != MODE_COPY) ||
                     ((s->all_fragments[i].coding_method == MODE_COPY) &&
                      (s->all_fragments[i - fragment_width].coding_method != MODE_COPY)) )) {
                    vertical_filter(
                        output_plane + s->all_fragments[i].first_pixel - stride,
                        -stride, bounding_values);
                }
#endif
            }
        }
    }

     /* this looks like a good place for slice dispatch... */
     /* algorithm:
      *   if (slice == s->macroblock_height - 1)
      *     dispatch (both last slice & 2nd-to-last slice);
      *   else if (slice > 0)
      *     dispatch (slice - 1);
      */

    emms_c();
}

static void horizontal_filter(unsigned char *first_pixel, int stride,
    int *bounding_values)
{
    unsigned char *end;
    int filter_value;

    for (end= first_pixel + 8*stride; first_pixel < end; first_pixel += stride) {
        filter_value =
            (first_pixel[-2] - first_pixel[ 1])
         +3*(first_pixel[ 0] - first_pixel[-1]);
        filter_value = bounding_values[(filter_value + 4) >> 3];
        first_pixel[-1] = clip_uint8(first_pixel[-1] + filter_value);
        first_pixel[ 0] = clip_uint8(first_pixel[ 0] - filter_value);
    }
}

static void vertical_filter(unsigned char *first_pixel, int stride,
    int *bounding_values)
{
    unsigned char *end;
    int filter_value;
    const int nstride= -stride;

    for (end= first_pixel + 8; first_pixel < end; first_pixel++) {
        filter_value =
            (first_pixel[2 * nstride] - first_pixel[ stride])
         +3*(first_pixel[0          ] - first_pixel[nstride]);
        filter_value = bounding_values[(filter_value + 4) >> 3];
        first_pixel[nstride] = clip_uint8(first_pixel[nstride] + filter_value);
        first_pixel[0] = clip_uint8(first_pixel[0] - filter_value);
    }
}

static void apply_loop_filter(Vp3DecodeContext *s)
{
    int x, y, plane;
    int width, height;
    int fragment;
    int stride;
    unsigned char *plane_data;
    int *bounding_values= s->bounding_values_array+127;

#if 0
    int bounding_values_array[256];
    int filter_limit;

    /* find the right loop limit value */
    for (x = 63; x >= 0; x--) {
        if (vp31_ac_scale_factor[x] >= s->quality_index)
            break;
    }
    filter_limit = vp31_filter_limit_values[s->quality_index];

    /* set up the bounding values */
    memset(bounding_values_array, 0, 256 * sizeof(int));
    for (x = 0; x < filter_limit; x++) {
        bounding_values[-x - filter_limit] = -filter_limit + x;
        bounding_values[-x] = -x;
        bounding_values[x] = x;
        bounding_values[x + filter_limit] = filter_limit - x;
    }
#endif

    for (plane = 0; plane < 3; plane++) {

        if (plane == 0) {
            /* Y plane parameters */
            fragment = 0;
            width = s->fragment_width;
            height = s->fragment_height;
            stride = s->current_frame.linesize[0];
            plane_data = s->current_frame.data[0];
        } else if (plane == 1) {
            /* U plane parameters */
            fragment = s->u_fragment_start;
            width = s->fragment_width / 2;
            height = s->fragment_height / 2;
            stride = s->current_frame.linesize[1];
            plane_data = s->current_frame.data[1];
        } else {
            /* V plane parameters */
            fragment = s->v_fragment_start;
            width = s->fragment_width / 2;
            height = s->fragment_height / 2;
            stride = s->current_frame.linesize[2];
            plane_data = s->current_frame.data[2];
        }

        for (y = 0; y < height; y++) {

            for (x = 0; x < width; x++) {
START_TIMER
                /* do not perform left edge filter for left columns frags */
                if ((x > 0) &&
                    (s->all_fragments[fragment].coding_method != MODE_COPY)) {
                    horizontal_filter(
                        plane_data + s->all_fragments[fragment].first_pixel - 7*stride,
                        stride, bounding_values);
                }

                /* do not perform top edge filter for top row fragments */
                if ((y > 0) &&
                    (s->all_fragments[fragment].coding_method != MODE_COPY)) {
                    vertical_filter(
                        plane_data + s->all_fragments[fragment].first_pixel + stride,
                        stride, bounding_values);
                }

                /* do not perform right edge filter for right column
                 * fragments or if right fragment neighbor is also coded
                 * in this frame (it will be filtered in next iteration) */
                if ((x < width - 1) &&
                    (s->all_fragments[fragment].coding_method != MODE_COPY) &&
                    (s->all_fragments[fragment + 1].coding_method == MODE_COPY)) {
                    horizontal_filter(
                        plane_data + s->all_fragments[fragment + 1].first_pixel - 7*stride,
                        stride, bounding_values);
                }

                /* do not perform bottom edge filter for bottom row
                 * fragments or if bottom fragment neighbor is also coded
                 * in this frame (it will be filtered in the next row) */
                if ((y < height - 1) &&
                    (s->all_fragments[fragment].coding_method != MODE_COPY) &&
                    (s->all_fragments[fragment + width].coding_method == MODE_COPY)) {
                    vertical_filter(
                        plane_data + s->all_fragments[fragment + width].first_pixel + stride,
                        stride, bounding_values);
                }

                fragment++;
STOP_TIMER("loop filter")
            }
        }
    }
}

/*
 * This function computes the first pixel addresses for each fragment.
 * This function needs to be invoked after the first frame is allocated
 * so that it has access to the plane strides.
 */
static void vp3_calculate_pixel_addresses(Vp3DecodeContext *s)
{

    int i, x, y;

    /* figure out the first pixel addresses for each of the fragments */
    /* Y plane */
    i = 0;
    for (y = s->fragment_height; y > 0; y--) {
        for (x = 0; x < s->fragment_width; x++) {
            s->all_fragments[i++].first_pixel =
                s->golden_frame.linesize[0] * y * FRAGMENT_PIXELS -
                    s->golden_frame.linesize[0] +
                    x * FRAGMENT_PIXELS;
            debug_init("  fragment %d, first pixel @ %d\n",
                i-1, s->all_fragments[i-1].first_pixel);
        }
    }

    /* U plane */
    i = s->u_fragment_start;
    for (y = s->fragment_height / 2; y > 0; y--) {
        for (x = 0; x < s->fragment_width / 2; x++) {
            s->all_fragments[i++].first_pixel =
                s->golden_frame.linesize[1] * y * FRAGMENT_PIXELS -
                    s->golden_frame.linesize[1] +
                    x * FRAGMENT_PIXELS;
            debug_init("  fragment %d, first pixel @ %d\n",
                i-1, s->all_fragments[i-1].first_pixel);
        }
    }

    /* V plane */
    i = s->v_fragment_start;
    for (y = s->fragment_height / 2; y > 0; y--) {
        for (x = 0; x < s->fragment_width / 2; x++) {
            s->all_fragments[i++].first_pixel =
                s->golden_frame.linesize[2] * y * FRAGMENT_PIXELS -
                    s->golden_frame.linesize[2] +
                    x * FRAGMENT_PIXELS;
            debug_init("  fragment %d, first pixel @ %d\n",
                i-1, s->all_fragments[i-1].first_pixel);
        }
    }
}

/* FIXME: this should be merged with the above! */
static void theora_calculate_pixel_addresses(Vp3DecodeContext *s)
{

    int i, x, y;

    /* figure out the first pixel addresses for each of the fragments */
    /* Y plane */
    i = 0;
    for (y = 1; y <= s->fragment_height; y++) {
        for (x = 0; x < s->fragment_width; x++) {
            s->all_fragments[i++].first_pixel =
                s->golden_frame.linesize[0] * y * FRAGMENT_PIXELS -
                    s->golden_frame.linesize[0] +
                    x * FRAGMENT_PIXELS;
            debug_init("  fragment %d, first pixel @ %d\n",
                i-1, s->all_fragments[i-1].first_pixel);
        }
    }

    /* U plane */
    i = s->u_fragment_start;
    for (y = 1; y <= s->fragment_height / 2; y++) {
        for (x = 0; x < s->fragment_width / 2; x++) {
            s->all_fragments[i++].first_pixel =
                s->golden_frame.linesize[1] * y * FRAGMENT_PIXELS -
                    s->golden_frame.linesize[1] +
                    x * FRAGMENT_PIXELS;
            debug_init("  fragment %d, first pixel @ %d\n",
                i-1, s->all_fragments[i-1].first_pixel);
        }
    }

    /* V plane */
    i = s->v_fragment_start;
    for (y = 1; y <= s->fragment_height / 2; y++) {
        for (x = 0; x < s->fragment_width / 2; x++) {
            s->all_fragments[i++].first_pixel =
                s->golden_frame.linesize[2] * y * FRAGMENT_PIXELS -
                    s->golden_frame.linesize[2] +
                    x * FRAGMENT_PIXELS;
            debug_init("  fragment %d, first pixel @ %d\n",
                i-1, s->all_fragments[i-1].first_pixel);
        }
    }
}

/*
 * This is the ffmpeg/libavcodec API init function.
 */
static int vp3_decode_init(AVCodecContext *avctx)
{
    Vp3DecodeContext *s = avctx->priv_data;
    int i;
    int c_width;
    int c_height;
    int y_superblock_count;
    int c_superblock_count;

    if (avctx->codec_tag == MKTAG('V','P','3','0'))
        s->version = 0;
    else
        s->version = 1;

    s->avctx = avctx;
    s->width = (avctx->width + 15) & 0xFFFFFFF0;
    s->height = (avctx->height + 15) & 0xFFFFFFF0;
    avctx->pix_fmt = PIX_FMT_YUV420P;
    avctx->has_b_frames = 0;
    if(avctx->idct_algo==FF_IDCT_AUTO)
        avctx->idct_algo=FF_IDCT_VP3;
    dsputil_init(&s->dsp, avctx);

    ff_init_scantable(s->dsp.idct_permutation, &s->scantable, ff_zigzag_direct);

    /* initialize to an impossible value which will force a recalculation
     * in the first frame decode */
    s->quality_index = -1;

    s->y_superblock_width = (s->width + 31) / 32;
    s->y_superblock_height = (s->height + 31) / 32;
    y_superblock_count = s->y_superblock_width * s->y_superblock_height;

    /* work out the dimensions for the C planes */
    c_width = s->width / 2;
    c_height = s->height / 2;
    s->c_superblock_width = (c_width + 31) / 32;
    s->c_superblock_height = (c_height + 31) / 32;
    c_superblock_count = s->c_superblock_width * s->c_superblock_height;

    s->superblock_count = y_superblock_count + (c_superblock_count * 2);
    s->u_superblock_start = y_superblock_count;
    s->v_superblock_start = s->u_superblock_start + c_superblock_count;
    s->superblock_coding = av_malloc(s->superblock_count);

    s->macroblock_width = (s->width + 15) / 16;
    s->macroblock_height = (s->height + 15) / 16;
    s->macroblock_count = s->macroblock_width * s->macroblock_height;

    s->fragment_width = s->width / FRAGMENT_PIXELS;
    s->fragment_height = s->height / FRAGMENT_PIXELS;

    /* fragment count covers all 8x8 blocks for all 3 planes */
    s->fragment_count = s->fragment_width * s->fragment_height * 3 / 2;
    s->u_fragment_start = s->fragment_width * s->fragment_height;
    s->v_fragment_start = s->fragment_width * s->fragment_height * 5 / 4;

    debug_init("  Y plane: %d x %d\n", s->width, s->height);
    debug_init("  C plane: %d x %d\n", c_width, c_height);
    debug_init("  Y superblocks: %d x %d, %d total\n",
        s->y_superblock_width, s->y_superblock_height, y_superblock_count);
    debug_init("  C superblocks: %d x %d, %d total\n",
        s->c_superblock_width, s->c_superblock_height, c_superblock_count);
    debug_init("  total superblocks = %d, U starts @ %d, V starts @ %d\n",
        s->superblock_count, s->u_superblock_start, s->v_superblock_start);
    debug_init("  macroblocks: %d x %d, %d total\n",
        s->macroblock_width, s->macroblock_height, s->macroblock_count);
    debug_init("  %d fragments, %d x %d, u starts @ %d, v starts @ %d\n",
        s->fragment_count,
        s->fragment_width,
        s->fragment_height,
        s->u_fragment_start,
        s->v_fragment_start);

    s->all_fragments = av_malloc(s->fragment_count * sizeof(Vp3Fragment));
    s->coeffs = av_malloc(s->fragment_count * sizeof(Coeff) * 65);
    s->coded_fragment_list = av_malloc(s->fragment_count * sizeof(int));
    s->pixel_addresses_inited = 0;

    if (!s->theora_tables)
    {
        for (i = 0; i < 64; i++)
            s->coded_dc_scale_factor[i] = vp31_dc_scale_factor[i];
        for (i = 0; i < 64; i++)
            s->coded_ac_scale_factor[i] = vp31_ac_scale_factor[i];
        for (i = 0; i < 64; i++)
            s->coded_intra_y_dequant[i] = vp31_intra_y_dequant[i];
        for (i = 0; i < 64; i++)
            s->coded_intra_c_dequant[i] = vp31_intra_c_dequant[i];
        for (i = 0; i < 64; i++)
            s->coded_inter_dequant[i] = vp31_inter_dequant[i];
        for (i = 0; i < 64; i++)
            s->filter_limit_values[i] = vp31_filter_limit_values[i];

        /* init VLC tables */
        for (i = 0; i < 16; i++) {

            /* DC histograms */
            init_vlc(&s->dc_vlc[i], 5, 32,
                &dc_bias[i][0][1], 4, 2,
                &dc_bias[i][0][0], 4, 2, 0);

            /* group 1 AC histograms */
            init_vlc(&s->ac_vlc_1[i], 5, 32,
                &ac_bias_0[i][0][1], 4, 2,
                &ac_bias_0[i][0][0], 4, 2, 0);

            /* group 2 AC histograms */
            init_vlc(&s->ac_vlc_2[i], 5, 32,
                &ac_bias_1[i][0][1], 4, 2,
                &ac_bias_1[i][0][0], 4, 2, 0);

            /* group 3 AC histograms */
            init_vlc(&s->ac_vlc_3[i], 5, 32,
                &ac_bias_2[i][0][1], 4, 2,
                &ac_bias_2[i][0][0], 4, 2, 0);

            /* group 4 AC histograms */
            init_vlc(&s->ac_vlc_4[i], 5, 32,
                &ac_bias_3[i][0][1], 4, 2,
                &ac_bias_3[i][0][0], 4, 2, 0);
        }
    } else {
        for (i = 0; i < 16; i++) {

            /* DC histograms */
            init_vlc(&s->dc_vlc[i], 5, 32,
                &s->huffman_table[i][0][1], 4, 2,
                &s->huffman_table[i][0][0], 4, 2, 0);

            /* group 1 AC histograms */
            init_vlc(&s->ac_vlc_1[i], 5, 32,
                &s->huffman_table[i+16][0][1], 4, 2,
                &s->huffman_table[i+16][0][0], 4, 2, 0);

            /* group 2 AC histograms */
            init_vlc(&s->ac_vlc_2[i], 5, 32,
                &s->huffman_table[i+16*2][0][1], 4, 2,
                &s->huffman_table[i+16*2][0][0], 4, 2, 0);

            /* group 3 AC histograms */
            init_vlc(&s->ac_vlc_3[i], 5, 32,
                &s->huffman_table[i+16*3][0][1], 4, 2,
                &s->huffman_table[i+16*3][0][0], 4, 2, 0);

            /* group 4 AC histograms */
            init_vlc(&s->ac_vlc_4[i], 5, 32,
                &s->huffman_table[i+16*4][0][1], 4, 2,
                &s->huffman_table[i+16*4][0][0], 4, 2, 0);
        }
    }

    init_vlc(&s->superblock_run_length_vlc, 6, 34,
        &superblock_run_length_vlc_table[0][1], 4, 2,
        &superblock_run_length_vlc_table[0][0], 4, 2, 0);

    init_vlc(&s->fragment_run_length_vlc, 5, 30,
        &fragment_run_length_vlc_table[0][1], 4, 2,
        &fragment_run_length_vlc_table[0][0], 4, 2, 0);

    init_vlc(&s->mode_code_vlc, 3, 8,
        &mode_code_vlc_table[0][1], 2, 1,
        &mode_code_vlc_table[0][0], 2, 1, 0);

    init_vlc(&s->motion_vector_vlc, 6, 63,
        &motion_vector_vlc_table[0][1], 2, 1,
        &motion_vector_vlc_table[0][0], 2, 1, 0);

    /* work out the block mapping tables */
    s->superblock_fragments = av_malloc(s->superblock_count * 16 * sizeof(int));
    s->superblock_macroblocks = av_malloc(s->superblock_count * 4 * sizeof(int));
    s->macroblock_fragments = av_malloc(s->macroblock_count * 6 * sizeof(int));
    s->macroblock_coding = av_malloc(s->macroblock_count + 1);
    init_block_mapping(s);

    for (i = 0; i < 3; i++) {
        s->current_frame.data[i] = NULL;
        s->last_frame.data[i] = NULL;
        s->golden_frame.data[i] = NULL;
    }

    return 0;
}

/*
 * This is the ffmpeg/libavcodec API frame decode function.
 */
static int vp3_decode_frame(AVCodecContext *avctx,
                            void *data, int *data_size,
                            uint8_t *buf, int buf_size)
{
    Vp3DecodeContext *s = avctx->priv_data;
    GetBitContext gb;
    static int counter = 0;
    int i;

    init_get_bits(&gb, buf, buf_size * 8);

    if (s->theora && get_bits1(&gb))
    {
#if 1
        av_log(avctx, AV_LOG_ERROR, "Header packet passed to frame decoder, skipping\n");
        return -1;
#else
        int ptype = get_bits(&gb, 7);

        skip_bits(&gb, 6*8); /* "theora" */

        switch(ptype)
        {
            case 1:
                theora_decode_comments(avctx, gb);
                break;
            case 2:
                theora_decode_tables(avctx, gb);
                    init_dequantizer(s);
                break;
            default:
                av_log(avctx, AV_LOG_ERROR, "Unknown Theora config packet: %d\n", ptype);
        }
        return buf_size;
#endif
    }

    s->keyframe = !get_bits1(&gb);
    if (!s->theora)
        skip_bits(&gb, 1);
    s->last_quality_index = s->quality_index;
    s->quality_index = get_bits(&gb, 6);
    if (s->theora >= 0x030200)
        skip_bits1(&gb);

    if (s->avctx->debug & FF_DEBUG_PICT_INFO)
        av_log(s->avctx, AV_LOG_INFO, " VP3 %sframe #%d: Q index = %d\n",
            s->keyframe?"key":"", counter, s->quality_index);
    counter++;

    if (s->quality_index != s->last_quality_index) {
        init_dequantizer(s);
        init_loop_filter(s);
    }

    if (s->keyframe) {
        if (!s->theora)
        {
            skip_bits(&gb, 4); /* width code */
            skip_bits(&gb, 4); /* height code */
            if (s->version)
            {
                s->version = get_bits(&gb, 5);
                if (counter == 1)
                    av_log(s->avctx, AV_LOG_DEBUG, "VP version: %d\n", s->version);
            }
        }
        if (s->version || s->theora)
        {
                if (get_bits1(&gb))
                    av_log(s->avctx, AV_LOG_ERROR, "Warning, unsupported keyframe coding type?!\n");
            skip_bits(&gb, 2); /* reserved? */
        }

        if (s->last_frame.data[0] == s->golden_frame.data[0]) {
            if (s->golden_frame.data[0])
                avctx->release_buffer(avctx, &s->golden_frame);
            s->last_frame= s->golden_frame; /* ensure that we catch any access to this released frame */
        } else {
            if (s->golden_frame.data[0])
                avctx->release_buffer(avctx, &s->golden_frame);
            if (s->last_frame.data[0])
                avctx->release_buffer(avctx, &s->last_frame);
        }

        s->golden_frame.reference = 3;
        if(avctx->get_buffer(avctx, &s->golden_frame) < 0) {
            av_log(s->avctx, AV_LOG_ERROR, "vp3: get_buffer() failed\n");
            return -1;
        }

        /* golden frame is also the current frame */
        memcpy(&s->current_frame, &s->golden_frame, sizeof(AVFrame));

        /* time to figure out pixel addresses? */
        if (!s->pixel_addresses_inited)
        {
            if (!s->flipped_image)
                vp3_calculate_pixel_addresses(s);
            else
                theora_calculate_pixel_addresses(s);
        }
    } else {
        /* allocate a new current frame */
        s->current_frame.reference = 3;
        if(avctx->get_buffer(avctx, &s->current_frame) < 0) {
            av_log(s->avctx, AV_LOG_ERROR, "vp3: get_buffer() failed\n");
            return -1;
        }
    }

    s->current_frame.qscale_table= s->qscale_table; //FIXME allocate individual tables per AVFrame
    s->current_frame.qstride= 0;

    {START_TIMER
    init_frame(s, &gb);
    STOP_TIMER("init_frame")}

#if KEYFRAMES_ONLY
if (!s->keyframe) {

    memcpy(s->current_frame.data[0], s->golden_frame.data[0],
        s->current_frame.linesize[0] * s->height);
    memcpy(s->current_frame.data[1], s->golden_frame.data[1],
        s->current_frame.linesize[1] * s->height / 2);
    memcpy(s->current_frame.data[2], s->golden_frame.data[2],
        s->current_frame.linesize[2] * s->height / 2);

} else {
#endif

    {START_TIMER
    if (unpack_superblocks(s, &gb)){
        av_log(s->avctx, AV_LOG_ERROR, "error in unpack_superblocks\n");
        return -1;
    }
    STOP_TIMER("unpack_superblocks")}
    {START_TIMER
    if (unpack_modes(s, &gb)){
        av_log(s->avctx, AV_LOG_ERROR, "error in unpack_modes\n");
        return -1;
    }
    STOP_TIMER("unpack_modes")}
    {START_TIMER
    if (unpack_vectors(s, &gb)){
        av_log(s->avctx, AV_LOG_ERROR, "error in unpack_vectors\n");
        return -1;
    }
    STOP_TIMER("unpack_vectors")}
    {START_TIMER
    if (unpack_dct_coeffs(s, &gb)){
        av_log(s->avctx, AV_LOG_ERROR, "error in unpack_dct_coeffs\n");
        return -1;
    }
    STOP_TIMER("unpack_dct_coeffs")}
    {START_TIMER

    reverse_dc_prediction(s, 0, s->fragment_width, s->fragment_height);
    if ((avctx->flags & CODEC_FLAG_GRAY) == 0) {
        reverse_dc_prediction(s, s->u_fragment_start,
            s->fragment_width / 2, s->fragment_height / 2);
        reverse_dc_prediction(s, s->v_fragment_start,
            s->fragment_width / 2, s->fragment_height / 2);
    }
    STOP_TIMER("reverse_dc_prediction")}
    {START_TIMER

    for (i = 0; i < s->macroblock_height; i++)
        render_slice(s, i);
    STOP_TIMER("render_fragments")}

    {START_TIMER
    apply_loop_filter(s);
    STOP_TIMER("apply_loop_filter")}
#if KEYFRAMES_ONLY
}
#endif

    *data_size=sizeof(AVFrame);
    *(AVFrame*)data= s->current_frame;

    /* release the last frame, if it is allocated and if it is not the
     * golden frame */
    if ((s->last_frame.data[0]) &&
        (s->last_frame.data[0] != s->golden_frame.data[0]))
        avctx->release_buffer(avctx, &s->last_frame);

    /* shuffle frames (last = current) */
    memcpy(&s->last_frame, &s->current_frame, sizeof(AVFrame));
    s->current_frame.data[0]= NULL; /* ensure that we catch any access to this released frame */

    return buf_size;
}

/*
 * This is the ffmpeg/libavcodec API module cleanup function.
 */
static int vp3_decode_end(AVCodecContext *avctx)
{
    Vp3DecodeContext *s = avctx->priv_data;

    av_free(s->all_fragments);
    av_free(s->coeffs);
    av_free(s->coded_fragment_list);
    av_free(s->superblock_fragments);
    av_free(s->superblock_macroblocks);
    av_free(s->macroblock_fragments);
    av_free(s->macroblock_coding);

    /* release all frames */
    if (s->golden_frame.data[0] && s->golden_frame.data[0] != s->last_frame.data[0])
        avctx->release_buffer(avctx, &s->golden_frame);
    if (s->last_frame.data[0])
        avctx->release_buffer(avctx, &s->last_frame);
    /* no need to release the current_frame since it will always be pointing
     * to the same frame as either the golden or last frame */

    return 0;
}

static int read_huffman_tree(AVCodecContext *avctx, GetBitContext *gb)
{
    Vp3DecodeContext *s = avctx->priv_data;

    if (get_bits(gb, 1)) {
        int token;
        if (s->entries >= 32) { /* overflow */
            av_log(avctx, AV_LOG_ERROR, "huffman tree overflow\n");
            return -1;
        }
        token = get_bits(gb, 5);
        //av_log(avctx, AV_LOG_DEBUG, "hti %d hbits %x token %d entry : %d size %d\n", s->hti, s->hbits, token, s->entries, s->huff_code_size);
        s->huffman_table[s->hti][token][0] = s->hbits;
        s->huffman_table[s->hti][token][1] = s->huff_code_size;
        s->entries++;
    }
    else {
        if (s->huff_code_size >= 32) {/* overflow */
            av_log(avctx, AV_LOG_ERROR, "huffman tree overflow\n");
            return -1;
        }
        s->huff_code_size++;
        s->hbits <<= 1;
        read_huffman_tree(avctx, gb);
        s->hbits |= 1;
        read_huffman_tree(avctx, gb);
        s->hbits >>= 1;
        s->huff_code_size--;
    }
    return 0;
}

static int theora_decode_header(AVCodecContext *avctx, GetBitContext gb)
{
    Vp3DecodeContext *s = avctx->priv_data;
    int major, minor, micro;

    major = get_bits(&gb, 8); /* version major */
    minor = get_bits(&gb, 8); /* version minor */
    micro = get_bits(&gb, 8); /* version micro */
    av_log(avctx, AV_LOG_INFO, "Theora bitstream version %d.%d.%d\n",
        major, minor, micro);

    /* FIXME: endianess? */
    s->theora = (major << 16) | (minor << 8) | micro;

    /* 3.2.0 aka alpha3 has the same frame orientation as original vp3 */
    /* but previous versions have the image flipped relative to vp3 */
    if (s->theora < 0x030200)
    {
        s->flipped_image = 1;
        av_log(avctx, AV_LOG_DEBUG, "Old (<alpha3) Theora bitstream, flipped image\n");
    }

    s->width = get_bits(&gb, 16) << 4;
    s->height = get_bits(&gb, 16) << 4;

    if(avcodec_check_dimensions(avctx, s->width, s->height)){
        av_log(avctx, AV_LOG_ERROR, "Invalid dimensions (%dx%d)\n", s->width, s->height);
        s->width= s->height= 0;
        return -1;
    }

    if (s->theora >= 0x030400)
    {
        skip_bits(&gb, 32); /* total number of superblocks in a frame */
        // fixme, the next field is 36bits long
        skip_bits(&gb, 32); /* total number of blocks in a frame */
        skip_bits(&gb, 4); /* total number of blocks in a frame */
        skip_bits(&gb, 32); /* total number of macroblocks in a frame */

        skip_bits(&gb, 24); /* frame width */
        skip_bits(&gb, 24); /* frame height */
    }
    else
    {
        skip_bits(&gb, 24); /* frame width */
        skip_bits(&gb, 24); /* frame height */
    }

    skip_bits(&gb, 8); /* offset x */
    skip_bits(&gb, 8); /* offset y */

    skip_bits(&gb, 32); /* fps numerator */
    skip_bits(&gb, 32); /* fps denumerator */
    skip_bits(&gb, 24); /* aspect numerator */
    skip_bits(&gb, 24); /* aspect denumerator */

    if (s->theora < 0x030200)
        skip_bits(&gb, 5); /* keyframe frequency force */
    skip_bits(&gb, 8); /* colorspace */
    if (s->theora >= 0x030400)
        skip_bits(&gb, 2); /* pixel format: 420,res,422,444 */
    skip_bits(&gb, 24); /* bitrate */

    skip_bits(&gb, 6); /* quality hint */

    if (s->theora >= 0x030200)
    {
        skip_bits(&gb, 5); /* keyframe frequency force */

        if (s->theora < 0x030400)
            skip_bits(&gb, 5); /* spare bits */
    }

//    align_get_bits(&gb);

    avctx->width = s->width;
    avctx->height = s->height;

    return 0;
}

static inline int theora_get_32bit(GetBitContext gb)
{
    int ret = get_bits(&gb, 8);
    ret += get_bits(&gb, 8) << 8;
    ret += get_bits(&gb, 8) << 16;
    ret += get_bits(&gb, 8) << 24;

    return ret;
}

static int theora_decode_tables(AVCodecContext *avctx, GetBitContext gb)
{
    Vp3DecodeContext *s = avctx->priv_data;
    int i, n, matrices;

    if (s->theora >= 0x030200) {
        n = get_bits(&gb, 3);
        /* loop filter limit values table */
        for (i = 0; i < 64; i++)
            s->filter_limit_values[i] = get_bits(&gb, n);
    }

    if (s->theora >= 0x030200)
        n = get_bits(&gb, 4) + 1;
    else
        n = 16;
    /* quality threshold table */
    for (i = 0; i < 64; i++)
        s->coded_ac_scale_factor[i] = get_bits(&gb, n);

    if (s->theora >= 0x030200)
        n = get_bits(&gb, 4) + 1;
    else
        n = 16;
    /* dc scale factor table */
    for (i = 0; i < 64; i++)
        s->coded_dc_scale_factor[i] = get_bits(&gb, n);

    if (s->theora >= 0x030200)
        matrices = get_bits(&gb, 9) + 1;
    else
        matrices = 3;
    if (matrices != 3) {
        av_log(avctx,AV_LOG_ERROR, "unsupported matrices: %d\n", matrices);
//        return -1;
    }
    /* y coeffs */
    for (i = 0; i < 64; i++)
        s->coded_intra_y_dequant[i] = get_bits(&gb, 8);

    /* uv coeffs */
    for (i = 0; i < 64; i++)
        s->coded_intra_c_dequant[i] = get_bits(&gb, 8);

    /* inter coeffs */
    for (i = 0; i < 64; i++)
        s->coded_inter_dequant[i] = get_bits(&gb, 8);

    /* skip unknown matrices */
    n = matrices - 3;
    while(n--)
        for (i = 0; i < 64; i++)
            skip_bits(&gb, 8);

    for (i = 0; i <= 1; i++) {
        for (n = 0; n <= 2; n++) {
            int newqr;
            if (i > 0 || n > 0)
                newqr = get_bits(&gb, 1);
            else
                newqr = 1;
            if (!newqr) {
                if (i > 0)
                    get_bits(&gb, 1);
            }
            else {
                int qi = 0;
                skip_bits(&gb, av_log2(matrices-1)+1);
                while (qi < 63) {
                    qi += get_bits(&gb, av_log2(63-qi)+1) + 1;
                    skip_bits(&gb, av_log2(matrices-1)+1);
                }
                if (qi > 63) {
                    av_log(avctx, AV_LOG_ERROR, "invalid qi %d > 63\n", qi);
                    return -1;
                }
            }
        }
    }

    /* Huffman tables */
    for (s->hti = 0; s->hti < 80; s->hti++) {
        s->entries = 0;
        s->huff_code_size = 1;
        if (!get_bits(&gb, 1)) {
            s->hbits = 0;
            read_huffman_tree(avctx, &gb);
            s->hbits = 1;
            read_huffman_tree(avctx, &gb);
        }
    }

    s->theora_tables = 1;

    return 0;
}

static int theora_decode_init(AVCodecContext *avctx)
{
    Vp3DecodeContext *s = avctx->priv_data;
    GetBitContext gb;
    int ptype;
    uint8_t *p= avctx->extradata;
    int op_bytes, i;

    s->theora = 1;

    if (!avctx->extradata_size)
    {
        av_log(avctx, AV_LOG_ERROR, "Missing extradata!\n");
        return -1;
    }

  for(i=0;i<3;i++) {
    op_bytes = *(p++)<<8;
    op_bytes += *(p++);

    init_get_bits(&gb, p, op_bytes);
    p += op_bytes;

    ptype = get_bits(&gb, 8);
    debug_vp3("Theora headerpacket type: %x\n", ptype);

     if (!(ptype & 0x80))
     {
        av_log(avctx, AV_LOG_ERROR, "Invalid extradata!\n");
        return -1;
     }

    // FIXME: check for this aswell
    skip_bits(&gb, 6*8); /* "theora" */

    switch(ptype)
    {
        case 0x80:
            theora_decode_header(avctx, gb);
                break;
        case 0x81:
// FIXME: is this needed? it breaks sometimes
//            theora_decode_comments(avctx, gb);
            break;
        case 0x82:
            theora_decode_tables(avctx, gb);
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, "Unknown Theora config packet: %d\n", ptype&~0x80);
            break;
    }
  }

    vp3_decode_init(avctx);
    return 0;
}

AVCodec vp3_decoder = {
    "vp3",
    CODEC_TYPE_VIDEO,
    CODEC_ID_VP3,
    sizeof(Vp3DecodeContext),
    vp3_decode_init,
    NULL,
    vp3_decode_end,
    vp3_decode_frame,
    0,
    NULL
};

#ifndef CONFIG_LIBTHEORA
AVCodec theora_decoder = {
    "theora",
    CODEC_TYPE_VIDEO,
    CODEC_ID_THEORA,
    sizeof(Vp3DecodeContext),
    theora_decode_init,
    NULL,
    vp3_decode_end,
    vp3_decode_frame,
    0,
    NULL
};
#endif
