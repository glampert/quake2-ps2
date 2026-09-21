;--------------------------------------------------------------------
; vu_clip.i
;
; Sutherland-Hodgman clipping on VU1, for the triangle programs.
;
; The microprograms used to reject a triangle whole when any corner left
; the clip volume, which is why geometry that can cross those planes had
; to be cut on the EE first. These macros cut it here instead.
;
; A polygon lives in one of two scratch buffers as 2 qwords per vertex -
; the clip-space position, then (rgba, s, t, q) exactly as the vertex
; arrived - with the first vertex repeated once at the end, so the edge
; walk runs 0-1, 1-2, ... n-1,0 with no wrap test. The buffers ping-pong:
; a plane pass reads one and writes the other.
;
; Signs come from the float distances rather than from the clip flag
; register. The flag register's six bits per vertex are documented
; nowhere in this tree - every existing use is an "any bit set" mask -
; and a wrong bit position would mis-render in silence. Clamping the
; distance and reading the sign of its ftoi4 is the same trick the lerp
; program's backface cull uses, and openvcl cannot reorder it away
; because the sign travels as data.
;
; Every macro here is a leaf: vclpp does not re-scan an expanded body,
; so these are called from a #vuprog body, never from inside DoVertex.
; Macros taking label names need one unique set per invocation.
;--------------------------------------------------------------------

; Scratch buffers, absolute VU addresses. Must match kClipScratchAddr in
; vu1.h; the second starts half way up.
#define kClipBufA  974
#define kClipBufA1 975
#define kClipBufB  992
#define kClipBufB1 993
#define kClipBufB2 994
#define kClipBufB3 995
#define kClipBufB4 996
#define kClipBufB5 997
#define kClipBufB6 998
#define kClipBufB7 999

; What the survivor end pointer reads when three corners came through the
; plane, and when four did. Cutting a triangle against one plane can leave no
; more than four, which is what lets the fan be written out rather than looped.
#define kClipSurvived3 998
#define kClipSurvived4 1000

; Where the survivor end pointer is parked between the plane pass and the
; fan. It cannot stay in a register: openvcl's liveness does not carry a
; value from one loop into a sibling loop, and it will hand the register
; to a temporary inside the second one - silently. Everything else the
; two loops share is written in both, which is why only this one moves.
#define kClipSpill 1009

; Distance to the near plane, in .x. The microprogram's judgement is
; |z| > |w|, so the plane is w - z >= 0 - which also excludes everything
; behind the camera.
;
; Scaled by 2048 twice before the sign is read. ftoi4 resolves 1/16 of a
; unit and the clamp below caps at 1, so without the scale every vertex
; within 1/16 of the plane would read as inside; with it the dead zone is
; about 1.5e-8 clip-space units. fGSScale.x is 2048 already, so this
; costs no constant of its own.
#macro ClipNearDist: vD, vPos
    addw.x vD, vf00, vPos
    sub.x  vD, vD,   vPos[z]
    mul.x  vD, vD,   fGSScale[x]
    mul.x  vD, vD,   fGSScale[x]
#endmacro

; Sign of a distance into an integer register: negative means outside.
; Clamped to [-1, +1] first so the ftoi4 cannot overflow the 16 bits
; mtir moves.
#macro ClipSignOf: iSign, vD
    mini.x  fClipT, vD,     vf00[w]
    max.x   fClipT, fClipT, fMinusOne[x]
    ftoi4.x fClipT, fClipT
    mtir    iSign,  fClipT[x]
#endmacro

; Lays the three transformed corners into buffer A, first one repeated
; at the end, and points the pass at it.
#macro ClipSeedTriangle: iScratch
    iaddiu iScratch, vi00, kClipBufA

    sq fPos0, 0(iScratch)
    sq fStq0, 1(iScratch)
    sq fPos1, 2(iScratch)
    sq fStq1, 3(iScratch)
    sq fPos2, 4(iScratch)
    sq fStq2, 5(iScratch)
    sq fPos0, 6(iScratch)
    sq fStq0, 7(iScratch)
#endmacro

; Interpolates one cut vertex onto the plane, between a vertex inside it
; and one outside. t = dIn / (dIn - dOut), applied to both qwords.
;
; The colour lane rides through the FMAC and comes out garbage - two
; denormal bit patterns lerped - which is exactly what mulq already does
; to it on the emit path. It never reaches the GS: the emit stores the
; colour with sq.x straight from an untouched register, so a cut vertex
; simply takes the colour of the inside end of its edge.
#macro ClipCutVertex: vOutPos, vOutStq, vInPos, vInStq, vFarPos, vFarStq, vDIn, vDFar
    sub.x fClipT, vDIn, vDFar
    div   q,      vDIn[x], fClipT[x]

    ; Q lands in a register here, and the two interpolations read it from
    ; there. Pipeline Q with more than one consumer is only correct while the
    ; scheduler keeps every consumer inside the divide's window, and the next
    ; regeneration is under no obligation to. One consumer, then broadcasts.
    ; vf00.x is zero - vf00.w is ONE, which is a different trap.
    addq.x fClipQ, vf00, q

    sub vOutPos, vFarPos, vInPos
    sub vOutStq, vFarStq, vInStq
    mul vOutPos, vOutPos, fClipQ[x]
    mul vOutStq, vOutStq, fClipQ[x]
    add vOutPos, vInPos,  vOutPos
    add vOutStq, vInStq,  vOutStq
#endmacro

; Guard-band judgement for a vertex the clipper produced. The plane pass
; cuts against near only, so a survivor can still lie outside the band
; and must be judged before it is emitted, exactly as an unclipped corner
; is. Leaves this vertex's flags newest in the clip flag register.
#macro ClipJudge: vPos
    mul.xyz   fJudge, vPos, fClipScale
    clipw.xyz fJudge, vPos[w]
#endmacro
