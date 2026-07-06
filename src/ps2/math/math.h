#pragma once
/* ================================================================================================
 * File: math.h
 * Brief: PS2-optimized math functions.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <float.h>
#include <math.h>

namespace ps2::math {

constexpr float kPI     = 3.1415926535897932384626433832795f;
constexpr float kTwoPI  = 6.283185307179586476925286766559f;
constexpr float kHalfPI = 1.5707963267948966192313216916398f;

// NOTE: Sine/cosine and angles are in radians as in the std lib counterparts.
float ASinf(float x);
float Cosf(float x);
float Fmodf(float x, float y);

inline float Fabsf(float x)
{
	float r;
	asm volatile (
		"abs.s %0, %1 \n\t"
		: "=&f" (r) : "f" (x)
	);
	return r;
}

inline float Minf(float a, float b)
{
	float r;
	asm volatile (
		"min.s %0, %1, %2 \n\t"
		: "=&f" (r) : "f" (a), "f" (b)
	);
	return r;
}

inline float Maxf(float a, float b)
{
	float r;
	asm volatile (
		"max.s %0, %1, %2 \n\t"
		: "=&f" (r) : "f" (a), "f" (b)
	);
	return r;
}

inline float Sqrtf(float x)
{
	float r;
	asm volatile (
		"sqrt.s %0, %1 \n\t"
		: "=&f" (r) : "f" (x)
	);
	return r;
}

inline float RSqrtf(float x)
{
	return 1.0f / Sqrtf(x);
}

inline float Sinf(float x)
{
	return Cosf(x - kHalfPI);
}

inline float ACosf(float x)
{
	return kHalfPI - ASinf(x);
}

inline int FloatEq(float a, float b, float tolerance)
{
	return Fabsf(a - b) < tolerance;
}

inline int FloatGE(float a, float b, float tolerance)
{
	return (a - b) > (-tolerance);
}

inline constexpr float DegToRad(float degrees)
{
	return degrees * (kPI / 180.0f);
}

inline constexpr float RadToDeg(float radians)
{
	return radians * (180.0f / kPI);
}

inline constexpr float MsecToSec(float ms)
{
    return ms * 0.001f;
}

inline constexpr float SecToMsec(float sec)
{
    return sec * 1000.0f;
}

} // namespace ps2::math
