/*
 * MPEG1 codec / MPEG2 decoder
 * Copyright (c) 2000,2001 Fabrice Bellard.
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
 
/**
 * @file mpeg12.c
 * MPEG1/2 codec
 */
 
//#define DEBUG
#include "avcodec.h"
#include "dsputil.h"
#include "mpegvideo.h"

#include "mpeg12data.h"


/* Start codes. */
#define SEQ_END_CODE		0x000001b7
#define SEQ_START_CODE		0x000001b3
#define GOP_START_CODE		0x000001b8
#define PICTURE_START_CODE	0x00000100
#define SLICE_MIN_START_CODE	0x00000101
#define SLICE_MAX_START_CODE	0x000001af
#define EXT_START_CODE		0x000001b5
#define USER_START_CODE		0x000001b2

#define DC_VLC_BITS 9
#define MV_VLC_BITS 9
#define MBINCR_VLC_BITS 9
#define MB_PAT_VLC_BITS 9
#define MB_PTYPE_VLC_BITS 6
#define MB_BTYPE_VLC_BITS 6
#define TEX_VLC_BITS 9

#ifdef CONFIG_ENCODERS
static void mpeg1_encode_block(MpegEncContext *s, 
                         DCTELEM *block, 
                         int component);
static void mpeg1_encode_motion(MpegEncContext *s, int val, int f_or_b_code);    // RAL: f_code parameter added
#endif //CONFIG_ENCODERS
static void mpeg1_skip_picture(MpegEncContext *s, int pict_num);
static inline int mpeg1_decode_block_inter(MpegEncContext *s, 
                              DCTELEM *block, 
                              int n);
static inline int mpeg1_decode_block_intra(MpegEncContext *s, 
                              DCTELEM *block, 
                              int n);
static inline int mpeg2_decode_block_non_intra(MpegEncContext *s, 
                                        DCTELEM *block, 
                                        int n);
static inline int mpeg2_decode_block_intra(MpegEncContext *s, 
                                    DCTELEM *block, 
                                    int n);
static int mpeg_decode_motion(MpegEncContext *s, int fcode, int pred);
static void exchange_uv(MpegEncContext *s);

#ifdef HAVE_XVMC
extern int XVMC_field_start(MpegEncContext *s, AVCodecContext *avctx);
extern int XVMC_field_end(MpegEncContext *s);
extern void XVMC_pack_pblocks(MpegEncContext *s,int cbp);
extern void XVMC_init_block(MpegEncContext *s);//set s->block
#endif

#ifdef CONFIG_ENCODERS
static uint8_t (*mv_penalty)[MAX_MV*2+1]= NULL;
static uint8_t fcode_tab[MAX_MV*2+1];

static uint32_t uni_mpeg1_ac_vlc_bits[64*64*2];
static uint8_t  uni_mpeg1_ac_vlc_len [64*64*2];

/* simple include everything table for dc, first byte is bits number next 3 are code*/
static uint32_t mpeg1_lum_dc_uni[512];
static uint32_t mpeg1_chr_dc_uni[512];

static uint8_t mpeg1_index_run[2][64];
static int8_t mpeg1_max_level[2][64];
#endif //CONFIG_ENCODERS

static void init_2d_vlc_rl(RLTable *rl)
{
    int i;
    
    init_vlc(&rl->vlc, TEX_VLC_BITS, rl->n + 2, 
             &rl->table_vlc[0][1], 4, 2,
             &rl->table_vlc[0][0], 4, 2);

    
    rl->rl_vlc[0]= av_malloc(rl->vlc.table_size*sizeof(RL_VLC_ELEM));
    for(i=0; i<rl->vlc.table_size; i++){
        int code= rl->vlc.table[i][0];
        int len = rl->vlc.table[i][1];
        int level, run;
    
        if(len==0){ // illegal code
            run= 65;
            level= MAX_LEVEL;
        }else if(len<0){ //more bits needed
            run= 0;
            level= code;
        }else{
            if(code==rl->n){ //esc
                run= 65;
                level= 0;
            }else if(code==rl->n+1){ //eob
                run= 0;
                level= 127;
            }else{
                run=   rl->table_run  [code] + 1;
                level= rl->table_level[code];
            }
        }
        rl->rl_vlc[0][i].len= len;
        rl->rl_vlc[0][i].level= level;
        rl->rl_vlc[0][i].run= run;
    }
}

#ifdef CONFIG_ENCODERS
static void init_uni_ac_vlc(RLTable *rl, uint32_t *uni_ac_vlc_bits, uint8_t *uni_ac_vlc_len){
    int i;

    for(i=0; i<128; i++){
        int level= i-64;
        int run;
        for(run=0; run<64; run++){
            int len, bits, code;
            
            int alevel= ABS(level);
            int sign= (level>>31)&1;

            if (alevel > rl->max_level[0][run])
                code= 111; /*rl->n*/
            else
                code= rl->index_run[0][run] + alevel - 1;

            if (code < 111 /* rl->n */) {
	    	/* store the vlc & sign at once */
                len=   mpeg1_vlc[code][1]+1;
                bits= (mpeg1_vlc[code][0]<<1) + sign;
            } else {
                len=  mpeg1_vlc[111/*rl->n*/][1]+6;
                bits= mpeg1_vlc[111/*rl->n*/][0]<<6;

                bits|= run;
                if (alevel < 128) {
                    bits<<=8; len+=8;
                    bits|= level & 0xff;
                } else {
                    bits<<=16; len+=16;
                    bits|= level & 0xff;
                    if (level < 0) {
                        bits|= 0x8001 + level + 255;
                    } else {
                        bits|= level & 0xffff;
                    }
                }
            }

            uni_ac_vlc_bits[UNI_AC_ENC_INDEX(run, i)]= bits;
            uni_ac_vlc_len [UNI_AC_ENC_INDEX(run, i)]= len;
        }
    }
}

static void put_header(MpegEncContext *s, int header)
{
    align_put_bits(&s->pb);
    put_bits(&s->pb, 16, header>>16);
    put_bits(&s->pb, 16, header&0xFFFF);
}

/* put sequence header if needed */
static void mpeg1_encode_sequence_header(MpegEncContext *s)
{
        unsigned int vbv_buffer_size;
        unsigned int fps, v;
        int n, i;
        uint64_t time_code;
        float best_aspect_error= 1E10;
        float aspect_ratio= av_q2d(s->avctx->sample_aspect_ratio);
        int constraint_parameter_flag;
        
        if(aspect_ratio==0.0) aspect_ratio= 1.0; //pixel aspect 1:1 (VGA)
        
        if (s->current_picture.key_frame) {
            /* mpeg1 header repeated every gop */
            put_header(s, SEQ_START_CODE);
            
            /* search closest frame rate */
            {
                int i, dmin, d;
                s->frame_rate_index = 0;
                dmin = 0x7fffffff;
                for(i=1;i<14;i++) {
                    if(s->avctx->strict_std_compliance >= 0 && i>=9) break;
                     
                    d = abs(MPEG1_FRAME_RATE_BASE*(int64_t)s->avctx->frame_rate/s->avctx->frame_rate_base - frame_rate_tab[i]);
                    if (d < dmin) {
                        dmin = d;
                        s->frame_rate_index = i;
                    }
                }
            }
 
            put_bits(&s->pb, 12, s->width);
            put_bits(&s->pb, 12, s->height);
            
            for(i=1; i<15; i++){
                float error= aspect_ratio;
                if(s->codec_id == CODEC_ID_MPEG1VIDEO || i <=1)
                    error-= 1.0/mpeg1_aspect[i];
                else
                    error-= av_q2d(mpeg2_aspect[i])*s->height/s->width;
             
                error= ABS(error);
                
                if(error < best_aspect_error){
                    best_aspect_error= error;
                    s->aspect_ratio_info= i;
                }
            }
            
            put_bits(&s->pb, 4, s->aspect_ratio_info);
            put_bits(&s->pb, 4, s->frame_rate_index);
            
            if(s->avctx->rc_max_rate){
                v = (s->avctx->rc_max_rate + 399) / 400;
                if (v > 0x3ffff && s->codec_id == CODEC_ID_MPEG1VIDEO)
                    v = 0x3ffff;
            }else{
                v= 0x3FFFF;
            }

            if(s->avctx->rc_buffer_size)
                vbv_buffer_size = s->avctx->rc_buffer_size;
            else
                /* VBV calculation: Scaled so that a VCD has the proper VBV size of 40 kilobytes */
                vbv_buffer_size = (( 20 * s->bit_rate) / (1151929 / 2)) * 8 * 1024;
            vbv_buffer_size= (vbv_buffer_size + 16383) / 16384;

            put_bits(&s->pb, 18, v & 0x3FFFF);
            put_bits(&s->pb, 1, 1); /* marker */
            put_bits(&s->pb, 10, vbv_buffer_size & 0x3FF);

            constraint_parameter_flag= 
                s->width <= 768 && s->height <= 576 && 
                s->mb_width * s->mb_height <= 396 &&
                s->mb_width * s->mb_height * frame_rate_tab[s->frame_rate_index] <= MPEG1_FRAME_RATE_BASE*396*25 &&
                frame_rate_tab[s->frame_rate_index] <= MPEG1_FRAME_RATE_BASE*30 &&
                vbv_buffer_size <= 20 &&
                v <= 1856000/400 &&
                s->codec_id == CODEC_ID_MPEG1VIDEO;
                
            put_bits(&s->pb, 1, constraint_parameter_flag);
            
            ff_write_quant_matrix(&s->pb, s->avctx->intra_matrix);
            ff_write_quant_matrix(&s->pb, s->avctx->inter_matrix);

            if(s->codec_id == CODEC_ID_MPEG2VIDEO){
                put_header(s, EXT_START_CODE);
                put_bits(&s->pb, 4, 1); //seq ext
                put_bits(&s->pb, 1, 0); //esc
                put_bits(&s->pb, 3, 4); //profile
                put_bits(&s->pb, 4, 8); //level
                put_bits(&s->pb, 1, s->progressive_sequence=1);
                put_bits(&s->pb, 2, 1); //chroma format 4:2:0
                put_bits(&s->pb, 2, 0); //horizontal size ext
                put_bits(&s->pb, 2, 0); //vertical size ext
                put_bits(&s->pb, 12, v>>18); //bitrate ext
                put_bits(&s->pb, 1, 1); //marker
                put_bits(&s->pb, 8, vbv_buffer_size >>10); //vbv buffer ext
                put_bits(&s->pb, 1, s->low_delay);
                put_bits(&s->pb, 2, 0); // frame_rate_ext_n
                put_bits(&s->pb, 5, 0); // frame_rate_ext_d
            }
            
            put_header(s, GOP_START_CODE);
            put_bits(&s->pb, 1, 0); /* do drop frame */
            /* time code : we must convert from the real frame rate to a
               fake mpeg frame rate in case of low frame rate */
            fps = frame_rate_tab[s->frame_rate_index];
            time_code = (int64_t)s->fake_picture_number * MPEG1_FRAME_RATE_BASE;
            s->gop_picture_number = s->fake_picture_number;
            put_bits(&s->pb, 5, (uint32_t)((time_code / (fps * 3600)) % 24));
            put_bits(&s->pb, 6, (uint32_t)((time_code / (fps * 60)) % 60));
            put_bits(&s->pb, 1, 1);
            put_bits(&s->pb, 6, (uint32_t)((time_code / fps) % 60));
            put_bits(&s->pb, 6, (uint32_t)((time_code % fps) / MPEG1_FRAME_RATE_BASE));
            put_bits(&s->pb, 1, 0); /* closed gop */
            put_bits(&s->pb, 1, 0); /* broken link */
        }

        if (s->avctx->frame_rate < (24 * s->avctx->frame_rate_base) && s->picture_number > 0) {
            /* insert empty P pictures to slow down to the desired
               frame rate. Each fake pictures takes about 20 bytes */
            fps = frame_rate_tab[s->frame_rate_index];
            n = av_rescale((int64_t)s->picture_number * s->avctx->frame_rate_base, fps, s->avctx->frame_rate) / MPEG1_FRAME_RATE_BASE - 1;
            while (s->fake_picture_number < n) {
                mpeg1_skip_picture(s, s->fake_picture_number - 
                                   s->gop_picture_number); 
                s->fake_picture_number++;
            }

        }
}

static inline void encode_mb_skip_run(MpegEncContext *s, int run){
    while (run >= 33) {
        put_bits(&s->pb, 11, 0x008);
        run -= 33;
    }
    put_bits(&s->pb, mbAddrIncrTable[run][1], 
             mbAddrIncrTable[run][0]);
}

/* insert a fake P picture */
static void mpeg1_skip_picture(MpegEncContext *s, int pict_num)
{
    assert(s->codec_id == CODEC_ID_MPEG1VIDEO); // mpeg2 can do these repeat things

    /* mpeg1 picture header */
    put_header(s, PICTURE_START_CODE);
    /* temporal reference */
    put_bits(&s->pb, 10, pict_num & 0x3ff); 
    
    put_bits(&s->pb, 3, P_TYPE);
    put_bits(&s->pb, 16, 0xffff); /* non constant bit rate */
    
    put_bits(&s->pb, 1, 1); /* integer coordinates */
    put_bits(&s->pb, 3, 1); /* forward_f_code */
    
    put_bits(&s->pb, 1, 0); /* extra bit picture */
    
    /* only one slice */
    put_header(s, SLICE_MIN_START_CODE);
    put_bits(&s->pb, 5, 1); /* quantizer scale */
    put_bits(&s->pb, 1, 0); /* slice extra information */
    
    encode_mb_skip_run(s, 0);
    
    /* empty macroblock */
    put_bits(&s->pb, 3, 1); /* motion only */
    
    /* zero motion x & y */
    put_bits(&s->pb, 1, 1); 
    put_bits(&s->pb, 1, 1); 

    /* output a number of empty slice */
    encode_mb_skip_run(s, s->mb_width * s->mb_height - 2);
    
    /* empty macroblock */
    put_bits(&s->pb, 3, 1); /* motion only */
    
    /* zero motion x & y */
    put_bits(&s->pb, 1, 1); 
    put_bits(&s->pb, 1, 1); 
}
#endif //CONFIG_ENCODERS

static void common_init(MpegEncContext *s)
{
    s->y_dc_scale_table=
    s->c_dc_scale_table= ff_mpeg1_dc_scale_table;
}

void ff_mpeg1_clean_buffers(MpegEncContext *s){
    s->last_dc[0] = 1 << (7 + s->intra_dc_precision);
    s->last_dc[1] = s->last_dc[0];
    s->last_dc[2] = s->last_dc[0];
    memset(s->last_mv, 0, sizeof(s->last_mv));
}

#ifdef CONFIG_ENCODERS

void ff_mpeg1_encode_slice_header(MpegEncContext *s){
    put_header(s, SLICE_MIN_START_CODE + s->mb_y);
    put_bits(&s->pb, 5, s->qscale); /* quantizer scale */
    put_bits(&s->pb, 1, 0); /* slice extra information */
}

void mpeg1_encode_picture_header(MpegEncContext *s, int picture_number)
{
    mpeg1_encode_sequence_header(s);

    /* mpeg1 picture header */
    put_header(s, PICTURE_START_CODE);
    /* temporal reference */

    // RAL: s->picture_number instead of s->fake_picture_number
    put_bits(&s->pb, 10, (s->picture_number - 
                          s->gop_picture_number) & 0x3ff); 
    s->fake_picture_number++;
    
    put_bits(&s->pb, 3, s->pict_type);
    put_bits(&s->pb, 16, 0xffff); /* non constant bit rate */
    
    // RAL: Forward f_code also needed for B frames
    if (s->pict_type == P_TYPE || s->pict_type == B_TYPE) {
        put_bits(&s->pb, 1, 0); /* half pel coordinates */
        if(s->codec_id == CODEC_ID_MPEG1VIDEO)
            put_bits(&s->pb, 3, s->f_code); /* forward_f_code */
        else
            put_bits(&s->pb, 3, 7); /* forward_f_code */
    }
    
    // RAL: Backward f_code necessary for B frames
    if (s->pict_type == B_TYPE) {
        put_bits(&s->pb, 1, 0); /* half pel coordinates */
        if(s->codec_id == CODEC_ID_MPEG1VIDEO)
            put_bits(&s->pb, 3, s->b_code); /* backward_f_code */
        else
            put_bits(&s->pb, 3, 7); /* backward_f_code */
    }

    put_bits(&s->pb, 1, 0); /* extra bit picture */
    
    if(s->codec_id == CODEC_ID_MPEG2VIDEO){
        put_header(s, EXT_START_CODE);
        put_bits(&s->pb, 4, 8); //pic ext
        if (s->pict_type == P_TYPE || s->pict_type == B_TYPE) {
            put_bits(&s->pb, 4, s->f_code);
            put_bits(&s->pb, 4, s->f_code);
        }else{
            put_bits(&s->pb, 8, 255);
        }
        if (s->pict_type == B_TYPE) {
            put_bits(&s->pb, 4, s->b_code);
            put_bits(&s->pb, 4, s->b_code);
        }else{
            put_bits(&s->pb, 8, 255);
        }
        put_bits(&s->pb, 2, s->intra_dc_precision);
        put_bits(&s->pb, 2, s->picture_structure= PICT_FRAME);
        put_bits(&s->pb, 1, s->top_field_first);
        put_bits(&s->pb, 1, s->frame_pred_frame_dct= 1);
        put_bits(&s->pb, 1, s->concealment_motion_vectors);
        put_bits(&s->pb, 1, s->q_scale_type);
        put_bits(&s->pb, 1, s->intra_vlc_format);
        put_bits(&s->pb, 1, s->alternate_scan);
        put_bits(&s->pb, 1, s->repeat_first_field);
        put_bits(&s->pb, 1, s->chroma_420_type=1);
        put_bits(&s->pb, 1, s->progressive_frame=1);
        put_bits(&s->pb, 1, 0); //composite_display_flag
    }
    
    s->mb_y=0;
    ff_mpeg1_encode_slice_header(s);
}

void mpeg1_encode_mb(MpegEncContext *s,
                     DCTELEM block[6][64],
                     int motion_x, int motion_y)
{
    int i, cbp;
    const int mb_x = s->mb_x;
    const int mb_y = s->mb_y;
    const int first_mb= mb_x == s->resync_mb_x && mb_y == s->resync_mb_y;

    /* compute cbp */
    cbp = 0;
    for(i=0;i<6;i++) {
        if (s->block_last_index[i] >= 0)
            cbp |= 1 << (5 - i);
    }

    if (cbp == 0 && !first_mb && (mb_x != s->mb_width - 1 || (mb_y != s->mb_height - 1 && s->codec_id == CODEC_ID_MPEG1VIDEO)) && 
        ((s->pict_type == P_TYPE && (motion_x | motion_y) == 0) ||
        (s->pict_type == B_TYPE && s->mv_dir == s->last_mv_dir && (((s->mv_dir & MV_DIR_FORWARD) ? ((s->mv[0][0][0] - s->last_mv[0][0][0])|(s->mv[0][0][1] - s->last_mv[0][0][1])) : 0) |
        ((s->mv_dir & MV_DIR_BACKWARD) ? ((s->mv[1][0][0] - s->last_mv[1][0][0])|(s->mv[1][0][1] - s->last_mv[1][0][1])) : 0)) == 0))) {
        s->mb_skip_run++;
        s->qscale -= s->dquant;
        s->skip_count++;
        s->misc_bits++;
        s->last_bits++;
    } else {
        if(first_mb){
            assert(s->mb_skip_run == 0);
            encode_mb_skip_run(s, s->mb_x);
        }else{
            encode_mb_skip_run(s, s->mb_skip_run);
        }
        
        if (s->pict_type == I_TYPE) {
            if(s->dquant && cbp){
                put_bits(&s->pb, 2, 1); /* macroblock_type : macroblock_quant = 1 */
                put_bits(&s->pb, 5, s->qscale);
            }else{
                put_bits(&s->pb, 1, 1); /* macroblock_type : macroblock_quant = 0 */
                s->qscale -= s->dquant;
            }
            s->misc_bits+= get_bits_diff(s);
            s->i_count++;
        } else if (s->mb_intra) {
            if(s->dquant && cbp){
                put_bits(&s->pb, 6, 0x01);
                put_bits(&s->pb, 5, s->qscale);
            }else{
                put_bits(&s->pb, 5, 0x03);
                s->qscale -= s->dquant;
            }
            s->misc_bits+= get_bits_diff(s);
            s->i_count++;
            s->last_mv[0][0][0] = 
            s->last_mv[0][0][1] = 0;
        } else if (s->pict_type == P_TYPE) { 
                if (cbp != 0) {
                    if (motion_x == 0 && motion_y == 0) {
                        if(s->dquant){
                            put_bits(&s->pb, 5, 1); /* macroblock_pattern & quant */
                            put_bits(&s->pb, 5, s->qscale);
                        }else{
                            put_bits(&s->pb, 2, 1); /* macroblock_pattern only */
                        }
                        s->misc_bits+= get_bits_diff(s);
                        put_bits(&s->pb, mbPatTable[cbp - 1][1], mbPatTable[cbp - 1][0]);
                    } else {
                        if(s->dquant){
                            put_bits(&s->pb, 5, 2); /* motion + cbp */
                            put_bits(&s->pb, 5, s->qscale);
                        }else{
                            put_bits(&s->pb, 1, 1); /* motion + cbp */
                        }
                        s->misc_bits+= get_bits_diff(s);
                        mpeg1_encode_motion(s, motion_x - s->last_mv[0][0][0], s->f_code);    // RAL: f_code parameter added
                        mpeg1_encode_motion(s, motion_y - s->last_mv[0][0][1], s->f_code);    // RAL: f_code parameter added
                        s->mv_bits+= get_bits_diff(s);
                        put_bits(&s->pb, mbPatTable[cbp - 1][1], mbPatTable[cbp - 1][0]);
                    }
                } else {
                    put_bits(&s->pb, 3, 1); /* motion only */
                    mpeg1_encode_motion(s, motion_x - s->last_mv[0][0][0], s->f_code);    // RAL: f_code parameter added
                    mpeg1_encode_motion(s, motion_y - s->last_mv[0][0][1], s->f_code);    // RAL: f_code parameter added
                    s->qscale -= s->dquant;
                    s->mv_bits+= get_bits_diff(s);
                }
                s->f_count++;
        } else
            {    // RAL: All the following bloc added for B frames:
                if (cbp != 0)
                    {    // With coded bloc pattern
                    if (s->mv_dir == (MV_DIR_FORWARD | MV_DIR_BACKWARD))
                        {    // Bi-directional motion
                        if (s->dquant)
                            {    // With QScale
                            put_bits(&s->pb, 5, 2);
                            put_bits(&s->pb, 5, s->qscale);
                            }
                        else    // Without QScale
                            put_bits(&s->pb, 2, 3);
                        s->misc_bits += get_bits_diff(s);
                        mpeg1_encode_motion(s, s->mv[0][0][0] - s->last_mv[0][0][0], s->f_code);
                        mpeg1_encode_motion(s, s->mv[0][0][1] - s->last_mv[0][0][1], s->f_code);
                        mpeg1_encode_motion(s, s->mv[1][0][0] - s->last_mv[1][0][0], s->b_code);
                        mpeg1_encode_motion(s, s->mv[1][0][1] - s->last_mv[1][0][1], s->b_code);
                        s->b_count++;
                        s->f_count++;
                        s->mv_bits += get_bits_diff(s);
                        put_bits(&s->pb, mbPatTable[cbp - 1][1], mbPatTable[cbp - 1][0]);
                        }
                    else if (s->mv_dir == MV_DIR_BACKWARD)
                        {    // Backward motion
                        if (s->dquant)
                            {    // With QScale
                            put_bits(&s->pb, 6, 2);
                            put_bits(&s->pb, 5, s->qscale);
                            }
                        else    // Without QScale
                            put_bits(&s->pb, 3, 3);
                        s->misc_bits += get_bits_diff(s);
                        mpeg1_encode_motion(s, motion_x - s->last_mv[1][0][0], s->b_code); 
                        mpeg1_encode_motion(s, motion_y - s->last_mv[1][0][1], s->b_code); 
                        s->b_count++;
                        s->mv_bits += get_bits_diff(s);
                        put_bits(&s->pb, mbPatTable[cbp - 1][1], mbPatTable[cbp - 1][0]);
                        }
                    else if (s->mv_dir == MV_DIR_FORWARD)
                        {    // Forward motion
                        if (s->dquant)
                            {    // With QScale
                            put_bits(&s->pb, 6, 3);
                            put_bits(&s->pb, 5, s->qscale);
                            }
                        else    // Without QScale
                            put_bits(&s->pb, 4, 3);
                        s->misc_bits += get_bits_diff(s);
                        mpeg1_encode_motion(s, motion_x - s->last_mv[0][0][0], s->f_code); 
                        mpeg1_encode_motion(s, motion_y - s->last_mv[0][0][1], s->f_code); 
                        s->f_count++;
                        s->mv_bits += get_bits_diff(s);
                        put_bits(&s->pb, mbPatTable[cbp - 1][1], mbPatTable[cbp - 1][0]);
                        }
                    }
                else
                    {    // No coded bloc pattern
                    if (s->mv_dir == (MV_DIR_FORWARD | MV_DIR_BACKWARD))
                        {    // Bi-directional motion 
                        put_bits(&s->pb, 2, 2); /* backward & forward motion */
                        mpeg1_encode_motion(s, s->mv[0][0][0] - s->last_mv[0][0][0], s->f_code);
                        mpeg1_encode_motion(s, s->mv[0][0][1] - s->last_mv[0][0][1], s->f_code);
                        mpeg1_encode_motion(s, s->mv[1][0][0] - s->last_mv[1][0][0], s->b_code);
                        mpeg1_encode_motion(s, s->mv[1][0][1] - s->last_mv[1][0][1], s->b_code);
                        s->b_count++;
                        s->f_count++;
                        }
                    else if (s->mv_dir == MV_DIR_BACKWARD)
                        {    // Backward motion
                        put_bits(&s->pb, 3, 2); /* backward motion only */
                        mpeg1_encode_motion(s, motion_x - s->last_mv[1][0][0], s->b_code); 
                        mpeg1_encode_motion(s, motion_y - s->last_mv[1][0][1], s->b_code); 
                        s->b_count++;
                        }
                    else if (s->mv_dir == MV_DIR_FORWARD)
                        {    // Forward motion
                        put_bits(&s->pb, 4, 2); /* forward motion only */
                        mpeg1_encode_motion(s, motion_x - s->last_mv[0][0][0], s->f_code); 
                        mpeg1_encode_motion(s, motion_y - s->last_mv[0][0][1], s->f_code); 
                        s->f_count++;
                        }
                    s->qscale -= s->dquant;
                    s->mv_bits += get_bits_diff(s);
                    }
            // End of bloc from RAL
            }
        for(i=0;i<6;i++) {
            if (cbp & (1 << (5 - i))) {
                mpeg1_encode_block(s, block[i], i);
            }
        }
        s->mb_skip_run = 0;
        if(s->mb_intra)
            s->i_tex_bits+= get_bits_diff(s);
        else
            s->p_tex_bits+= get_bits_diff(s);
    }

    // RAL: By this:
    if (s->mv_dir & MV_DIR_FORWARD)
        {
        s->last_mv[0][0][0]= s->mv[0][0][0];
        s->last_mv[0][0][1]= s->mv[0][0][1];
        }
    if (s->mv_dir & MV_DIR_BACKWARD)
        {
        s->last_mv[1][0][0]= s->mv[1][0][0];
        s->last_mv[1][0][1]= s->mv[1][0][1];
        }
}

// RAL: Parameter added: f_or_b_code
static void mpeg1_encode_motion(MpegEncContext *s, int val, int f_or_b_code)
{
    int code, bit_size, l, m, bits, range, sign;

    if (val == 0) {
        /* zero vector */
        code = 0;
        put_bits(&s->pb,
                 mbMotionVectorTable[0][1], 
                 mbMotionVectorTable[0][0]); 
    } else {
        bit_size = f_or_b_code - 1;
        range = 1 << bit_size;
        /* modulo encoding */
        l = 16 * range;
        m = 2 * l;
        if (val < -l) {
            val += m;
        } else if (val >= l) {
            val -= m;
        }

        if (val >= 0) {
            val--;
            code = (val >> bit_size) + 1;
            bits = val & (range - 1);
            sign = 0;
        } else {
            val = -val;
            val--;
            code = (val >> bit_size) + 1;
            bits = val & (range - 1);
            sign = 1;
        }

        assert(code > 0 && code <= 16);

        put_bits(&s->pb,
                 mbMotionVectorTable[code][1], 
                 mbMotionVectorTable[code][0]); 

        put_bits(&s->pb, 1, sign);
        if (bit_size > 0) {
            put_bits(&s->pb, bit_size, bits);
        }
    }
}

void ff_mpeg1_encode_init(MpegEncContext *s)
{
    static int done=0;

    common_init(s);

    if(!done){
        int f_code;
        int mv;
	int i;

        done=1;
        init_rl(&rl_mpeg1);

	for(i=0; i<64; i++)
	{
		mpeg1_max_level[0][i]= rl_mpeg1.max_level[0][i];
		mpeg1_index_run[0][i]= rl_mpeg1.index_run[0][i];
	}
        
        init_uni_ac_vlc(&rl_mpeg1, uni_mpeg1_ac_vlc_bits, uni_mpeg1_ac_vlc_len);

	/* build unified dc encoding tables */
	for(i=-255; i<256; i++)
	{
		int adiff, index;
		int bits, code;
		int diff=i;

		adiff = ABS(diff);
		if(diff<0) diff--;
		index = vlc_dc_table[adiff];

		bits= vlc_dc_lum_bits[index] + index;
		code= (vlc_dc_lum_code[index]<<index) + (diff & ((1 << index) - 1));
		mpeg1_lum_dc_uni[i+255]= bits + (code<<8);
		
		bits= vlc_dc_chroma_bits[index] + index;
		code= (vlc_dc_chroma_code[index]<<index) + (diff & ((1 << index) - 1));
		mpeg1_chr_dc_uni[i+255]= bits + (code<<8);
	}

        mv_penalty= av_mallocz( sizeof(uint8_t)*(MAX_FCODE+1)*(2*MAX_MV+1) );

        for(f_code=1; f_code<=MAX_FCODE; f_code++){
            for(mv=-MAX_MV; mv<=MAX_MV; mv++){
                int len;

                if(mv==0) len= mbMotionVectorTable[0][1];
                else{
                    int val, bit_size, range, code;

                    bit_size = s->f_code - 1;
                    range = 1 << bit_size;

                    val=mv;
                    if (val < 0) 
                        val = -val;
                    val--;
                    code = (val >> bit_size) + 1;
                    if(code<17){
                        len= mbMotionVectorTable[code][1] + 1 + bit_size;
                    }else{
                        len= mbMotionVectorTable[16][1] + 2 + bit_size;
                    }
                }

                mv_penalty[f_code][mv+MAX_MV]= len;
            }
        }
        

        for(f_code=MAX_FCODE; f_code>0; f_code--){
            for(mv=-(8<<f_code); mv<(8<<f_code); mv++){
                fcode_tab[mv+MAX_MV]= f_code;
            }
        }
    }
    s->me.mv_penalty= mv_penalty;
    s->fcode_tab= fcode_tab;
    if(s->codec_id == CODEC_ID_MPEG1VIDEO){
        s->min_qcoeff=-255;
        s->max_qcoeff= 255;
    }else{
        s->min_qcoeff=-2047;
        s->max_qcoeff= 2047;
    }
    s->intra_ac_vlc_length=
    s->inter_ac_vlc_length=
    s->intra_ac_vlc_last_length=
    s->inter_ac_vlc_last_length= uni_mpeg1_ac_vlc_len;
}

static inline void encode_dc(MpegEncContext *s, int diff, int component)
{
    if (component == 0) {
        put_bits(
	    &s->pb, 
	    mpeg1_lum_dc_uni[diff+255]&0xFF,
	    mpeg1_lum_dc_uni[diff+255]>>8);
    } else {
        put_bits(
            &s->pb, 
	    mpeg1_chr_dc_uni[diff+255]&0xFF,
	    mpeg1_chr_dc_uni[diff+255]>>8);
    }
}

static void mpeg1_encode_block(MpegEncContext *s, 
                               DCTELEM *block, 
                               int n)
{
    int alevel, level, last_non_zero, dc, diff, i, j, run, last_index, sign;
    int code, component;
//    RLTable *rl = &rl_mpeg1;

    last_index = s->block_last_index[n];

    /* DC coef */
    if (s->mb_intra) {
        component = (n <= 3 ? 0 : n - 4 + 1);
        dc = block[0]; /* overflow is impossible */
        diff = dc - s->last_dc[component];
        encode_dc(s, diff, component);
        s->last_dc[component] = dc;
        i = 1;
/*
        if (s->intra_vlc_format)
            rl = &rl_mpeg2;
        else
            rl = &rl_mpeg1;
*/
    } else {
        /* encode the first coefficient : needs to be done here because
           it is handled slightly differently */
        level = block[0];
        if (abs(level) == 1) {
                code = ((uint32_t)level >> 31); /* the sign bit */
                put_bits(&s->pb, 2, code | 0x02);
                i = 1;
        } else {
            i = 0;
            last_non_zero = -1;
            goto next_coef;
        }
    }

    /* now quantify & encode AC coefs */
    last_non_zero = i - 1;

    for(;i<=last_index;i++) {
        j = s->intra_scantable.permutated[i];
        level = block[j];
    next_coef:
#if 0
        if (level != 0)
            dprintf("level[%d]=%d\n", i, level);
#endif            
        /* encode using VLC */
        if (level != 0) {
            run = i - last_non_zero - 1;
            
            alevel= level;
            MASK_ABS(sign, alevel)
            sign&=1;

//            code = get_rl_index(rl, 0, run, alevel);
            if (alevel <= mpeg1_max_level[0][run]){
                code= mpeg1_index_run[0][run] + alevel - 1;
	    	/* store the vlc & sign at once */
                put_bits(&s->pb, mpeg1_vlc[code][1]+1, (mpeg1_vlc[code][0]<<1) + sign);
            } else {
		/* escape seems to be pretty rare <5% so i dont optimize it */
                put_bits(&s->pb, mpeg1_vlc[111/*rl->n*/][1], mpeg1_vlc[111/*rl->n*/][0]);
                /* escape: only clip in this case */
                put_bits(&s->pb, 6, run);
                if(s->codec_id == CODEC_ID_MPEG1VIDEO){
                    if (alevel < 128) {
                        put_bits(&s->pb, 8, level & 0xff);
                    } else {
                        if (level < 0) {
                            put_bits(&s->pb, 16, 0x8001 + level + 255);
                        } else {
                            put_bits(&s->pb, 16, level & 0xffff);
                        }
                    }
                }else{
                    put_bits(&s->pb, 12, level & 0xfff);
                }
            }
            last_non_zero = i;
        }
    }
    /* end of block */
    put_bits(&s->pb, 2, 0x2);
}
#endif //CONFIG_ENCODERS

/******************************************/
/* decoding */

static VLC dc_lum_vlc;
static VLC dc_chroma_vlc;
static VLC mv_vlc;
static VLC mbincr_vlc;
static VLC mb_ptype_vlc;
static VLC mb_btype_vlc;
static VLC mb_pat_vlc;

static void init_vlcs()
{
    static int done = 0;

    if (!done) {
        done = 1;

        init_vlc(&dc_lum_vlc, DC_VLC_BITS, 12, 
                 vlc_dc_lum_bits, 1, 1,
                 vlc_dc_lum_code, 2, 2);
        init_vlc(&dc_chroma_vlc,  DC_VLC_BITS, 12, 
                 vlc_dc_chroma_bits, 1, 1,
                 vlc_dc_chroma_code, 2, 2);
        init_vlc(&mv_vlc, MV_VLC_BITS, 17, 
                 &mbMotionVectorTable[0][1], 2, 1,
                 &mbMotionVectorTable[0][0], 2, 1);
        init_vlc(&mbincr_vlc, MBINCR_VLC_BITS, 36, 
                 &mbAddrIncrTable[0][1], 2, 1,
                 &mbAddrIncrTable[0][0], 2, 1);
        init_vlc(&mb_pat_vlc, MB_PAT_VLC_BITS, 63, 
                 &mbPatTable[0][1], 2, 1,
                 &mbPatTable[0][0], 2, 1);
        
        init_vlc(&mb_ptype_vlc, MB_PTYPE_VLC_BITS, 7, 
                 &table_mb_ptype[0][1], 2, 1,
                 &table_mb_ptype[0][0], 2, 1);
        init_vlc(&mb_btype_vlc, MB_BTYPE_VLC_BITS, 11, 
                 &table_mb_btype[0][1], 2, 1,
                 &table_mb_btype[0][0], 2, 1);
        init_rl(&rl_mpeg1);
        init_rl(&rl_mpeg2);

        init_2d_vlc_rl(&rl_mpeg1);
        init_2d_vlc_rl(&rl_mpeg2);
    }
}

static inline int get_dmv(MpegEncContext *s)
{
    if(get_bits1(&s->gb)) 
        return 1 - (get_bits1(&s->gb) << 1);
    else
        return 0;
}

static inline int get_qscale(MpegEncContext *s)
{
    int qscale = get_bits(&s->gb, 5);
    if (s->codec_id == CODEC_ID_MPEG2VIDEO) {
        if (s->q_scale_type) {
            return non_linear_qscale[qscale];
        } else {
            return qscale << 1;
        }
    }
    return qscale;
}

/* motion type (for mpeg2) */
#define MT_FIELD 1
#define MT_FRAME 2
#define MT_16X8  2
#define MT_DMV   3

static int mpeg_decode_mb(MpegEncContext *s,
                          DCTELEM block[6][64])
{
    int i, j, k, cbp, val, mb_type, motion_type;
    
    dprintf("decode_mb: x=%d y=%d\n", s->mb_x, s->mb_y);

    assert(s->mb_skiped==0);

    if (s->mb_skip_run-- != 0) {
        if(s->pict_type == I_TYPE){
            av_log(s->avctx, AV_LOG_ERROR, "skiped MB in I frame at %d %d\n", s->mb_x, s->mb_y);
            return -1;
        }
    
        /* skip mb */
        s->mb_intra = 0;
        for(i=0;i<6;i++)
            s->block_last_index[i] = -1;
        s->mv_type = MV_TYPE_16X16;
        if (s->pict_type == P_TYPE) {
            /* if P type, zero motion vector is implied */
            s->mv_dir = MV_DIR_FORWARD;
            s->mv[0][0][0] = s->mv[0][0][1] = 0;
            s->last_mv[0][0][0] = s->last_mv[0][0][1] = 0;
            s->last_mv[0][1][0] = s->last_mv[0][1][1] = 0;
            s->mb_skiped = 1;
            s->current_picture.mb_type[ s->mb_x + s->mb_y*s->mb_stride ]= MB_TYPE_SKIP | MB_TYPE_L0 | MB_TYPE_16x16;
        } else {
            /* if B type, reuse previous vectors and directions */
            s->mv[0][0][0] = s->last_mv[0][0][0];
            s->mv[0][0][1] = s->last_mv[0][0][1];
            s->mv[1][0][0] = s->last_mv[1][0][0];
            s->mv[1][0][1] = s->last_mv[1][0][1];

            s->current_picture.mb_type[ s->mb_x + s->mb_y*s->mb_stride ]= 
                s->current_picture.mb_type[ s->mb_x + s->mb_y*s->mb_stride - 1] | MB_TYPE_SKIP;
//            assert(s->current_picture.mb_type[ s->mb_x + s->mb_y*s->mb_stride - 1]&(MB_TYPE_16x16|MB_TYPE_16x8));

            if((s->mv[0][0][0]|s->mv[0][0][1]|s->mv[1][0][0]|s->mv[1][0][1])==0) 
                s->mb_skiped = 1;
        }

        return 0;
    }

    switch(s->pict_type) {
    default:
    case I_TYPE:
        if (get_bits1(&s->gb) == 0) {
            if (get_bits1(&s->gb) == 0){
                av_log(s->avctx, AV_LOG_ERROR, "invalid mb type in I Frame at %d %d\n", s->mb_x, s->mb_y);
                return -1;
            }
            mb_type = MB_TYPE_QUANT | MB_TYPE_INTRA;
        } else {
            mb_type = MB_TYPE_INTRA;
        }
        break;
    case P_TYPE:
        mb_type = get_vlc2(&s->gb, mb_ptype_vlc.table, MB_PTYPE_VLC_BITS, 1);
        if (mb_type < 0){
            av_log(s->avctx, AV_LOG_ERROR, "invalid mb type in P Frame at %d %d\n", s->mb_x, s->mb_y);
            return -1;
        }
        mb_type = ptype2mb_type[ mb_type ];
        break;
    case B_TYPE:
        mb_type = get_vlc2(&s->gb, mb_btype_vlc.table, MB_BTYPE_VLC_BITS, 1);
        if (mb_type < 0){
            av_log(s->avctx, AV_LOG_ERROR, "invalid mb type in B Frame at %d %d\n", s->mb_x, s->mb_y);
            return -1;
        }
        mb_type = btype2mb_type[ mb_type ];
        break;
    }
    dprintf("mb_type=%x\n", mb_type);
//    motion_type = 0; /* avoid warning */
    if (IS_INTRA(mb_type)) {
        /* compute dct type */
        if (s->picture_structure == PICT_FRAME && //FIXME add a interlaced_dct coded var?
            !s->frame_pred_frame_dct) {
            s->interlaced_dct = get_bits1(&s->gb);
        }

        if (IS_QUANT(mb_type))
            s->qscale = get_qscale(s);
        
        if (s->concealment_motion_vectors) {
            /* just parse them */
            if (s->picture_structure != PICT_FRAME) 
                skip_bits1(&s->gb); /* field select */
            
            s->mv[0][0][0]= s->last_mv[0][0][0]= s->last_mv[0][1][0] = 
                mpeg_decode_motion(s, s->mpeg_f_code[0][0], s->last_mv[0][0][0]);
            s->mv[0][0][1]= s->last_mv[0][0][1]= s->last_mv[0][1][1] = 
                mpeg_decode_motion(s, s->mpeg_f_code[0][1], s->last_mv[0][0][1]);

            skip_bits1(&s->gb); /* marker */
        }else
            memset(s->last_mv, 0, sizeof(s->last_mv)); /* reset mv prediction */
        s->mb_intra = 1;
#ifdef HAVE_XVMC
        //one 1 we memcpy blocks in xvmcvideo
        if(s->avctx->xvmc_acceleration > 1){
            XVMC_pack_pblocks(s,-1);//inter are always full blocks
            if(s->swap_uv){
                exchange_uv(s);
            }
        }
#endif

        if (s->codec_id == CODEC_ID_MPEG2VIDEO) {
            for(i=0;i<6;i++) {
                if (mpeg2_decode_block_intra(s, s->pblocks[i], i) < 0)
                    return -1;
            }
        } else {
            for(i=0;i<6;i++) {
                if (mpeg1_decode_block_intra(s, s->pblocks[i], i) < 0)
                    return -1;
            }
        }
    } else {
        if (mb_type & MB_TYPE_ZERO_MV){
            assert(mb_type & MB_TYPE_CBP);

            /* compute dct type */
            if (s->picture_structure == PICT_FRAME && //FIXME add a interlaced_dct coded var?
                !s->frame_pred_frame_dct) {
                s->interlaced_dct = get_bits1(&s->gb);
            }

            if (IS_QUANT(mb_type))
                s->qscale = get_qscale(s);

            s->mv_dir = MV_DIR_FORWARD;
            s->mv_type = MV_TYPE_16X16;
            s->last_mv[0][0][0] = 0;
            s->last_mv[0][0][1] = 0;
            s->last_mv[0][1][0] = 0;
            s->last_mv[0][1][1] = 0;
            s->mv[0][0][0] = 0;
            s->mv[0][0][1] = 0;
        }else{
            assert(mb_type & MB_TYPE_L0L1);
//FIXME decide if MBs in field pictures are MB_TYPE_INTERLACED
            /* get additionnal motion vector type */
            if (s->frame_pred_frame_dct) 
                motion_type = MT_FRAME;
            else{
                motion_type = get_bits(&s->gb, 2);
            }

            /* compute dct type */
            if (s->picture_structure == PICT_FRAME && //FIXME add a interlaced_dct coded var?
                !s->frame_pred_frame_dct && HAS_CBP(mb_type)) {
                s->interlaced_dct = get_bits1(&s->gb);
            }

            if (IS_QUANT(mb_type))
                s->qscale = get_qscale(s);

            /* motion vectors */
            s->mv_dir = 0;
            for(i=0;i<2;i++) {
                if (USES_LIST(mb_type, i)) {
                    s->mv_dir |= (MV_DIR_FORWARD >> i);
                    dprintf("motion_type=%d\n", motion_type);
                    switch(motion_type) {
                    case MT_FRAME: /* or MT_16X8 */
                        if (s->picture_structure == PICT_FRAME) {
                            /* MT_FRAME */
                            mb_type |= MB_TYPE_16x16; 
                            s->mv_type = MV_TYPE_16X16;
                            s->mv[i][0][0]= s->last_mv[i][0][0]= s->last_mv[i][1][0] = 
                                mpeg_decode_motion(s, s->mpeg_f_code[i][0], s->last_mv[i][0][0]);
                            s->mv[i][0][1]= s->last_mv[i][0][1]= s->last_mv[i][1][1] = 
                                mpeg_decode_motion(s, s->mpeg_f_code[i][1], s->last_mv[i][0][1]);
                            /* full_pel: only for mpeg1 */
                            if (s->full_pel[i]){
                                s->mv[i][0][0] <<= 1;
                                s->mv[i][0][1] <<= 1;
                            }
                        } else {
                            /* MT_16X8 */
                            mb_type |= MB_TYPE_16x8; 
                            s->mv_type = MV_TYPE_16X8;
                            for(j=0;j<2;j++) {
                                s->field_select[i][j] = get_bits1(&s->gb);
                                for(k=0;k<2;k++) {
                                    val = mpeg_decode_motion(s, s->mpeg_f_code[i][k],
                                                             s->last_mv[i][j][k]);
                                    s->last_mv[i][j][k] = val;
                                    s->mv[i][j][k] = val;
                                }
                            }
                        }
                        break;
                    case MT_FIELD:
                        s->mv_type = MV_TYPE_FIELD;
                        if (s->picture_structure == PICT_FRAME) {
                            mb_type |= MB_TYPE_16x8 | MB_TYPE_INTERLACED; 
                            for(j=0;j<2;j++) {
                                s->field_select[i][j] = get_bits1(&s->gb);
                                val = mpeg_decode_motion(s, s->mpeg_f_code[i][0],
                                                         s->last_mv[i][j][0]);
                                s->last_mv[i][j][0] = val;
                                s->mv[i][j][0] = val;
                                dprintf("fmx=%d\n", val);
                                val = mpeg_decode_motion(s, s->mpeg_f_code[i][1],
                                                         s->last_mv[i][j][1] >> 1);
                                s->last_mv[i][j][1] = val << 1;
                                s->mv[i][j][1] = val;
                                dprintf("fmy=%d\n", val);
                            }
                        } else {
                            mb_type |= MB_TYPE_16x16; 
                            s->field_select[i][0] = get_bits1(&s->gb);
                            for(k=0;k<2;k++) {
                                val = mpeg_decode_motion(s, s->mpeg_f_code[i][k],
                                                         s->last_mv[i][0][k]);
                                s->last_mv[i][0][k] = val;
                                s->last_mv[i][1][k] = val;
                                s->mv[i][0][k] = val;
                            }
                        }
                        break;
                    case MT_DMV:
                        {
                            int dmx, dmy, mx, my, m;

                            mx = mpeg_decode_motion(s, s->mpeg_f_code[i][0], 
                                                    s->last_mv[i][0][0]);
                            s->last_mv[i][0][0] = mx;
                            s->last_mv[i][1][0] = mx;
                            dmx = get_dmv(s);
                            my = mpeg_decode_motion(s, s->mpeg_f_code[i][1], 
                                                    s->last_mv[i][0][1] >> 1);
                            dmy = get_dmv(s);
                            s->mv_type = MV_TYPE_DMV;


                            s->last_mv[i][0][1] = my<<1;
                            s->last_mv[i][1][1] = my<<1;

                            s->mv[i][0][0] = mx;
                            s->mv[i][0][1] = my;
                            s->mv[i][1][0] = mx;//not used
                            s->mv[i][1][1] = my;//not used

                            if (s->picture_structure == PICT_FRAME) {
                                mb_type |= MB_TYPE_16x16 | MB_TYPE_INTERLACED; 

                                //m = 1 + 2 * s->top_field_first;
                                m = s->top_field_first ? 1 : 3;

                                /* top -> top pred */
                                s->mv[i][2][0] = ((mx * m + (mx > 0)) >> 1) + dmx;
                                s->mv[i][2][1] = ((my * m + (my > 0)) >> 1) + dmy - 1;
                                m = 4 - m;
                                s->mv[i][3][0] = ((mx * m + (mx > 0)) >> 1) + dmx;
                                s->mv[i][3][1] = ((my * m + (my > 0)) >> 1) + dmy + 1;
                            } else {
                                mb_type |= MB_TYPE_16x16;

                                s->mv[i][2][0] = ((mx + (mx > 0)) >> 1) + dmx;
                                s->mv[i][2][1] = ((my + (my > 0)) >> 1) + dmy;
                                if(s->picture_structure == PICT_TOP_FIELD)
                                    s->mv[i][2][1]--;
                                else 
                                    s->mv[i][2][1]++;
                            }
                        }
                        break;
                    }
                }
            }
        }
        
        s->mb_intra = 0;

        if (HAS_CBP(mb_type)) {
            cbp = get_vlc2(&s->gb, mb_pat_vlc.table, MB_PAT_VLC_BITS, 1);
            if (cbp < 0){
                av_log(s->avctx, AV_LOG_ERROR, "invalid cbp at %d %d\n", s->mb_x, s->mb_y);
                return -1;
            }
            cbp++;

#ifdef HAVE_XVMC
            //on 1 we memcpy blocks in xvmcvideo
            if(s->avctx->xvmc_acceleration > 1){
                XVMC_pack_pblocks(s,cbp);
                if(s->swap_uv){
                    exchange_uv(s);
                }
            }    
#endif

            if (s->codec_id == CODEC_ID_MPEG2VIDEO) {
                for(i=0;i<6;i++) {
                    if (cbp & 32) {
                        if (mpeg2_decode_block_non_intra(s, s->pblocks[i], i) < 0)
                            return -1;
                    } else {
                        s->block_last_index[i] = -1;
                    }
                    cbp+=cbp;
                }
            } else {
                for(i=0;i<6;i++) {
                    if (cbp & 32) {
                        if (mpeg1_decode_block_inter(s, s->pblocks[i], i) < 0)
                            return -1;
                    } else {
                        s->block_last_index[i] = -1;
                    }
                    cbp+=cbp;
                }
            }
        }else{
            for(i=0;i<6;i++)
                s->block_last_index[i] = -1;
        }
    }

    s->current_picture.mb_type[ s->mb_x + s->mb_y*s->mb_stride ]= mb_type;

    return 0;
}

/* as h263, but only 17 codes */
static int mpeg_decode_motion(MpegEncContext *s, int fcode, int pred)
{
    int code, sign, val, l, shift;

    code = get_vlc2(&s->gb, mv_vlc.table, MV_VLC_BITS, 2);
    if (code == 0) {
        return pred;
    }
    if (code < 0) {
        return 0xffff;
    }

    sign = get_bits1(&s->gb);
    shift = fcode - 1;
    val = code;
    if (shift) {
        val = (val - 1) << shift;
        val |= get_bits(&s->gb, shift);
        val++;
    }
    if (sign)
        val = -val;
    val += pred;
    
    /* modulo decoding */
    l = 1 << (shift+4);
    val = ((val + l)&(l*2-1)) - l;
    return val;
}

static inline int decode_dc(GetBitContext *gb, int component)
{
    int code, diff;

    if (component == 0) {
        code = get_vlc2(gb, dc_lum_vlc.table, DC_VLC_BITS, 2);
    } else {
        code = get_vlc2(gb, dc_chroma_vlc.table, DC_VLC_BITS, 2);
    }
    if (code < 0){
        av_log(NULL, AV_LOG_ERROR, "invalid dc code at\n");
        return 0xffff;
    }
    if (code == 0) {
        diff = 0;
    } else {
        diff = get_xbits(gb, code);
    }
    return diff;
}

static inline int mpeg1_decode_block_intra(MpegEncContext *s, 
                               DCTELEM *block, 
                               int n)
{
    int level, dc, diff, i, j, run;
    int component;
    RLTable *rl = &rl_mpeg1;
    uint8_t * const scantable= s->intra_scantable.permutated;
    const uint16_t *quant_matrix= s->intra_matrix;
    const int qscale= s->qscale;

    /* DC coef */
    component = (n <= 3 ? 0 : n - 4 + 1);
    diff = decode_dc(&s->gb, component);
    if (diff >= 0xffff)
        return -1;
    dc = s->last_dc[component];
    dc += diff;
    s->last_dc[component] = dc;
    block[0] = dc<<3;
    dprintf("dc=%d diff=%d\n", dc, diff);
    i = 0;
    {
        OPEN_READER(re, &s->gb);    
        /* now quantify & encode AC coefs */
        for(;;) {
            UPDATE_CACHE(re, &s->gb);
            GET_RL_VLC(level, run, re, &s->gb, rl->rl_vlc[0], TEX_VLC_BITS, 2);
            
            if(level == 127){
                break;
            } else if(level != 0) {
                i += run;
                j = scantable[i];
                level= (level*qscale*quant_matrix[j])>>3;
                level= (level-1)|1;
                level = (level ^ SHOW_SBITS(re, &s->gb, 1)) - SHOW_SBITS(re, &s->gb, 1);
                LAST_SKIP_BITS(re, &s->gb, 1);
            } else {
                /* escape */
                run = SHOW_UBITS(re, &s->gb, 6)+1; LAST_SKIP_BITS(re, &s->gb, 6);
                UPDATE_CACHE(re, &s->gb);
                level = SHOW_SBITS(re, &s->gb, 8); SKIP_BITS(re, &s->gb, 8);
                if (level == -128) {
                    level = SHOW_UBITS(re, &s->gb, 8) - 256; LAST_SKIP_BITS(re, &s->gb, 8);
                } else if (level == 0) {
                    level = SHOW_UBITS(re, &s->gb, 8)      ; LAST_SKIP_BITS(re, &s->gb, 8);
                }
                i += run;
                j = scantable[i];
                if(level<0){
                    level= -level;
                    level= (level*qscale*quant_matrix[j])>>3;
                    level= (level-1)|1;
                    level= -level;
                }else{
                    level= (level*qscale*quant_matrix[j])>>3;
                    level= (level-1)|1;
                }
            }
            if (i > 63){
                av_log(s->avctx, AV_LOG_ERROR, "ac-tex damaged at %d %d\n", s->mb_x, s->mb_y);
                return -1;
            }

            block[j] = level;
        }
        CLOSE_READER(re, &s->gb);
    }
    s->block_last_index[n] = i;
   return 0;
}

static inline int mpeg1_decode_block_inter(MpegEncContext *s, 
                               DCTELEM *block, 
                               int n)
{
    int level, i, j, run;
    RLTable *rl = &rl_mpeg1;
    uint8_t * const scantable= s->intra_scantable.permutated;
    const uint16_t *quant_matrix= s->inter_matrix;
    const int qscale= s->qscale;

    {
        int v;
        OPEN_READER(re, &s->gb);
        i = -1;
        /* special case for the first coef. no need to add a second vlc table */
        UPDATE_CACHE(re, &s->gb);
        v= SHOW_UBITS(re, &s->gb, 2);
        if (v & 2) {
            LAST_SKIP_BITS(re, &s->gb, 2);
            level= (3*qscale*quant_matrix[0])>>4;
            level= (level-1)|1;
            if(v&1)
                level= -level;
            block[0] = level;
            i++;
        }

        /* now quantify & encode AC coefs */
        for(;;) {
            UPDATE_CACHE(re, &s->gb);
            GET_RL_VLC(level, run, re, &s->gb, rl->rl_vlc[0], TEX_VLC_BITS, 2);
            
            if(level == 127){
                break;
            } else if(level != 0) {
                i += run;
                j = scantable[i];
                level= ((level*2+1)*qscale*quant_matrix[j])>>4;
                level= (level-1)|1;
                level = (level ^ SHOW_SBITS(re, &s->gb, 1)) - SHOW_SBITS(re, &s->gb, 1);
                LAST_SKIP_BITS(re, &s->gb, 1);
            } else {
                /* escape */
                run = SHOW_UBITS(re, &s->gb, 6)+1; LAST_SKIP_BITS(re, &s->gb, 6);
                UPDATE_CACHE(re, &s->gb);
                level = SHOW_SBITS(re, &s->gb, 8); SKIP_BITS(re, &s->gb, 8);
                if (level == -128) {
                    level = SHOW_UBITS(re, &s->gb, 8) - 256; LAST_SKIP_BITS(re, &s->gb, 8);
                } else if (level == 0) {
                    level = SHOW_UBITS(re, &s->gb, 8)      ; LAST_SKIP_BITS(re, &s->gb, 8);
                }
                i += run;
                j = scantable[i];
                if(level<0){
                    level= -level;
                    level= ((level*2+1)*qscale*quant_matrix[j])>>4;
                    level= (level-1)|1;
                    level= -level;
                }else{
                    level= ((level*2+1)*qscale*quant_matrix[j])>>4;
                    level= (level-1)|1;
                }
            }
            if (i > 63){
                av_log(s->avctx, AV_LOG_ERROR, "ac-tex damaged at %d %d\n", s->mb_x, s->mb_y);
                return -1;
            }

            block[j] = level;
        }
        CLOSE_READER(re, &s->gb);
    }
    s->block_last_index[n] = i;
    return 0;
}

/* Also does unquantization here, since I will never support mpeg2
   encoding */
static inline int mpeg2_decode_block_non_intra(MpegEncContext *s, 
                               DCTELEM *block, 
                               int n)
{
    int level, i, j, run;
    RLTable *rl = &rl_mpeg1;
    uint8_t * const scantable= s->intra_scantable.permutated;
    const uint16_t *quant_matrix;
    const int qscale= s->qscale;
    int mismatch;

    mismatch = 1;

    {
        int v;
        OPEN_READER(re, &s->gb);
        i = -1;
        if (n < 4)
            quant_matrix = s->inter_matrix;
        else
            quant_matrix = s->chroma_inter_matrix;

        /* special case for the first coef. no need to add a second vlc table */
        UPDATE_CACHE(re, &s->gb);
        v= SHOW_UBITS(re, &s->gb, 2);
        if (v & 2) {
            LAST_SKIP_BITS(re, &s->gb, 2);
            level= (3*qscale*quant_matrix[0])>>5;
            if(v&1)
                level= -level;
            block[0] = level;
            mismatch ^= level;
            i++;
        }

        /* now quantify & encode AC coefs */
        for(;;) {
            UPDATE_CACHE(re, &s->gb);
            GET_RL_VLC(level, run, re, &s->gb, rl->rl_vlc[0], TEX_VLC_BITS, 2);
            
            if(level == 127){
                break;
            } else if(level != 0) {
                i += run;
                j = scantable[i];
                level= ((level*2+1)*qscale*quant_matrix[j])>>5;
                level = (level ^ SHOW_SBITS(re, &s->gb, 1)) - SHOW_SBITS(re, &s->gb, 1);
                LAST_SKIP_BITS(re, &s->gb, 1);
            } else {
                /* escape */
                run = SHOW_UBITS(re, &s->gb, 6)+1; LAST_SKIP_BITS(re, &s->gb, 6);
                UPDATE_CACHE(re, &s->gb);
                level = SHOW_SBITS(re, &s->gb, 12); SKIP_BITS(re, &s->gb, 12);

                i += run;
                j = scantable[i];
                if(level<0){
                    level= ((-level*2+1)*qscale*quant_matrix[j])>>5;
                    level= -level;
                }else{
                    level= ((level*2+1)*qscale*quant_matrix[j])>>5;
                }
            }
            if (i > 63){
                av_log(s->avctx, AV_LOG_ERROR, "ac-tex damaged at %d %d\n", s->mb_x, s->mb_y);
                return -1;
            }
            
            mismatch ^= level;
            block[j] = level;
        }
        CLOSE_READER(re, &s->gb);
    }
    block[63] ^= (mismatch & 1);
    
    s->block_last_index[n] = i;
    return 0;
}

static inline int mpeg2_decode_block_intra(MpegEncContext *s, 
                               DCTELEM *block, 
                               int n)
{
    int level, dc, diff, i, j, run;
    int component;
    RLTable *rl;
    uint8_t * const scantable= s->intra_scantable.permutated;
    const uint16_t *quant_matrix;
    const int qscale= s->qscale;
    int mismatch;

    /* DC coef */
    if (n < 4){
        quant_matrix = s->intra_matrix;
        component = 0; 
    }else{
        quant_matrix = s->chroma_intra_matrix;
        component = n - 3;
    }
    diff = decode_dc(&s->gb, component);
    if (diff >= 0xffff)
        return -1;
    dc = s->last_dc[component];
    dc += diff;
    s->last_dc[component] = dc;
    block[0] = dc << (3 - s->intra_dc_precision);
    dprintf("dc=%d\n", block[0]);
    mismatch = block[0] ^ 1;
    i = 0;
    if (s->intra_vlc_format)
        rl = &rl_mpeg2;
    else
        rl = &rl_mpeg1;

    {
        OPEN_READER(re, &s->gb);    
        /* now quantify & encode AC coefs */
        for(;;) {
            UPDATE_CACHE(re, &s->gb);
            GET_RL_VLC(level, run, re, &s->gb, rl->rl_vlc[0], TEX_VLC_BITS, 2);
            
            if(level == 127){
                break;
            } else if(level != 0) {
                i += run;
                j = scantable[i];
                level= (level*qscale*quant_matrix[j])>>4;
                level = (level ^ SHOW_SBITS(re, &s->gb, 1)) - SHOW_SBITS(re, &s->gb, 1);
                LAST_SKIP_BITS(re, &s->gb, 1);
            } else {
                /* escape */
                run = SHOW_UBITS(re, &s->gb, 6)+1; LAST_SKIP_BITS(re, &s->gb, 6);
                UPDATE_CACHE(re, &s->gb);
                level = SHOW_SBITS(re, &s->gb, 12); SKIP_BITS(re, &s->gb, 12);
                i += run;
                j = scantable[i];
                if(level<0){
                    level= (-level*qscale*quant_matrix[j])>>4;
                    level= -level;
                }else{
                    level= (level*qscale*quant_matrix[j])>>4;
                }
            }
            if (i > 63){
                av_log(s->avctx, AV_LOG_ERROR, "ac-tex damaged at %d %d\n", s->mb_x, s->mb_y);
                return -1;
            }
            
            mismatch^= level;
            block[j] = level;
        }
        CLOSE_READER(re, &s->gb);
    }
    block[63]^= mismatch&1;
    
    s->block_last_index[n] = i;
    return 0;
}

typedef struct Mpeg1Context {
    MpegEncContext mpeg_enc_ctx;
    int mpeg_enc_ctx_allocated; /* true if decoding context allocated */
    int repeat_field; /* true if we must repeat the field */
    AVPanScan pan_scan; /** some temporary storage for the panscan */
} Mpeg1Context;

static int mpeg_decode_init(AVCodecContext *avctx)
{
    Mpeg1Context *s = avctx->priv_data;
    
    s->mpeg_enc_ctx.flags= avctx->flags;
    common_init(&s->mpeg_enc_ctx);
    init_vlcs();

    s->mpeg_enc_ctx_allocated = 0;
    s->mpeg_enc_ctx.picture_number = 0;
    s->repeat_field = 0;
    s->mpeg_enc_ctx.codec_id= avctx->codec->id;
    return 0;
}

/* return the 8 bit start code value and update the search
   state. Return -1 if no start code found */
static int find_start_code(uint8_t **pbuf_ptr, uint8_t *buf_end)
{
    uint8_t *buf_ptr;
    unsigned int state=0xFFFFFFFF, v;
    int val;

    buf_ptr = *pbuf_ptr;
    while (buf_ptr < buf_end) {
        v = *buf_ptr++;
        if (state == 0x000001) {
            state = ((state << 8) | v) & 0xffffff;
            val = state;
            goto found;
        }
        state = ((state << 8) | v) & 0xffffff;
    }
    val = -1;
 found:
    *pbuf_ptr = buf_ptr;
    return val;
}

static int mpeg1_decode_picture(AVCodecContext *avctx, 
                                uint8_t *buf, int buf_size)
{
    Mpeg1Context *s1 = avctx->priv_data;
    MpegEncContext *s = &s1->mpeg_enc_ctx;
    int ref, f_code;

    init_get_bits(&s->gb, buf, buf_size*8);

    ref = get_bits(&s->gb, 10); /* temporal ref */
    s->pict_type = get_bits(&s->gb, 3);
    dprintf("pict_type=%d number=%d\n", s->pict_type, s->picture_number);

    skip_bits(&s->gb, 16);
    if (s->pict_type == P_TYPE || s->pict_type == B_TYPE) {
        s->full_pel[0] = get_bits1(&s->gb);
        f_code = get_bits(&s->gb, 3);
        if (f_code == 0)
            return -1;
        s->mpeg_f_code[0][0] = f_code;
        s->mpeg_f_code[0][1] = f_code;
    }
    if (s->pict_type == B_TYPE) {
        s->full_pel[1] = get_bits1(&s->gb);
        f_code = get_bits(&s->gb, 3);
        if (f_code == 0)
            return -1;
        s->mpeg_f_code[1][0] = f_code;
        s->mpeg_f_code[1][1] = f_code;
    }
    s->current_picture.pict_type= s->pict_type;
    s->current_picture.key_frame= s->pict_type == I_TYPE;
    
    s->y_dc_scale = 8;
    s->c_dc_scale = 8;
    s->first_slice = 1;
    return 0;
}

static void mpeg_decode_sequence_extension(MpegEncContext *s)
{
    int horiz_size_ext, vert_size_ext;
    int bit_rate_ext, vbv_buf_ext;
    int frame_rate_ext_n, frame_rate_ext_d;
    int level, profile;

    skip_bits(&s->gb, 1); /* profil and level esc*/
    profile= get_bits(&s->gb, 3);
    level= get_bits(&s->gb, 4);
    s->progressive_sequence = get_bits1(&s->gb); /* progressive_sequence */
    skip_bits(&s->gb, 2); /* chroma_format */
    horiz_size_ext = get_bits(&s->gb, 2);
    vert_size_ext = get_bits(&s->gb, 2);
    s->width |= (horiz_size_ext << 12);
    s->height |= (vert_size_ext << 12);
    bit_rate_ext = get_bits(&s->gb, 12);  /* XXX: handle it */
    s->bit_rate = ((s->bit_rate / 400) | (bit_rate_ext << 12)) * 400;
    skip_bits1(&s->gb); /* marker */
    vbv_buf_ext = get_bits(&s->gb, 8);

    s->low_delay = get_bits1(&s->gb);
    if(s->flags & CODEC_FLAG_LOW_DELAY) s->low_delay=1;

    frame_rate_ext_n = get_bits(&s->gb, 2);
    frame_rate_ext_d = get_bits(&s->gb, 5);
    av_reduce(
        &s->avctx->frame_rate, 
        &s->avctx->frame_rate_base, 
        frame_rate_tab[s->frame_rate_index] * (frame_rate_ext_n+1),
        MPEG1_FRAME_RATE_BASE * (frame_rate_ext_d+1),
        1<<30);

    dprintf("sequence extension\n");
    s->codec_id= s->avctx->codec_id= CODEC_ID_MPEG2VIDEO;
    s->avctx->sub_id = 2; /* indicates mpeg2 found */

    if(s->aspect_ratio_info <= 1)
        s->avctx->sample_aspect_ratio= mpeg2_aspect[s->aspect_ratio_info];
    else{
        s->avctx->sample_aspect_ratio= 
            av_div_q(
                mpeg2_aspect[s->aspect_ratio_info], 
                (AVRational){s->width, s->height}
            );
    }
    
    if(s->avctx->debug & FF_DEBUG_PICT_INFO)
        av_log(s->avctx, AV_LOG_DEBUG, "profile: %d, level: %d \n", profile, level);
}

static void mpeg_decode_sequence_display_extension(Mpeg1Context *s1)
{
    MpegEncContext *s= &s1->mpeg_enc_ctx;
    int color_description, w, h;

    skip_bits(&s->gb, 3); /* video format */
    color_description= get_bits1(&s->gb);
    if(color_description){
        skip_bits(&s->gb, 8); /* color primaries */
        skip_bits(&s->gb, 8); /* transfer_characteristics */
        skip_bits(&s->gb, 8); /* matrix_coefficients */
    }
    w= get_bits(&s->gb, 14);
    skip_bits(&s->gb, 1); //marker
    h= get_bits(&s->gb, 14);
    skip_bits(&s->gb, 1); //marker
    
    s1->pan_scan.width= 16*w;
    s1->pan_scan.height=16*h;

    if(s->aspect_ratio_info > 1)
        s->avctx->sample_aspect_ratio= 
            av_div_q(
                mpeg2_aspect[s->aspect_ratio_info], 
                (AVRational){w, h}
            );
    
    if(s->avctx->debug & FF_DEBUG_PICT_INFO)
        av_log(s->avctx, AV_LOG_DEBUG, "sde w:%d, h:%d\n", w, h);
}

static void mpeg_decode_picture_display_extension(Mpeg1Context *s1)
{
    MpegEncContext *s= &s1->mpeg_enc_ctx;
    int i;

    for(i=0; i<1; i++){ //FIXME count
        s1->pan_scan.position[i][0]= get_sbits(&s->gb, 16);
        skip_bits(&s->gb, 1); //marker
        s1->pan_scan.position[i][1]= get_sbits(&s->gb, 16);
        skip_bits(&s->gb, 1); //marker
    }
   
    if(s->avctx->debug & FF_DEBUG_PICT_INFO)
        av_log(s->avctx, AV_LOG_DEBUG, "pde (%d,%d) (%d,%d) (%d,%d)\n", 
            s1->pan_scan.position[0][0], s1->pan_scan.position[0][1], 
            s1->pan_scan.position[1][0], s1->pan_scan.position[1][1], 
            s1->pan_scan.position[2][0], s1->pan_scan.position[2][1]
        );
}

static void mpeg_decode_quant_matrix_extension(MpegEncContext *s)
{
    int i, v, j;

    dprintf("matrix extension\n");

    if (get_bits1(&s->gb)) {
        for(i=0;i<64;i++) {
            v = get_bits(&s->gb, 8);
            j= s->dsp.idct_permutation[ ff_zigzag_direct[i] ];
            s->intra_matrix[j] = v;
            s->chroma_intra_matrix[j] = v;
        }
    }
    if (get_bits1(&s->gb)) {
        for(i=0;i<64;i++) {
            v = get_bits(&s->gb, 8);
            j= s->dsp.idct_permutation[ ff_zigzag_direct[i] ];
            s->inter_matrix[j] = v;
            s->chroma_inter_matrix[j] = v;
        }
    }
    if (get_bits1(&s->gb)) {
        for(i=0;i<64;i++) {
            v = get_bits(&s->gb, 8);
            j= s->dsp.idct_permutation[ ff_zigzag_direct[i] ];
            s->chroma_intra_matrix[j] = v;
        }
    }
    if (get_bits1(&s->gb)) {
        for(i=0;i<64;i++) {
            v = get_bits(&s->gb, 8);
            j= s->dsp.idct_permutation[ ff_zigzag_direct[i] ];
            s->chroma_inter_matrix[j] = v;
        }
    }
}

static void mpeg_decode_picture_coding_extension(MpegEncContext *s)
{
    s->full_pel[0] = s->full_pel[1] = 0;
    s->mpeg_f_code[0][0] = get_bits(&s->gb, 4);
    s->mpeg_f_code[0][1] = get_bits(&s->gb, 4);
    s->mpeg_f_code[1][0] = get_bits(&s->gb, 4);
    s->mpeg_f_code[1][1] = get_bits(&s->gb, 4);
    s->intra_dc_precision = get_bits(&s->gb, 2);
    s->picture_structure = get_bits(&s->gb, 2);
    s->top_field_first = get_bits1(&s->gb);
    s->frame_pred_frame_dct = get_bits1(&s->gb);
    s->concealment_motion_vectors = get_bits1(&s->gb);
    s->q_scale_type = get_bits1(&s->gb);
    s->intra_vlc_format = get_bits1(&s->gb);
    s->alternate_scan = get_bits1(&s->gb);
    s->repeat_first_field = get_bits1(&s->gb);
    s->chroma_420_type = get_bits1(&s->gb);
    s->progressive_frame = get_bits1(&s->gb);
    
    if(s->picture_structure == PICT_FRAME)
        s->first_field=0;
    else{
        s->first_field ^= 1;
        memset(s->mbskip_table, 0, s->mb_stride*s->mb_height);
    }
    
    if(s->alternate_scan){
        ff_init_scantable(s->dsp.idct_permutation, &s->inter_scantable  , ff_alternate_vertical_scan);
        ff_init_scantable(s->dsp.idct_permutation, &s->intra_scantable  , ff_alternate_vertical_scan);
        ff_init_scantable(s->dsp.idct_permutation, &s->intra_h_scantable, ff_alternate_vertical_scan);
        ff_init_scantable(s->dsp.idct_permutation, &s->intra_v_scantable, ff_alternate_vertical_scan);
    }else{
        ff_init_scantable(s->dsp.idct_permutation, &s->inter_scantable  , ff_zigzag_direct);
        ff_init_scantable(s->dsp.idct_permutation, &s->intra_scantable  , ff_zigzag_direct);
        ff_init_scantable(s->dsp.idct_permutation, &s->intra_h_scantable, ff_alternate_horizontal_scan);
        ff_init_scantable(s->dsp.idct_permutation, &s->intra_v_scantable, ff_alternate_vertical_scan);
    }
    
    /* composite display not parsed */
    dprintf("intra_dc_precision=%d\n", s->intra_dc_precision);
    dprintf("picture_structure=%d\n", s->picture_structure);
    dprintf("top field first=%d\n", s->top_field_first);
    dprintf("repeat first field=%d\n", s->repeat_first_field);
    dprintf("conceal=%d\n", s->concealment_motion_vectors);
    dprintf("intra_vlc_format=%d\n", s->intra_vlc_format);
    dprintf("alternate_scan=%d\n", s->alternate_scan);
    dprintf("frame_pred_frame_dct=%d\n", s->frame_pred_frame_dct);
    dprintf("progressive_frame=%d\n", s->progressive_frame);
}

static void mpeg_decode_extension(AVCodecContext *avctx, 
                                  uint8_t *buf, int buf_size)
{
    Mpeg1Context *s1 = avctx->priv_data;
    MpegEncContext *s = &s1->mpeg_enc_ctx;
    int ext_type;

    init_get_bits(&s->gb, buf, buf_size*8);
    
    ext_type = get_bits(&s->gb, 4);
    switch(ext_type) {
    case 0x1:
        mpeg_decode_sequence_extension(s);
        break;
    case 0x2:
        mpeg_decode_sequence_display_extension(s1);
        break;
    case 0x3:
        mpeg_decode_quant_matrix_extension(s);
        break;
    case 0x7:
        mpeg_decode_picture_display_extension(s1);
        break;
    case 0x8:
        mpeg_decode_picture_coding_extension(s);
        break;
    }
}

static void exchange_uv(MpegEncContext *s){
short * tmp;

    tmp = s->pblocks[4];
    s->pblocks[4] = s->pblocks[5];
    s->pblocks[5] = tmp;
}

#define DECODE_SLICE_FATAL_ERROR -2
#define DECODE_SLICE_ERROR -1
#define DECODE_SLICE_OK 0

/**
 * decodes a slice.
 * @return DECODE_SLICE_FATAL_ERROR if a non recoverable error occured<br>
 *         DECODE_SLICE_ERROR if the slice is damaged<br>
 *         DECODE_SLICE_OK if this slice is ok<br>
 */
static int mpeg_decode_slice(AVCodecContext *avctx, 
                              AVFrame *pict,
                              int start_code,
                              uint8_t **buf, int buf_size)
{
    Mpeg1Context *s1 = avctx->priv_data;
    MpegEncContext *s = &s1->mpeg_enc_ctx;
    int ret;
    const int field_pic= s->picture_structure != PICT_FRAME;

    s->resync_mb_x= s->mb_x = 
    s->resync_mb_y= s->mb_y = -1;
    
    start_code = (start_code - 1) & 0xff;
    if (start_code >= s->mb_height){
        av_log(s->avctx, AV_LOG_ERROR, "slice below image (%d >= %d)\n", start_code, s->mb_height);
        return -1;
    }
    
    ff_mpeg1_clean_buffers(s);
    s->interlaced_dct = 0;
        
    /* start frame decoding */
    if (s->first_slice) {
      if(s->first_field || s->picture_structure==PICT_FRAME){
        if(MPV_frame_start(s, avctx) < 0)
            return DECODE_SLICE_FATAL_ERROR;

        ff_er_frame_start(s);

        /* first check if we must repeat the frame */
        s->current_picture_ptr->repeat_pict = 0;
        if (s->repeat_first_field) {
            if (s->progressive_sequence) {
                if (s->top_field_first)
                    s->current_picture_ptr->repeat_pict = 4;
                else
                    s->current_picture_ptr->repeat_pict = 2;
            } else if (s->progressive_frame) {
                s->current_picture_ptr->repeat_pict = 1;
            }
        }         

        *s->current_picture_ptr->pan_scan= s1->pan_scan;

        //printf("%d\n", s->current_picture_ptr->repeat_pict);

        if(s->avctx->debug&FF_DEBUG_PICT_INFO){
             av_log(s->avctx, AV_LOG_DEBUG, "qp:%d fc:%2d%2d%2d%2d %s %s %s %s dc:%d pstruct:%d fdct:%d cmv:%d qtype:%d ivlc:%d rff:%d %s\n", 
                 s->qscale, s->mpeg_f_code[0][0],s->mpeg_f_code[0][1],s->mpeg_f_code[1][0],s->mpeg_f_code[1][1],
                 s->pict_type == I_TYPE ? "I" : (s->pict_type == P_TYPE ? "P" : (s->pict_type == B_TYPE ? "B" : "S")), 
                 s->progressive_sequence ? "pro" :"", s->alternate_scan ? "alt" :"", s->top_field_first ? "top" :"", 
                 s->intra_dc_precision, s->picture_structure, s->frame_pred_frame_dct, s->concealment_motion_vectors,
                 s->q_scale_type, s->intra_vlc_format, s->repeat_first_field, s->chroma_420_type ? "420" :"");
        }
      }else{ //second field
            int i;
            
            if(!s->current_picture_ptr){
                av_log(s->avctx, AV_LOG_ERROR, "first field missing\n");
                return -1;
            }
            
            for(i=0; i<4; i++){
                s->current_picture.data[i] = s->current_picture_ptr->data[i];
                if(s->picture_structure == PICT_BOTTOM_FIELD){
                    s->current_picture.data[i] += s->current_picture_ptr->linesize[i];
                } 
            }
      }
#ifdef HAVE_XVMC
// MPV_frame_start will call this function too,
// but we need to call it on every field
      if(s->avctx->xvmc_acceleration)
         XVMC_field_start(s,avctx);
#endif
    }//fi(s->first_slice)
    s->first_slice = 0;

    init_get_bits(&s->gb, *buf, buf_size*8);

    s->qscale = get_qscale(s);
    if(s->qscale == 0){
        av_log(s->avctx, AV_LOG_ERROR, "qscale == 0\n");
        return -1;
    }
    
    /* extra slice info */
    while (get_bits1(&s->gb) != 0) {
        skip_bits(&s->gb, 8);
    }
    
    s->mb_x=0;

    for(;;) {
        int code = get_vlc2(&s->gb, mbincr_vlc.table, MBINCR_VLC_BITS, 2);
        if (code < 0){
            av_log(s->avctx, AV_LOG_ERROR, "first mb_incr damaged\n");
            return -1;
        }
        if (code >= 33) {
            if (code == 33) {
                s->mb_x += 33;
            }
            /* otherwise, stuffing, nothing to do */
        } else {
            s->mb_x += code;
            break;
        }
    }
    
    s->resync_mb_x= s->mb_x;
    s->resync_mb_y= s->mb_y = start_code;
    s->mb_skip_run= 0;
    ff_init_block_index(s);

    for(;;) {
#ifdef HAVE_XVMC
        //one 1 we memcpy blocks in xvmcvideo
        if(s->avctx->xvmc_acceleration > 1)
            XVMC_init_block(s);//set s->block
#endif

	s->dsp.clear_blocks(s->block[0]);

        ret = mpeg_decode_mb(s, s->block);
        s->chroma_qscale= s->qscale;

        dprintf("ret=%d\n", ret);
        if (ret < 0)
            return -1;

        if(s->current_picture.motion_val[0]){ //note motion_val is normally NULL unless we want to extract the MVs
            const int wrap = s->block_wrap[0];
            const int xy = s->mb_x*2 + 1 + (s->mb_y*2 +1)*wrap;
            int motion_for_x, motion_for_y, motion_back_x, motion_back_y;
            if (s->mb_intra) {
                motion_for_x = motion_for_y = motion_back_x = motion_back_y = 0;
            }else if (s->mv_type == MV_TYPE_16X16){
                motion_for_x = s->mv[0][0][0];
                motion_for_y = s->mv[0][0][1];
                motion_back_x = s->mv[1][0][0];
                motion_back_y = s->mv[1][0][1];                
            } else /*if ((s->mv_type == MV_TYPE_FIELD) || (s->mv_type == MV_TYPE_16X8))*/ {
                motion_for_x = s->mv[0][0][0] + s->mv[0][1][0];
                motion_for_y = s->mv[0][0][1] + s->mv[0][1][1];
                motion_for_x = (motion_for_x>>1) | (motion_for_x&1);
                motion_back_x = s->mv[1][0][0] + s->mv[1][1][0];
                motion_back_y = s->mv[1][0][1] + s->mv[1][1][1];
                motion_back_x = (motion_back_x>>1) | (motion_back_x&1);
            }

            s->current_picture.motion_val[0][xy][0] = motion_for_x;
            s->current_picture.motion_val[0][xy][1] = motion_for_y;
            s->current_picture.motion_val[0][xy + 1][0] = motion_for_x;
            s->current_picture.motion_val[0][xy + 1][1] = motion_for_y;
            s->current_picture.motion_val[0][xy + wrap][0] = motion_for_x;
            s->current_picture.motion_val[0][xy + wrap][1] = motion_for_y;
            s->current_picture.motion_val[0][xy + 1 + wrap][0] = motion_for_x;
            s->current_picture.motion_val[0][xy + 1 + wrap][1] = motion_for_y;

            if(s->pict_type != B_TYPE){
                motion_back_x = motion_back_y = 0;
            }

            s->current_picture.motion_val[1][xy][0] = motion_back_x;
            s->current_picture.motion_val[1][xy][1] = motion_back_y;
            s->current_picture.motion_val[1][xy + 1][0] = motion_back_x;
            s->current_picture.motion_val[1][xy + 1][1] = motion_back_y;
            s->current_picture.motion_val[1][xy + wrap][0] = motion_back_x;
            s->current_picture.motion_val[1][xy + wrap][1] = motion_back_y;
            s->current_picture.motion_val[1][xy + 1 + wrap][0] = motion_back_x;
            s->current_picture.motion_val[1][xy + 1 + wrap][1] = motion_back_y;
        }

        s->dest[0] += 16;
        s->dest[1] += 8;
        s->dest[2] += 8;

        MPV_decode_mb(s, s->block);
        
        if (++s->mb_x >= s->mb_width) {

            ff_draw_horiz_band(s, 16*s->mb_y, 16);

            s->mb_x = 0;
            s->mb_y++;

            if(s->mb_y<<field_pic >= s->mb_height){
                int left= s->gb.size_in_bits - get_bits_count(&s->gb);

                if(left < 0 || (left && show_bits(&s->gb, FFMIN(left, 23)))
                   || (avctx->error_resilience >= FF_ER_AGGRESSIVE && left>8)){
                    av_log(avctx, AV_LOG_ERROR, "end mismatch left=%d\n", left);
                    return -1;
                }else
                    goto eos;
            }
            
            ff_init_block_index(s);
        }

        /* skip mb handling */
        if (s->mb_skip_run == -1) {
            /* read again increment */
            s->mb_skip_run = 0;
            for(;;) {
                int code = get_vlc2(&s->gb, mbincr_vlc.table, MBINCR_VLC_BITS, 2);
                if (code < 0){
                    av_log(s->avctx, AV_LOG_ERROR, "mb incr damaged\n");
                    return -1;
                }
                if (code >= 33) {
                    if (code == 33) {
                        s->mb_skip_run += 33;
                    }else if(code == 35){
                        if(s->mb_skip_run != 0 || show_bits(&s->gb, 15) != 0){
                            av_log(s->avctx, AV_LOG_ERROR, "slice mismatch\n");
                            return -1;
                        }
                        goto eos; /* end of slice */
                    }
                    /* otherwise, stuffing, nothing to do */
                } else {
                    s->mb_skip_run += code;
                    break;
                }
            }
        }
    }
eos: // end of slice
    *buf += get_bits_count(&s->gb)/8 - 1;
//printf("y %d %d %d %d\n", s->resync_mb_x, s->resync_mb_y, s->mb_x, s->mb_y);
    return 0;
}

/**
 * handles slice ends.
 * @return 1 if it seems to be the last slice of 
 */
static int slice_end(AVCodecContext *avctx, AVFrame *pict)
{
    Mpeg1Context *s1 = avctx->priv_data;
    MpegEncContext *s = &s1->mpeg_enc_ctx;
       
    if (!s1->mpeg_enc_ctx_allocated || !s->current_picture_ptr)
        return 0;

#ifdef HAVE_XVMC
    if(s->avctx->xvmc_acceleration)
        XVMC_field_end(s);
#endif
    /* end of slice reached */
    if (/*s->mb_y<<field_pic == s->mb_height &&*/ !s->first_field) {
        /* end of image */

        if(s->codec_id == CODEC_ID_MPEG2VIDEO){
            s->current_picture_ptr->qscale_type= FF_QSCALE_TYPE_MPEG2;
        }else
            s->current_picture_ptr->qscale_type= FF_QSCALE_TYPE_MPEG1;

        ff_er_frame_end(s);

        MPV_frame_end(s);

        if (s->pict_type == B_TYPE || s->low_delay) {
            *pict= *(AVFrame*)s->current_picture_ptr;
            ff_print_debug_info(s, s->current_picture_ptr);
        } else {
            s->picture_number++;
            /* latency of 1 frame for I and P frames */
            /* XXX: use another variable than picture_number */
            if (s->last_picture_ptr != NULL) {
                *pict= *(AVFrame*)s->last_picture_ptr;
                 ff_print_debug_info(s, s->last_picture_ptr);
            }
        }

        return 1;
    } else {
        return 0;
    }
}

static int mpeg1_decode_sequence(AVCodecContext *avctx, 
                                 uint8_t *buf, int buf_size)
{
    Mpeg1Context *s1 = avctx->priv_data;
    MpegEncContext *s = &s1->mpeg_enc_ctx;
    int width, height, i, v, j;
    float aspect;

    init_get_bits(&s->gb, buf, buf_size*8);

    width = get_bits(&s->gb, 12);
    height = get_bits(&s->gb, 12);
    s->aspect_ratio_info= get_bits(&s->gb, 4);
    if(s->codec_id == CODEC_ID_MPEG1VIDEO){
        aspect= 1.0/mpeg1_aspect[s->aspect_ratio_info];
        if(aspect!=0.0) avctx->sample_aspect_ratio= av_d2q(aspect, 255);
    }

    s->frame_rate_index = get_bits(&s->gb, 4);
    if (s->frame_rate_index == 0)
        return -1;
    s->bit_rate = get_bits(&s->gb, 18) * 400;
    if (get_bits1(&s->gb) == 0) /* marker */
        return -1;
    if (width <= 0 || height <= 0 ||
        (width % 2) != 0 || (height % 2) != 0)
        return -1;
    if (width != s->width ||
        height != s->height) {
        /* start new mpeg1 context decoding */
        s->out_format = FMT_MPEG1;
        if (s1->mpeg_enc_ctx_allocated) {
            MPV_common_end(s);
        }
        s->width = width;
        s->height = height;
        avctx->has_b_frames= 1;
        s->avctx = avctx;
        avctx->width = width;
        avctx->height = height;
        av_reduce(
            &avctx->frame_rate, 
            &avctx->frame_rate_base,
            frame_rate_tab[s->frame_rate_index],
            MPEG1_FRAME_RATE_BASE, //FIXME store in allready reduced form 
            1<<30
            );
        avctx->bit_rate = s->bit_rate;
        
        //get_format() or set_video(width,height,aspect,pix_fmt);
        //until then pix_fmt may be changed right after codec init
        if( avctx->pix_fmt == PIX_FMT_XVMC_MPEG2_IDCT )
            if( avctx->idct_algo == FF_IDCT_AUTO )
                avctx->idct_algo = FF_IDCT_SIMPLE;

        if (MPV_common_init(s) < 0)
            return -1;
        s1->mpeg_enc_ctx_allocated = 1;
        s->swap_uv = 0;//just in case vcr2 and mpeg2 stream have been concatinated
    }

    skip_bits(&s->gb, 10); /* vbv_buffer_size */
    skip_bits(&s->gb, 1);

    /* get matrix */
    if (get_bits1(&s->gb)) {
        for(i=0;i<64;i++) {
            v = get_bits(&s->gb, 8);
            j = s->intra_scantable.permutated[i];
            s->intra_matrix[j] = v;
            s->chroma_intra_matrix[j] = v;
        }
#ifdef DEBUG
        dprintf("intra matrix present\n");
        for(i=0;i<64;i++)
            dprintf(" %d", s->intra_matrix[s->intra_scantable.permutated[i]]);
        printf("\n");
#endif
    } else {
        for(i=0;i<64;i++) {
            int j= s->dsp.idct_permutation[i];
            v = ff_mpeg1_default_intra_matrix[i];
            s->intra_matrix[j] = v;
            s->chroma_intra_matrix[j] = v;
        }
    }
    if (get_bits1(&s->gb)) {
        for(i=0;i<64;i++) {
            v = get_bits(&s->gb, 8);
            j = s->intra_scantable.permutated[i];
            s->inter_matrix[j] = v;
            s->chroma_inter_matrix[j] = v;
        }
#ifdef DEBUG
        dprintf("non intra matrix present\n");
        for(i=0;i<64;i++)
            dprintf(" %d", s->inter_matrix[s->intra_scantable.permutated[i]]);
        printf("\n");
#endif
    } else {
        for(i=0;i<64;i++) {
            int j= s->dsp.idct_permutation[i];
            v = ff_mpeg1_default_non_intra_matrix[i];
            s->inter_matrix[j] = v;
            s->chroma_inter_matrix[j] = v;
        }
    }

    /* we set mpeg2 parameters so that it emulates mpeg1 */
    s->progressive_sequence = 1;
    s->progressive_frame = 1;
    s->picture_structure = PICT_FRAME;
    s->frame_pred_frame_dct = 1;
    s->codec_id= s->avctx->codec_id= CODEC_ID_MPEG1VIDEO;
    avctx->sub_id = 1; /* indicates mpeg1 */
    return 0;
}

static int vcr2_init_sequence(AVCodecContext *avctx)
{
    Mpeg1Context *s1 = avctx->priv_data;
    MpegEncContext *s = &s1->mpeg_enc_ctx;
    int i, v;

    /* start new mpeg1 context decoding */
    s->out_format = FMT_MPEG1;
    if (s1->mpeg_enc_ctx_allocated) {
        MPV_common_end(s);
    }
    s->width = avctx->width;
    s->height = avctx->height;
    avctx->has_b_frames= 0; //true?
    s->low_delay= 1;
    s->avctx = avctx;

    //get_format() or set_video(width,height,aspect,pix_fmt);
    //until then pix_fmt may be changed right after codec init
    if( avctx->pix_fmt == PIX_FMT_XVMC_MPEG2_IDCT )
        if( avctx->idct_algo == FF_IDCT_AUTO )
            avctx->idct_algo = FF_IDCT_SIMPLE;
    
    if (MPV_common_init(s) < 0)
        return -1;
    exchange_uv(s);//common init reset pblocks, so we swap them here
    s->swap_uv = 1;// in case of xvmc we need to swap uv for each MB 
    s1->mpeg_enc_ctx_allocated = 1;

    for(i=0;i<64;i++) {
        int j= s->dsp.idct_permutation[i];
        v = ff_mpeg1_default_intra_matrix[i];
        s->intra_matrix[j] = v;
        s->chroma_intra_matrix[j] = v;

        v = ff_mpeg1_default_non_intra_matrix[i];
        s->inter_matrix[j] = v;
        s->chroma_inter_matrix[j] = v;
    }

    s->progressive_sequence = 1;
    s->progressive_frame = 1;
    s->picture_structure = PICT_FRAME;
    s->frame_pred_frame_dct = 1;
    s->codec_id= s->avctx->codec_id= CODEC_ID_MPEG2VIDEO;
    avctx->sub_id = 2; /* indicates mpeg2 */
    return 0;
}


static void mpeg_decode_user_data(AVCodecContext *avctx, 
                                  const uint8_t *buf, int buf_size)
{
    const uint8_t *p;
    int len, flags;
    p = buf;
    len = buf_size;

    /* we parse the DTG active format information */
    if (len >= 5 &&
        p[0] == 'D' && p[1] == 'T' && p[2] == 'G' && p[3] == '1') {
        flags = p[4];
        p += 5;
        len -= 5;
        if (flags & 0x80) {
            /* skip event id */
            if (len < 2)
                return;
            p += 2;
            len -= 2;
        }
        if (flags & 0x40) {
            if (len < 1)
                return;
            avctx->dtg_active_format = p[0] & 0x0f;
        }
    }
}

/**
 * finds the end of the current frame in the bitstream.
 * @return the position of the first byte of the next frame, or -1
 */
static int mpeg1_find_frame_end(MpegEncContext *s, uint8_t *buf, int buf_size){
    ParseContext *pc= &s->parse_context;
    int i;
    uint32_t state;
    
    state= pc->state;
    
    i=0;
    if(!pc->frame_start_found){
        for(i=0; i<buf_size; i++){
            state= (state<<8) | buf[i];
            if(state >= SLICE_MIN_START_CODE && state <= SLICE_MAX_START_CODE){
                i++;
                pc->frame_start_found=1;
                break;
            }
        }
    }
    
    if(pc->frame_start_found){
        for(; i<buf_size; i++){
            state= (state<<8) | buf[i];
            if((state&0xFFFFFF00) == 0x100){
                if(state < SLICE_MIN_START_CODE || state > SLICE_MAX_START_CODE){
                    pc->frame_start_found=0;
                    pc->state=-1; 
                    return i-3;
                }
            }
        }
    }        
    pc->state= state;
    return END_NOT_FOUND;
}

/* handle buffering and image synchronisation */
static int mpeg_decode_frame(AVCodecContext *avctx, 
                             void *data, int *data_size,
                             uint8_t *buf, int buf_size)
{
    Mpeg1Context *s = avctx->priv_data;
    uint8_t *buf_end, *buf_ptr;
    int ret, start_code, input_size;
    AVFrame *picture = data;
    MpegEncContext *s2 = &s->mpeg_enc_ctx;
    dprintf("fill_buffer\n");

    *data_size = 0;

    /* special case for last picture */
    if (buf_size == 0 && s2->low_delay==0 && s2->next_picture_ptr) {
        *picture= *(AVFrame*)s2->next_picture_ptr;
        s2->next_picture_ptr= NULL;

        *data_size = sizeof(AVFrame);
        return 0;
    }

    if(s2->flags&CODEC_FLAG_TRUNCATED){
        int next= mpeg1_find_frame_end(s2, buf, buf_size);
        
        if( ff_combine_frame(s2, next, &buf, &buf_size) < 0 )
            return buf_size;
    }    
    
    buf_ptr = buf;
    buf_end = buf + buf_size;

#if 0    
    if (s->repeat_field % 2 == 1) { 
        s->repeat_field++;
        //fprintf(stderr,"\nRepeating last frame: %d -> %d! pict: %d %d", avctx->frame_number-1, avctx->frame_number,
        //        s2->picture_number, s->repeat_field);
        if (avctx->flags & CODEC_FLAG_REPEAT_FIELD) {
            *data_size = sizeof(AVPicture);
            goto the_end;
        }
    }
#endif

    if(s->mpeg_enc_ctx_allocated==0 && avctx->codec_tag == ff_get_fourcc("VCR2"))
        vcr2_init_sequence(avctx);

    for(;;) {
        /* find start next code */
        start_code = find_start_code(&buf_ptr, buf_end);
        if (start_code < 0){
            if(s2->pict_type != B_TYPE || avctx->hurry_up==0){
                if (slice_end(avctx, picture)) {
                    if(s2->last_picture_ptr) //FIXME merge with the stuff in mpeg_decode_slice
                        *data_size = sizeof(AVPicture);
                }
            }
            return FFMAX(0, buf_ptr - buf - s2->parse_context.last_index);
        }
        
        input_size = buf_end - buf_ptr;

        if(avctx->debug & FF_DEBUG_STARTCODE){
            av_log(avctx, AV_LOG_DEBUG, "%3X at %d left %d\n", start_code, buf_ptr-buf, input_size);
        }

                /* prepare data for next start code */
                switch(start_code) {
                case SEQ_START_CODE:
                    mpeg1_decode_sequence(avctx, buf_ptr, 
                                          input_size);
                    break;
                            
                case PICTURE_START_CODE:
                    /* we have a complete image : we try to decompress it */
                    mpeg1_decode_picture(avctx, 
                                         buf_ptr, input_size);
                    break;
                case EXT_START_CODE:
                    mpeg_decode_extension(avctx,
                                          buf_ptr, input_size);
                    break;
                case USER_START_CODE:
                    mpeg_decode_user_data(avctx, 
                                          buf_ptr, input_size);
                    break;
                case GOP_START_CODE:
                    s2->first_field=0;
                    break;
                default:
                    if (start_code >= SLICE_MIN_START_CODE &&
                        start_code <= SLICE_MAX_START_CODE) {
                        
                        /* skip b frames if we dont have reference frames */
                        if(s2->last_picture_ptr==NULL && s2->pict_type==B_TYPE) break;
                        /* skip b frames if we are in a hurry */
                        if(avctx->hurry_up && s2->pict_type==B_TYPE) break;
                        /* skip everything if we are in a hurry>=5 */
                        if(avctx->hurry_up>=5) break;
                        
                        if (!s->mpeg_enc_ctx_allocated) break;

                        ret = mpeg_decode_slice(avctx, picture,
                                                start_code, &buf_ptr, input_size);
                        emms_c();

                        if(ret < 0){
                            if(s2->resync_mb_x>=0 && s2->resync_mb_y>=0)
                                ff_er_add_slice(s2, s2->resync_mb_x, s2->resync_mb_y, s2->mb_x, s2->mb_y, AC_ERROR|DC_ERROR|MV_ERROR);
                            if(ret==DECODE_SLICE_FATAL_ERROR) return -1;
                        }else{
                            ff_er_add_slice(s2, s2->resync_mb_x, s2->resync_mb_y, s2->mb_x-1, s2->mb_y, AC_END|DC_END|MV_END);
                        }
                    }
                    break;
                }
    }
}

static int mpeg_decode_end(AVCodecContext *avctx)
{
    Mpeg1Context *s = avctx->priv_data;

    if (s->mpeg_enc_ctx_allocated)
        MPV_common_end(&s->mpeg_enc_ctx);
    return 0;
}

AVCodec mpeg1video_decoder = {
    "mpeg1video",
    CODEC_TYPE_VIDEO,
    CODEC_ID_MPEG1VIDEO,
    sizeof(Mpeg1Context),
    mpeg_decode_init,
    NULL,
    mpeg_decode_end,
    mpeg_decode_frame,
    CODEC_CAP_DRAW_HORIZ_BAND | CODEC_CAP_DR1 | CODEC_CAP_TRUNCATED,
    .flush= ff_mpeg_flush,
};

AVCodec mpeg2video_decoder = {
    "mpeg2video",
    CODEC_TYPE_VIDEO,
    CODEC_ID_MPEG2VIDEO,
    sizeof(Mpeg1Context),
    mpeg_decode_init,
    NULL,
    mpeg_decode_end,
    mpeg_decode_frame,
    CODEC_CAP_DRAW_HORIZ_BAND | CODEC_CAP_DR1 | CODEC_CAP_TRUNCATED,
    .flush= ff_mpeg_flush,
};

//legacy decoder
AVCodec mpegvideo_decoder = {
    "mpegvideo",
    CODEC_TYPE_VIDEO,
    CODEC_ID_MPEG2VIDEO,
    sizeof(Mpeg1Context),
    mpeg_decode_init,
    NULL,
    mpeg_decode_end,
    mpeg_decode_frame,
    CODEC_CAP_DRAW_HORIZ_BAND | CODEC_CAP_DR1 | CODEC_CAP_TRUNCATED,
    .flush= ff_mpeg_flush,
};

#ifdef HAVE_XVMC
static int mpeg_mc_decode_init(AVCodecContext *avctx){
    Mpeg1Context *s;

    if( !(avctx->slice_flags & SLICE_FLAG_CODED_ORDER) )
        return -1;
    if( !(avctx->slice_flags & SLICE_FLAG_ALLOW_FIELD) ){
        dprintf("mpeg12.c: XvMC decoder will work better if SLICE_FLAG_ALLOW_FIELD is set\n");
    }
    mpeg_decode_init(avctx);
    s = avctx->priv_data;

    avctx->pix_fmt = PIX_FMT_XVMC_MPEG2_IDCT;
    avctx->xvmc_acceleration = 2;//2 - the blocks are packed!

    return 0;
}

AVCodec mpeg_xvmc_decoder = {
    "mpegvideo_xvmc",
    CODEC_TYPE_VIDEO,
    CODEC_ID_MPEG2VIDEO_XVMC,
    sizeof(Mpeg1Context),
    mpeg_mc_decode_init,
    NULL,
    mpeg_decode_end,
    mpeg_decode_frame,
    CODEC_CAP_DRAW_HORIZ_BAND | CODEC_CAP_DR1 | CODEC_CAP_TRUNCATED,
};

#endif

/* this is ugly i know, but the alternative is too make 
   hundreds of vars global and prefix them with ff_mpeg1_
   which is far uglier. */
#include "mdec.c"  
