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

; The two ping-pong buffers, absolute VU addresses, and the count spill.
; Must match kClipScratchAddr and kClipSpillAddr in vu1.h.
;
; A pass reads one buffer and writes the other, so they alternate and an
; odd number of planes ends where one plane does - in B. Sized for the
; worst case rather than the observed one: cutting an n-gon against a
; plane can add a corner, so three corners in means at most eight out
; after five planes.
;
;   A   8 entries (7 corners + wrap), 16 qwords, 974..989 - read by
;       passes 1, 3, 5 and written by 2, 4
;   B   9 entries (8 corners + wrap), 18 qwords, 990..1007 - the other
;       way round, and where the survivors always end up
;
; 1008 and 1009 are the spill qwords; see vu1.h. Together that is exactly
; the 36 qwords of clip scratch, with nothing left over.
#define kClipBufA   974
#define kClipBufA1  975
#define kClipBufB   990
#define kClipBufB1  991

; Where the survivor count is parked between the last plane pass and the
; fan. It cannot stay in a register: openvcl's liveness does not carry a
; value from one loop into a sibling loop, and it will hand the register
; to a temporary inside the second one - silently. Everything else the
; two share is written in both, which is why only this one moves.
#define kClipCount 1009

; Lays the three transformed corners into buffer A, first one repeated at
; the end, and sets the corner count the first pass reads.
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

    iaddiu iCount, vi00, 3
#endmacro

; One Sutherland-Hodgman pass, against one plane, over a polygon of any
; size. This is the whole clipper: the program invokes it once per plane
; with a different selector, which is what lets the planes need no table
; in VU memory and no pointer register of their own.
;
; 'vSel' names the lane of the judgement vector, or of its negation, that
; carries this plane's term. It is the only thing that differs between
; the invocations:
;
;   near   fJn[z]    w - z      (which also excludes everything behind the eye)
;   x-     fJn[x]    w - G*x    G being what fClipScale already carries,
;   x+     fJp[x]    w + G*x    the reciprocal of the guard band limit
;   y-     fJn[y]    w - G*y
;   y+     fJp[y]    w + G*y
;
; There is no far plane. Across 606,931 clipped triangles it was never
; once straddled, and leaving it out takes a corner off the worst case.
;
; vclpp does not re-scan an expanded macro body, so a macro cannot invoke
; another one: the sign extraction and the cut are written out here
; rather than called. The label parameters need one unique set per
; invocation - vclpp substitutes them as text.
;
; In:  iCount  corners in srcBuf, first one repeated at the end
; Out: iCount  corners in dstBuf, same arrangement. Zero, or three and up.
#macro ClipPlanePass: vSel, srcBuf, dstBuf, lblLoop, lblKept, lblNoCut, lblOut
    iaddiu iWalk,  vi00,   srcBuf
    iaddiu iOut,   vi00,   dstBuf
    iaddiu iLeft,  iCount, 0
    iaddiu iCount, vi00,   0

    lblLoop:

        lq fCurPos, 0(iWalk)
        lq fCurStq, 1(iWalk)
        lq fNxtPos, 2(iWalk)
        lq fNxtStq, 3(iWalk)

        ; Both endpoints' distance to this plane, in .x.
        ;
        ; fJp is the same product the guard-band judgement takes -
        ; (G*x, G*y, z) - so every plane's term is one lane of it or of
        ; its negation, and the distance is w plus that lane.
        ;
        ; Scaled by 2048 twice afterwards. The sign is read through ftoi4,
        ; which resolves 1/16 of a unit, and the crossing test below
        ; multiplies two of these together, so the headroom has to cover
        ; the product and not just the distance. fGSScale.x is 2048
        ; already, so this costs no constant of its own.
        mul.xyz fJp,   fCurPos, fClipScale
        sub.xyz fJn,   vf00,    fJp
        addw.x  fDCur, vf00,    fCurPos
        add.x   fDCur, fDCur,   vSel
        mul.x   fDCur, fDCur,   fGSScale[x]
        mul.x   fDCur, fDCur,   fGSScale[x]

        mul.xyz fJp,   fNxtPos, fClipScale
        sub.xyz fJn,   vf00,    fJp
        addw.x  fDNxt, vf00,    fNxtPos
        add.x   fDNxt, fDNxt,   vSel
        mul.x   fDNxt, fDNxt,   fGSScale[x]
        mul.x   fDNxt, fDNxt,   fGSScale[x]

        ; Sign of this corner's distance; negative is outside. Clamped to
        ; [-1, +1] first so the ftoi4 cannot overflow the 16 bits mtir
        ; moves. vf00.w is ONE - vf00.x is zero, which is a different trap.
        mini.x  fClipT,   fDCur,  vf00[w]
        max.x   fClipT,   fClipT, fMinusOne[x]
        ftoi4.x fClipT,   fClipT
        mtir    iSignCur, fClipT[x]

        ; Keep this corner if it is inside the plane.
        ibltz iSignCur, lblKept
        sq fCurPos, 0(iOut)
        sq fCurStq, 1(iOut)
        iaddiu iOut,   iOut,   2
        iaddiu iCount, iCount, 1
        lblKept:

        ; The edge crosses when the two distances differ in sign, which is
        ; when their product is negative.
        mul.x   fClipT,    fDCur,  fDNxt
        mini.x  fClipT,    fClipT, vf00[w]
        max.x   fClipT,    fClipT, fMinusOne[x]
        ftoi4.x fClipT,    fClipT
        mtir    iSignProd, fClipT[x]
        ibgez   iSignProd, lblNoCut

        ; Interpolate the cut onto the plane: t = dCur / (dCur - dNxt).
        ;
        ; Q lands in a register and both interpolations read it from there.
        ; Pipeline Q with more than one consumer is only correct while the
        ; scheduler keeps every consumer inside the divide's window, and
        ; the next regeneration is under no obligation to.
        sub.x  fClipT, fDCur,    fDNxt
        div    q,      fDCur[x], fClipT[x]
        addq.x fClipQ, vf00,     q

        sub fCutPos, fNxtPos, fCurPos
        sub fCutStq, fNxtStq, fCurStq
        mul fCutPos, fCutPos, fClipQ[x]
        mul fCutStq, fCutStq, fClipQ[x]
        add fCutPos, fCurPos, fCutPos
        add fCutStq, fCurStq, fCutStq

        sq fCutPos, 0(iOut)
        sq fCutStq, 1(iOut)
        ; The cut's colour lane came out of the FMAC as garbage - two
        ; denormal bit patterns lerped - so put the edge's first corner's
        ; packed colour back over it with a raw store. Telling the inside
        ; corner from the outside one would cost a branch, and for a
        ; flat-coloured batch, which is nearly all of them, they are equal.
        sq.x fCurStq, 1(iOut)
        iaddiu iOut,   iOut,   2
        iaddiu iCount, iCount, 1
        lblNoCut:

        iaddiu iWalk, iWalk, 2
        iaddi  iLeft, iLeft, -1
        ibgtz  iLeft, lblLoop

    ; Repeat the first survivor at the end, so the next pass - or the fan -
    ; walks edges without a wrap test. A polygon that lost too many corners
    ; is finished, and leaves iCount below three for everyone downstream.
    iaddi iLeft, iCount, -3
    ibltz iLeft, lblOut
    lq fCurPos, dstBuf + 0(vi00)
    lq fCurStq, dstBuf + 1(vi00)
    sq fCurPos, 0(iOut)
    sq fCurStq, 1(iOut)
    lblOut:
#endmacro

; Guard-band judgement for a vertex the clipper produced. A survivor can
; still lie outside a plane the clipper does not cut against, so it is
; judged before it is emitted exactly as an unclipped corner is. Leaves
; this vertex's flags newest in the clip flag register.
#macro ClipJudge: vPos
    mul.xyz   fJudge, vPos, fClipScale
    clipw.xyz fJudge, vPos[w]
#endmacro
