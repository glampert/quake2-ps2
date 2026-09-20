;--------------------------------------------------------------------
; vu_common.i
;
; Macros every VU1 microprogram shares. Included by bare name: vclpp
; has no include path and opens the file relative to its working
; directory, so the Makefile recipe runs it from this directory.
;
; Two vclpp rules shape everything here:
;
;  - A macro body is substituted without being re-scanned, so a macro
;    can never invoke another one. Every macro below is a leaf, meant
;    to be called directly from a #vuprog body.
;  - Macros expand before #defines, so a body may reference a constant
;    each program defines for itself - kGifTags below is one.
;--------------------------------------------------------------------

; The frame constants, at the fixed low addresses every program reads
; them from (see the VU data memory map in vu1.h). The fcset comes
; with them because VCL requires zeroed clip flags before any CLIP
; instruction, and every program issues one.
#macro LoadFrameConstants
    fcset 0x000000

    lq fMVP0,      0(vi00)
    lq fMVP1,      1(vi00)
    lq fMVP2,      2(vi00)
    lq fMVP3,      3(vi00)
    lq fGSScale,   4(vi00)
    lq fGSOffset,  5(vi00)
    lq fClipScale, 6(vi00)
#endmacro

; Copies the batch's GIF tag block, prepared by the EE, to the head of
; the GS packet at iKick. kGifTags is the program's own offset for it
; and kNumGifTagQwords in vu1.h pins the count at the seven unrolled
; here. Leaves iOutPtr just past the block, where the vertices go.
#macro CopyGifTags
    iaddiu iTagPtr, iBase, kGifTags
    iaddiu iOutPtr, iKick, 0

    lqi fTag0, (iTagPtr++)
    lqi fTag1, (iTagPtr++)
    lqi fTag2, (iTagPtr++)
    lqi fTag3, (iTagPtr++)
    lqi fTag4, (iTagPtr++)
    lqi fTag5, (iTagPtr++)
    lqi fTag6, (iTagPtr++)

    sqi fTag0, (iOutPtr++)
    sqi fTag1, (iOutPtr++)
    sqi fTag2, (iOutPtr++)
    sqi fTag3, (iOutPtr++)
    sqi fTag4, (iOutPtr++)
    sqi fTag5, (iOutPtr++)
    sqi fTag6, (iOutPtr++)
#endmacro

; Whole-triangle guard band reject, for the three-vertex programs whose
; output is 3 qwords a vertex. Judges the last 3 vertices' 18 clip flags
; together: if any one left the band, 0x7FFF + flags reaches bit 15 - the
; ADC bit - and the GS skips this triangle's drawing kick. Written to
; every XYZ2 .w so the kicking vertex always carries it.
#macro WholeTriangleReject
    fcand  vi01, 0x3FFFF
    iaddiu iADC, vi01, 0x7FFF
    isw.w  iADC, 2(iOutPtr)
    isw.w  iADC, 5(iOutPtr)
    isw.w  iADC, 8(iOutPtr)
#endmacro
