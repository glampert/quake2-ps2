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
;
; Only particles still uses this - every triangle program sends its
; output through a window instead, and OpenOutputWindow below repeats
; these lines rather than calling them, because vclpp cannot nest one
; macro inside another.
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

; The same judgement, split, for a program that has to *act* on it as well as
; record it. Reading the clip flags twice for one triangle is the thing to
; avoid: openvcl reorders around flag reads (see the note in the lerp
; program's backface cull), so the two reads are not guaranteed to see the
; same flags. Judge once, branch on vi01, and store the ADC afterwards.
#macro JudgeTriangleAdc
    fcand  vi01, 0x3FFFF
    iaddiu iADC, vi01, 0x7FFF
#endmacro

#macro StoreTriangleAdc
    isw.w  iADC, 2(iOutPtr)
    isw.w  iADC, 5(iOutPtr)
    isw.w  iADC, 8(iOutPtr)
#endmacro

; ---------------------------------------------------------------------
; Output windows
;
; The GS packet is built in one of two fixed windows rather than
; immediately after the input vertices, so that a program which does not
; know its output count in advance can still place it. See the window
; constants in vu1.h.
;
; The including program must define kGifTags, kWindowVerts (the window's
; vertex capacity) and kWindowPrimTag (the drawing tag's qword offset
; inside the window), and must call InitOutputWindows before the first
; Open.
;
; The window's own state - which of the two windows is current, the step
; to the other one, and where this batch's GIF tag block sits - lives in
; a spill qword rather than in VI registers. There are only 13 of those
; and openvcl hands them out by live interval, so three values that are
; live across the whole triangle loop but read at one place each cost
; three registers for the loop's entire length. Parked here they cost
; two loads at an open and two at a close, which happen once per window
; rather than once per triangle. See kWindowSpillAddr in vu1.h.
;
;   .x  iWin     the current window's address
;   .y  iDelta   +/- the step to the other window; flips on every kick
;   .z  iTagPtr  this batch's GIF tag block, iBase + kGifTags
;   .w  free for the including program's own batch state, for the same reason:
;       a flag read once per vertex is cheaper reloaded than held, and VI is
;       what the clipper runs out of first (see the warp flag in textured)
; ---------------------------------------------------------------------
#define kWindowSpill 1008

; Sets up the window state the macros below read. Must run before the
; first OpenOutputWindow, and needs iBase.
;
; The barrier is load-bearing: the very next thing a program does is an
; OpenOutputWindow, whose first act is to read back what is stored here,
; and vcl does not model VU memory aliasing - without it the loads are
; free to be hoisted above the stores. Elsewhere the two are separated
; by a label, which fences them for free.
#macro InitOutputWindows
    iaddiu iWin,    iBase, kWindowA
    iaddiu iDelta,  vi00,  kWindowQwords
    iaddiu iTagPtr, iBase, kGifTags

    isw.x iWin,    kWindowSpill(vi00)
    isw.y iDelta,  kWindowSpill(vi00)
    isw.z iTagPtr, kWindowSpill(vi00)

    --barrier
#endmacro

; Starts a packet in the window at iWin: the batch's GIF tag block, then
; the write cursor and the room left. Every window carries the whole
; block, the A+D state qwords included; re-latching state the GS already
; holds costs six qwords a kick and keeps one code path.
;
; The copy below is CopyGifTags' body written out again - see the note
; there. Change one and change the other.
#macro OpenOutputWindow
    ; The window address lands straight in the write cursor: an open has no
    ; use for iWin itself, and not naming it here is what keeps it out of
    ; the loop's live set.
    ilw.x iOutPtr, kWindowSpill(vi00)
    ilw.z iTagPtr, kWindowSpill(vi00)

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

    iaddiu iVertsLeft, vi00, kWindowVerts
#endmacro

; Patches the drawing tag with what was actually written, sends the
; window, and moves to the other one.
;
; NLOOP counts reglist loops, which for these programs is vertices. It
; and EOP are the only fields in the tag's low word - PRE, PRIM, FLG and
; NREG all sit at bit 46 and above - so isw.x cannot disturb them. The
; 0x8000 sets EOP, in two steps because iaddiu's immediate is 15 bits.
;
; Reusing the other window is safe without any fence of our own: XGKICK
; stalls if a PATH1 transfer is already in process, so this kick cannot
; return until the *previous* window has drained - and that window is
; the next one to be written. The VU manual's caution about rewriting VU
; Mem mid-transfer is about the window being sent now, which nothing
; touches again until it has come round.
;
; What does need care is the tag store just above. XGKICK hands the
; address to the GIF and ends; the GIF starts reading immediately, so a
; store in the shadow of the kick races its own latency to VU Mem. The
; window advance is placed in that gap deliberately, which is why the
; kick takes its address from a copy made before the advance rather than
; from iWin.
;
; The first instruction below must stay harmless if it lands in a branch
; delay slot - callers skip this macro by branching over it.
#macro CloseOutputWindowAndKick: lblEmpty
    iaddiu iNloop, vi00,   kWindowVerts
    isub   iNloop, iNloop, iVertsLeft

    ; Nothing was written, so there is no packet to send. Worth the test even
    ; though only a clipping program can get here: a window is otherwise closed
    ; only when a triangle will not fit, and that triangle goes straight into
    ; the fresh one - but a triangle that is clipped away emits nothing, and
    ; kicking the empty window leaves VIF1 waiting at the chain terminator's
    ; FLUSH for a PATH1 transfer the GIF never reports finishing.
    ;
    ; 'lblEmpty' has to be a label name unique to this invocation - vclpp
    ; substitutes it as text, and two invocations cannot share one.
    ibeq   iNloop, vi00, lblEmpty

    ; Read here and written back below, so neither is live outside this
    ; macro. Harmless in the branch delay slot above: on the empty path
    ; the window does not advance and nothing reads either value.
    ilw.x  iWin,   kWindowSpill(vi00)
    ilw.y  iDelta, kWindowSpill(vi00)

    iaddiu iNloop, iNloop, 0x7FFF
    iaddiu iNloop, iNloop, 1
    isw.x  iNloop, kWindowPrimTag(iWin)

    --barrier

    iaddiu iKickAt, iWin, 0
    iadd   iWin,    iWin, iDelta
    isub   iDelta,  vi00, iDelta
    isw.x  iWin,    kWindowSpill(vi00)
    isw.y  iDelta,  kWindowSpill(vi00)

    --barrier

    xgkick iKickAt

    lblEmpty:
#endmacro
