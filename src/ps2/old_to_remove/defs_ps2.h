
/* ================================================================================================
 * -*- C -*-
 * File: defs_ps2.h
 * Author: Guilherme R. Lampert
 * Created on: 01/02/16
 * Brief: Macro constants and #defines related to the PS2 system and hardware.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#ifndef DEFS_PS2_H
#define DEFS_PS2_H

#include <tamtypes.h>

// Shorthand for the awfully verbose GCC attribute...
#define PS2_ALIGN(alignment) __attribute__((aligned(alignment)))
#define PS2_AS_U128(n) ((u128)(n))

// SPR = Scratch Pad
#define PS2_SPR_SIZE_QWORDS 1024       // 16 Kilobytes (16384 bytes) = 1024 quadwords.
#define PS2_SPR_MEM_BEGIN   0x70000000 // Starting address of the Scratch Pad memory.
#define PS2_UCAB_MEM_MASK   0x30000000 // ORing a pointer with this mask sets it to Uncached Accelerated (UCAB) space.

// Convert the bit pattern of a float to integer or vice-versa.
typedef union
{
    float asFloat;
    u32   asU32;
    s32   asI32;
} FU32_t;

//
// Primitive topologies supported by the Graphics Synthesizer:
//
typedef enum
{
    PS2_PRIM_POINT,
    PS2_PRIM_LINE,
    PS2_PRIM_LINESTRIP,
    PS2_PRIM_TRIANGLE,
    PS2_PRIM_TRISTRIP,
    PS2_PRIM_TRIFAN,
    PS2_PRIM_SPRITE
} ps2_gs_primitives_t;

//
// PRIM Register - Setup Drawing Primitive
//   PRI  - Primitive type
//   IIP  - Shading method (0=flat, 1=gouraud)
//   TME  - Texture mapping (0=off, 1=on)
//   FGE  - Fog (0=off, 1=on)
//   ABE  - Alpha Blending (0=off, 1=on)
//   AA1  - Anti-aliasing (0=off,1=on)
//   FST  - Texture coordinate specification (0=use ST/RGBAQ register, 1=use UV register) (UV means no perspective correction, good for 2D)
//   CTXT - Drawing context (0=1, 1=2)
//   FIX  - ?? Fragment value control (use 0)
//
enum
{
    PS2_PRIM_IIP_FLAT      = 0,
    PS2_PRIM_IIP_GOURAUD   = 1,
    PS2_PRIM_TME_OFF       = 0,
    PS2_PRIM_TME_ON        = 1,
    PS2_PRIM_FGE_OFF       = 0,
    PS2_PRIM_FGE_ON        = 1,
    PS2_PRIM_ABE_OFF       = 0,
    PS2_PRIM_ABE_ON        = 1,
    PS2_PRIM_AA1_OFF       = 0,
    PS2_PRIM_AA1_ON        = 1,
    PS2_PRIM_FST_STQ       = 0,
    PS2_PRIM_FST_UV        = 1,
    PS2_PRIM_CTXT_CONTEXT1 = 0,
    PS2_PRIM_CTXT_CONTEXT2 = 1,
    PS2_PRIM_FIX_NOFIXDDA  = 0,
    PS2_PRIM_FIX_FIXDDA    = 1
};

//
// GS batch/vertex specification flags:
//
enum
{
    PS2_GIFTAG_PACKED   = 0,
    PS2_GIFTAG_REGLIST  = 1,
    PS2_GIFTAG_IMAGE    = 2,
    PS2_GIFTAG_DISABLE  = 3,
    PS2_GIFTAG_EWITH    = 0,
    PS2_GIFTAG_EWITHOUT = 1,
    PS2_GIFTAG_PRIM     = 0,
    PS2_GIFTAG_RGBAQ    = 1,
    PS2_GIFTAG_ST       = 2,
    PS2_GIFTAG_UV       = 3,
    PS2_GIFTAG_XYZF2    = 4,
    PS2_GIFTAG_XYZ2     = 5,
    PS2_GIFTAG_TEX_0    = 6,
    PS2_GIFTAG_TEX_1    = 7,
    PS2_GIFTAG_CLAMP_0  = 8,
    PS2_GIFTAG_CLAMP_1  = 9,
    PS2_GIFTAG_FOG      = 10,
    PS2_GIFTAG_XYZF3    = 12,
    PS2_GIFTAG_XYZ3     = 13,
    PS2_GIFTAG_AD       = 14,
    PS2_GIFTAG_NOP      = 15
};

#define PS2_GS_BATCH_1(r1) ((PS2_AS_U128(1)) | (PS2_AS_U128(r1) << 4))
#define PS2_GS_BATCH_2(r1, r2) ((PS2_AS_U128(2)) | (PS2_AS_U128(r1) << 4) | (PS2_AS_U128(r2) << 8))
#define PS2_GS_BATCH_3(r1, r2, r3) ((PS2_AS_U128(3)) | (PS2_AS_U128(r1) << 4) | (PS2_AS_U128(r2) << 8) | (PS2_AS_U128(r3) << 12))
#define PS2_GS_BATCH_4(r1, r2, r3, r4) ((PS2_AS_U128(4)) | (PS2_AS_U128(r1) << 4) | (PS2_AS_U128(r2) << 8) | (PS2_AS_U128(r3) << 12) | (PS2_AS_U128(r4) << 16))

#define PS2_GS_PRIM(PRIM, IIP, TME, FGE, ABE, AA1, FST, CTXT, FIX) PS2_AS_U128((FIX << 10) | (CTXT << 9) | (FST << 8) | (AA1 << 7) | (ABE << 6) | (FGE << 5) | (TME << 4) | (IIP << 3) | (PRIM))
#define PS2_GS_GIFTAG(NLOOP, EOP, PRE, PRIM, FLG, NREG) (((u64)(NREG) << 60) | ((u64)(FLG) << 58) | ((u64)(PRIM) << 47) | ((u64)(PRE) << 46) | (EOP << 15) | (NLOOP << 0))
#define PS2_GS_GIFTAG_BATCH(NLOOP, EOP, PRE, PRIM, FLG, BATCH) ((PS2_AS_U128(BATCH) << 60) | (PS2_AS_U128(FLG) << 58) | (PS2_AS_U128(PRIM) << 47) | (PS2_AS_U128(PRE) << 46) | (PS2_AS_U128(EOP) << 15) | (PS2_AS_U128(NLOOP)))

#define PS2_PACKED_UV(U, V) (((u64)(V) << 32) | ((u64)(U)))
#define PS2_PACKED_XYZ2(X, Y, Z, ADC) ((PS2_AS_U128(ADC) << 111) | (PS2_AS_U128(Z) << 64) | (PS2_AS_U128(Y) << 32) | (PS2_AS_U128(X)))
#define PS2_PACKED_RGBA(R, G, B, A) ((PS2_AS_U128(A) << 96) | (PS2_AS_U128(B) << 64) | (PS2_AS_U128(G) << 32) | (PS2_AS_U128(R)))

#endif // DEFS_PS2_H
