/*
 * FFV1 codec for libavcodec
 *
 * Copyright (c) 2003 Michael Niedermayer <michaelni@gmx.at>
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
 *
 */
 
/**
 * @file ffv1.c
 * FF Video Codec 1 (an experimental lossless codec)
 */

#include "common.h"
#include "avcodec.h"
#include "dsputil.h"
#include "cabac.h"
#include "golomb.h"

#define MAX_PLANES 4
#define CONTEXT_SIZE 32

static const int8_t quant3[256]={
 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, 0,
};
static const int8_t quant5[256]={
 0, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,
-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,
-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,
-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,
-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,
-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,
-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,
-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-1,-1,-1,
};
static const int8_t quant7[256]={
 0, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,
-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,
-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,
-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,
-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,
-3,-3,-3,-3,-3,-3,-3,-3,-3,-2,-2,-2,-2,-2,-2,-2,
-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,
-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-2,-1,-1,
};
static const int8_t quant9[256]={
 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3,
 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,
-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,
-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,
-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,
-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,
-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,
-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-3,-3,-3,-3,
-3,-3,-3,-3,-3,-3,-3,-3,-3,-3,-2,-2,-2,-2,-1,-1,
};
static const int8_t quant11[256]={
 0, 1, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4,
 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,
-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,
-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,
-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,
-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,
-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-4,-4,
-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,-4,
-4,-4,-4,-4,-4,-3,-3,-3,-3,-3,-3,-3,-2,-2,-2,-1,
};
static const int8_t quant13[256]={
 0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,
-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,
-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,
-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,
-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-6,-5,
-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,
-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,-5,
-4,-4,-4,-4,-4,-4,-4,-4,-4,-3,-3,-3,-3,-2,-2,-1,
};

static const uint8_t log2_run[32]={
 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 
 4, 4, 5, 5, 6, 6, 7, 7, 
 8, 9,10,11,12,13,14,15,
};

typedef struct VlcState{
    int16_t drift;
    uint16_t error_sum;
    int8_t bias;
    uint8_t count;
} VlcState;

typedef struct PlaneContext{
    int context_count;
    uint8_t (*state)[CONTEXT_SIZE];
    VlcState *vlc_state;
    uint8_t interlace_bit_state[2];
} PlaneContext;

typedef struct FFV1Context{
    AVCodecContext *avctx;
    CABACContext c;
    GetBitContext gb;
    PutBitContext pb;
    int version;
    int width, height;
    int chroma_h_shift, chroma_v_shift;
    int flags;
    int picture_number;
    AVFrame picture;
    int plane_count;
    int ac;                              ///< 1-> CABAC 0-> golomb rice
    PlaneContext plane[MAX_PLANES];
    int16_t quant_table[5][256];
    int run_index;
    int colorspace;
    
    DSPContext dsp; 
}FFV1Context;

static always_inline int fold(int diff, int bits){
    if(bits==8)
        diff= (int8_t)diff;
    else{
        diff+= 1<<(bits-1);
        diff&=(1<<bits)-1;
        diff-= 1<<(bits-1);
    }

    return diff;
}

static inline int predict(int_fast16_t *src, int_fast16_t *last){
    const int LT= last[-1];
    const int  T= last[ 0];
    const int L =  src[-1];

    return mid_pred(L, L + T - LT, T);
}

static inline int get_context(FFV1Context *f, int_fast16_t *src, int_fast16_t *last, int_fast16_t *last2){
    const int LT= last[-1];
    const int  T= last[ 0];
    const int RT= last[ 1];
    const int L =  src[-1];

    if(f->quant_table[3][127]){
        const int TT= last2[0];
        const int LL=  src[-2];
        return f->quant_table[0][(L-LT) & 0xFF] + f->quant_table[1][(LT-T) & 0xFF] + f->quant_table[2][(T-RT) & 0xFF]
              +f->quant_table[3][(LL-L) & 0xFF] + f->quant_table[4][(TT-T) & 0xFF];
    }else
        return f->quant_table[0][(L-LT) & 0xFF] + f->quant_table[1][(LT-T) & 0xFF] + f->quant_table[2][(T-RT) & 0xFF];
}

/**
 * put 
 */
static inline void put_symbol(CABACContext *c, uint8_t *state, int v, int is_signed, int max_exp){
    int i;

    if(v){
        const int a= ABS(v);
        const int e= av_log2(a);

        put_cabac(c, state+0, 0);
        
        for(i=0; i<e; i++){
            put_cabac(c, state+1+i, 1);  //1..8
        }

        if(e<max_exp){
            put_cabac(c, state+1+i, 0);      //1..8

            for(i=e-1; i>=0; i--){
                put_cabac(c, state+16+e+i, (a>>i)&1); //17..29
            }
            if(is_signed)
                put_cabac(c, state+9 + e, v < 0); //9..16
        }
    }else{
        put_cabac(c, state+0, 1);
    }
}

static inline int get_symbol(CABACContext *c, uint8_t *state, int is_signed, int max_exp){
    if(get_cabac(c, state+0))
        return 0;
    else{
        int i, e;
 
        for(e=0; e<max_exp; e++){ 
            int a= 1<<e;

            if(get_cabac(c, state + 1 + e)==0){ // 1..8
                for(i=e-1; i>=0; i--){
                    a += get_cabac(c, state+16+e+i)<<i; //17..29
                }

                if(is_signed && get_cabac(c, state+9 + e)) //9..16
                    return -a;
                else
                    return a;
            }
        }
        return -(1<<e);
    }
}

static inline void update_vlc_state(VlcState * const state, const int v){
    int drift= state->drift;
    int count= state->count;
    state->error_sum += ABS(v);
    drift += v;

    if(count == 128){ //FIXME variable
        count >>= 1;
        drift >>= 1;
        state->error_sum >>= 1;
    }
    count++;

    if(drift <= -count){
        if(state->bias > -128) state->bias--;
        
        drift += count;
        if(drift <= -count)
            drift= -count + 1;
    }else if(drift > 0){
        if(state->bias <  127) state->bias++;
        
        drift -= count;
        if(drift > 0) 
            drift= 0;
    }

    state->drift= drift;
    state->count= count;
}

static inline void put_vlc_symbol(PutBitContext *pb, VlcState * const state, int v, int bits){
    int i, k, code;
//printf("final: %d ", v);
    v = fold(v - state->bias, bits);

    i= state->count;
    k=0;
    while(i < state->error_sum){ //FIXME optimize
        k++;
        i += i;
    }

    assert(k<=8);

#if 0 // JPEG LS
    if(k==0 && 2*state->drift <= - state->count) code= v ^ (-1);
    else                                         code= v;
#else
     code= v ^ ((2*state->drift + state->count)>>31);
#endif
    
    code = -2*code-1;
    code^= (code>>31);
//printf("v:%d/%d bias:%d error:%d drift:%d count:%d k:%d\n", v, code, state->bias, state->error_sum, state->drift, state->count, k);
    set_ur_golomb(pb, code, k, 12, bits);

    update_vlc_state(state, v);
}

static inline int get_vlc_symbol(GetBitContext *gb, VlcState * const state, int bits){
    int k, i, v, ret;

    i= state->count;
    k=0;
    while(i < state->error_sum){ //FIXME optimize
        k++;
        i += i;
    }

    assert(k<=8);

    v= get_ur_golomb(gb, k, 12, bits);
//printf("v:%d bias:%d error:%d drift:%d count:%d k:%d", v, state->bias, state->error_sum, state->drift, state->count, k);

    v++;
    if(v&1) v=  (v>>1);
    else    v= -(v>>1);

#if 0 // JPEG LS
    if(k==0 && 2*state->drift <= - state->count) v ^= (-1);
#else
     v ^= ((2*state->drift + state->count)>>31);
#endif

    ret= fold(v + state->bias, bits);
    
    update_vlc_state(state, v);
//printf("final: %d\n", ret);
    return ret;
}

static inline void encode_line(FFV1Context *s, int w, int_fast16_t *sample[2], int plane_index, int bits){
    PlaneContext * const p= &s->plane[plane_index];
    CABACContext * const c= &s->c;
    int x;
    int run_index= s->run_index;
    int run_count=0;
    int run_mode=0;

    for(x=0; x<w; x++){
        int diff, context;
        
        context= get_context(s, sample[1]+x, sample[0]+x, sample[2]+x);
        diff= sample[1][x] - predict(sample[1]+x, sample[0]+x);

        if(context < 0){
            context = -context;
            diff= -diff;
        }

        diff= fold(diff, bits);
        
        if(s->ac){
            put_symbol(c, p->state[context], diff, 1, bits-1);
        }else{
            if(context == 0) run_mode=1;
            
            if(run_mode){

                if(diff){
                    while(run_count >= 1<<log2_run[run_index]){
                        run_count -= 1<<log2_run[run_index];
                        run_index++;
                        put_bits(&s->pb, 1, 1);
                    }
                    
                    put_bits(&s->pb, 1 + log2_run[run_index], run_count);
                    if(run_index) run_index--;
                    run_count=0;
                    run_mode=0;
                    if(diff>0) diff--;
                }else{
                    run_count++;
                }
            }
            
//            printf("count:%d index:%d, mode:%d, x:%d y:%d pos:%d\n", run_count, run_index, run_mode, x, y, (int)get_bit_count(&s->pb));

            if(run_mode == 0)
                put_vlc_symbol(&s->pb, &p->vlc_state[context], diff, bits);
        }
    }
    if(run_mode){
        while(run_count >= 1<<log2_run[run_index]){
            run_count -= 1<<log2_run[run_index];
            run_index++;
            put_bits(&s->pb, 1, 1);
        }

        if(run_count)
            put_bits(&s->pb, 1, 1);
    }
    s->run_index= run_index;
}

static void encode_plane(FFV1Context *s, uint8_t *src, int w, int h, int stride, int plane_index){
    int x,y;
    int_fast16_t sample_buffer[3][w+6];
    int_fast16_t *sample[3]= {sample_buffer[0]+3, sample_buffer[1]+3, sample_buffer[2]+3};
    s->run_index=0;
    
    memset(sample_buffer, 0, sizeof(sample_buffer));
    
    for(y=0; y<h; y++){
        int_fast16_t *temp= sample[0]; //FIXME try a normal buffer

        sample[0]= sample[1];
        sample[1]= sample[2];
        sample[2]= temp;
        
        sample[1][-1]= sample[0][0  ];
        sample[0][ w]= sample[0][w-1];
//{START_TIMER
        for(x=0; x<w; x++){
            sample[1][x]= src[x + stride*y];
        }
        encode_line(s, w, sample, plane_index, 8);
//STOP_TIMER("encode line")}
    }
}

static void encode_rgb_frame(FFV1Context *s, uint32_t *src, int w, int h, int stride){
    int x, y, p;
    int_fast16_t sample_buffer[3][2][w+6];
    int_fast16_t *sample[3][2]= {
        {sample_buffer[0][0]+3, sample_buffer[0][1]+3},
        {sample_buffer[1][0]+3, sample_buffer[1][1]+3},
        {sample_buffer[2][0]+3, sample_buffer[2][1]+3}};
    s->run_index=0;
    
    memset(sample_buffer, 0, sizeof(sample_buffer));
    
    for(y=0; y<h; y++){
        for(x=0; x<w; x++){
            int v= src[x + stride*y];
            int b= v&0xFF;
            int g= (v>>8)&0xFF;
            int r= (v>>16)&0xFF;
            
            b -= g;
            r -= g;
            g += (b + r)>>2;
            b += 0x100;
            r += 0x100;
            
//            assert(g>=0 && b>=0 && r>=0);
//            assert(g<256 && b<512 && r<512);
            sample[0][0][x]= g;
            sample[1][0][x]= b;
            sample[2][0][x]= r;
        }
        for(p=0; p<3; p++){
            int_fast16_t *temp= sample[p][0]; //FIXME try a normal buffer

            sample[p][0]= sample[p][1];
            sample[p][1]= temp;

            sample[p][1][-1]= sample[p][0][0  ];
            sample[p][0][ w]= sample[p][0][w-1];
            encode_line(s, w, sample[p], FFMIN(p, 1), 9);
        }
    }
}

static void write_quant_table(CABACContext *c, int16_t *quant_table){
    int last=0;
    int i;
    uint8_t state[CONTEXT_SIZE]={0};

    for(i=1; i<128 ; i++){
        if(quant_table[i] != quant_table[i-1]){
            put_symbol(c, state, i-last-1, 0, 7);
            last= i;
        }
    }
    put_symbol(c, state, i-last-1, 0, 7);
}

static void write_header(FFV1Context *f){
    uint8_t state[CONTEXT_SIZE]={0};
    int i;
    CABACContext * const c= &f->c;

    put_symbol(c, state, f->version, 0, 7);
    put_symbol(c, state, f->avctx->coder_type, 0, 7);
    put_symbol(c, state, f->colorspace, 0, 7); //YUV cs type 
    put_cabac(c, state, 1); //chroma planes
        put_symbol(c, state, f->chroma_h_shift, 0, 7);
        put_symbol(c, state, f->chroma_v_shift, 0, 7);
    put_cabac(c, state, 0); //no transparency plane

    for(i=0; i<5; i++)
        write_quant_table(c, f->quant_table[i]);
}

static int common_init(AVCodecContext *avctx){
    FFV1Context *s = avctx->priv_data;
    int width, height;

    s->avctx= avctx;
    s->flags= avctx->flags;
        
    dsputil_init(&s->dsp, avctx);
    
    width= s->width= avctx->width;
    height= s->height= avctx->height;
    
    assert(width && height);

    return 0;
}

static int encode_init(AVCodecContext *avctx)
{
    FFV1Context *s = avctx->priv_data;
    int i;

    common_init(avctx);
 
    s->version=0;
    s->ac= avctx->coder_type;
    
    s->plane_count=2;
    for(i=0; i<256; i++){
        s->quant_table[0][i]=           quant11[i];
        s->quant_table[1][i]=        11*quant11[i];
        if(avctx->context_model==0){
            s->quant_table[2][i]=     11*11*quant11[i];
            s->quant_table[3][i]=
            s->quant_table[4][i]=0;
        }else{
            s->quant_table[2][i]=     11*11*quant5 [i];
            s->quant_table[3][i]=   5*11*11*quant5 [i];
            s->quant_table[4][i]= 5*5*11*11*quant5 [i];
        }
    }

    for(i=0; i<s->plane_count; i++){
        PlaneContext * const p= &s->plane[i];
               
        if(avctx->context_model==0){
            p->context_count= (11*11*11+1)/2;
        }else{        
            p->context_count= (11*11*5*5*5+1)/2;
        }

        if(s->ac){
            if(!p->state) p->state= av_malloc(CONTEXT_SIZE*p->context_count*sizeof(uint8_t));
        }else{
            if(!p->vlc_state) p->vlc_state= av_malloc(p->context_count*sizeof(VlcState));
        }
    }

    avctx->coded_frame= &s->picture;
    switch(avctx->pix_fmt){
    case PIX_FMT_YUV444P:
    case PIX_FMT_YUV422P:
    case PIX_FMT_YUV420P:
    case PIX_FMT_YUV411P:
    case PIX_FMT_YUV410P:
        s->colorspace= 0;
        break;
    case PIX_FMT_RGBA32:
        s->colorspace= 1;
        break;
    default:
        av_log(avctx, AV_LOG_ERROR, "format not supported\n");
        return -1;
    }
    avcodec_get_chroma_sub_sample(avctx->pix_fmt, &s->chroma_h_shift, &s->chroma_v_shift);

    s->picture_number=0;
    
    return 0;
}


static void clear_state(FFV1Context *f){
    int i, j;

    for(i=0; i<f->plane_count; i++){
        PlaneContext *p= &f->plane[i];

        p->interlace_bit_state[0]= 0;
        p->interlace_bit_state[1]= 0;
        
        for(j=0; j<p->context_count; j++){
            if(f->ac){
                memset(p->state[j], 0, sizeof(uint8_t)*CONTEXT_SIZE);
                p->state[j][7] = 2*62;
            }else{
                p->vlc_state[j].drift= 0;
                p->vlc_state[j].error_sum= 4; //FFMAX((RANGE + 32)/64, 2);
                p->vlc_state[j].bias= 0;
                p->vlc_state[j].count= 1;
            }
        }
    }
}

static int encode_frame(AVCodecContext *avctx, unsigned char *buf, int buf_size, void *data){
    FFV1Context *f = avctx->priv_data;
    CABACContext * const c= &f->c;
    AVFrame *pict = data;
    const int width= f->width;
    const int height= f->height;
    AVFrame * const p= &f->picture;
    int used_count= 0;

    if(avctx->strict_std_compliance >= 0){
        av_log(avctx, AV_LOG_ERROR, "this codec is under development, files encoded with it wont be decodeable with future versions!!!\n"
               "use vstrict=-1 to use it anyway\n");
        return -1;
    }
        
    ff_init_cabac_encoder(c, buf, buf_size);
    ff_init_cabac_states(c, ff_h264_lps_range, ff_h264_mps_state, ff_h264_lps_state, 64);
    
    *p = *pict;
    p->pict_type= FF_I_TYPE;
    
    if(avctx->gop_size==0 || f->picture_number % avctx->gop_size == 0){
        put_cabac_bypass(c, 1);
        p->key_frame= 1;
        write_header(f);
        clear_state(f);
    }else{
        put_cabac_bypass(c, 0);
        p->key_frame= 0;
    }

    if(!f->ac){
        used_count += put_cabac_terminate(c, 1);
//printf("pos=%d\n", used_count);
        init_put_bits(&f->pb, buf + used_count, buf_size - used_count);
    }
    
    if(f->colorspace==0){
        const int chroma_width = -((-width )>>f->chroma_h_shift);
        const int chroma_height= -((-height)>>f->chroma_v_shift);

        encode_plane(f, p->data[0], width, height, p->linesize[0], 0);

        encode_plane(f, p->data[1], chroma_width, chroma_height, p->linesize[1], 1);
        encode_plane(f, p->data[2], chroma_width, chroma_height, p->linesize[2], 1);
    }else{
        encode_rgb_frame(f, (uint32_t*)(p->data[0]), width, height, p->linesize[0]/4);
    }
    emms_c();
    
    f->picture_number++;

    if(f->ac){
        return put_cabac_terminate(c, 1);
    }else{
        flush_put_bits(&f->pb); //nicer padding FIXME
        return used_count + (get_bit_count(&f->pb)+7)/8;
    }
}

static void common_end(FFV1Context *s){
    int i; 

    for(i=0; i<s->plane_count; i++){
        PlaneContext *p= &s->plane[i];

        av_freep(&p->state);
    }
}

static int encode_end(AVCodecContext *avctx)
{
    FFV1Context *s = avctx->priv_data;

    common_end(s);

    return 0;
}

static inline void decode_line(FFV1Context *s, int w, int_fast16_t *sample[2], int plane_index, int bits){
    PlaneContext * const p= &s->plane[plane_index];
    CABACContext * const c= &s->c;
    int x;
    int run_count=0;
    int run_mode=0;
    int run_index= s->run_index;

    for(x=0; x<w; x++){
        int diff, context, sign;
         
        context= get_context(s, sample[1] + x, sample[0] + x, sample[1] + x);
        if(context < 0){
            context= -context;
            sign=1;
        }else
            sign=0;
        

        if(s->ac)
            diff= get_symbol(c, p->state[context], 1, bits-1);
        else{
            if(context == 0 && run_mode==0) run_mode=1;
            
            if(run_mode){
                if(run_count==0 && run_mode==1){
                    if(get_bits1(&s->gb)){
                        run_count = 1<<log2_run[run_index];
                        if(x + run_count <= w) run_index++;
                    }else{
                        if(log2_run[run_index]) run_count = get_bits(&s->gb, log2_run[run_index]);
                        else run_count=0;
                        if(run_index) run_index--;
                        run_mode=2;
                    }
                }
                run_count--;
                if(run_count < 0){
                    run_mode=0;
                    run_count=0;
                    diff= get_vlc_symbol(&s->gb, &p->vlc_state[context], bits);
                    if(diff>=0) diff++;
                }else
                    diff=0;
            }else
                diff= get_vlc_symbol(&s->gb, &p->vlc_state[context], bits);
            
//            printf("count:%d index:%d, mode:%d, x:%d y:%d pos:%d\n", run_count, run_index, run_mode, x, y, get_bits_count(&s->gb));
        }

        if(sign) diff= -diff;

        sample[1][x]= (predict(sample[1] + x, sample[0] + x) + diff) & ((1<<bits)-1);
    }
    s->run_index= run_index;        
}

static void decode_plane(FFV1Context *s, uint8_t *src, int w, int h, int stride, int plane_index){
    int x, y;
    int_fast16_t sample_buffer[2][w+6];
    int_fast16_t *sample[2]= {sample_buffer[0]+3, sample_buffer[1]+3};

    s->run_index=0;
    
    memset(sample_buffer, 0, sizeof(sample_buffer));
    
    for(y=0; y<h; y++){
        int_fast16_t *temp= sample[0]; //FIXME try a normal buffer

        sample[0]= sample[1];
        sample[1]= temp;

        sample[1][-1]= sample[0][0  ];
        sample[0][ w]= sample[0][w-1];
        
//{START_TIMER
        decode_line(s, w, sample, plane_index, 8);
        for(x=0; x<w; x++){
            src[x + stride*y]= sample[1][x];
        }
//STOP_TIMER("decode-line")}
    }
}

static void decode_rgb_frame(FFV1Context *s, uint32_t *src, int w, int h, int stride){
    int x, y, p;
    int_fast16_t sample_buffer[3][2][w+6];
    int_fast16_t *sample[3][2]= {
        {sample_buffer[0][0]+3, sample_buffer[0][1]+3},
        {sample_buffer[1][0]+3, sample_buffer[1][1]+3},
        {sample_buffer[2][0]+3, sample_buffer[2][1]+3}};

    s->run_index=0;
    
    memset(sample_buffer, 0, sizeof(sample_buffer));
    
    for(y=0; y<h; y++){
        for(p=0; p<3; p++){
            int_fast16_t *temp= sample[p][0]; //FIXME try a normal buffer

            sample[p][0]= sample[p][1];
            sample[p][1]= temp;

            sample[p][1][-1]= sample[p][0][0  ];
            sample[p][0][ w]= sample[p][0][w-1];
            decode_line(s, w, sample[p], FFMIN(p, 1), 9);
        }
        for(x=0; x<w; x++){
            int g= sample[0][1][x];
            int b= sample[1][1][x];
            int r= sample[2][1][x];

//            assert(g>=0 && b>=0 && r>=0);
//            assert(g<256 && b<512 && r<512);
            
            b -= 0x100;
            r -= 0x100;
            g -= (b + r)>>2;
            b += g;
            r += g;
            
            src[x + stride*y]= b + (g<<8) + (r<<16);
        }
    }
}

static int read_quant_table(CABACContext *c, int16_t *quant_table, int scale){
    int v;
    int i=0;
    uint8_t state[CONTEXT_SIZE]={0};

    for(v=0; i<128 ; v++){
        int len= get_symbol(c, state, 0, 7) + 1;

        if(len + i > 128) return -1;
        
        while(len--){
            quant_table[i] = scale*v;
            i++;
//printf("%2d ",v);
//if(i%16==0) printf("\n");
        }
    }

    for(i=1; i<128; i++){
        quant_table[256-i]= -quant_table[i];
    }
    quant_table[128]= -quant_table[127];
    
    return 2*v - 1;
}

static int read_header(FFV1Context *f){
    uint8_t state[CONTEXT_SIZE]={0};
    int i, context_count;
    CABACContext * const c= &f->c;
    
    f->version= get_symbol(c, state, 0, 7);
    f->ac= f->avctx->coder_type= get_symbol(c, state, 0, 7);
    f->colorspace= get_symbol(c, state, 0, 7); //YUV cs type
    get_cabac(c, state); //no chroma = false
    f->chroma_h_shift= get_symbol(c, state, 0, 7);
    f->chroma_v_shift= get_symbol(c, state, 0, 7);
    get_cabac(c, state); //transparency plane
    f->plane_count= 2;

    if(f->colorspace==0){
        switch(16*f->chroma_h_shift + f->chroma_v_shift){
        case 0x00: f->avctx->pix_fmt= PIX_FMT_YUV444P; break;
        case 0x10: f->avctx->pix_fmt= PIX_FMT_YUV422P; break;
        case 0x11: f->avctx->pix_fmt= PIX_FMT_YUV420P; break;
        case 0x20: f->avctx->pix_fmt= PIX_FMT_YUV411P; break;
        case 0x33: f->avctx->pix_fmt= PIX_FMT_YUV410P; break;
        default:
            av_log(f->avctx, AV_LOG_ERROR, "format not supported\n");
            return -1;
        }
    }else if(f->colorspace==1){
        if(f->chroma_h_shift || f->chroma_v_shift){
            av_log(f->avctx, AV_LOG_ERROR, "chroma subsampling not supported in this colorspace\n");
            return -1;
        }
        f->avctx->pix_fmt= PIX_FMT_RGBA32;
    }else{
        av_log(f->avctx, AV_LOG_ERROR, "colorspace not supported\n");
        return -1;
    }

//printf("%d %d %d\n", f->chroma_h_shift, f->chroma_v_shift,f->avctx->pix_fmt);

    context_count=1;
    for(i=0; i<5; i++){
        context_count*= read_quant_table(c, f->quant_table[i], context_count);
        if(context_count < 0){
            av_log(f->avctx, AV_LOG_ERROR, "read_quant_table error\n");
            return -1;
        }
    }
    context_count= (context_count+1)/2;
    
    for(i=0; i<f->plane_count; i++){
        PlaneContext * const p= &f->plane[i];

        p->context_count= context_count;

        if(f->ac){
            if(!p->state) p->state= av_malloc(CONTEXT_SIZE*p->context_count*sizeof(uint8_t));
        }else{
            if(!p->vlc_state) p->vlc_state= av_malloc(p->context_count*sizeof(VlcState));
        }
    }
    
    return 0;
}

static int decode_init(AVCodecContext *avctx)
{
//    FFV1Context *s = avctx->priv_data;

    common_init(avctx);
    
    return 0;
}

static int decode_frame(AVCodecContext *avctx, void *data, int *data_size, uint8_t *buf, int buf_size){
    FFV1Context *f = avctx->priv_data;
    CABACContext * const c= &f->c;
    const int width= f->width;
    const int height= f->height;
    AVFrame * const p= &f->picture;
    int bytes_read;

    AVFrame *picture = data;

    *data_size = 0;

    /* no supplementary picture */
    if (buf_size == 0)
        return 0;

    ff_init_cabac_decoder(c, buf, buf_size);
    ff_init_cabac_states(c, ff_h264_lps_range, ff_h264_mps_state, ff_h264_lps_state, 64);

    p->pict_type= FF_I_TYPE; //FIXME I vs. P
    if(get_cabac_bypass(c)){
        p->key_frame= 1;
        read_header(f);
        clear_state(f);
    }else{
        p->key_frame= 0;
    }

    p->reference= 0;
    if(avctx->get_buffer(avctx, p) < 0){
        av_log(avctx, AV_LOG_ERROR, "get_buffer() failed\n");
        return -1;
    }

    if(avctx->debug&FF_DEBUG_PICT_INFO)
        av_log(avctx, AV_LOG_ERROR, "keyframe:%d coder:%d\n", p->key_frame, f->ac);
    
    if(!f->ac){
        bytes_read = get_cabac_terminate(c);
        if(bytes_read ==0) av_log(avctx, AV_LOG_ERROR, "error at end of AC stream\n");
//printf("pos=%d\n", bytes_read);
        init_get_bits(&f->gb, buf + bytes_read, buf_size - bytes_read);
    } else {
        bytes_read = 0; /* avoid warning */
    }
    
    if(f->colorspace==0){
        const int chroma_width = -((-width )>>f->chroma_h_shift);
        const int chroma_height= -((-height)>>f->chroma_v_shift);
        decode_plane(f, p->data[0], width, height, p->linesize[0], 0);
        
        decode_plane(f, p->data[1], chroma_width, chroma_height, p->linesize[1], 1);
        decode_plane(f, p->data[2], chroma_width, chroma_height, p->linesize[2], 1);
    }else{
        decode_rgb_frame(f, (uint32_t*)p->data[0], width, height, p->linesize[0]/4);
    }
        
    emms_c();

    f->picture_number++;

    *picture= *p;
    
    avctx->release_buffer(avctx, p); //FIXME

    *data_size = sizeof(AVFrame);
    
    if(f->ac){
        bytes_read= get_cabac_terminate(c);
        if(bytes_read ==0) av_log(f->avctx, AV_LOG_ERROR, "error at end of frame\n");
    }else{
        bytes_read+= (get_bits_count(&f->gb)+7)/8;
    }

    return bytes_read;
}

static int decode_end(AVCodecContext *avctx)
{
    FFV1Context *s = avctx->priv_data;
    int i;
    
    if(avctx->get_buffer == avcodec_default_get_buffer){
        for(i=0; i<4; i++){
            av_freep(&s->picture.base[i]);
            s->picture.data[i]= NULL;
        }
        av_freep(&s->picture.opaque);
    }

    return 0;
}

AVCodec ffv1_decoder = {
    "ffv1",
    CODEC_TYPE_VIDEO,
    CODEC_ID_FFV1,
    sizeof(FFV1Context),
    decode_init,
    NULL,
    decode_end,
    decode_frame,
    CODEC_CAP_DR1 /*| CODEC_CAP_DRAW_HORIZ_BAND*/,
    NULL
};

#ifdef CONFIG_ENCODERS
AVCodec ffv1_encoder = {
    "ffv1",
    CODEC_TYPE_VIDEO,
    CODEC_ID_FFV1,
    sizeof(FFV1Context),
    encode_init,
    encode_frame,
    encode_end,
};
#endif
