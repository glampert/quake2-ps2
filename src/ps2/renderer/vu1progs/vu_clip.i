;--------------------------------------------------------------------
; vu_clip.i
;
; Sutherland-Hodgman clipping on VU1, for textured_triangles.vcl.
;
; The microprograms used to reject a triangle whole when any corner left
; the clip volume, which is why geometry that can cross those planes had
; to be cut on the EE first. These macros cut it here instead.
;
; A polygon lives in one of two scratch buffers as 3 qwords per vertex -
; the clip-space position, then (rgba, s, t, q) exactly as the vertex
; arrived, then the colour the transform computed - with the first vertex
; repeated once at the end, so the edge walk runs 0-1, 1-2, ... n-1,0 with
; no wrap test. The buffers ping-pong: a plane pass reads one and writes
; the other.
;
; Everything in those three qwords is interpolated at a cut, the packed
; colour word excepted: it is a bit pattern, not a number, and is put back
; over the result afterwards. The third qword is what lets the dynamic
; light mode clip at all - its colour is computed from the world position,
; which a cut vertex does not have.
;
; Signs come from the float distances rather than from the clip flag
; register. The flag register's six bits per vertex are documented
; nowhere in this tree - every existing use is an "any bit set" mask -
; and a wrong bit position would mis-render in silence. Clamping the
; distance and reading the sign of its ftoi4 is the same trick the
; keyframe backface cull uses, and openvcl cannot reorder it away
; because the sign travels as data.
;
; Every macro here is a leaf: vclpp does not re-scan an expanded body,
; so these are called from a #vuprog body, never from inside another macro.
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
;   A   8 entries (7 corners + wrap), 24 qwords, 957..980 - read by
;       passes 1, 3, 5 and written by 2, 4
;   B   9 entries (8 corners + wrap), 27 qwords, 981..1007 - the other
;       way round, and where the survivors always end up
;
; 1008 and 1009 are the spill qwords; see vu1.h. 956 is the odd qword the
; re-tile left over and nothing uses it.
#define kClipBufA   957
#define kClipBufB   981

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
    sq fCol0, 2(iScratch)
    sq fPos1, 3(iScratch)
    sq fStq1, 4(iScratch)
    sq fCol1, 5(iScratch)
    sq fPos2, 6(iScratch)
    sq fStq2, 7(iScratch)
    sq fCol2, 8(iScratch)
    sq fPos0, 9(iScratch)
    sq fStq0, 10(iScratch)
    sq fCol0, 11(iScratch)

    iaddiu iCount, vi00, 3
    isw.x  iCount, kClipCount(vi00)
#endmacro

; One Sutherland-Hodgman pass, against one plane, over a polygon of any
; size. This is the whole clipper: the program invokes it once per plane
; with a different selector, which is what lets the planes need no table
; in VU memory and no pointer register of their own.
;
; 'vSelCur'/'vSelNxt' name the lane of the judgement vector, or of its
; negation, that carries this plane's term. They are the only thing that
; differs between the invocations:
;
;   near   fJn?[z]   w - z      (which also excludes everything behind the eye)
;   x-     fJn?[x]   w - G*x    G being what fClipScale already carries,
;   x+     fJp?[x]   w + G*x    the reciprocal of the guard band limit
;   y-     fJn?[y]   w - G*y
;   y+     fJp?[y]   w + G*y
;
; The two endpoints get their own judgement registers - C for cur, N for nxt -
; which is why the selector is passed twice. Sharing one pair made every
; instruction of the second endpoint's chain wait on the first's through a
; write-after-read hazard, and openvcl ran the two strictly in series: 58% of
; the loop was nops covering FMAC latency that the other endpoint's work
; should have been filling. The chains are independent; only the register
; names were not.
;
; There is no far plane. Across 606,931 clipped triangles it was never
; once straddled, and leaving it out takes a corner off the worst case.
;
; vclpp does not re-scan an expanded macro body, so a macro cannot invoke
; another one: the sign extraction and the cut are written out here
; rather than called. The label parameters need one unique set per
; invocation - vclpp substitutes them as text.
;
; The corner count travels through kClipCount rather than in a register.
; Two passes in a row are sibling loops, and openvcl's liveness does not
; carry a value from one into the next - it hands the register to a
; temporary inside the second one, with no diagnostic. Reading and writing
; it here also means the count is live nowhere but inside a pass.
;
; In:  kClipCount  corners in srcBuf, first one repeated at the end
; Out: kClipCount  corners in dstBuf, same arrangement. Zero, or three up.
#macro ClipPlanePass: vSelCur, vSelNxt, srcBuf, dstBuf, lblLoop, lblKept, lblNoCut, lblWrap, lblOut
    ilw.x  iLeft,  kClipCount(vi00)

    ; Dead unless proven otherwise: every path out of here that drops the
    ; polygon leaves this store standing, and only the surviving one
    ; overwrites it.
    iaddiu iCount, vi00, 0
    isw.x  iCount, kClipCount(vi00)

    ; An earlier plane already finished it off.
    iaddi iSignCur, iLeft, -3
    ibltz iSignCur, lblOut

    iaddiu iWalk, vi00, srcBuf
    iaddiu iOut,  vi00, dstBuf

    lblLoop:

        lq fCurPos, 0(iWalk)
        lq fCurStq, 1(iWalk)
        lq fCurCol, 2(iWalk)
        lq fNxtPos, 3(iWalk)
        lq fNxtStq, 4(iWalk)
        lq fNxtCol, 5(iWalk)

        ; Both endpoints' distance to this plane, in .x.
        ;
        ; fJp is the same product the guard-band judgement takes -
        ; (G*x, G*y, z) - so every plane's term is one lane of it or of
        ; its negation, and the distance is w plus that lane.
        ;
        ; w is shrunk by fGSScale[w] first, so a cut lands strictly inside the
        ; plane rather than exactly on it - the judgement further down tests the
        ; same quantity, and on a tie it is the divide's rounding that decides
        ; whether the triangle lives. See vu1::kVuClipShrink.
        ;
        ; Scaled by fGSOffset[w] afterwards - see vu1::kVuClipDistScale. The
        ; sign is read through ftoi4, which resolves 1/16 of a unit, and the
        ; crossing test below multiplies two of these together, so the
        ; headroom has to cover the product and not just the distance.
        ;
        ; The two endpoints are interleaved by hand. They are independent, but
        ; openvcl will not reorder across them on its own - it emitted one
        ; chain then the other, three nops between every pair, and renaming the
        ; registers changed nothing because it coalesces them straight back.
        ; Written alternately, each instruction covers the other's latency.
        mul.xyz fJpC,  fCurPos, fClipScale
        mul.xyz fJpN,  fNxtPos, fClipScale
        addw.x  fDCur, vf00,    fCurPos
        addw.x  fDNxt, vf00,    fNxtPos
        sub.xyz fJnC,  vf00,    fJpC
        sub.xyz fJnN,  vf00,    fJpN
        mul.x   fDCur, fDCur,   fGSScale[w]
        mul.x   fDNxt, fDNxt,   fGSScale[w]
        add.x   fDCur, fDCur,   vSelCur
        add.x   fDNxt, fDNxt,   vSelNxt
        mul.x   fDCur, fDCur,   fGSOffset[w]
        mul.x   fDNxt, fDNxt,   fGSOffset[w]

        ; Two signs, taken together: this corner's distance, and the product
        ; of the two endpoints' - which is negative exactly when the edge
        ; crosses the plane, since that is when they differ in sign.
        ;
        ; Both are clamped to [-1, +1] first so the ftoi4 cannot overflow the
        ; 16 bits mtir moves. vf00.w is ONE - vf00.x is zero, which is a
        ; different trap.
        ;
        ; The two chains are four deep and independent, and are interleaved by
        ; hand for the same reason the distances above are: openvcl will not
        ; overlap them on its own, and run one after the other they are three
        ; nops in every four. The crossing test used to sit after the keep
        ; branch, where it could not be overlapped with anything at all.
        mul.x   fClipP,    fDCur,   fDNxt
        mini.x  fClipT,    fDCur,   vf00[w]
        mini.x  fClipP,    fClipP,  vf00[w]
        max.x   fClipT,    fClipT,  fMinusOne[x]
        max.x   fClipP,    fClipP,  fMinusOne[x]
        ftoi4.x fClipT,    fClipT
        ftoi4.x fClipP,    fClipP
        mtir    iSignCur,  fClipT[x]
        mtir    iSignProd, fClipP[x]

        ; Keep this corner if it is inside the plane.
        ibltz iSignCur, lblKept
        sq fCurPos, 0(iOut)
        sq fCurStq, 1(iOut)
        sq fCurCol, 2(iOut)
        iaddiu iOut,   iOut,   3
        iaddiu iCount, iCount, 1
        lblKept:

        ibgez iSignProd, lblNoCut

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
        sub fCutCol, fNxtCol, fCurCol
        mul fCutPos, fCutPos, fClipQ[x]
        mul fCutStq, fCutStq, fClipQ[x]
        mul fCutCol, fCutCol, fClipQ[x]
        add fCutPos, fCurPos, fCutPos
        add fCutStq, fCurStq, fCutStq
        add fCutCol, fCurCol, fCutCol

        sq fCutPos, 0(iOut)
        sq fCutStq, 1(iOut)
        sq fCutCol, 2(iOut)
        ; The cut's packed colour lane came out of the FMAC as garbage - two
        ; denormal bit patterns lerped - so put the edge's first corner's
        ; packed colour back over it with a raw store. Telling the inside
        ; corner from the outside one would cost a branch, and for a
        ; flat-coloured batch, which is nearly all of them, they are equal.
        ;
        ; Unconditional although only one colour mode reads that word. The
        ; computed colour lives in its own qword and this store cannot reach
        ; it; the two forms share no lane, which is the whole reason the third
        ; qword exists.
        sq.x fCurStq, 1(iOut)
        iaddiu iOut,   iOut,   3
        iaddiu iCount, iCount, 1
        lblNoCut:

        iaddiu iWalk, iWalk, 3
        iaddi  iLeft, iLeft, -1
        ibgtz  iLeft, lblLoop

    ; Repeat the first survivor at the end, so the next pass - or the fan -
    ; walks edges without a wrap test.
    ;
    ; A polygon that lost too many corners is finished, and is forced to
    ; nothing rather than left as it fell. Sutherland-Hodgman on a convex
    ; input leaves none or three and up, but a corner sitting exactly on the
    ; plane can leave two - and two corners with no wrap vertex is an edge
    ; list the next pass would walk straight off the end of.
    iaddi  iSignCur, iCount, -3
    ibgez  iSignCur, lblWrap
    iaddiu iCount,   vi00, 0
    lblWrap:

    ibltz iSignCur, lblOut
    lq fCurPos, dstBuf + 0(vi00)
    lq fCurStq, dstBuf + 1(vi00)
    lq fCurCol, dstBuf + 2(vi00)
    sq fCurPos, 0(iOut)
    sq fCurStq, 1(iOut)
    sq fCurCol, 2(iOut)
    isw.x iCount, kClipCount(vi00)
    lblOut:
#endmacro

; Guard-band judgement for a vertex the clipper produced, before it is
; emitted, exactly as an unclipped corner is judged. What it can still catch
; is the far plane, which the clipper does not cut against - measured never to
; be straddled, so in practice this passes every survivor; it stays because a
; triangle that did cross far would otherwise go out unjudged. Leaves this
; vertex's flags newest in the clip flag register.
#macro ClipJudge: vPos
    mul.xyz   fJudge, vPos, fClipScale
    clipw.xyz fJudge, vPos[w]
#endmacro
