
/* ================================================================================================
 * -*- C -*-
 * File: math_funcs.c
 * Author: Guilherme R. Lampert
 * Created on: 13/10/15
 * Brief: Optimized math functions for the PS2 and replacements for the
 *        C-library double precision ones that are not available on PlayStation 2.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include "ps2/math_funcs.h"

// Asm code originally from Morten "Sparky" Mikkelsen's fast maths routines
// (From a post on the forums at www.playstation2-linux.com)

/*
=================
Arc-sine function
=================
*/
float ps2_asinf(float x)
{
    float r;
    asm volatile (
        ".set noreorder             \n\t"
        ".align 3                   \n\t"
        "lui     $8,  0x3f80        \n\t"
        "mtc1    $0,  $f8           \n\t"
        "mtc1    $8,  $f1           \n\t"
        "lui     $8,  0x3f35        \n\t"
        "mfc1    $9,  %1            \n\t"
        "ori     $8,  $8,    0x04f3 \n\t"
        "adda.s  $f1, $f8           \n\t"
        "lui     $10, 0x8000        \n\t"
        "msub.s  $f2, %1,    %1     \n\t"
        "not     $11, $10           \n\t"
        "and     $11, $9,    $11    \n\t"
        "subu    $8,  $8,    $11    \n\t"
        "nop                        \n\t"
        "and     $9,  $9,    $10    \n\t"
        "sqrt.s  $f2, $f2           \n\t"
        "srl     $8,  $8,    31     \n\t"
        "abs.s   %0,  %1            \n\t"
        "lui     $11, 0x3fc9        \n\t"
        "ori     $11, 0x0fdb        \n\t"
        "or      $11, $9,    $11    \n\t"
        "movz    $11, $0,    $8     \n\t"
        "xor     $10, $9,    $10    \n\t"
        "mtc1    $11, $f6           \n\t"
        "movz    $10, $9,    $8     \n\t"
        "min.s   %0,  $f2,   %0     \n\t"
        "lui     $9,  0x3e2a        \n\t"
        "lui     $8,  0x3f80        \n\t"
        "ori     $9,  $9,    0xaaab \n\t"
        "or      $8,  $10,   $8     \n\t"
        "or      $9,  $10,   $9     \n\t"
        "mtc1    $8,  $f3           \n\t"
        "lui     $8,  0x3d99        \n\t"
        "mul.s   $f7, %0,    %0     \n\t"
        "ori     $8,  $8,    0x999a \n\t"
        "mtc1    $9,  $f4           \n\t"
        "lui     $9,  0x3d36        \n\t"
        "or      $8,  $10,   $8     \n\t"
        "ori     $9,  $9,    0xdb6e \n\t"
        "mtc1    $8,  $f5           \n\t"
        "or      $9,  $10,   $9     \n\t"
        "mul.s   $f1, %0,    $f7    \n\t"
        "lui     $8,  0x3cf8        \n\t"
        "adda.s  $f6, $f8           \n\t"
        "ori     $8,  $8,    0xe38e \n\t"
        "mul.s   $f8, $f7,   $f7    \n\t"
        "or      $8,  $10,   $8     \n\t"
        "madda.s $f3, %0            \n\t"
        "mul.s   $f2, $f1,   $f7    \n\t"
        "madda.s $f4, $f1           \n\t"
        "mtc1    $9,  $f6           \n\t"
        "mul.s   $f1, $f1,   $f8    \n\t"
        "mul.s   %0,  $f2,   $f8    \n\t"
        "mtc1    $8,  $f7           \n\t"
        "madda.s $f5, $f2           \n\t"
        "madda.s $f6, $f1           \n\t"
        "madd.s  %0,  $f7,   %0     \n\t"
        ".set reorder               \n\t"
        : "=&f" (r)
        : "f" (x)
        : "$f1", "$f2", "$f3", "$f4", "$f5", "$f6", "$f7", "$f8"
    );
    return r;
}

/*
=================
Cosine function
=================
*/
float ps2_cosf(float x)
{
    float r;
    asm volatile (
        "lui     $9,  0x3f00        \n\t"
        ".set noreorder             \n\t"
        ".align 3                   \n\t"
        "abs.s   %0,  %1            \n\t"
        "lui     $8,  0xbe22        \n\t"
        "mtc1    $9,  $f1           \n\t"
        "ori     $8,  $8,    0xf983 \n\t"
        "mtc1    $8,  $f8           \n\t"
        "lui     $9,  0x4b00        \n\t"
        "mtc1    $9,  $f3           \n\t"
        "lui     $8,  0x3f80        \n\t"
        "mtc1    $8,  $f2           \n\t"
        "mula.s  %0,  $f8           \n\t"
        "msuba.s $f3, $f2           \n\t"
        "madda.s $f3, $f2           \n\t"
        "lui     $8,  0x40c9        \n\t"
        "msuba.s %0,  $f8           \n\t"
        "ori     $8,  0x0fdb        \n\t"
        "msub.s  %0,  $f1,   $f2    \n\t"
        "lui     $9,  0xc225        \n\t"
        "abs.s   %0,  %0            \n\t"
        "lui     $10, 0x3e80        \n\t"
        "mtc1    $10, $f7           \n\t"
        "ori     $9,  0x5de1        \n\t"
        "sub.s   %0,  %0,    $f7    \n\t"
        "lui     $10, 0x42a3        \n\t"
        "mtc1    $8,  $f3           \n\t"
        "ori     $10, 0x3458        \n\t"
        "mtc1    $9,  $f4           \n\t"
        "lui     $8,  0xc299        \n\t"
        "mtc1    $10, $f5           \n\t"
        "ori     $8,  0x2663        \n\t"
        "mul.s   $f8, %0,    %0     \n\t"
        "lui     $9,  0x421e        \n\t"
        "mtc1    $8,  $f6           \n\t"
        "ori     $9,  0xd7bb        \n\t"
        "mtc1    $9,  $f7           \n\t"
        "nop                        \n\t"
        "mul.s   $f1, %0,    $f8    \n\t"
        "mul.s   $f9, $f8,   $f8    \n\t"
        "mula.s  $f3, %0            \n\t"
        "mul.s   $f2, $f1,   $f8    \n\t"
        "madda.s $f4, $f1           \n\t"
        "mul.s   $f1, $f1,   $f9    \n\t"
        "mul.s   %0,  $f2,   $f9    \n\t"
        "madda.s $f5, $f2           \n\t"
        "madda.s $f6, $f1           \n\t"
        "madd.s  %0,  $f7,   %0     \n\t"
        ".set reorder               \n\t"
        : "=&f" (r)
        : "f" (x)
        : "$f1", "$f2", "$f3", "$f4", "$f5", "$f6", "$f7", "$f8", "$f9", "$8", "$9", "$10"
    );
    return r;
}

/*
=================
fmodf for the PS2
=================
*/
float ps2_fmodf(float x, float y)
{
    /*
     * Portable fmod(x,y) implementation for systems
     * that don't have it. Adapted from code found here:
     * http://www.opensource.apple.com/source/python/python-3/python/Python/fmod.c
     */
    float i, f;

    if (y == 0.0f)
    {
        return 0.0f;
    }

    i = floorf(x / y);
    f = x - i * y;

    if ((x < 0.0f) != (y < 0.0f))
    {
        f = f - y;
    }

    return f;
}
