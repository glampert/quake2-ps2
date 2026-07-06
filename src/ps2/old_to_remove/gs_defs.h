
/* ================================================================================================
 * -*- C -*-
 * File: gs_defs.h
 * Author: Guilherme R. Lampert
 * Created on: 17/01/16
 * Brief: Macros constants and #defines related to Graphics Synthesizer and GIF channel.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

/*
 * Based on 'gs.h' from lib PDK by Jesper Svennevid, Daniel Collin.
 */
#ifndef PS2_GS_DEFS_H
#define PS2_GS_DEFS_H

#include <tamtypes.h>

// *** GS: GENERAL PURPOSE REGISTERS ***

#define GS_REG(r) (GS_REG_##r)
#define GS_REG_ALPHA_1 0x42
#define GS_REG_ALPHA_2 0x43
#define GS_REG_BITBLTBUF 0x50
#define GS_REG_CLAMP_1 0x08
#define GS_REG_CLAMP_2 0x09
#define GS_REG_COLCLAMP 0x46
#define GS_REG_DIMX 0x44
#define GS_REG_DTHE 0x45
#define GS_REG_FBA_1 0x4a
#define GS_REG_FBA_2 0x4b
#define GS_REG_FINISH 0x61
#define GS_REG_FOG 0x0a
#define GS_REG_FOGCOL 0x3d
#define GS_REG_FRAME_1 0x4c
#define GS_REG_FRAME_2 0x4d
#define GS_REG_HWREG 0x54
#define GS_REG_LABEL 0x62
#define GS_REG_MIPTBP1_1 0x34
#define GS_REG_MIPTBP1_2 0x35
#define GS_REG_MIPTBP2_1 0x36
#define GS_REG_MIPTBP2_2 0x37
#define GS_REG_PABE 0x49
#define GS_REG_PRIM 0x00
#define GS_REG_PRMODE 0x1b
#define GS_REG_PRMODECONT 0x1a
#define GS_REG_RGBAQ 0x01
#define GS_REG_SCANMSK 0x22
#define GS_REG_SCISSOR_1 0x40
#define GS_REG_SCISSOR_2 0x41
#define GS_REG_SIGNAL 0x60
#define GS_REG_ST 0x02
#define GS_REG_TEST_1 0x47
#define GS_REG_TEST_2 0x48
#define GS_REG_TEX0_1 0x06
#define GS_REG_TEX0_2 0x07
#define GS_REG_TEX1_1 0x14
#define GS_REG_TEX1_2 0x15
#define GS_REG_TEX2_1 0x16
#define GS_REG_TEX2_2 0x17
#define GS_REG_TEXA 0x3b
#define GS_REG_TEXCLUT 0x1c
#define GS_REG_TEXFLUSH 0x3f
#define GS_REG_TRXDIR 0x53
#define GS_REG_TRXPOS 0x51
#define GS_REG_TRXREG 0x52
#define GS_REG_UV 0x03
#define GS_REG_XYOFFSET_1 0x18
#define GS_REG_XYOFFSET_2 0x19
#define GS_REG_XYZ2 0x05
#define GS_REG_XYZ3 0x0d
#define GS_REG_XYZF2 0x04
#define GS_REG_XYZF3 0x0c
#define GS_REG_ZBUF_1 0x4e
#define GS_REG_ZBUF_2 0x4f

// do we need these? (grabbed from GS user manual)
#define GS_REG_BGCOLOR 0x0e
#define GS_REG_BUSDIR 0x44
#define GS_REG_CSR 0x40
#define GS_REG_DISPFB1 0x07
#define GS_REG_DISPFB2 0x09
#define GS_REG_DISPLAY1 0x08
#define GS_REG_DISPLAY2 0x0a
#define GS_REG_EXTBUF 0x0b
#define GS_REG_EXTDATA 0x0c
#define GS_REG_EXTWRITE 0x0d
#define GS_REG_IMR 0x41
#define GS_REG_PMODE 0x00
#define GS_REG_SIGBLID 0x48
#define GS_REG_SMODE2 0x02

// pixel formats
#define GS_PSM_CT32 0x00
#define GS_PSM_CT24 0x01
#define GS_PSM_CT16 0x02
#define GS_PSM_CT16S 0x0a
#define GS_PSM_T8 0x13
#define GS_PSM_T4 0x14
#define GS_PSM_T8H 0x1b
#define GS_PSM_T4HL 0x24
#define GS_PSM_T4HH 0x2c
#define GS_PSM_Z32 0x30
#define GS_PSM_Z24 0x31
#define GS_PSM_Z16 0x32
#define GS_PSM_Z16S 0x3a

// primitive types
#define GS_PRIM_POINT 0
#define GS_PRIM_LINE 1
#define GS_PRIM_LINESTRIP 2
#define GS_PRIM_TRIANGLE 3
#define GS_PRIM_TRISTRIP 4
#define GS_PRIM_TRIFAN 5
#define GS_PRIM_SPRITE 6

// shading method
#define GS_PRIM_SFLAT 0
#define GS_PRIM_SGOURAUD 1

// texture mapping
#define GS_PRIM_TOFF 0
#define GS_PRIM_TON 1

// fogging
#define GS_PRIM_FOFF 0
#define GS_PRIM_FON 1

// alpha blending
#define GS_PRIM_ABOFF 0
#define GS_PRIM_ABON 1

// 1-pass antialiasing
#define GS_PRIM_AAOFF 0
#define GS_PRIM_AAON 1

// texture coordinates
#define GS_PRIM_FSTQ 0
#define GS_PRIM_FUV 1

// context
#define GS_PRIM_C1 0
#define GS_PRIM_C2 1

// alpha test enable
#define GS_TEST_AOFF 0
#define GS_TEST_AON 1

// alpha test method
#define GS_TEST_ANEVER 0
#define GS_TEST_AALWAYS 1
#define GS_TEST_ALESS 2
#define GS_TEST_ALEQUAL 3
#define GS_TEST_AEQUAL 4
#define GS_TEST_AGEQUAL 5
#define GS_TEST_AGREATER 6
#define GS_TEST_ANOTEQUAL 7

// destination alpha test enable
#define GS_TEST_DAOFF 0
#define GS_TEST_DAON 1

// alpha fail conditions
#define GS_TEST_AKEEP 0
#define GS_TEST_AFB_ONLY 1
#define GS_TEST_AZB_ONLY 2
#define GS_TEST_ARGB_ONLY 3

// depth test enable
#define GS_TEST_ZOFF 0
#define GS_TEST_ZON 1

// depth test method
#define GS_TEST_ZNEVER 0
#define GS_TEST_ZALWAYS 1
#define GS_TEST_GEQUAL 2
#define GS_TEST_GREATER 3

// color clamping
#define GS_COLCLAMP_OFF 0
#define GS_COLCLAMP_ON 1

// dithering
#define GS_DTHE_OFF 0
#define GS_DTHE_ON 1

// signal event
#define GS_CSR_SIGOFF 0
#define GS_CSR_SIGON 1

// finish event
#define GS_CSR_FINOFF 0
#define GS_CSR_FINON 1

// horizontal sync event
#define GS_CSR_HSOFF 0
#define GS_CSR_HSON 1

// vertical sync event
#define GS_CSR_VSOFF 0
#define GS_CSR_VSON 1

// rectangular area write termination
#define GS_CSR_RAWTOFF 0
#define GS_CSR_RAWTON 1

// drawing suspend / fifo clear
#define GS_CSR_FLUSHOFF 0
#define GS_CSR_FLUSHON 1

// gs system reset
#define GS_CSR_RSTOFF 0
#define GS_CSR_RSTON 1

// field currently displayed
#define GS_CSR_FEVEN 0
#define GS_CSR_FODD 1

// fifo status
#define GS_CSR_FIFO_RUNNING 0 // neither empty or almost full
#define GS_CSR_FIFO_EMPTY 1   // empty
#define GS_CSR_FIFO_AFULL 2   // almost full

// zbuffer format
#define GS_ZBUF_FORMAT(psm) ((GS_PSM_##psm) & 0xf)

// zbuffer enable
#define GS_ZBUF_ON 0
#define GS_ZBUF_OFF 1

static inline u32 GS_FLOAT_ENCODE(float f)
{
    union F2U
    {
        float asFloat;
        u32 asU32;
    } val;
    val.asFloat = f;
    return val.asU32;
}

// masking could be removed, but we keep it for now for safety precautions
#define GS_BIT_ENCODE(val, ofs, count) (((u64)(val)) & (((u64)~0) >> (64 - count))) << ofs

#define GS_ALPHA(a, b, c, d, fix) \
    (GS_BIT_ENCODE((a), 0, 2) |   \
     GS_BIT_ENCODE((b), 2, 2) |   \
     GS_BIT_ENCODE((c), 4, 2) |   \
     GS_BIT_ENCODE((d), 6, 2) |   \
     GS_BIT_ENCODE((fix), 32, 8))

#define GS_BITBLTBUF(sbp, sbw, spsm, dbp, dbw, dpsm) \
    (GS_BIT_ENCODE((sbp),  0, 14)  |                 \
     GS_BIT_ENCODE((sbw),  16, 6)  |                 \
     GS_BIT_ENCODE((spsm), 24, 6)  |                 \
     GS_BIT_ENCODE((dbp),  32, 14) |                 \
     GS_BIT_ENCODE((dbw),  48, 6)  |                 \
     GS_BIT_ENCODE((dpsm), 56, 6))

#define GS_CLAMP(wms, wmt, minu, maxu, minv, maxv) \
    (GS_BIT_ENCODE((wms),  0,  2)  |               \
     GS_BIT_ENCODE((wmt),  2,  2)  |               \
     GS_BIT_ENCODE((minu), 4,  10) |               \
     GS_BIT_ENCODE((maxu), 14, 10) |               \
     GS_BIT_ENCODE((minv), 24, 10) |               \
     GS_BIT_ENCODE((maxv), 34, 10))

#define GS_COLCLAMP(clamp) GS_BIT_ENCODE((clamp), 0, 1)

#define GS_DIMX(dm00, dm01, dm02, dm03, dm10, dm11, dm12, dm13, dm20, dm21, dm22, dm23, dm30, dm31, dm32, dm33) \
    (GS_BIT_ENCODE((dm00), 0,  3) |                                                                             \
     GS_BIT_ENCODE((dm01), 4,  3) |                                                                             \
     GS_BIT_ENCODE((dm02), 8,  3) |                                                                             \
     GS_BIT_ENCODE((dm03), 12, 3) |                                                                             \
     GS_BIT_ENCODE((dm10), 16, 3) |                                                                             \
     GS_BIT_ENCODE((dm11), 20, 3) |                                                                             \
     GS_BIT_ENCODE((dm12), 24, 3) |                                                                             \
     GS_BIT_ENCODE((dm13), 28, 3) |                                                                             \
     GS_BIT_ENCODE((dm20), 32, 3) |                                                                             \
     GS_BIT_ENCODE((dm21), 36, 3) |                                                                             \
     GS_BIT_ENCODE((dm22), 40, 3) |                                                                             \
     GS_BIT_ENCODE((dm23), 44, 3) |                                                                             \
     GS_BIT_ENCODE((dm30), 48, 3) |                                                                             \
     GS_BIT_ENCODE((dm31), 52, 3) |                                                                             \
     GS_BIT_ENCODE((dm32), 56, 3) |                                                                             \
     GS_BIT_ENCODE((dm33), 60, 3))

#define GS_DTHE(dthe) GS_BIT_ENCODE((dthe), 0, 1)
#define GS_FBA(fba)   GS_BIT_ENCODE((fba),  0, 1)
#define GS_FOG(f)     GS_BIT_ENCODE((f),   56, 8)
#define GS_FINISH()   (0)

#define GS_FOGCOL(fcr, fcg, fcb)   \
    (GS_BIT_ENCODE((fcr), 0,  8) | \
     GS_BIT_ENCODE((fcg), 8,  8) | \
     GS_BIT_ENCODE((fcb), 16, 8))

#define GS_FRAME(fbp, fbw, psm, fbmsk) \
    (GS_BIT_ENCODE((fbp), 0,  9) |     \
     GS_BIT_ENCODE((fbw), 16, 6) |     \
     GS_BIT_ENCODE((psm), 24, 6) |     \
     GS_BIT_ENCODE((fbmsk), 32, 32))

#define GS_HWREG(data) ((md_u64_t)data)

#define GS_LABEL(id, idmsk)       \
    (GS_BIT_ENCODE((id), 0, 32) | \
     GS_BIT_ENCODE((idmsk), 32, 32))

#define GS_MIPTBP1_ENCODE(tbp1, tbw1, tbp2, tbw2, tbp3, tbw3) \
    (GS_BIT_ENCODE((tbp1), 0,  14) |                          \
     GS_BIT_ENCODE((tbw1), 14, 6)  |                          \
     GS_BIT_ENCODE((tbp2), 20, 14) |                          \
     GS_BIT_ENCODE((tbw2), 34, 6)  |                          \
     GS_BIT_ENCODE((tbp3), 40, 14) |                          \
     GS_BIT_ENCODE((tbw3), 54, 6))

#define GS_MIPTBP2_ENCODE(tbp4, tbw4, tbp5, tbw5, tbp6, tbw6) \
    (GS_BIT_ENCODE((tbp4), 0,  14) |                          \
     GS_BIT_ENCODE((tbw4), 14, 6)  |                          \
     GS_BIT_ENCODE((tbp5), 20, 14) |                          \
     GS_BIT_ENCODE((tbw5), 34, 6)  |                          \
     GS_BIT_ENCODE((tbp6), 40, 14) |                          \
     GS_BIT_ENCODE((tbw6), 54, 6))

#define GS_PABE(pabe) GS_BIT_ENCODE((pabe), 0, 1)

#define GS_PRIM(prim, iip, tme, fge, abe, aa1, fst, ctxt, fix) \
    (GS_BIT_ENCODE((prim), 0, 3) |                             \
     GS_BIT_ENCODE((iip),  3, 1) |                             \
     GS_BIT_ENCODE((tme),  4, 1) |                             \
     GS_BIT_ENCODE((fge),  5, 1) |                             \
     GS_BIT_ENCODE((abe),  6, 1) |                             \
     GS_BIT_ENCODE((aa1),  6, 1) |                             \
     GS_BIT_ENCODE((fst),  8, 1) |                             \
     GS_BIT_ENCODE((ctxt), 9, 1) |                             \
     GS_BIT_ENCODE((fix), 10, 1))

#define GS_PRMODE(iip, tme, fge, abe, aa1, fst, ctxt, fix) \
    (GS_BIT_ENCODE((iip),  3, 1) |                         \
     GS_BIT_ENCODE((tme),  4, 1) |                         \
     GS_BIT_ENCODE((fge),  5, 1) |                         \
     GS_BIT_ENCODE((abe),  6, 1) |                         \
     GS_BIT_ENCODE((aa1),  6, 1) |                         \
     GS_BIT_ENCODE((fst),  8, 1) |                         \
     GS_BIT_ENCODE((ctxt), 9, 1) |                         \
     GS_BIT_ENCODE((fix), 10, 1))

#define GS_PRMODECONT(ac) GS_BIT_ENCODE((ac), 0, 1)

#define GS_RGBAQ(r, g, b, a, q)   \
    (GS_BIT_ENCODE((r), 0,  8) |  \
     GS_BIT_ENCODE((g), 8,  8) |  \
     GS_BIT_ENCODE((b), 16, 8) |  \
     GS_BIT_ENCODE((a), 24, 8) |  \
     GS_BIT_ENCODE(GS_FLOAT_ENCODE((q)), 32, 32))

#define GS_SCANMSK(msk) GS_BIT_ENCODE((msk), 0, 2)

#define GS_SCISSOR(scax0, scax1, scay0, scay1) \
    (GS_BIT_ENCODE(scax0, 0,  11) |            \
     GS_BIT_ENCODE(scax1, 16, 11) |            \
     GS_BIT_ENCODE(scay0, 32, 11) |            \
     GS_BIT_ENCODE(scay1, 48, 11))

#define GS_SIGNAL(id, idmsk)      \
    (GS_BIT_ENCODE((id), 0, 32) | \
     GS_BIT_ENCODE((idmsk), 32, 32))

#define GS_ST(s, t)                               \
    (GS_BIT_ENCODE(GS_FLOAT_ENCODE((s)), 0, 32) | \
     GS_BIT_ENCODE(GS_FLOAT_ENCODE((t)), 32, 32))

#define GS_TEST(ate, atst, aref, afail, date, datm, zte, ztst) \
    (GS_BIT_ENCODE((ate),   0,  1) |                           \
     GS_BIT_ENCODE((atst),  1,  3) |                           \
     GS_BIT_ENCODE((aref),  4,  8) |                           \
     GS_BIT_ENCODE((afail), 12, 2) |                           \
     GS_BIT_ENCODE((date),  14, 1) |                           \
     GS_BIT_ENCODE((datm),  15, 1) |                           \
     GS_BIT_ENCODE((zte),   16, 1) |                           \
     GS_BIT_ENCODE((ztst),  17, 2))

#define GS_TEX0(tbp0, tbw, psm, tw, th, tcc, tfx, cbp, cpsm, csm, csa, cld) \
    (GS_BIT_ENCODE((tbp0), 0, 14)  |                                        \
     GS_BIT_ENCODE((tbw),  14, 6)  |                                        \
     GS_BIT_ENCODE((psm),  20, 6)  |                                        \
     GS_BIT_ENCODE((tw),   26, 4)  |                                        \
     GS_BIT_ENCODE((th),   30, 4)  |                                        \
     GS_BIT_ENCODE((tcc),  34, 1)  |                                        \
     GS_BIT_ENCODE((tfx),  35, 2)  |                                        \
     GS_BIT_ENCODE((cbp),  37, 14) |                                        \
     GS_BIT_ENCODE((cpsm), 51, 4)  |                                        \
     GS_BIT_ENCODE((csm),  55, 1)  |                                        \
     GS_BIT_ENCODE((csa),  56, 5)  |                                        \
     GS_BIT_ENCODE((cld),  61, 3))

#define GS_TEX1_ENCODE(lcm, mxl, mmag, mmin, mtba, l, k) \
    (GS_BIT_ENCODE((lcm),  0, 1) |                       \
     GS_BIT_ENCODE((mxl),  2, 3) |                       \
     GS_BIT_ENCODE((mmag), 5, 1) |                       \
     GS_BIT_ENCODE((mmin), 6, 3) |                       \
     GS_BIT_ENCODE((mtba), 9, 1) |                       \
     GS_BIT_ENCODE((l), 19, 2) |                         \
     GS_BIT_ENCODE((k), 32, 12))

#define GS_TEX2_ENCODE(psm, cbp, cpsm, csm, csa, cld) \
    (GS_BIT_ENCODE((psm),  20, 6)  |                  \
     GS_BIT_ENCODE((cbp),  37, 14) |                  \
     GS_BIT_ENCODE((cpsm), 51, 4)  |                  \
     GS_BIT_ENCODE((csm),  55, 1)  |                  \
     GS_BIT_ENCODE((csa),  56, 5)  |                  \
     GS_BIT_ENCODE((cld),  61, 3))

#define GS_TEXA(ta0, aem, ta1)     \
    (GS_BIT_ENCODE((ta0), 0,  8) | \
     GS_BIT_ENCODE((aem), 15, 1) | \
     GS_BIT_ENCODE((ta1), 32, 8))

#define GS_TEXCLUT(cbw, cou, cov) \
    (GS_BIT_ENCODE((cbw), 0, 6) | \
     GS_BIT_ENCODE((cou), 6, 6) | \
     GS_BIT_ENCODE((cov), 12, 10))

#define GS_TEXFLUSH() (0)
#define GS_TRXDIR(xdir) GS_BIT_ENCODE((xdir), 0, 2)

#define GS_TRXPOS(ssax, ssay, dsax, dsay, dir) \
    (GS_BIT_ENCODE((ssax), 0,  11) |           \
     GS_BIT_ENCODE((ssay), 16, 11) |           \
     GS_BIT_ENCODE((dsax), 32, 11) |           \
     GS_BIT_ENCODE((dsay), 48, 11) |           \
     GS_BIT_ENCODE((dir),  59, 2))

#define GS_TRXREG(rrw, rrh)         \
    (GS_BIT_ENCODE((rrw), 0,  12) | \
     GS_BIT_ENCODE((rrh), 32, 12))

#define GS_UV(u, v)               \
    (GS_BIT_ENCODE((u), 0,  14) | \
     GS_BIT_ENCODE((v), 16, 14))

#define GS_XYOFFSET(ofx, ofy)       \
    (GS_BIT_ENCODE((ofx), 0,  16) | \
     GS_BIT_ENCODE((ofy), 32, 16))

#define GS_XYZ2(x, y, z)          \
    (GS_BIT_ENCODE((x), 0,  16) | \
     GS_BIT_ENCODE((y), 16, 16) | \
     GS_BIT_ENCODE((z), 32, 32))

#define GS_XYZ3(x, y, z) GS_XYZ2((x), (y), (z))

#define GS_XYZF2(x, y, z, f)      \
    (GS_BIT_ENCODE((x), 0,  16) | \
     GS_BIT_ENCODE((y), 16, 16) | \
     GS_BIT_ENCODE((z), 32, 24) | \
     GS_BIT_ENCODE((f), 56, 8))

#define GS_XYZF3(x, y, z, f) GS_XYZF2((x), (y), (z), (f))

#define GS_ZBUF(zbp, psm, zmsk)     \
    (GS_BIT_ENCODE((zbp),  0,  9) | \
     GS_BIT_ENCODE((psm),  24, 4) | \
     GS_BIT_ENCODE((zmsk), 32, 1))

#define GS_GIFTAG(nloop, eop, pre, prim, flg, nreg) \
    (GS_BIT_ENCODE((nloop), 0, 15) |                \
     GS_BIT_ENCODE((eop),  15, 1)  |                \
     GS_BIT_ENCODE((pre),  46, 1)  |                \
     GS_BIT_ENCODE((prim), 47, 11) |                \
     GS_BIT_ENCODE((flg),  58, 2)  |                \
     GS_BIT_ENCODE((nreg), 60, 4))

#define GS_GIFTAG_PACKED 0
#define GS_GIFTAG_REGLIST 1
#define GS_GIFTAG_IMAGE 2
#define GS_GIFTAG_DISABLE 3
#define GS_GIFTAG_EWITH 0
#define GS_GIFTAG_EWITHOUT 1
#define GS_GIFTAG_PRIM 0
#define GS_GIFTAG_RGBAQ 1
#define GS_GIFTAG_ST 2
#define GS_GIFTAG_UV 3
#define GS_GIFTAG_XYZF2 4
#define GS_GIFTAG_XYZ2 5
#define GS_GIFTAG_TEX_0 6
#define GS_GIFTAG_TEX_1 7
#define GS_GIFTAG_CLAMP_0 8
#define GS_GIFTAG_CLAMP_1 9
#define GS_GIFTAG_FOG 10
#define GS_GIFTAG_XYZF3 12
#define GS_GIFTAG_XYZ3 13
#define GS_GIFTAG_AD 14
#define GS_GIFTAG_NOP 15

// *** GS: PRIVILEGED REGISTERS ***

#define GS_PREG(r) (GS_PREG_##r)
#define GS_PREG_PMODE 0x0000
#define GS_PREG_SMODE_1 0x0010
#define GS_PREG_SMODE_2 0x0020
#define GS_PREG_SRFSH 0x0030
#define GS_PREG_SYNCH_1 0x0040
#define GS_PREG_SYNCH_2 0x0060
#define GS_PREG_DISPFB_1 0x0070
#define GS_PREG_DISPLAY_1 0x0080
#define GS_PREG_DISPFB_2 0x0090
#define GS_PREG_DISPLAY_2 0x00a0
#define GS_PREG_EXTBUF 0x00b0
#define GS_PREG_EXTDATA 0x00c0
#define GS_PREG_EXTWRITE 0x00d0
#define GS_PREG_BGCOLOR 0x00e0
#define GS_PREG_CSR 0x1000
#define GS_PREG_IMR 0x1010
#define GS_PREG_BUSDIR 0x1040
#define GS_PREG_SIGBLID 0x1080

#define GS_WRITE(r, v) *((volatile u64 *)(0x12000000 + GS_PREG(r))) = (v)
#define GS_READ(r)     *((volatile u64 *)(0x12000000 + GS_PREG(r)))

#define GS_PMODE(en1, en2, mmod, amod, slbg, alp) \
    (GS_BIT_ENCODE((en1), 0, 1) |                 \
     GS_BIT_ENCODE((en2), 1, 1) |                 \
     GS_BIT_ENCODE(1, 2, 3) |                     \
     GS_BIT_ENCODE((mmod), 5, 1) |                \
     GS_BIT_ENCODE((amod), 6, 1) |                \
     GS_BIT_ENCODE((slbg), 7, 1) |                \
     GS_BIT_ENCODE((alp), 8, 8))

#define GS_DISPFB(fbp, fbw, psm, dbx, dby) \
    (GS_BIT_ENCODE((fbp), 0,  9)  |        \
     GS_BIT_ENCODE((fbw), 9,  6)  |        \
     GS_BIT_ENCODE((psm), 15, 5)  |        \
     GS_BIT_ENCODE((dbx), 32, 11) |        \
     GS_BIT_ENCODE((dby), 43, 11))

#define GS_BGCOLOR(r, g, b)      \
    (GS_BIT_ENCODE((r), 0,  8) | \
     GS_BIT_ENCODE((g), 8,  8) | \
     GS_BIT_ENCODE((b), 16, 8))

#define GS_BUSDIR(dir) GS_BIT_ENCODE((dir), 0, 1)

#define GS_DISPLAY(dx, dy, magh, magv, dw, dh) \
    (GS_BIT_ENCODE((dx),   0,  12) |           \
     GS_BIT_ENCODE((dy),   12, 11) |           \
     GS_BIT_ENCODE((magh), 23, 4)  |           \
     GS_BIT_ENCODE((magv), 27, 2)  |           \
     GS_BIT_ENCODE((dw),   32, 12) |           \
     GS_BIT_ENCODE((dh),   44, 11))

#define GS_EXTBUF(exbp, xbw, fbin, wffmd, emoda, emodc, wdx, wdy) \
    (GS_BIT_ENCODE((exbp),  0, 14)  |                             \
     GS_BIT_ENCODE((exbw),  14, 6)  |                             \
     GS_BIT_ENCODE((fbin),  20, 2)  |                             \
     GS_BIT_ENCODE((wffmd), 22, 1)  |                             \
     GS_BIT_ENCODE((emoda), 23, 2)  |                             \
     GS_BIT_ENCODE((emodc), 25, 2)  |                             \
     GS_BIT_ENCODE((wdx),   32, 11) |                             \
     GS_BIT_ENCODE((wdy),   43, 11))

#define GS_EXTDATA(sx, sy, smph, smpv, ww, wh) \
    (GS_BIT_ENCODE((sx),   0,  12) |           \
     GS_BIT_ENCODE((sy),   12, 11) |           \
     GS_BIT_ENCODE((smph), 23, 4)  |           \
     GS_BIT_ENCODE((smpv), 27, 2)  |           \
     GS_BIT_ENCODE((ww),   32, 12) |           \
     GS_BIT_ENCODE((wh),   44, 11))

#define GS_EXTWRITE(write) GS_BIT_ENCODE((write), 0, 1)

#define GS_CSR(signal, finish, hsint, vsint, edwint, flush, reset, nfield, field, fifo, rev, id) \
    (GS_BIT_ENCODE((signal), 0,  1) |                                                            \
     GS_BIT_ENCODE((finish), 1,  1) |                                                            \
     GS_BIT_ENCODE((hsint),  2,  1) |                                                            \
     GS_BIT_ENCODE((vsint),  3,  1) |                                                            \
     GS_BIT_ENCODE((edwint), 4,  1) |                                                            \
     GS_BIT_ENCODE((flush),  8,  1) |                                                            \
     GS_BIT_ENCODE((reset),  9,  1) |                                                            \
     GS_BIT_ENCODE((nfield), 12, 1) |                                                            \
     GS_BIT_ENCODE((field),  13, 1) |                                                            \
     GS_BIT_ENCODE((fifo),   16, 8) |                                                            \
     GS_BIT_ENCODE((rev),    24, 8) |                                                            \
     GS_BIT_ENCODE((id),     0,  1))

#define GS_SMODE2(it, ffmd, dpms)  \
    (GS_BIT_ENCODE((it),   0, 1) | \
     GS_BIT_ENCODE((ffmd), 1, 1) | \
     GS_BIT_ENCODE((dpms), 2, 2))

#endif // PS2_GS_DEFS_H
