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
#include <tamtypes.h>

#define bits_to_u32(x) __builtin_bit_cast(u32,   (x))
#define bits_to_f32(x) __builtin_bit_cast(float, (x))

// TODO:
// - Give Vec3/Vec4 operator[], so we can replace most uses of Quake's vec3_t with Vec3.
// - Consider giving custom copy assignment/ctor to Vec4/Mat4 that use lq/sq vector instructions.

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

constexpr Mat4 Identity()
{
    return {{ { 1.0f, 0.0f, 0.0f, 0.0f },
              { 0.0f, 1.0f, 0.0f, 0.0f },
              { 0.0f, 0.0f, 1.0f, 0.0f },
              { 0.0f, 0.0f, 0.0f, 1.0f } }};
}

// Matrix concatenation on VU0: (a * b) applies a first under the row-vector convention.
Mat4 operator*(const Mat4 & a, const Mat4 & b);

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

// ------------------------------------------------------------------------------------------------
// VU0 macro-mode helpers
//
//  GCC never auto-vectorizes into VU0 macro mode, so anything hot enough to want the
//  vector unit has to ask for it in asm. Quadword-aligned Vec4 is the shape the unit
//  wants - lqc2/sqc2 move a whole vector per instruction - so prefer the Vec4 forms
//  wherever the layout is yours to choose. The Vec3 overloads are for data that is
//  already stored packed (the model vertex arrays, by the thousand): 12 bytes at
//  4-byte alignment cannot be lqc2'd, so they merge the three words into one 128-bit
//  GPR with the MMI shuffles and hand that over with qmtc2 - six instructions per
//  operand, which Transform earns back several times over but Lerp only breaks even
//  on. All are inline so the loads and merges schedule into the caller's loop.
// ------------------------------------------------------------------------------------------------

// Row-vector transform: result = v * m. All four components of v are used, so set
// w to 1 for points. Also handy for EE-side verification of the VU1 path.
inline Vec4 Transform(const Vec4 & v, const Mat4 & m)
{
    Vec4 result;
    asm volatile (
        "lqc2    $vf4, 0x00(%1)    \n\t" // vf4-vf7 = rows of m
        "lqc2    $vf5, 0x10(%1)    \n\t"
        "lqc2    $vf6, 0x20(%1)    \n\t"
        "lqc2    $vf7, 0x30(%1)    \n\t"
        "lqc2    $vf8, 0x00(%2)    \n\t" // vf8 = v
        "vmulax  $ACC, $vf4, $vf8  \n\t" // result = v.x*row0 + v.y*row1 + v.z*row2 + v.w*row3
        "vmadday $ACC, $vf5, $vf8  \n\t"
        "vmaddaz $ACC, $vf6, $vf8  \n\t"
        "vmaddw  $vf9, $vf7, $vf8  \n\t"
        "sqc2    $vf9, 0x00(%0)    \n\t"
        : : "r" (&result), "r" (&m), "r" (&v)
        : "memory"
    );
    return result;
}

// Component-wise linear interpolation: out = a + t * (b - a), with the multiply and
// the add fused into one VU multiply-accumulate. Exact at both ends: t = 0 gives a.
// Writes through 'out', which may alias 'a' or 'b' (both are loaded up front).
inline void LerpTo(Vec4 & out, const Vec4 & a, const Vec4 & b, float t)
{
    [[maybe_unused]] u32 tmp;
    asm (
        "lqc2    $vf8,  %2           \n\t" // vf8 = a, vf9 = b
        "lqc2    $vf9,  %3           \n\t"
        "mfc1    %0, %4              \n\t" // vf10.x = t
        "qmtc2   %0, $vf10           \n\t"
        "vsub    $vf11, $vf9,  $vf8  \n\t" // vf11 = b - a
        "vmulax  $ACC,  $vf11, $vf10 \n\t" // ACC  = (b - a) * t
        "vmaddw  $vf11, $vf8,  $vf0  \n\t" // vf11 = ACC + a * 1
        "sqc2    $vf11, %1           \n\t"
        : "=&r" (tmp), "=m" (out)
        : "m" (a), "m" (b), "f" (t)
    );
}

// Value-returning form of LerpTo. Prefer LerpTo in hot loops: the vector unit can
// only deliver its result to memory, so this form lands it in a temporary that the
// caller then has to copy out (four instructions GCC will not elide).
inline Vec4 Lerp(const Vec4 & a, const Vec4 & b, float t)
{
    Vec4 result;
    LerpTo(result, a, b, t);
    return result;
}

// Row-vector transform of a packed point: result = { v, 1 } * m, i.e. the full
// clip-space position of a world-space point, w included. The merged vector's 4th
// lane is left as whatever the shuffle produced and never read - the row 3 term is
// taken from vf0's hardwired w = 1 instead of from the vector.
inline Vec4 Transform(const Vec3 & v, const Mat4 & m)
{
    Vec4 result;
    [[maybe_unused]] u32 t0, t1, t2;
    asm volatile (
        "lw      %0, 0x0(%3)       \n\t" // merge v into one quadword: { ?, z, y, x }
        "lw      %1, 0x4(%3)       \n\t"
        "lw      %2, 0x8(%3)       \n\t"
        "pextlw  %0, %1, %0        \n\t"
        "pcpyld  %0, %2, %0        \n\t"
        "qmtc2   %0, $vf8          \n\t"
        "lqc2    $vf4, 0x00(%4)    \n\t" // vf4-vf7 = rows of m
        "lqc2    $vf5, 0x10(%4)    \n\t"
        "lqc2    $vf6, 0x20(%4)    \n\t"
        "lqc2    $vf7, 0x30(%4)    \n\t"
        "vmulax  $ACC, $vf4, $vf8  \n\t" // result = v.x*row0 + v.y*row1 + v.z*row2 + row3
        "vmadday $ACC, $vf5, $vf8  \n\t"
        "vmaddaz $ACC, $vf6, $vf8  \n\t"
        "vmaddw  $vf9, $vf7, $vf0  \n\t"
        "sqc2    $vf9, 0x00(%5)    \n\t"
        : "=&r" (t0), "=&r" (t1), "=&r" (t2)
        : "r" (&v), "r" (&m), "r" (&result)
        : "memory"
    );
    return result;
}

// Component-wise linear interpolation: a + t * (b - a), with the multiply and the
// add fused into one VU multiply-accumulate. Exact at both ends: t = 0 gives a.
//
// The result comes back in GPRs rather than being sqc2'd into a local, so that the
// caller stores the three words straight into wherever its Vec3 lives - an sqc2
// would need a 16-byte-aligned home first and then a copy out of it. The two source
// operands are declared to the asm as memory rather than clobbering all of it.
inline Vec3 Lerp(const Vec3 & a, const Vec3 & b, float t)
{
    u32 rx, ry, rz;
    asm (
        "lw      %0, 0x0(%3)            \n\t" // vf8 = { ?, a.z, a.y, a.x }
        "lw      %1, 0x4(%3)            \n\t"
        "lw      %2, 0x8(%3)            \n\t"
        "pextlw  %0, %1, %0             \n\t"
        "pcpyld  %0, %2, %0             \n\t"
        "qmtc2   %0, $vf8               \n\t"
        "lw      %0, 0x0(%4)            \n\t" // vf9 = { ?, b.z, b.y, b.x }
        "lw      %1, 0x4(%4)            \n\t"
        "lw      %2, 0x8(%4)            \n\t"
        "pextlw  %0, %1, %0             \n\t"
        "pcpyld  %0, %2, %0             \n\t"
        "qmtc2   %0, $vf9               \n\t"
        "mfc1    %0, %5                 \n\t" // vf10.x = t
        "qmtc2   %0, $vf10              \n\t"
        "vsub.xyz   $vf11, $vf9,  $vf8  \n\t" // vf11 = b - a
        "vmulax.xyz $ACC,  $vf11, $vf10 \n\t" // ACC  = (b - a) * t
        "vmaddw.xyz $vf11, $vf8,  $vf0  \n\t" // vf11 = ACC + a * 1
        "qmfc2   %0, $vf11              \n\t" // split the quadword back into words
        "dsrl32  %1, %0, 0              \n\t"
        "pcpyud  %2, %0, %0             \n\t"
        : "=&r" (rx), "=&r" (ry), "=&r" (rz)
        : "r" (&a), "r" (&b), "f" (t), "m" (a), "m" (b)
    );
    return { bits_to_f32(rx), bits_to_f32(ry), bits_to_f32(rz) };
}

// True when the triangle faces away from 'eye' and can be dropped. The
// triangle's plane normal gives its orientation, compared against the
// direction from the viewer:
//
//   const Vec3 d = Cross(v1 - v0, v2 - v0);
//   const Vec3 c = v0 - eye;
//   return Dot(c, d) <= 0.0f;
//
// This winds the other way from the usual textbook form: a triangle CLOCKWISE
// as seen from 'eye' is the front-facing one, which is what Quake 2's data
// uses - ref_gl leaves OpenGL's default counter-clockwise front face in place
// and then culls GL_FRONT (gl_rmain.c), so its visible polygons are the
// clockwise ones. Reverse the cross product for the opposite convention.
//
// All four points must be in the same space - for a transformed model that
// means passing the eye in model space, not the world camera.
//
// The verdict leaves the coprocessor as raw float bits rather than through an
// FPU compare: for any finite float, reading the bits as a signed integer
// preserves ordering against zero (the sign bit lands in the integer's sign,
// and both +0 and -0 test <= 0). That turns the caller's test into an integer
// branch and drops the qmfc2 -> mtc1 -> c.le.s chain the float version needs.
// The sll is not redundant - qmfc2 writes all 64 low bits of the GPR, and a
// 32-bit compare on MIPS64 requires the value sign-extended from bit 31.
inline bool CullBackFacingTriangle(const Vec4 & eye, const Vec4 & v0, const Vec4 & v1, const Vec4 & v2)
{
    s32 dotBits;
    asm (
        "lqc2        $vf4, 0x0(%1)     \n\t" // vf4 = eye
        "lqc2        $vf5, 0x0(%2)     \n\t" // vf5 = v0
        "lqc2        $vf6, 0x0(%3)     \n\t" // vf6 = v1
        "lqc2        $vf7, 0x0(%4)     \n\t" // vf7 = v2
        "vsub.xyz    $vf8, $vf6, $vf5  \n\t" // vf8 = v1 - v0
        "vsub.xyz    $vf9, $vf7, $vf5  \n\t" // vf9 = v2 - v0
        "vsub.xyz    $vf7, $vf5, $vf4  \n\t" // vf7 = v0 - eye; independent of the
                                             // cross product, so it fills the FMAC
                                             // latency shadow ahead of vopmula
        "vopmula.xyz $ACC, $vf8, $vf9  \n\t" // vf6 = cross(vf8, vf9)
        "vopmsub.xyz $vf6, $vf9, $vf8  \n\t"
        "vmul.xyz    $vf5, $vf7, $vf6  \n\t" // vf5 = dot(vf7, vf6), summed into .x
        "vaddy.x     $vf5, $vf5, $vf5  \n\t"
        "vaddz.x     $vf5, $vf5, $vf5  \n\t"
        "qmfc2       %0,   $vf5        \n\t" // low word = the dot product's bits
        "sll         %0,   %0, 0       \n\t" // sign-extend it for the 32-bit compare
        : "=r" (dotBits)
        : "r" (&eye), "r" (&v0), "r" (&v1), "r" (&v2),
          "m" (eye), "m" (v0), "m" (v1), "m" (v2)
    );
    return dotBits <= 0;
}

} // namespace ps2::math
