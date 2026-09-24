/* ================================================================================================
 * File: math_c.h
 * Brief: Single-precision math shared by the C engine and ps2::math (math.h).
 *        NOTE: Shared header between C and C++.
 *
 *        The EE's FPU is single precision only. Double arithmetic goes through libgcc's soft-float
 *        routines, and libm's sin/cos/sqrt/floor all take and return double, so every engine call
 *        to one paid for a software double implementation plus the conversions either side of it.
 *        The engine calls these instead.
 *
 *        Inline rather than out of line: most callers want several results at once (sine and
 *        cosine of three angles, or one per particle in a loop), and inline lets GCC load the
 *        constants once, interleave the independent calls and drop the ones whose result is
 *        never read - which matters more than the instruction count of any single call.
 *
 *        Written as C89 (the engine builds as -std=gnu89) that also passes the C++ backend's
 *        warnings. Only the four FPU instructions GCC will not emit on its own are asm, and they
 *        fall back to libm off the EE so a host harness can include this header.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#ifndef PS2_MATH_MATH_C_H
#define PS2_MATH_MATH_C_H

#ifndef _EE
#include <math.h>
#endif /* _EE */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* ------------------------------------------------------------------------------------------------
 * FPU primitives
 *
 * GCC keeps fabsf to integer bit masking under the legacy NaN encoding, and wraps sqrt.s in an
 * errno check for negative inputs. Neither applies to the EE, which has no NaN to preserve and
 * returns sqrt(|x|) for a negative input.
 * ------------------------------------------------------------------------------------------------ */

static inline float PS2Quake_Fabsf(float x)
{
#ifdef _EE
    float r;
    __asm__ ("abs.s %0, %1" : "=f" (r) : "f" (x));
    return r;
#else
    return fabsf(x);
#endif
}

static inline float PS2Quake_Sqrtf(float x)
{
#ifdef _EE
    float r;
    __asm__ ("sqrt.s %0, %1" : "=f" (r) : "f" (x));
    return r;
#else
    return sqrtf(x);
#endif
}

static inline float PS2Quake_Minf(float a, float b)
{
#ifdef _EE
    float r;
    __asm__ ("min.s %0, %1, %2" : "=f" (r) : "f" (a), "f" (b));
    return r;
#else
    return fminf(a, b);
#endif
}

static inline float PS2Quake_Maxf(float a, float b)
{
#ifdef _EE
    float r;
    __asm__ ("max.s %0, %1, %2" : "=f" (r) : "f" (a), "f" (b));
    return r;
#else
    return fmaxf(a, b);
#endif
}

/* ------------------------------------------------------------------------------------------------
 * Rounding
 *
 * The float to int conversion truncates (trunc.w.s, which is the EE's cvt.w.s). A float of
 * magnitude 2^23 or more is already an integer, and one of 2^31 or more would saturate the
 * conversion, so those come back unchanged.
 * ------------------------------------------------------------------------------------------------ */

static inline float PS2Quake_Truncf(float x)
{
    return PS2Quake_Fabsf(x) < 8388608.0f ? (float)(int)x : x;
}

static inline float PS2Quake_Floorf(float x)
{
    const float t = PS2Quake_Truncf(x);
    return t > x ? t - 1.0f : t;
}

static inline float PS2Quake_Ceilf(float x)
{
    const float t = PS2Quake_Truncf(x);
    return t < x ? t + 1.0f : t;
}

/* C's fmod: the remainder takes the sign of x. Returns 0 for y == 0, where libm gives NaN. */
static inline float PS2Quake_Fmodf(float x, float y)
{
    if (y == 0.0f)
    {
        return 0.0f;
    }
    return x - PS2Quake_Truncf(x / y) * y;
}

/* ------------------------------------------------------------------------------------------------
 * Trigonometry. Angles in radians, as in libm.
 * ------------------------------------------------------------------------------------------------ */

/* Cosine as sin(2 pi r) with r folded into [-1/4, 1/4], where a degree 9 minimax polynomial is
 * exact to float precision. The reduction measures the angle in turns: cos is even, so
 * u = |x| / 2 pi, and its fraction f in [0, 1) gives cos(2 pi f) = sin(2 pi (|f - 1/2| - 1/4)).
 *
 * Error, with the EE's round-toward-zero: 6e-7 within one turn either side of zero, growing
 * with |x| as the division loses the fraction (5e-6 at |x| = 50). Degree 7 would save two
 * instructions for twice the error. Valid for |x| < 2^31 turns (1.3e10), where the truncation
 * saturates. 18 FPU instructions, 8 constants. */
static inline float PS2Quake_Cosf(float x)
{
    const float u  = PS2Quake_Fabsf(x) * 0.159154943f;
    const float r  = PS2Quake_Fabsf((u - (float)(int)u) - 0.5f) - 0.25f;
    const float r2 = r * r;
    return r * (6.28318548f + r2 * (-41.3416901f + r2 * (81.6032639f + r2 * (-76.5982056f + r2 * 39.87323f))));
}

static inline float PS2Quake_Sinf(float x)
{
    return PS2Quake_Cosf(x - 1.57079637f);
}

/* Past 1/sqrt(2), asin(a) = pi/2 - asin(sqrt(1 - a^2)), and whichever of a and sqrt(1 - a^2)
 * is smaller is always at most 1/sqrt(2) - so one polynomial on [0, 1/sqrt(2)] serves both
 * halves, fed by a min.s. Degree 9 minimax, good to 3e-6 (the Taylor series of the same degree
 * this replaced was off by 8e-4 at 1/sqrt(2)). |x| > 1 behaves as |x| = 1, near enough: the EE's sqrt.s of a
 * slightly negative 1 - x^2 gives a small positive root. */
static inline float PS2Quake_ASinf(float x)
{
    const float a  = PS2Quake_Fabsf(x);
    const float s  = PS2Quake_Sqrtf(1.0f - a * a);
    const float z  = PS2Quake_Minf(a, s);
    const float z2 = z * z;
    float r = z * (1.00003564f + z2 * (0.165359229f + z2 * (0.0881947204f + z2 * (-0.00743118813f + z2 * 0.110115983f))));
    if (a > s)
    {
        r = 1.57079637f - r;
    }
    return x < 0.0f ? -r : r;
}

static inline float PS2Quake_ACosf(float x)
{
    return 1.57079637f - PS2Quake_ASinf(x);
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* PS2_MATH_MATH_C_H */
