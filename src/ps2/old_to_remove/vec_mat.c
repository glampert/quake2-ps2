
/* ================================================================================================
 * -*- C -*-
 * File: vec_mat.c
 * Author: Guilherme R. Lampert
 * Created on: 19/01/16
 * Brief: Vector and Matrix maths.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include "ps2/vec_mat.h"
#include "ps2/math_funcs.h"

//=============================================================================
//
// m_vec4_t functions
//
// Remarks: Some functions are optimized to use VU0 inline assembly.
//
//=============================================================================

m_vec4_t * Vec4_Set3(m_vec4_t * dest, float x, float y, float z)
{
    dest->x = x;
    dest->y = y;
    dest->z = z;
    return dest;
}

m_vec4_t * Vec4_Set4(m_vec4_t * dest, float x, float y, float z, float w)
{
    dest->x = x;
    dest->y = y;
    dest->z = z;
    dest->w = w;
    return dest;
}

m_vec4_t * Vec4_Copy3(m_vec4_t * dest, const m_vec4_t * src)
{
    dest->x = src->x;
    dest->y = src->y;
    dest->z = src->z;
    return dest;
}

m_vec4_t * Vec4_Copy4(m_vec4_t * dest, const m_vec4_t * src)
{
    asm volatile (
        "lq $6, 0x0(%1) \n\t"
        "sq $6, 0x0(%0) \n\t"
        : : "r" (dest), "r" (src)
        : "$6"
    );
    return dest;
}

m_vec4_t * Vec4_Negate3(m_vec4_t * result, const m_vec4_t * v)
{
    result->x = - v->x;
    result->y = - v->y;
    result->z = - v->z;
    return result;
}

m_vec4_t * Vec4_Divide3(m_vec4_t * result, const m_vec4_t * v, float s)
{
    asm volatile (
        "lqc2      vf4, 0x0(%1)    \n\t"
        "mfc1      $8,  %2         \n\t"
        "qmtc2     $8,  vf5        \n\t"
        "vdiv      Q,   vf0w, vf5x \n\t"
        "vwaitq                    \n\t"
        "vmulq.xyz vf4, vf4,  Q    \n\t"
        "sqc2      vf4, 0x0(%0)    \n\t"
        : : "r" (result), "r" (v), "f" (s)
        : "$8"
    );
    return result;
}

m_vec4_t * Vec4_Multiply3(m_vec4_t * result, const m_vec4_t * v, float s)
{
    asm volatile (
        "lqc2      vf4, 0x0(%1)  \n\t"
        "mfc1      $8,  %2       \n\t"
        "qmtc2     $8,  vf5      \n\t"
        "vmulx.xyz vf4, vf4, vf5 \n\t"
        "sqc2      vf4, 0x0(%0)  \n\t"
        : : "r" (result), "r" (v), "f" (s)
        : "$8"
    );
    return result;
}

m_vec4_t * Vec4_Add3(m_vec4_t * result, const m_vec4_t * a, const m_vec4_t * b)
{
    asm volatile (
        "lqc2      vf4, 0x0(%1)  \n\t"
        "lqc2      vf5, 0x0(%2)  \n\t"
        "vadd.xyz  vf6, vf4, vf5 \n\t"
        "sqc2      vf6, 0x0(%0)  \n\t"
        : : "r" (result), "r" (a), "r" (b)
    );
    return result;
}

m_vec4_t * Vec4_Sub3(m_vec4_t * result, const m_vec4_t * a, const m_vec4_t * b)
{
    asm volatile (
        "lqc2      vf4, 0x0(%1)  \n\t"
        "lqc2      vf5, 0x0(%2)  \n\t"
        "vsub.xyz  vf6, vf4, vf5 \n\t"
        "sqc2      vf6, 0x0(%0)  \n\t"
        : : "r" (result), "r" (a), "r" (b)
    );
    return result;
}

float Vec4_Length3(const m_vec4_t * v)
{
    return ps2_sqrtf((v->x * v->x) + (v->y * v->y) + (v->z * v->z));
}

float Vec4_Length3Sqr(const m_vec4_t * v)
{
    return (v->x * v->x) + (v->y * v->y) + (v->z * v->z);
}

float Vec4_Dist3(const m_vec4_t * a, const m_vec4_t * b)
{
    m_vec4_t diff;
    Vec4_Sub3(&diff, a, b);
    return Vec4_Length3(&diff);
}

float Vec4_Dist3Sqr(const m_vec4_t * a, const m_vec4_t * b)
{
    register float dist;
    asm volatile (
        "lqc2     vf4, 0x0(%1)  \n\t" // vf4 = a
        "lqc2     vf5, 0x0(%2)  \n\t" // vf5 = b
        "vsub.xyz vf6, vf4, vf5 \n\t" // vf6 = vf4(a) - vf5(b)
        "vmul.xyz vf7, vf6, vf6 \n\t" // vf7 = vf6 * vf6
        "vaddy.x  vf7, vf7, vf7 \n\t" // dot(vf7, vf7)
        "vaddz.x  vf7, vf7, vf7 \n\t"
        "qmfc2    $2,  vf7      \n\t" // Store result on 'dist'
        "mtc1     $2,  %0       \n\t"
        : "=f" (dist)
        : "r" (a), "r" (b)
        : "$2"
    );
    return dist;
}

float Vec4_Dot3(const m_vec4_t * a, const m_vec4_t * b)
{
    register float result;
    asm volatile (
        "lqc2     vf4, 0x0(%1)  \n\t"
        "lqc2     vf5, 0x0(%2)  \n\t"
        "vmul.xyz vf5, vf4, vf5 \n\t"
        "vaddy.x  vf5, vf5, vf5 \n\t"
        "vaddz.x  vf5, vf5, vf5 \n\t"
        "qmfc2    $2,  vf5      \n\t"
        "mtc1     $2,  %0       \n\t"
        : "=f" (result)
        : "r" (a), "r" (b)
        : "$2"
    );
    return result;
}

m_vec4_t * Vec4_Cross3(m_vec4_t * result, const m_vec4_t * a, const m_vec4_t * b)
{
    float x = (a->y * b->z) - (a->z * b->y);
    float y = (a->z * b->x) - (a->x * b->z);
    float z = (a->x * b->y) - (a->y * b->x);
    result->x = x;
    result->y = y;
    result->z = z;
    return result;
}

m_vec4_t * Vec4_Normalize3(m_vec4_t * result)
{
    float length = Vec4_Length3(result);
    return Vec4_Divide3(result, result, length);
}

m_vec4_t * Vec4_Normalized3(m_vec4_t * result, const m_vec4_t * v)
{
    float length = Vec4_Length3(v);
    return Vec4_Divide3(result, v, length);
}

m_vec4_t * Vec4_Min3PerElement(m_vec4_t * result, const m_vec4_t * a, const m_vec4_t * b)
{
    result->x = ps2_minf(a->x, b->x);
    result->y = ps2_minf(a->y, b->y);
    result->z = ps2_minf(a->z, b->z);
    return result;
}

m_vec4_t * Vec4_Max3PerElement(m_vec4_t * result, const m_vec4_t * a, const m_vec4_t * b)
{
    result->x = ps2_maxf(a->x, b->x);
    result->y = ps2_maxf(a->y, b->y);
    result->z = ps2_maxf(a->z, b->z);
    return result;
}

m_vec4_t * Vec4_Lerp3(m_vec4_t * result, const m_vec4_t * a, const m_vec4_t * b, float t)
{
    asm volatile (
        "lqc2      vf4, 0x0(%1)  \n\t" // vf4 = a
        "lqc2      vf5, 0x0(%2)  \n\t" // vf5 = b
        "mfc1      $8,  %3       \n\t" // vf6 = t
        "qmtc2     $8,  vf6      \n\t" // lerp:
        "vsub.xyz  vf7, vf5, vf4 \n\t" // vf7 = v2 - v1
        "vmulx.xyz vf8, vf7, vf6 \n\t" // vf8 = vf7 * t
        "vadd.xyz  vf9, vf8, vf4 \n\t" // vf9 = vf8 + vf4
        "sqc2      vf9, 0x0(%0)  \n\t" // result = vf9
        : : "r" (result), "r" (a), "r" (b), "f" (t)
        : "$8"
    );
    return result;
}

m_vec4_t * Vec4_LerpScale3(m_vec4_t * result, const m_vec4_t * a, const m_vec4_t * b, float t, float s)
{
    asm volatile (
        "mfc1      $8,  %3       \n\t"
        "mfc1      $9,  %4       \n\t"
        "lqc2      vf4, 0x0(%1)  \n\t" // vf4 = a
        "lqc2      vf5, 0x0(%2)  \n\t" // vf5 = b
        "qmtc2     $8,  vf6      \n\t" // vf6 = t
        "qmtc2     $9,  vf7      \n\t" // vf7 = s
        "vsub.xyz  vf8, vf5, vf4 \n\t" // vf8 = v2 - v1
        "vmulx.xyz vf8, vf8, vf6 \n\t" // vf8 = vf8 * t
        "vadd.xyz  vf9, vf8, vf4 \n\t" // vf9 = vf8 + vf4
        "vmulx.xyz vf9, vf9, vf7 \n\t" // vf9 = vf9 * s
        "sqc2      vf9, 0x0(%0)  \n\t" // result = vf9
        : : "r" (result), "r" (a), "r" (b), "f" (t), "f" (s)
        : "$8", "$9"
    );
    return result;
}

//=============================================================================
//
// m_mat4_t functions
//
// Remarks: Some functions are optimized to use VU0 inline assembly.
//
//=============================================================================

m_mat4_t * Mat4_Set(m_mat4_t * m, float m11, float m12, float m13, float m14,
                                  float m21, float m22, float m23, float m24,
                                  float m31, float m32, float m33, float m34,
                                  float m41, float m42, float m43, float m44)
{
    m->m[0][0] = m11;  m->m[0][1] = m12;  m->m[0][2] = m13;  m->m[0][3] = m14;
    m->m[1][0] = m21;  m->m[1][1] = m22;  m->m[1][2] = m23;  m->m[1][3] = m24;
    m->m[2][0] = m31;  m->m[2][1] = m32;  m->m[2][2] = m33;  m->m[2][3] = m34;
    m->m[3][0] = m41;  m->m[3][1] = m42;  m->m[3][2] = m43;  m->m[3][3] = m44;
}

m_mat4_t * Mat4_Copy(m_mat4_t * dest, const m_mat4_t * src)
{
    asm volatile (
        "lq $6, 0x00(%1) \n\t"
        "lq $7, 0x10(%1) \n\t"
        "lq $8, 0x20(%1) \n\t"
        "lq $9, 0x30(%1) \n\t"
        "sq $6, 0x00(%0) \n\t"
        "sq $7, 0x10(%0) \n\t"
        "sq $8, 0x20(%0) \n\t"
        "sq $9, 0x30(%0) \n\t"
        : : "r" (dest), "r" (src)
        : "$6", "$7", "$8", "$9"
    );
    return dest;
}

m_mat4_t * Mat4_Identity(m_mat4_t * m)
{
    asm volatile (
        "vsub.xyzw  vf4, vf0, vf0 \n\t"
        "vadd.w     vf4, vf4, vf0 \n\t"
        "vmr32.xyzw vf5, vf4      \n\t"
        "vmr32.xyzw vf6, vf5      \n\t"
        "vmr32.xyzw vf7, vf6      \n\t"
        "sqc2       vf4, 0x30(%0) \n\t"
        "sqc2       vf5, 0x20(%0) \n\t"
        "sqc2       vf6, 0x10(%0) \n\t"
        "sqc2       vf7, 0x0(%0)  \n\t"
        : : "r" (m)
    );
    return m;
}

m_mat4_t * Mat4_Transpose(m_mat4_t * m)
{
    int i, j;
    m_mat4_t result;
    for (j = 0; j < 4; ++j)
    {
        for (i = 0; i < 4; ++i)
        {
            result.m[i][j] = m->m[j][i];
        }
    }
    return Mat4_Copy(m, &result);
}

m_mat4_t * Mat4_Scale(m_mat4_t * m, float s)
{
    int i, j;
    for (j = 0; j < 4; ++j)
    {
        for (i = 0; i < 4; ++i)
        {
            m->m[i][j] *= s;
        }
    }
    return m;
}

m_mat4_t * Mat4_Multiply(m_mat4_t * result, const m_mat4_t * a, const m_mat4_t * b)
{
    asm volatile (
        "lqc2         vf1, 0x00(%1) \n\t"
        "lqc2         vf2, 0x10(%1) \n\t"
        "lqc2         vf3, 0x20(%1) \n\t"
        "lqc2         vf4, 0x30(%1) \n\t"
        "lqc2         vf5, 0x00(%2) \n\t"
        "lqc2         vf6, 0x10(%2) \n\t"
        "lqc2         vf7, 0x20(%2) \n\t"
        "lqc2         vf8, 0x30(%2) \n\t"
        "vmulax.xyzw  ACC, vf5, vf1 \n\t"
        "vmadday.xyzw ACC, vf6, vf1 \n\t"
        "vmaddaz.xyzw ACC, vf7, vf1 \n\t"
        "vmaddw.xyzw  vf1, vf8, vf1 \n\t"
        "vmulax.xyzw  ACC, vf5, vf2 \n\t"
        "vmadday.xyzw ACC, vf6, vf2 \n\t"
        "vmaddaz.xyzw ACC, vf7, vf2 \n\t"
        "vmaddw.xyzw  vf2, vf8, vf2 \n\t"
        "vmulax.xyzw  ACC, vf5, vf3 \n\t"
        "vmadday.xyzw ACC, vf6, vf3 \n\t"
        "vmaddaz.xyzw ACC, vf7, vf3 \n\t"
        "vmaddw.xyzw  vf3, vf8, vf3 \n\t"
        "vmulax.xyzw  ACC, vf5, vf4 \n\t"
        "vmadday.xyzw ACC, vf6, vf4 \n\t"
        "vmaddaz.xyzw ACC, vf7, vf4 \n\t"
        "vmaddw.xyzw  vf4, vf8, vf4 \n\t"
        "sqc2         vf1, 0x00(%0) \n\t"
        "sqc2         vf2, 0x10(%0) \n\t"
        "sqc2         vf3, 0x20(%0) \n\t"
        "sqc2         vf4, 0x30(%0) \n\t"
        : : "r" (result), "r" (a), "r" (b)
    );
    return result;
}

m_vec4_t * Mat4_TransformVec4(m_vec4_t * result, const m_mat4_t * m, const m_vec4_t * v)
{
    // v->w is also multiplied, so be sure to set it to 1 or whatever meaningful value!
    asm volatile (
        "lqc2         vf4, 0x0(%1)  \n\t"
        "lqc2         vf5, 0x10(%1) \n\t"
        "lqc2         vf6, 0x20(%1) \n\t"
        "lqc2         vf7, 0x30(%1) \n\t"
        "lqc2         vf8, 0x0(%2)  \n\t"
        "vmulax.xyzw  ACC, vf4, vf8 \n\t"
        "vmadday.xyzw ACC, vf5, vf8 \n\t"
        "vmaddaz.xyzw ACC, vf6, vf8 \n\t"
        "vmaddw.xyzw  vf9, vf7, vf8 \n\t"
        "sqc2         vf9, 0x0(%0)  \n\t"
        : : "r" (result), "r" (m), "r" (v)
    );
    return result;
}

m_mat4_t * Mat4_MakeTranslation(m_mat4_t * m, float x, float y, float z)
{
    Mat4_Identity(m);
    m->m[3][0] = x;
    m->m[3][1] = y;
    m->m[3][2] = z;
    return m;
}

m_mat4_t * Mat4_MakeTranslationV(m_mat4_t * m, const m_vec4_t * xyz)
{
    Mat4_Identity(m);
    m->m[3][0] = xyz->x;
    m->m[3][1] = xyz->y;
    m->m[3][2] = xyz->z;
    return m;
}

m_mat4_t * Mat4_MakeScaling(m_mat4_t * m, float x, float y, float z)
{
    Mat4_Identity(m);
    m->m[0][0] = x;
    m->m[1][1] = y;
    m->m[2][2] = z;
    return m;
}

m_mat4_t * Mat4_MakeScalingV(m_mat4_t * m, const m_vec4_t * xyz)
{
    Mat4_Identity(m);
    m->m[0][0] = xyz->x;
    m->m[1][1] = xyz->y;
    m->m[2][2] = xyz->z;
    return m;
}

m_mat4_t * Mat4_MakeRotationX(m_mat4_t * m, float radians)
{
    Mat4_Identity(m);
    float c = ps2_cosf(radians);
    float s = ps2_sinf(radians);
    m->m[1][1] =  c;
    m->m[1][2] =  s;
    m->m[2][1] = -s;
    m->m[2][2] =  c;
    return m;
}

m_mat4_t * Mat4_MakeRotationY(m_mat4_t * m, float radians)
{
    Mat4_Identity(m);
    float c = ps2_cosf(radians);
    float s = ps2_sinf(radians);
    m->m[0][0] =  c;
    m->m[2][0] =  s;
    m->m[0][2] = -s;
    m->m[2][2] =  c;
    return m;
}

m_mat4_t * Mat4_MakeRotationZ(m_mat4_t * m, float radians)
{
    Mat4_Identity(m);
    float c = ps2_cosf(radians);
    float s = ps2_sinf(radians);
    m->m[0][0] =  c;
    m->m[0][1] =  s;
    m->m[1][0] = -s;
    m->m[1][1] =  c;
    return m;
}

m_mat4_t * Mat4_MakeLookAt(m_mat4_t * m, const m_vec4_t * from_vec,
                           const m_vec4_t * to_vec, const m_vec4_t * up_vec)
{
    m_vec4_t v_x, v_y, v_z;
    m_vec4_t v_temp;

    Vec4_Sub3(&v_temp, from_vec, to_vec);
    Vec4_Normalized3(&v_z, &v_temp);

    Vec4_Cross3(&v_temp, up_vec, &v_z);
    Vec4_Normalized3(&v_x, &v_temp);

    Vec4_Cross3(&v_y, &v_z, &v_x);

    m->m[0][0] = v_x.x;  m->m[0][1] = v_y.x;  m->m[0][2] = v_z.x;  m->m[0][3] = 0.0f;
    m->m[1][0] = v_x.y;  m->m[1][1] = v_y.y;  m->m[1][2] = v_z.y;  m->m[1][3] = 0.0f;
    m->m[2][0] = v_x.z;  m->m[2][1] = v_y.z;  m->m[2][2] = v_z.z;  m->m[2][3] = 0.0f;

    m->m[3][0] = -Vec4_Dot3(&v_x, from_vec);
    m->m[3][1] = -Vec4_Dot3(&v_y, from_vec);
    m->m[3][2] = -Vec4_Dot3(&v_z, from_vec);
    m->m[3][3] = 1.0f;

    return m;
}

m_mat4_t * Mat4_MakePerspProjection(m_mat4_t * m, float fovy, float aspect, float scr_w,
                                    float scr_h, float z_near, float z_far, float proj_scale)
{
    float half_fovy = fovy * 0.5f;
    float cot_fov = 1.0f / (ps2_sinf(half_fovy) / ps2_cosf(half_fovy));

    float w = cot_fov * (scr_w / proj_scale) / aspect;
    float h = cot_fov * (scr_h / proj_scale);

    Mat4_Set(m, w, 0.0f, 0.0f, 0.0f, 0.0f, -h, 0.0f, 0.0f,
             0.0f, 0.0f, (z_far + z_near) / (z_far - z_near), -1.0f,
             0.0f, 0.0f, (2.0f * z_far * z_near) / (z_far - z_near), 0.0f);

    return m;
}
