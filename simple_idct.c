/*
 * Simple IDCT
 *
 * Copyright (c) 2001 Michael Niedermayer <michaelni@gmx.at>
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
/*
  based upon some outcommented c code from mpeg2dec (idct_mmx.c
  written by Aaron Holtzman <aholtzma@ess.engr.uvic.ca>) 
 */
#include "avcodec.h"
#include "dsputil.h"
#include "simple_idct.h"

//#define ARCH_ALPHA

#if 0
#define W1 2841 /* 2048*sqrt (2)*cos (1*pi/16) */
#define W2 2676 /* 2048*sqrt (2)*cos (2*pi/16) */
#define W3 2408 /* 2048*sqrt (2)*cos (3*pi/16) */
#define W4 2048 /* 2048*sqrt (2)*cos (4*pi/16) */
#define W5 1609 /* 2048*sqrt (2)*cos (5*pi/16) */
#define W6 1108 /* 2048*sqrt (2)*cos (6*pi/16) */
#define W7 565  /* 2048*sqrt (2)*cos (7*pi/16) */
#define ROW_SHIFT 8
#define COL_SHIFT 17
#else
#define W1  22725  //cos(i*M_PI/16)*sqrt(2)*(1<<14) + 0.5
#define W2  21407  //cos(i*M_PI/16)*sqrt(2)*(1<<14) + 0.5
#define W3  19266  //cos(i*M_PI/16)*sqrt(2)*(1<<14) + 0.5
#define W4  16383  //cos(i*M_PI/16)*sqrt(2)*(1<<14) + 0.5
#define W5  12873  //cos(i*M_PI/16)*sqrt(2)*(1<<14) + 0.5
#define W6  8867   //cos(i*M_PI/16)*sqrt(2)*(1<<14) + 0.5
#define W7  4520   //cos(i*M_PI/16)*sqrt(2)*(1<<14) + 0.5
#define ROW_SHIFT 11
#define COL_SHIFT 20 // 6
#endif

#ifdef ARCH_ALPHA
#define FAST_64BIT
#endif

#if defined(ARCH_POWERPC_405)

/* signed 16x16 -> 32 multiply add accumulate */
#define MAC16(rt, ra, rb) \
    asm ("maclhw %0, %2, %3" : "=r" (rt) : "0" (rt), "r" (ra), "r" (rb));

/* signed 16x16 -> 32 multiply */
#define MUL16(rt, ra, rb) \
    asm ("mullhw %0, %1, %2" : "=r" (rt) : "r" (ra), "r" (rb));

#else

/* signed 16x16 -> 32 multiply add accumulate */
#define MAC16(rt, ra, rb) rt += (ra) * (rb)

/* signed 16x16 -> 32 multiply */
#define MUL16(rt, ra, rb) rt = (ra) * (rb)

#endif

#ifdef ARCH_ALPHA
/* 0: all entries 0, 1: only first entry nonzero, 2: otherwise  */
static inline int idctRowCondDC(int16_t *row)
{
	int_fast32_t a0, a1, a2, a3, b0, b1, b2, b3;
	uint64_t *lrow = (uint64_t *) row;

	if (lrow[1] == 0) {
		if (lrow[0] == 0)
			return 0;
		if ((lrow[0] & ~0xffffULL) == 0) {
			uint64_t v;
#if 1			//is ok if |a0| < 1024 than theres an +-1 error (for the *W4 case for W4=16383 !!!)
			a0 = row[0]<<3;
#else
			a0 = W4 * row[0];
			a0 += 1 << (ROW_SHIFT - 1);
			a0 >>= ROW_SHIFT;
#endif
			v = (uint16_t) a0;
			v += v << 16;
			v += v << 32;
			lrow[0] = v;
			lrow[1] = v;

			return 1;
		}
	}

        a0 = (W4 * row[0]) + (1 << (ROW_SHIFT - 1));
	a1 = a0;
	a2 = a0;
	a3 = a0;

	if (row[2]) {
		a0 += W2 * row[2];
		a1 += W6 * row[2];
		a2 -= W6 * row[2];
		a3 -= W2 * row[2];
	}

	if (row[4]) {
		a0 += W4 * row[4];
		a1 -= W4 * row[4];
		a2 -= W4 * row[4];
		a3 += W4 * row[4];
	}

	if (row[6]) {
		a0 += W6 * row[6];
		a1 -= W2 * row[6];
		a2 += W2 * row[6];
		a3 -= W6 * row[6];
	}

	if (row[1]) {
		b0 = W1 * row[1];
		b1 = W3 * row[1];
		b2 = W5 * row[1];
		b3 = W7 * row[1];
	} else {
		b0 = 0;
		b1 = 0;
		b2 = 0;
		b3 = 0;
	}

	if (row[3]) {
		b0 += W3 * row[3];
		b1 -= W7 * row[3];
		b2 -= W1 * row[3];
		b3 -= W5 * row[3];
	}

	if (row[5]) {
		b0 += W5 * row[5];
		b1 -= W1 * row[5];
		b2 += W7 * row[5];
		b3 += W3 * row[5];
	}

	if (row[7]) {
		b0 += W7 * row[7];
		b1 -= W5 * row[7];
		b2 += W3 * row[7];
		b3 -= W1 * row[7];
	}

	row[0] = (a0 + b0) >> ROW_SHIFT;
	row[1] = (a1 + b1) >> ROW_SHIFT;
	row[2] = (a2 + b2) >> ROW_SHIFT;
	row[3] = (a3 + b3) >> ROW_SHIFT;
	row[4] = (a3 - b3) >> ROW_SHIFT;
	row[5] = (a2 - b2) >> ROW_SHIFT;
	row[6] = (a1 - b1) >> ROW_SHIFT;
	row[7] = (a0 - b0) >> ROW_SHIFT;

	return 2;
}

inline static void idctSparseCol2(int16_t *col)
{
        int a0, a1, a2, a3, b0, b1, b2, b3;

        col[0] += (1 << (COL_SHIFT - 1)) / W4;

        a0 = W4 * col[8 * 0];
        a1 = W4 * col[8 * 0];
        a2 = W4 * col[8 * 0];
        a3 = W4 * col[8 * 0];

        if (col[8 * 2]) {
                a0 += W2 * col[8 * 2];
                a1 += W6 * col[8 * 2];
                a2 -= W6 * col[8 * 2];
                a3 -= W2 * col[8 * 2];
        }

        if (col[8 * 4]) {
                a0 += W4 * col[8 * 4];
                a1 -= W4 * col[8 * 4];
                a2 -= W4 * col[8 * 4];
                a3 += W4 * col[8 * 4];
        }

        if (col[8 * 6]) {
                a0 += W6 * col[8 * 6];
                a1 -= W2 * col[8 * 6];
                a2 += W2 * col[8 * 6];
                a3 -= W6 * col[8 * 6];
        }

        if (col[8 * 1]) {
                b0 = W1 * col[8 * 1];
                b1 = W3 * col[8 * 1];
                b2 = W5 * col[8 * 1];
                b3 = W7 * col[8 * 1];
        } else {
                b0 = b1 = b2 = b3 = 0;
        }

        if (col[8 * 3]) {
                b0 += W3 * col[8 * 3];
                b1 -= W7 * col[8 * 3];
                b2 -= W1 * col[8 * 3];
                b3 -= W5 * col[8 * 3];
        }

        if (col[8 * 5]) {
                b0 += W5 * col[8 * 5];
                b1 -= W1 * col[8 * 5];
                b2 += W7 * col[8 * 5];
                b3 += W3 * col[8 * 5];
        }

        if (col[8 * 7]) {
                b0 += W7 * col[8 * 7];
                b1 -= W5 * col[8 * 7];
                b2 += W3 * col[8 * 7];
                b3 -= W1 * col[8 * 7];
        }

        col[8 * 0] = (a0 + b0) >> COL_SHIFT;
        col[8 * 7] = (a0 - b0) >> COL_SHIFT;
        col[8 * 1] = (a1 + b1) >> COL_SHIFT;
        col[8 * 6] = (a1 - b1) >> COL_SHIFT;
        col[8 * 2] = (a2 + b2) >> COL_SHIFT;
        col[8 * 5] = (a2 - b2) >> COL_SHIFT;
        col[8 * 3] = (a3 + b3) >> COL_SHIFT;
        col[8 * 4] = (a3 - b3) >> COL_SHIFT;
}

#else  /* not ARCH_ALPHA */

static inline void idctRowCondDC (int16_t * row)
{
	int a0, a1, a2, a3, b0, b1, b2, b3;
#ifdef FAST_64BIT
        uint64_t temp;
#else
        uint32_t temp;
#endif

#ifdef FAST_64BIT
#ifdef WORDS_BIGENDIAN
#define ROW0_MASK 0xffff000000000000LL
#else
#define ROW0_MASK 0xffffLL
#endif
	if ( ((((uint64_t *)row)[0] & ~ROW0_MASK) | 
              ((uint64_t *)row)[1]) == 0) {
            temp = (row[0] << 3) & 0xffff;
            temp += temp << 16;
            temp += temp << 32;
            ((uint64_t *)row)[0] = temp;
            ((uint64_t *)row)[1] = temp;
            return;
	}
#else
	if (!(((uint32_t*)row)[1] |
              ((uint32_t*)row)[2] |
              ((uint32_t*)row)[3] | 
              row[1])) {
            temp = (row[0] << 3) & 0xffff;
            temp += temp << 16;
            ((uint32_t*)row)[0]=((uint32_t*)row)[1] =
		((uint32_t*)row)[2]=((uint32_t*)row)[3] = temp;
		return;
	}
#endif

        a0 = (W4 * row[0]) + (1 << (ROW_SHIFT - 1));
	a1 = a0;
	a2 = a0;
	a3 = a0;

        /* no need to optimize : gcc does it */
        a0 += W2 * row[2];
        a1 += W6 * row[2];
        a2 -= W6 * row[2];
        a3 -= W2 * row[2];

        MUL16(b0, W1, row[1]);
        MAC16(b0, W3, row[3]);
        MUL16(b1, W3, row[1]);
        MAC16(b1, -W7, row[3]);
        MUL16(b2, W5, row[1]);
        MAC16(b2, -W1, row[3]);
        MUL16(b3, W7, row[1]);
        MAC16(b3, -W5, row[3]);

#ifdef FAST_64BIT
        temp = ((uint64_t*)row)[1];
#else
        temp = ((uint32_t*)row)[2] | ((uint32_t*)row)[3];
#endif
	if (temp != 0) {
            a0 += W4*row[4] + W6*row[6];
            a1 += - W4*row[4] - W2*row[6];
            a2 += - W4*row[4] + W2*row[6];
            a3 += W4*row[4] - W6*row[6];

            MAC16(b0, W5, row[5]);
            MAC16(b0, W7, row[7]);
            
            MAC16(b1, -W1, row[5]);
            MAC16(b1, -W5, row[7]);
            
            MAC16(b2, W7, row[5]);
            MAC16(b2, W3, row[7]);
            
            MAC16(b3, W3, row[5]);
            MAC16(b3, -W1, row[7]);
	}

	row[0] = (a0 + b0) >> ROW_SHIFT;
	row[7] = (a0 - b0) >> ROW_SHIFT;
	row[1] = (a1 + b1) >> ROW_SHIFT;
	row[6] = (a1 - b1) >> ROW_SHIFT;
	row[2] = (a2 + b2) >> ROW_SHIFT;
	row[5] = (a2 - b2) >> ROW_SHIFT;
	row[3] = (a3 + b3) >> ROW_SHIFT;
	row[4] = (a3 - b3) >> ROW_SHIFT;
}
#endif /* not ARCH_ALPHA */

static inline void idctSparseColPut (UINT8 *dest, int line_size, 
                                     int16_t * col)
{
	int a0, a1, a2, a3, b0, b1, b2, b3;
        UINT8 *cm = cropTbl + MAX_NEG_CROP;

        /* XXX: I did that only to give same values as previous code */
	a0 = W4 * (col[8*0] + ((1<<(COL_SHIFT-1))/W4));
	a1 = a0;
	a2 = a0;
	a3 = a0;

        a0 +=  + W2*col[8*2];
        a1 +=  + W6*col[8*2];
        a2 +=  - W6*col[8*2];
        a3 +=  - W2*col[8*2];

        MUL16(b0, W1, col[8*1]);
        MUL16(b1, W3, col[8*1]);
        MUL16(b2, W5, col[8*1]);
        MUL16(b3, W7, col[8*1]);

        MAC16(b0, + W3, col[8*3]);
        MAC16(b1, - W7, col[8*3]);
        MAC16(b2, - W1, col[8*3]);
        MAC16(b3, - W5, col[8*3]);

	if(col[8*4]){
            a0 += + W4*col[8*4];
            a1 += - W4*col[8*4];
            a2 += - W4*col[8*4];
            a3 += + W4*col[8*4];
	}

	if (col[8*5]) {
            MAC16(b0, + W5, col[8*5]);
            MAC16(b1, - W1, col[8*5]);
            MAC16(b2, + W7, col[8*5]);
            MAC16(b3, + W3, col[8*5]);
	}

	if(col[8*6]){
            a0 += + W6*col[8*6];
            a1 += - W2*col[8*6];
            a2 += + W2*col[8*6];
            a3 += - W6*col[8*6];
	}

	if (col[8*7]) {
            MAC16(b0, + W7, col[8*7]);
            MAC16(b1, - W5, col[8*7]);
            MAC16(b2, + W3, col[8*7]);
            MAC16(b3, - W1, col[8*7]);
	}

        dest[0] = cm[(a0 + b0) >> COL_SHIFT];
        dest += line_size;
        dest[0] = cm[(a1 + b1) >> COL_SHIFT];
        dest += line_size;
        dest[0] = cm[(a2 + b2) >> COL_SHIFT];
        dest += line_size;
        dest[0] = cm[(a3 + b3) >> COL_SHIFT];
        dest += line_size;
        dest[0] = cm[(a3 - b3) >> COL_SHIFT];
        dest += line_size;
        dest[0] = cm[(a2 - b2) >> COL_SHIFT];
        dest += line_size;
        dest[0] = cm[(a1 - b1) >> COL_SHIFT];
        dest += line_size;
        dest[0] = cm[(a0 - b0) >> COL_SHIFT];
}

static inline void idctSparseColAdd (UINT8 *dest, int line_size, 
                                     int16_t * col)
{
	int a0, a1, a2, a3, b0, b1, b2, b3;
        UINT8 *cm = cropTbl + MAX_NEG_CROP;

        /* XXX: I did that only to give same values as previous code */
	a0 = W4 * (col[8*0] + ((1<<(COL_SHIFT-1))/W4));
	a1 = a0;
	a2 = a0;
	a3 = a0;

        a0 +=  + W2*col[8*2];
        a1 +=  + W6*col[8*2];
        a2 +=  - W6*col[8*2];
        a3 +=  - W2*col[8*2];

        MUL16(b0, W1, col[8*1]);
        MUL16(b1, W3, col[8*1]);
        MUL16(b2, W5, col[8*1]);
        MUL16(b3, W7, col[8*1]);

        MAC16(b0, + W3, col[8*3]);
        MAC16(b1, - W7, col[8*3]);
        MAC16(b2, - W1, col[8*3]);
        MAC16(b3, - W5, col[8*3]);

	if(col[8*4]){
            a0 += + W4*col[8*4];
            a1 += - W4*col[8*4];
            a2 += - W4*col[8*4];
            a3 += + W4*col[8*4];
	}

	if (col[8*5]) {
            MAC16(b0, + W5, col[8*5]);
            MAC16(b1, - W1, col[8*5]);
            MAC16(b2, + W7, col[8*5]);
            MAC16(b3, + W3, col[8*5]);
	}

	if(col[8*6]){
            a0 += + W6*col[8*6];
            a1 += - W2*col[8*6];
            a2 += + W2*col[8*6];
            a3 += - W6*col[8*6];
	}

	if (col[8*7]) {
            MAC16(b0, + W7, col[8*7]);
            MAC16(b1, - W5, col[8*7]);
            MAC16(b2, + W3, col[8*7]);
            MAC16(b3, - W1, col[8*7]);
	}

        dest[0] = cm[dest[0] + ((a0 + b0) >> COL_SHIFT)];
        dest += line_size;
        dest[0] = cm[dest[0] + ((a1 + b1) >> COL_SHIFT)];
        dest += line_size;
        dest[0] = cm[dest[0] + ((a2 + b2) >> COL_SHIFT)];
        dest += line_size;
        dest[0] = cm[dest[0] + ((a3 + b3) >> COL_SHIFT)];
        dest += line_size;
        dest[0] = cm[dest[0] + ((a3 - b3) >> COL_SHIFT)];
        dest += line_size;
        dest[0] = cm[dest[0] + ((a2 - b2) >> COL_SHIFT)];
        dest += line_size;
        dest[0] = cm[dest[0] + ((a1 - b1) >> COL_SHIFT)];
        dest += line_size;
        dest[0] = cm[dest[0] + ((a0 - b0) >> COL_SHIFT)];
}

static inline void idctSparseCol (int16_t * col)
{
	int a0, a1, a2, a3, b0, b1, b2, b3;

        /* XXX: I did that only to give same values as previous code */
	a0 = W4 * (col[8*0] + ((1<<(COL_SHIFT-1))/W4));
	a1 = a0;
	a2 = a0;
	a3 = a0;

        a0 +=  + W2*col[8*2];
        a1 +=  + W6*col[8*2];
        a2 +=  - W6*col[8*2];
        a3 +=  - W2*col[8*2];

        MUL16(b0, W1, col[8*1]);
        MUL16(b1, W3, col[8*1]);
        MUL16(b2, W5, col[8*1]);
        MUL16(b3, W7, col[8*1]);

        MAC16(b0, + W3, col[8*3]);
        MAC16(b1, - W7, col[8*3]);
        MAC16(b2, - W1, col[8*3]);
        MAC16(b3, - W5, col[8*3]);

	if(col[8*4]){
            a0 += + W4*col[8*4];
            a1 += - W4*col[8*4];
            a2 += - W4*col[8*4];
            a3 += + W4*col[8*4];
	}

	if (col[8*5]) {
            MAC16(b0, + W5, col[8*5]);
            MAC16(b1, - W1, col[8*5]);
            MAC16(b2, + W7, col[8*5]);
            MAC16(b3, + W3, col[8*5]);
	}

	if(col[8*6]){
            a0 += + W6*col[8*6];
            a1 += - W2*col[8*6];
            a2 += + W2*col[8*6];
            a3 += - W6*col[8*6];
	}

	if (col[8*7]) {
            MAC16(b0, + W7, col[8*7]);
            MAC16(b1, - W5, col[8*7]);
            MAC16(b2, + W3, col[8*7]);
            MAC16(b3, - W1, col[8*7]);
	}

        col[0 ] = ((a0 + b0) >> COL_SHIFT);
        col[8 ] = ((a1 + b1) >> COL_SHIFT);
        col[16] = ((a2 + b2) >> COL_SHIFT);
        col[24] = ((a3 + b3) >> COL_SHIFT);
        col[32] = ((a3 - b3) >> COL_SHIFT);
        col[40] = ((a2 - b2) >> COL_SHIFT);
        col[48] = ((a1 - b1) >> COL_SHIFT);
        col[56] = ((a0 - b0) >> COL_SHIFT);
}


#ifdef ARCH_ALPHA
/* If all rows but the first one are zero after row transformation,
   all rows will be identical after column transformation.  */
static inline void idctCol2(int16_t *col)
{
	int i;
	uint64_t l, r;
	uint64_t *lcol = (uint64_t *) col;

	for (i = 0; i < 8; ++i) {
		int a0 = col[0] + (1 << (COL_SHIFT - 1)) / W4;

		a0 *= W4;
		col[0] = a0 >> COL_SHIFT;
		++col;
	}

	l = lcol[0];
	r = lcol[1];
	lcol[ 2] = l; lcol[ 3] = r;
	lcol[ 4] = l; lcol[ 5] = r;
	lcol[ 6] = l; lcol[ 7] = r;
	lcol[ 8] = l; lcol[ 9] = r;
	lcol[10] = l; lcol[11] = r;
	lcol[12] = l; lcol[13] = r;
	lcol[14] = l; lcol[15] = r;
}

void simple_idct (short *block)
{

	int i;
        int rowsZero = 1;       /* all rows except row 0 zero */
        int rowsConstant = 1;	/* all rows consist of a constant value */

	for (i = 0; i < 8; i++) {
		int sparseness = idctRowCondDC(block + 8 * i);

		if (i > 0 && sparseness > 0)
                        rowsZero = 0;
                if (sparseness == 2)
                        rowsConstant = 0;
	}

        if (rowsZero) {
                idctCol2(block);
        } else if (rowsConstant) {
		uint64_t *lblock = (uint64_t *) block;

		idctSparseCol2(block);
		for (i = 0; i < 8; i++) {
			uint64_t v = (uint16_t) block[i * 8];

			v += v << 16;
			v += v << 32;
			lblock[0] = v;
			lblock[1] = v;
			lblock += 2;
		}
	} else {
		for (i = 0; i < 8; i++)
			idctSparseCol2(block + i);
	}
}

/* XXX: suppress this mess */
void simple_idct_put(UINT8 *dest, int line_size, DCTELEM *block)
{
    simple_idct(block);
    put_pixels_clamped(block, dest, line_size);
}

void simple_idct_add(UINT8 *dest, int line_size, DCTELEM *block)
{
    simple_idct(block);
    add_pixels_clamped(block, dest, line_size);
}

#else

void simple_idct_put(UINT8 *dest, int line_size, INT16 *block)
{
    int i;
    for(i=0; i<8; i++)
        idctRowCondDC(block + i*8);
    
    for(i=0; i<8; i++)
        idctSparseColPut(dest + i, line_size, block + i);
}

void simple_idct_add(UINT8 *dest, int line_size, INT16 *block)
{
    int i;
    for(i=0; i<8; i++)
        idctRowCondDC(block + i*8);
    
    for(i=0; i<8; i++)
        idctSparseColAdd(dest + i, line_size, block + i);
}

void simple_idct(INT16 *block)
{
    int i;
    for(i=0; i<8; i++)
        idctRowCondDC(block + i*8);
    
    for(i=0; i<8; i++)
        idctSparseCol(block + i);
}

#endif

/* 2x4x8 idct */

#define CN_SHIFT 12
#define C_FIX(x) ((int)((x) * (1 << CN_SHIFT) + 0.5))
#define C0 C_FIX(0.7071067811)
#define C1 C_FIX(0.9238795324)
#define C2 C_FIX(0.3826834324)

/* row idct is multiple by 16 * sqrt(2.0), col idct4 is multiplied by
   sqrt(2). An extra division by two is needed for the first butterfly
   stage */
#define C_SHIFT (4+1+12+1)

static inline void idct4col(UINT8 *dest, int line_size, const INT16 *col)
{
    int c0, c1, c2, c3, a0, a1, a2, a3;
    const UINT8 *cm = cropTbl + MAX_NEG_CROP;

    a0 = col[8*0];
    a1 = col[8*2];
    a2 = col[8*4];
    a3 = col[8*6];
    c0 = (a0 + a2) * C0 + (1 << (C_SHIFT - 1)) + (128 << C_SHIFT);
    c2 = (a0 - a2) * C0 + (1 << (C_SHIFT - 1)) + (128 << C_SHIFT);
    c1 = a1 * C1 + a3 * C2;
    c3 = a1 * C2 - a3 * C1;
    dest[0] = cm[(c0 + c1) >> C_SHIFT];
    dest += line_size;
    dest[0] = cm[(c2 + c3) >> C_SHIFT];
    dest += line_size;
    dest[0] = cm[(c2 - c3) >> C_SHIFT];
    dest += line_size;
    dest[0] = cm[(c0 - c1) >> C_SHIFT];
}

#define BF(k) \
{\
    int a0, a1;\
    a0 = ptr[k];\
    a1 = ptr[8 + k];\
    ptr[k] = a0 + a1;\
    ptr[8 + k] = a0 - a1;\
}

/* only used by DV codec. The input must be interlaced. 128 is added
   to the pixels before clamping to avoid systematic error
   (1024*sqrt(2)) offset would be needed otherwise. */
/* XXX: I think a 1.0/sqrt(2) normalization should be needed to
   compensate the extra butterfly stage - I don't have the full DV
   specification */
void simple_idct248_put(UINT8 *dest, int line_size, INT16 *block)
{
    int i;
    INT16 *ptr;
    
    /* butterfly */
    ptr = block;
    for(i=0;i<4;i++) {
        BF(0);
        BF(1);
        BF(2);
        BF(3);
        BF(4);
        BF(5);
        BF(6);
        BF(7);
        ptr += 2 * 8;
    }

    /* IDCT8 on each line */
    for(i=0; i<8; i++) {
        idctRowCondDC(block + i*8);
    }

    /* IDCT4 and store */
    for(i=0;i<8;i++) {
        idct4col(dest + i, 2 * line_size, block + i);
        idct4col(dest + line_size + i, 2 * line_size, block + 8 + i);
    }
}
