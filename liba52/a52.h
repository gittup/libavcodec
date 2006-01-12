/*
 * a52.h
 * Copyright (C) 2000-2003 Michel Lespinasse <walken@zoy.org>
 * Copyright (C) 1999-2000 Aaron Holtzman <aholtzma@ess.engr.uvic.ca>
 *
 * This file is part of a52dec, a free ATSC A-52 stream decoder.
 * See http://liba52.sourceforge.net/ for updates.
 *
 * a52dec is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * a52dec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef A52_H
#define A52_H

#include "../avcodec.h"

#undef malloc
#undef free
#undef realloc

#if defined(LIBA52_FIXED)
typedef int32_t sample_t;
typedef int32_t level_t;
#elif defined(LIBA52_DOUBLE)
typedef double sample_t;
typedef double level_t;
#else
typedef float sample_t;
typedef float level_t;
#endif

typedef struct a52_state_s a52_state_t;

#define A52_CHANNEL 0
#define A52_MONO 1
#define A52_STEREO 2
#define A52_3F 3
#define A52_2F1R 4
#define A52_3F1R 5
#define A52_2F2R 6
#define A52_3F2R 7
#define A52_CHANNEL1 8
#define A52_CHANNEL2 9
#define A52_DOLBY 10
#define A52_CHANNEL_MASK 15

#define A52_LFE 16
#define A52_ADJUST_LEVEL 32

a52_state_t * a52_init (uint32_t mm_accel);
sample_t * a52_samples (a52_state_t * state);
int a52_syncinfo (uint8_t * buf, int * flags,
		  int * sample_rate, int * bit_rate);
int a52_frame (a52_state_t * state, uint8_t * buf, int * flags,
	       level_t * level, sample_t bias);
void a52_dynrng (a52_state_t * state,
		 level_t (* call) (level_t, void *), void * data);
int a52_block (a52_state_t * state);
void a52_free (a52_state_t * state);

#endif /* A52_H */
