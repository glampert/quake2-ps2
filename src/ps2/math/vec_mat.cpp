/* ================================================================================================
 * File: vec_mat.cpp
 * Brief: Vector and matrix maths. See vec_mat.h for the conventions.
 *
 *  The multiply/transform hot paths run on VU0 in macro mode (lqc2/sqc2 + vector ops),
 *  which requires every Mat4/Vec4 to live at a 16-byte boundary - alignas on the types
 *  guarantees that for locals, statics and members alike.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/math/vec_mat.h"

namespace ps2::math {

Mat4 operator*(const Mat4 & a, const Mat4 & b)
{
    Mat4 result;
    asm volatile (
        "lqc2    $vf1, 0x00(%1)     \n\t" // vf1-vf4 = rows of a
        "lqc2    $vf2, 0x10(%1)     \n\t"
        "lqc2    $vf3, 0x20(%1)     \n\t"
        "lqc2    $vf4, 0x30(%1)     \n\t"
        "lqc2    $vf5, 0x00(%2)     \n\t" // vf5-vf8 = rows of b
        "lqc2    $vf6, 0x10(%2)     \n\t"
        "lqc2    $vf7, 0x20(%2)     \n\t"
        "lqc2    $vf8, 0x30(%2)     \n\t"
        "vmulax  $ACC, $vf5, $vf1   \n\t" // row i of result = sum(a[i][j] * b row j)
        "vmadday $ACC, $vf6, $vf1   \n\t"
        "vmaddaz $ACC, $vf7, $vf1   \n\t"
        "vmaddw  $vf1, $vf8, $vf1   \n\t"
        "vmulax  $ACC, $vf5, $vf2   \n\t"
        "vmadday $ACC, $vf6, $vf2   \n\t"
        "vmaddaz $ACC, $vf7, $vf2   \n\t"
        "vmaddw  $vf2, $vf8, $vf2   \n\t"
        "vmulax  $ACC, $vf5, $vf3   \n\t"
        "vmadday $ACC, $vf6, $vf3   \n\t"
        "vmaddaz $ACC, $vf7, $vf3   \n\t"
        "vmaddw  $vf3, $vf8, $vf3   \n\t"
        "vmulax  $ACC, $vf5, $vf4   \n\t"
        "vmadday $ACC, $vf6, $vf4   \n\t"
        "vmaddaz $ACC, $vf7, $vf4   \n\t"
        "vmaddw  $vf4, $vf8, $vf4   \n\t"
        "sqc2    $vf1, 0x00(%0)     \n\t"
        "sqc2    $vf2, 0x10(%0)     \n\t"
        "sqc2    $vf3, 0x20(%0)     \n\t"
        "sqc2    $vf4, 0x30(%0)     \n\t"
        : : "r" (&result), "r" (&a), "r" (&b)
        : "memory"
    );
    return result;
}

Vec4 Transform(const Vec4 & v, const Mat4 & m)
{
    Vec4 result;
    asm volatile (
        "lqc2    $vf4, 0x00(%1)     \n\t" // vf4-vf7 = rows of m
        "lqc2    $vf5, 0x10(%1)     \n\t"
        "lqc2    $vf6, 0x20(%1)     \n\t"
        "lqc2    $vf7, 0x30(%1)     \n\t"
        "lqc2    $vf8, 0x00(%2)     \n\t" // vf8 = v
        "vmulax  $ACC, $vf4, $vf8   \n\t" // result = v.x*row0 + v.y*row1 + v.z*row2 + v.w*row3
        "vmadday $ACC, $vf5, $vf8   \n\t"
        "vmaddaz $ACC, $vf6, $vf8   \n\t"
        "vmaddw  $vf9, $vf7, $vf8   \n\t"
        "sqc2    $vf9, 0x00(%0)     \n\t"
        : : "r" (&result), "r" (&m), "r" (&v)
        : "memory"
    );
    return result;
}

Mat4 Identity()
{
    return {{ { 1.0f, 0.0f, 0.0f, 0.0f },
              { 0.0f, 1.0f, 0.0f, 0.0f },
              { 0.0f, 0.0f, 1.0f, 0.0f },
              { 0.0f, 0.0f, 0.0f, 1.0f } }};
}

Mat4 Translation(float x, float y, float z)
{
    Mat4 result = Identity();
    result.m[3][0] = x;
    result.m[3][1] = y;
    result.m[3][2] = z;
    return result;
}

Mat4 RotationX(float radians)
{
    const float c = Cosf(radians);
    const float s = Sinf(radians);

    Mat4 result = Identity();
    result.m[1][1] =  c;
    result.m[1][2] =  s;
    result.m[2][1] = -s;
    result.m[2][2] =  c;
    return result;
}

Mat4 RotationY(float radians)
{
    const float c = Cosf(radians);
    const float s = Sinf(radians);

    Mat4 result = Identity();
    result.m[0][0] =  c;
    result.m[0][2] = -s;
    result.m[2][0] =  s;
    result.m[2][2] =  c;
    return result;
}

Mat4 RotationZ(float radians)
{
    const float c = Cosf(radians);
    const float s = Sinf(radians);

    Mat4 result = Identity();
    result.m[0][0] =  c;
    result.m[0][1] =  s;
    result.m[1][0] = -s;
    result.m[1][1] =  c;
    return result;
}

Mat4 LookAt(const Vec3 & eye, const Vec3 & target, const Vec3 & up)
{
    // Right-handed basis: zAxis points from the target back towards the eye.
    const Vec3 zAxis = Normalize(eye - target);
    const Vec3 xAxis = Normalize(Cross(up, zAxis));
    const Vec3 yAxis = Cross(zAxis, xAxis);

    return {{ { xAxis.x, yAxis.x, zAxis.x, 0.0f },
              { xAxis.y, yAxis.y, zAxis.y, 0.0f },
              { xAxis.z, yAxis.z, zAxis.z, 0.0f },
              { -Dot(xAxis, eye), -Dot(yAxis, eye), -Dot(zAxis, eye), 1.0f } }};
}

Mat4 PerspectiveProjection(float fovyRadians, float aspect,
                           float screenW, float screenH,
                           float zNear, float zFar)
{
    // The GS drawing window is 4096 units wide; dividing the screen size by it
    // shrinks NDC so everything inside the window (up to ~6 screens of overdraw)
    // survives the VU1 clipw guard-band test and is scissored by the GS instead.
    constexpr float kProjectionScale = 4096.0f;

    const float halfFovy = fovyRadians * 0.5f;
    const float cotFov   = Cosf(halfFovy) / Sinf(halfFovy);

    const float w = cotFov * (screenW / kProjectionScale) / aspect;
    const float h = cotFov * (screenH / kProjectionScale);

    return {{ { w,    0.0f, 0.0f, 0.0f },
              { 0.0f, -h,   0.0f, 0.0f }, // Y flipped: GS screen space grows downwards
              { 0.0f, 0.0f, (zFar + zNear) / (zFar - zNear), -1.0f },
              { 0.0f, 0.0f, (2.0f * zFar * zNear) / (zFar - zNear), 0.0f } }};
}

} // namespace ps2::math
