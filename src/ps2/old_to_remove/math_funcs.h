
/* ================================================================================================
 * -*- C -*-
 * File: math_funcs.h
 * Author: Guilherme R. Lampert
 * Created on: 13/10/15
 * Brief: Optimized math functions for the PS2 and replacements for the
 *        C-library double precision ones that are not available on PlayStation 2.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#ifndef PS2_MATH_FUNCS_H
#define PS2_MATH_FUNCS_H

#include <float.h>
#include <math.h>

#define PS2MATH_PI     3.1415926535897932384626433832795f
#define PS2MATH_TWOPI  6.283185307179586476925286766559f
#define PS2MATH_HALFPI 1.5707963267948966192313216916398f

// NOTE: sine/cosine and angles are in radians.
float ps2_asinf(float x);
float ps2_cosf(float x);
float ps2_fmodf(float x, float y);

static inline float ps2_fabsf(float x)
{
	float r;
	asm volatile (
		"abs.s %0, %1 \n\t"
		: "=&f" (r) : "f" (x)
	);
	return r;
}

static inline float ps2_minf(float a, float b)
{
	float r;
	asm volatile (
		"min.s %0, %1, %2 \n\t"
		: "=&f" (r) : "f" (a), "f" (b)
	);
	return r;
}

static inline float ps2_maxf(float a, float b)
{
	float r;
	asm volatile (
		"max.s %0, %1, %2 \n\t"
		: "=&f" (r) : "f" (a), "f" (b)
	);
	return r;
}

static inline float ps2_sqrtf(float x)
{
	float r;
	asm volatile (
		"sqrt.s %0, %1 \n\t"
		: "=&f" (r) : "f" (x)
	);
	return r;
}

static inline float ps2_rsqrtf(float x)
{
	return 1.0f / ps2_sqrtf(x);
}

static inline float ps2_sinf(float x)
{
	return ps2_cosf(x - PS2MATH_HALFPI);
}

static inline float ps2_acosf(float x)
{
	return PS2MATH_HALFPI - ps2_asinf(x);
}

static inline int ps2_float_equals(float a, float b, float tolerance)
{
	return ps2_fabsf(a - b) < tolerance;
}

static inline int ps2_float_greater_equal(float a, float b, float tolerance)
{
	return (a - b) > (-tolerance);
}

static inline float ps2_deg_to_rad(float degrees)
{
	return degrees * (PS2MATH_PI / 180.0f);
}

static inline float ps2_rad_to_deg(float radians)
{
	return radians * (180.0f / PS2MATH_PI);
}

static inline float ps2_msec_to_sec(float ms)
{
    return ms * 0.001f;
}

static inline float ps2_sec_to_msec(float sec)
{
    return sec * 1000.0f;
}

/*
 * PS2 doesn't support double-precision, so all the
 * default math.h functions are not defined, instead
 * only the single-precision ones are (float, f suffix).
 * This hack fixes the problem in a quick-'n-dirty way.
 */
/*test
#define sin   ps2_sinf
#define cos   ps2_cosf
#define acos  ps2_acosf
#define asin  ps2_asinf
#define sqrt  ps2_sqrtf
#define rsqrt ps2_rsqrtf
#define fmod  ps2_fmodf
#define tan   tanf
#define atan  atanf
#define atan2 atan2f
#define floor floorf
#define ceil  ceilf
*/

#endif // PS2_MATH_FUNCS_H
