/*
    Copyright (C) 2001-2002 Michael Niedermayer (michaelni@gmx.at)

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#define V_DEBLOCK	0x01
#define H_DEBLOCK	0x02
#define DERING		0x04
#define LEVEL_FIX	0x08 /* Brightness & Contrast */

#define LUM_V_DEBLOCK	V_DEBLOCK		//   1
#define LUM_H_DEBLOCK	H_DEBLOCK		//   2
#define CHROM_V_DEBLOCK	(V_DEBLOCK<<4)		//  16
#define CHROM_H_DEBLOCK	(H_DEBLOCK<<4)		//  32
#define LUM_DERING	DERING			//   4
#define CHROM_DERING	(DERING<<4)		//  64
#define LUM_LEVEL_FIX	LEVEL_FIX		//   8
#define CHROM_LEVEL_FIX	(LEVEL_FIX<<4)		// 128 (not implemented yet)

// Experimental vertical filters
#define V_X1_FILTER	0x0200			// 512

// Experimental horizontal filters
#define H_X1_FILTER	0x2000			// 8192

// select between full y range (255-0) or standart one (234-16)
#define FULL_Y_RANGE	0x8000			// 32768

//Deinterlacing Filters
#define	LINEAR_IPOL_DEINT_FILTER	0x10000	// 65536
#define	LINEAR_BLEND_DEINT_FILTER	0x20000	// 131072
#define	CUBIC_BLEND_DEINT_FILTER	0x8000	// (not implemented yet)
#define	CUBIC_IPOL_DEINT_FILTER		0x40000	// 262144
#define	MEDIAN_DEINT_FILTER		0x80000	// 524288
#define	FFMPEG_DEINT_FILTER		0x400000

#define TEMP_NOISE_FILTER		0x100000
#define FORCE_QUANT			0x200000

//use if u want a faster postprocessing code
//cant differentiate between chroma & luma filters (both on or both off)
//obviosly the -pp option at the commandline has no effect except turning the here selected
//filters on
//#define COMPILE_TIME_MODE 0x77

struct PPFilter{
	char *shortName;
	char *longName;
	int chromDefault; 	// is chrominance filtering on by default if this filter is manually activated
	int minLumQuality; 	// minimum quality to turn luminance filtering on
	int minChromQuality;	// minimum quality to turn chrominance filtering on
	int mask; 		// Bitmask to turn this filter on
};

typedef struct PPMode{
	int lumMode; 			// acivates filters for luminance
	int chromMode; 			// acivates filters for chrominance
	int error; 			// non zero on error

	int minAllowedY; 		// for brigtness correction
	int maxAllowedY; 		// for brihtness correction
	float maxClippedThreshold;	// amount of "black" u r willing to loose to get a brightness corrected picture

	int maxTmpNoise[3]; 		// for Temporal Noise Reducing filter (Maximal sum of abs differences)

	int baseDcDiff;
	int flatnessThreshold;

	int forcedQuant; 		// quantizer if FORCE_QUANT is used
} PPMode;

typedef struct PPContext{
	uint8_t *tempBlocks; //used for the horizontal code

	/* we need 64bit here otherwise we�ll going to have a problem
	   after watching a black picture for 5 hours*/
	uint64_t *yHistogram;

	uint64_t __attribute__((aligned(8))) packedYOffset;
	uint64_t __attribute__((aligned(8))) packedYScale;

	/* Temporal noise reducing buffers */
	uint8_t *tempBlured[3];
	int32_t *tempBluredPast[3];

	/* Temporary buffers for handling the last row(s) */
	uint8_t *tempDst;
	uint8_t *tempSrc;

	uint8_t *deintTemp;

	uint64_t __attribute__((aligned(8))) pQPb;
	uint64_t __attribute__((aligned(8))) pQPb2;

	uint64_t __attribute__((aligned(8))) mmxDcOffset[32];
	uint64_t __attribute__((aligned(8))) mmxDcThreshold[32];

	QP_STORE_T *nonBQPTable;
	QP_STORE_T *forcedQPTable;

	int QP;
	int nonBQP;

	int frameNum;
	
	int cpuCaps;
        
	int stride; //size of some buffers (needed to realloc them if needed)
        
	int hChromaSubSample;
	int vChromaSubSample;

	PPMode ppMode;
} PPContext;

