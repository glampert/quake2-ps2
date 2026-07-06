
/* ================================================================================================
 * -*- C -*-
 * File: vec_mat.h
 * Author: Guilherme R. Lampert
 * Created on: 19/01/16
 * Brief: Vector and Matrix maths.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#ifndef PS2_VEC_MAT_H
#define PS2_VEC_MAT_H

#include "ps2/defs_ps2.h"

/*
==============================================================

m_vec4_t:

Functions ending with a '3' only operate on the XYZ components.
The W component ins usually just padding to ensure 16-bytes alignment.

==============================================================
*/

typedef struct
{
    float x;
    float y;
    float z;
    float w;
} m_vec4_t PS2_ALIGN(16);

m_vec4_t * Vec4_Set3(m_vec4_t * dest, float x, float y, float z);
m_vec4_t * Vec4_Set4(m_vec4_t * dest, float x, float y, float z, float w);

m_vec4_t * Vec4_Copy3(m_vec4_t * dest, const m_vec4_t * src);
m_vec4_t * Vec4_Copy4(m_vec4_t * dest, const m_vec4_t * src);

m_vec4_t * Vec4_Negate3(m_vec4_t * result, const m_vec4_t * v);
m_vec4_t * Vec4_Divide3(m_vec4_t * result, const m_vec4_t * v, float s);
m_vec4_t * Vec4_Multiply3(m_vec4_t * result, const m_vec4_t * v, float s);

m_vec4_t * Vec4_Add3(m_vec4_t * result, const m_vec4_t * a, const m_vec4_t * b);
m_vec4_t * Vec4_Sub3(m_vec4_t * result, const m_vec4_t * a, const m_vec4_t * b);

float Vec4_Length3(const m_vec4_t * v);
float Vec4_Length3Sqr(const m_vec4_t * v);

float Vec4_Dist3(const m_vec4_t * a, const m_vec4_t * b);
float Vec4_Dist3Sqr(const m_vec4_t * a, const m_vec4_t * b);

float Vec4_Dot3(const m_vec4_t * a, const m_vec4_t * b);
m_vec4_t * Vec4_Cross3(m_vec4_t * result, const m_vec4_t * a, const m_vec4_t * b);

m_vec4_t * Vec4_Normalize3(m_vec4_t * result);
m_vec4_t * Vec4_Normalized3(m_vec4_t * result, const m_vec4_t * v);

m_vec4_t * Vec4_Min3PerElement(m_vec4_t * result, const m_vec4_t * a, const m_vec4_t * b);
m_vec4_t * Vec4_Max3PerElement(m_vec4_t * result, const m_vec4_t * a, const m_vec4_t * b);

m_vec4_t * Vec4_Lerp3(m_vec4_t * result, const m_vec4_t * a, const m_vec4_t * b, float t);
m_vec4_t * Vec4_LerpScale3(m_vec4_t * result, const m_vec4_t * a, const m_vec4_t * b, float t, float s);

/*
==============================================================

m_mat4_t:

Homogeneous matrix of 16 floats to represent rotation,
translation and scaling. Also used for camera and projection
matrices. ROW-MAJOR format. Z+ goes into the screen.

==============================================================
*/

typedef struct
{
    float m[4][4];
} m_mat4_t PS2_ALIGN(16);

m_mat4_t * Mat4_Set(m_mat4_t * m, float m11, float m12, float m13, float m14,
                                  float m21, float m22, float m23, float m24,
                                  float m31, float m32, float m33, float m34,
                                  float m41, float m42, float m43, float m44);

m_mat4_t * Mat4_Copy(m_mat4_t * dest, const m_mat4_t * src);
m_mat4_t * Mat4_Identity(m_mat4_t * m);
m_mat4_t * Mat4_Transpose(m_mat4_t * m);

m_mat4_t * Mat4_Scale(m_mat4_t * m, float s);
m_mat4_t * Mat4_Multiply(m_mat4_t * result, const m_mat4_t * a, const m_mat4_t * b);
m_vec4_t * Mat4_TransformVec4(m_vec4_t * result, const m_mat4_t * m, const m_vec4_t * v);

m_mat4_t * Mat4_MakeTranslation(m_mat4_t * m, float x, float y, float z);
m_mat4_t * Mat4_MakeTranslationV(m_mat4_t * m, const m_vec4_t * xyz);

m_mat4_t * Mat4_MakeScaling(m_mat4_t * m, float x, float y, float z);
m_mat4_t * Mat4_MakeScalingV(m_mat4_t * m, const m_vec4_t * xyz);

m_mat4_t * Mat4_MakeRotationX(m_mat4_t * m, float radians);
m_mat4_t * Mat4_MakeRotationY(m_mat4_t * m, float radians);
m_mat4_t * Mat4_MakeRotationZ(m_mat4_t * m, float radians);

m_mat4_t * Mat4_MakeLookAt(m_mat4_t * m, const m_vec4_t * from_vec,
                           const m_vec4_t * to_vec, const m_vec4_t * up_vec);

m_mat4_t * Mat4_MakePerspProjection(m_mat4_t * m, float fovy, float aspect, float scr_w,
                                    float scr_h, float z_near, float z_far, float proj_scale);

#endif // PS2_VEC_MAT_H
