#pragma once
/* ================================================================================================
 * File: math.h
 * Brief: PS2-optimized math functions.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <float.h>
#include <math.h>

// The scalar functions the C engine shares, implemented once in math_c.h.
#include "ps2/math/math_c.h"

namespace ps2::math {

constexpr float kPI     = 3.1415926535897932384626433832795f;
constexpr float kTwoPI  = 6.283185307179586476925286766559f;
constexpr float kHalfPI = 1.5707963267948966192313216916398f;

// NOTE: Sine/cosine and angles are in radians as in the std lib counterparts.
inline float Sinf(float x)            { return PS2Quake_Sinf(x);     }
inline float Cosf(float x)            { return PS2Quake_Cosf(x);     }
inline float ASinf(float x)           { return PS2Quake_ASinf(x);    }
inline float ACosf(float x)           { return PS2Quake_ACosf(x);    }
inline float Fabsf(float x)           { return PS2Quake_Fabsf(x);    }
inline float Sqrtf(float x)           { return PS2Quake_Sqrtf(x);    }
inline float Minf(float a, float b)   { return PS2Quake_Minf(a, b);  }
inline float Maxf(float a, float b)   { return PS2Quake_Maxf(a, b);  }
inline float Floorf(float x)          { return PS2Quake_Floorf(x);   }
inline float Ceilf(float x)           { return PS2Quake_Ceilf(x);    }
inline float Fmodf(float x, float y)  { return PS2Quake_Fmodf(x, y); }

inline float RSqrtf(float x)
{
	return 1.0f / Sqrtf(x);
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

inline void AngleVectors(const float angles[3], float * forward, float * right, float * up)
{
	const float angle_p = angles[/* PITCH */0] * (kPI * 2.0f / 360.0f);
	const float sp = Sinf(angle_p);
	const float cp = Cosf(angle_p);

	const float angle_y = angles[/*  YAW  */1] * (kPI * 2.0f / 360.0f);
	const float sy = Sinf(angle_y);
	const float cy = Cosf(angle_y);

	const float angle_r = angles[/*  ROLL */2] * (kPI * 2.0f / 360.0f);
	const float sr = Sinf(angle_r);
	const float cr = Cosf(angle_r);

	forward[0] = cp * cy;
	forward[1] = cp * sy;
	forward[2] = -sp;

	right[0] = (-1.0f * sr * sp * cy + -1.0f * cr * -sy);
	right[1] = (-1.0f * sr * sp * sy + -1.0f * cr *  cy);
	right[2] = (-1.0f * sr * cp);

	up[0] = (cr * sp * cy + -sr * -sy);
	up[1] = (cr * sp * sy + -sr *  cy);
	up[2] = (cr * cp);
}

} // namespace ps2::math
