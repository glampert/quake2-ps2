#pragma once
/* ================================================================================================
 * File: vec_mat.h
 * Brief: Vector and matrix maths for the 3D renderer: camera, projection and model transforms.
 *
 *  Mat4 is ROW-MAJOR with the ROW-VECTOR convention: v' = v * M, so concatenation reads
 *  left-to-right in application order (model * view * proj). Z+ goes into the screen.
 *  Mat4/Vec4 are 16-byte aligned so they can be handed straight to the DMAC/VU.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/math/math.h"

namespace ps2::math {

// ------------------------------------------------------------------------------------------------
// Vec3 - math-only 3-component vector (no alignment requirements).
// ------------------------------------------------------------------------------------------------

struct Vec3
{
    float x, y, z;
};

constexpr Vec3 operator+(const Vec3 & a, const Vec3 & b)
{
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

constexpr Vec3 operator-(const Vec3 & a, const Vec3 & b)
{
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}

constexpr Vec3 operator-(const Vec3 & v)
{
    return { -v.x, -v.y, -v.z };
}

constexpr Vec3 operator*(const Vec3 & v, float s)
{
    return { v.x * s, v.y * s, v.z * s };
}

constexpr Vec3 operator*(float s, const Vec3 & v)
{
    return v * s;
}

constexpr float Dot(const Vec3 & a, const Vec3 & b)
{
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

constexpr Vec3 Cross(const Vec3 & a, const Vec3 & b)
{
    return { (a.y * b.z) - (a.z * b.y),
             (a.z * b.x) - (a.x * b.z),
             (a.x * b.y) - (a.y * b.x) };
}

inline float Length(const Vec3 & v)
{
    return Sqrtf(Dot(v, v));
}

inline Vec3 Normalize(const Vec3 & v)
{
    return v * RSqrtf(Dot(v, v));
}

// ------------------------------------------------------------------------------------------------
// Vec4 - homogeneous vector; one quadword, DMA-able.
// ------------------------------------------------------------------------------------------------

struct alignas(16) Vec4
{
    float x, y, z, w;
};

// ------------------------------------------------------------------------------------------------
// Mat4 - homogeneous transform; four quadwords, DMA-able.
// ------------------------------------------------------------------------------------------------

struct alignas(16) Mat4
{
    float m[4][4];
};

// Matrix concatenation on VU0: (a * b) applies a first under the row-vector convention.
Mat4 operator*(const Mat4 & a, const Mat4 & b);

// Row-vector transform on VU0: result = v * m. All four components of v are
// used, so set w to 1 for points. Handy for EE-side verification of the VU1 path.
Vec4 Transform(const Vec4 & v, const Mat4 & m);

Mat4 Identity();
Mat4 Translation(float x, float y, float z);
Mat4 RotationX(float radians);
Mat4 RotationY(float radians);
Mat4 RotationZ(float radians);

// Right-handed view matrix looking from 'eye' towards 'target'.
Mat4 LookAt(const Vec3 & eye, const Vec3 & target, const Vec3 & up);

// Perspective projection tuned for the GS: after the divide, the visible screen
// maps to +-(screenW/4096, screenH/4096) in NDC so the 4096-unit GS drawing
// window doubles as a clipping guard band. Depth maps to z/w = +1 at the near
// plane and -1 at the far plane (larger GS depth = closer; pair with a
// GREATER_EQUAL depth test). Y is flipped for the GS top-left origin.
Mat4 PerspectiveProjection(float fovyRadians, float aspect,
                           float screenW, float screenH,
                           float zNear, float zFar);

} // namespace ps2::math
