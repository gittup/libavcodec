#ifndef DSPUTIL_H
#define DSPUTIL_H

#include "common.h"

/* dct code */
typedef short DCTELEM;

void jpeg_fdct_ifast (DCTELEM *data);

void j_rev_dct (DCTELEM *data);

void fdct_mmx(DCTELEM *block);

void (*av_fdct)(DCTELEM *block);

/* encoding scans */
extern UINT8 ff_alternate_horizontal_scan[64];
extern UINT8 ff_alternate_vertical_scan[64];
extern UINT8 zigzag_direct[64];

/* pixel operations */
#define MAX_NEG_CROP 384

/* temporary */
extern UINT32 squareTbl[512];
extern UINT8 cropTbl[256 + 2 * MAX_NEG_CROP];

void dsputil_init(void);

/* pixel ops : interface with DCT */

extern void (*ff_idct)(DCTELEM *block);
extern void (*get_pixels)(DCTELEM *block, const UINT8 *pixels, int line_size);
extern void (*put_pixels_clamped)(const DCTELEM *block, UINT8 *pixels, int line_size);
extern void (*add_pixels_clamped)(const DCTELEM *block, UINT8 *pixels, int line_size);

void get_pixels_c(DCTELEM *block, const UINT8 *pixels, int line_size);
void put_pixels_clamped_c(const DCTELEM *block, UINT8 *pixels, int line_size);
void add_pixels_clamped_c(const DCTELEM *block, UINT8 *pixels, int line_size);

/* add and put pixel (decoding) */
typedef void (*op_pixels_func)(UINT8 *block, const UINT8 *pixels, int line_size, int h);

extern op_pixels_func put_pixels_tab[4];
extern op_pixels_func avg_pixels_tab[4];
extern op_pixels_func put_no_rnd_pixels_tab[4];
extern op_pixels_func avg_no_rnd_pixels_tab[4];

/* sub pixel (encoding) */
extern void (*sub_pixels_tab[4])(DCTELEM *block, const UINT8 *pixels, int line_size, int h);

#define sub_pixels_2(block, pixels, line_size, dxy) \
   sub_pixels_tab[dxy](block, pixels, line_size, 8)

/* motion estimation */

typedef int (*op_pixels_abs_func)(UINT8 *blk1, UINT8 *blk2, int line_size, int h);

extern op_pixels_abs_func pix_abs16x16;
extern op_pixels_abs_func pix_abs16x16_x2;
extern op_pixels_abs_func pix_abs16x16_y2;
extern op_pixels_abs_func pix_abs16x16_xy2;

int pix_abs16x16_c(UINT8 *blk1, UINT8 *blk2, int lx, int h);
int pix_abs16x16_x2_c(UINT8 *blk1, UINT8 *blk2, int lx, int h);
int pix_abs16x16_y2_c(UINT8 *blk1, UINT8 *blk2, int lx, int h);
int pix_abs16x16_xy2_c(UINT8 *blk1, UINT8 *blk2, int lx, int h);

#if defined (SIMPLE_IDCT) && defined (HAVE_MMX)
static inline int block_permute_op(int j)
{
static const int table[64]={
	0x00, 0x08, 0x01, 0x09, 0x04, 0x0C, 0x05, 0x0D,
	0x10, 0x18, 0x11, 0x19, 0x14, 0x1C, 0x15, 0x1D,
	0x02, 0x0A, 0x03, 0x0B, 0x06, 0x0E, 0x07, 0x0F,
	0x12, 0x1A, 0x13, 0x1B, 0x16, 0x1E, 0x17, 0x1F,
	0x20, 0x28, 0x21, 0x29, 0x24, 0x2C, 0x25, 0x2D,
	0x30, 0x38, 0x31, 0x39, 0x34, 0x3C, 0x35, 0x3D,
	0x22, 0x2A, 0x23, 0x2B, 0x26, 0x2E, 0x27, 0x2F,
	0x32, 0x3A, 0x33, 0x3B, 0x36, 0x3E, 0x37, 0x3F,
};

	return table[j];
}
#elif defined (SIMPLE_IDCT)
static inline int block_permute_op(int j)
{
    return j;
}
#else
static inline int block_permute_op(int j)
{
    return (j & 0x38) | ((j & 6) >> 1) | ((j & 1) << 2);
}
#endif

void block_permute(INT16 *block);

#if defined(HAVE_MMX)

#define MM_MMX    0x0001 /* standard MMX */
#define MM_3DNOW  0x0004 /* AMD 3DNOW */
#define MM_MMXEXT 0x0002 /* SSE integer functions or AMD MMX ext */
#define MM_SSE    0x0008 /* SSE functions */
#define MM_SSE2   0x0010 /* PIV SSE2 functions */

extern int mm_flags;

int mm_support(void);

static inline void emms(void)
{
    __asm __volatile ("emms;":::"memory");
}

#define emms_c() \
{\
    if (mm_flags & MM_MMX)\
        emms();\
}

#define __align8 __attribute__ ((aligned (8)))

void dsputil_init_mmx(void);

#elif defined(ARCH_ARMV4L)

#define emms_c()

/* This is to use 4 bytes read to the IDCT pointers for some 'zero'
   line ptimizations */
#define __align8 __attribute__ ((aligned (4)))

void dsputil_init_armv4l(void);   

#elif defined(HAVE_MLIB)
 
#define emms_c()

/* SPARC/VIS IDCT needs 8-byte aligned DCT blocks */
#define __align8 __attribute__ ((aligned (8)))

void dsputil_init_mlib(void);   

#else

#define emms_c()

#define __align8

#endif

#endif
