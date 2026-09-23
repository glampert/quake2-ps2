;--------------------------------------------------------------------
; textured_triangles.vcl
;
; A VU1 microprogram to draw a batch of gouraud-shaded, textured
; triangles (triangle list). Preprocessed with vclpp; the -j flag
; injects the VCL boilerplate (.init_*, --enter/--exit blocks).
;
; VU data memory layout (qwords; must match vu1.cpp):
;   0-3  MVP matrix rows (row-vector convention)
;   4    GS scale  (2048, 2048, zScale)
;   5    GS offset (2048 + width/2, 2048 + height/2, zScale)
;   6    clip-judgement scale (guard band for x/y, 1.0 for z)
;   8+   XTOP double buffers (VIF1 BASE/OFFSET)
;
; Batch layout at XTOP:
;   +0    header: vertex count in .w
;   +1    7 GIF tag qwords (set tag, TEST/TEX1/TEX0/ALPHA/ZBUF A+D, prim tag)
;   +8    vertices, 2 qwords each: position, then (rgba, s, t, q)
;   +188  output window A: a copy of the 7 tags, then 3 qwords per
;   +330  output window B:   vertex - ST, RGBAQ, XYZ2 - up to 45 of them
;
; The GS packet goes to one of the two output windows rather than to
; the space after the input vertices. The program fills a window, patches
; its drawing tag with the vertex count it actually wrote, sends it with
; XGKICK and carries on in the other one, so a full 90-vertex batch is
; two kicks. That indirection buys nothing yet - the count is known up
; front here - but it is what lets a clipping program, whose output count
; is not known until it has run, place its packet at all.
;
; The color arrives packed in the .x word of the second input qword and
; is raw-copied into an A+D qword: the native RGBAQ register layout is
; exactly that u32 with Q in the word above, so the bytes never need
; spreading apart (and Q rides inline, independent of the GIF's
; ST-latched Q). Clipping is a whole-triangle guard band reject: clipw
; flags outside the scaled |w| range set the ADC bit on all 3 vertices so
; the GS skips the drawing kick.
;--------------------------------------------------------------------

#include "vu_common.i"
#include "vu_clip.i"

; Batch offsets, relative to XTOP. The window values must match the
; kOutputWindow* / kMaxVertsPerWindow constants in vu1.h.
#define kBatchHeader 0
#define kGifTags     1
#define kVertexData  8

; The two output windows: where they start, how far apart they are, how
; many vertices each holds, and where the drawing tag sits inside one.
#define kWindowA       188
#define kWindowB       330
#define kWindowQwords  142
#define kWindowVerts   45
#define kWindowPrimTag 6

; The dynamic light block, uploaded once per draw chain. Only colour mode 1
; reads it. Must match vu1::kLightBlockAddr.
#define kLightBlock  1010

; Reads one vertex and leaves it in clip space, in vPos/vStq, without
; touching the output. Splitting the transform from the emission is what
; lets a clipper sit between them: the survivors of a cut are emitted,
; the rest never are, and neither is known until all three corners have
; been transformed.
;
; Leaves this vertex's clipw flags as the newest entry in the clip flag
; register; the caller judges whole triangles with fcand after 3 calls.
;
; C-like pseudo-code ('in' is the qword array at iInPtr):
;
;   void ClipTransform(int offPos, int offStq, vec4& pos, vec4& stq)
;   {
;       pos = in[offPos];
;       stq = in[offStq]; // (rgba, s, t, q); raw packed color in .x
;
;       // Object space to clip space (row-vector MVP):
;       pos = pos.x * mvp[0] + pos.y * mvp[1]
;           + pos.z * mvp[2] + pos.w * mvp[3];
;
;       // Guard-band clip judgement: compare the scaled position
;       // against |w| and push the 6 outside flags (+x,-x,+y,-y,+z,-z)
;       // onto the clip flag queue for the caller to inspect:
;       vec3 judge = pos.xyz * clipScale.xyz;
;       clipFlagQueue.push(judge, abs(pos.w));
;   }
#macro ClipTransform: vPos, vStq, vCol, offPos, offStq, lblNoLight

    lq vPos, offPos(iInPtr)
    lq vStq, offStq(iInPtr)

    ; Colour mode 1: the frame's point lights, summed here rather than at the
    ; emit because the sum is a function of the vertex's *world* position, and
    ; the transform below is about to overwrite it. Computing it here also makes
    ; it an ordinary per-vertex attribute, which is what a clipper needs - a cut
    ; vertex has no world position to light, only two endpoints to interpolate
    ; between, and light is linear in exactly the way Gouraud already assumes.
    ; Defined on both paths before it is read: openvcl checks that statically and
    ; nothing below this branch writes it in mode 0. One instruction, and the
    ; alternative is a register the emit could read as whatever was left in it.
    move vCol, vf00
    ibeq iColorMode, vi00, lblNoLight

    ; Four light vectors at once, one lane per light, then squared distances.
    sub fLx, fLightX, vPos[x]
    sub fLy, fLightY, vPos[y]
    sub fLz, fLightZ, vPos[z]
    mul  acc,   fLx, fLx
    madd acc,   fLy, fLy
    madd fDist, fLz, fLz

    ; Start at black, with the alpha the lightmap pass needs already in place:
    ; .w stays untouched by the .xyz lighting below and converts to the GS 1.0.
    move.xyz vCol, vf00
    move.w   vCol, fLightClamp

    ; light += max(distSqr.i * -(colour_i / radius_i^2) + colour_i, 0)
    mula.xyz acc,   fNegDiv0, fDist[x]
    madd.xyz fTerm, fColour0, vf00[w]
    max.xyz  fTerm, fTerm,    vf00
    add.xyz  vCol,  vCol,     fTerm

    mula.xyz acc,   fNegDiv1, fDist[y]
    madd.xyz fTerm, fColour1, vf00[w]
    max.xyz  fTerm, fTerm,    vf00
    add.xyz  vCol,  vCol,     fTerm

    mula.xyz acc,   fNegDiv2, fDist[z]
    madd.xyz fTerm, fColour2, vf00[w]
    max.xyz  fTerm, fTerm,    vf00
    add.xyz  vCol,  vCol,     fTerm

    mula.xyz acc,   fNegDiv3, fDist[w]
    madd.xyz fTerm, fColour3, vf00[w]
    max.xyz  fTerm, fTerm,    vf00
    add.xyz  vCol,  vCol,     fTerm

    ; Saturate to the GS byte range. ftoi0 happens at the emit, one byte per
    ; word, which is the PACKED RGBAQ layout.
    mini.xyz vCol, vCol, fLightClamp

    lblNoLight:

    ; Position to clip space (row-vector MVP):
    mul  acc,  fMVP0, vPos[x]
    madd acc,  fMVP1, vPos[y]
    madd acc,  fMVP2, vPos[z]
    ; The MVP's translation row is scaled by a hardwired 1.0, not by the vertex's
    ; own .w: PolyVertex parks its lightmap S there, and every other DrawVertex
    ; producer writes a 1.0 that this no longer needs. Same reason
    ; lerped_triangles.vcl does it - see the note on mod::PolyVertex.
    madd vPos, fMVP3, vf00[w]

    ; Guard-band clip judgement against |w|: scaled x/y, exact z.
    mul.xyz   fJudge, vPos, fClipScale
    clipw.xyz fJudge, vPos[w]

#endmacro

; Projects one clip-space vertex and writes its three output qwords: ST,
; RGBAQ (via A+D) and XYZ2, at offST/offAD/offXyz from iOutPtr.
;
; The packed color is moved with raw copies only (lq/sq.x): FMAC ops
; would flush denormal color bit patterns (e.g. 0x800000FF) to zero.
;
; vPos is read, never written - the clipper needs the clip-space position
; to survive, so the projection lands in a scratch register instead.
;
; C-like pseudo-code:
;
;   void EmitVertex(vec4 pos, vec4 stq, int offST, int offAD, int offXyz)
;   {
;       // Perspective divide; the STQ words share the 1/w so the GS
;       // gets (s/w, t/w, 1/w) for perspective-correct interpolation.
;       // The color bits get scaled too - garbage, but the rotate
;       // below moves it into the ST qword's ignored .w:
;       float q    = 1.0f / pos.w;
;       vec3  proj = pos.xyz * q;         // now NDC
;       vec4  stqScaled = stq * q;        // (junk, s/w, t/w, 1/w)
;
;       // NDC to GS window coordinates, in 12.4 fixed point:
;       proj = ftoi4(gsOffset.xyz + proj * gsScale.xyz);
;
;       out[offST]      = stqScaled.yzwx; // ST (.z carries Q; .w junk)
;       out[offAD].x    = stq.x;          // native RGBAQ: packed color...
;       out[offAD].y    = q;              // ...with Q in the word above
;       out[offAD].z    = 0x01;           // A+D destination: RGBAQ register
;       out[offXyz].xyz = proj;           // XYZ (.w ADC bit set by caller)
;   }
#macro EmitVertex: vPos, vStq, vCol, offST, offAD, offXyz, lblPacked, lblDone

    div        q,          vf00[w], vPos[w]
    mul.xyz    fProj,      vPos,    q
    mulq       fStqScaled, vStq,    q
    addq.y     fQ,         vf00,    q

    ; NDC to GS window coordinates, in 12.4 fixed point:
    mula.xyz  acc,   fGSOffset, vf00[w]
    madd.xyz  fProj, fProj, fGSScale
    ftoi4.xyz fProj, fProj

    ; Rotate (junk, sq, tq, q) into ST order (sq, tq, q, junk):
    mr32 fST, fStqScaled

    ; Word 2 of a PACKED ST write latches Q for an RGBAQ that follows it, which
    ; is where colour mode 1 gets its Q from - it cannot ride in from the vertex
    ; the way the A+D form's does, because PolyVertex keeps its lightmap T in
    ; that lane. Written unconditionally: the A+D form sets Q in its own write
    ; and leaves this word unread. vf00.z is zero, so the reciprocal lands exact.
    ;
    addq.z fST, vf00, q

    sq     fST,  offST(iOutPtr)
    sq.xyz fProj, offXyz(iOutPtr)

    ibeq iColorMode, vi00, lblPacked

    ; Mode 1: a computed colour, four floats. ftoi0 lands them one byte per
    ; word, which is exactly what the PACKED RGBAQ descriptor reads.
    ftoi0 fRGBA, vCol
    sq    fRGBA, offAD(iOutPtr)
    b lblDone

    lblPacked:
    ; Mode 0: the packed u32 straight out of the untouched input register, as an
    ; A+D write to RGBAQ with Q alongside it.
    sq.x   vStq, offAD(iOutPtr)
    sq.y   fQ,   offAD(iOutPtr)
    isw.z  iRegRGBAQ, offAD(iOutPtr)

    lblDone:

#endmacro

; C-like pseudo-code of the program below ('vuMem' is VU1 data memory
; seen as an array of qwords):
;
;   void VU1Prog_TexturedTriangles()
;   {
;       // Frame constants at the fixed low addresses:
;       mat4 mvp       = vuMem[0..3];
;       vec4 gsScale   = vuMem[4];
;       vec4 gsOffset  = vuMem[5];
;       vec4 clipScale = vuMem[6];
;
;       // This batch, in the current double buffer:
;       qword* batch    = &vuMem[XTOP];
;       int    numVerts = batch[kBatchHeader].w;
;       qword* in       = &batch[kVertexData]; // 2 qwords per vertex
;
;       // Output goes to a window, which is opened by copying the 7
;       // GIF tag qwords the EE prepared to its head:
;       qword* win       = &vuMem[XTOP + kWindowA];
;       int    winStep   = kWindowQwords; // flips sign on every kick
;       qword* out       = OpenWindow(win);
;       int    vertsLeft = kWindowVerts;
;
;       do // One triangle per iteration:
;       {
;           if (vertsLeft < 3) // No room: send this window, start the other.
;           {
;               win[kWindowPrimTag].nloop = kWindowVerts - vertsLeft;
;               XGKICK(win);
;               win += winStep; winStep = -winStep;
;               out = OpenWindow(win); vertsLeft = kWindowVerts;
;           }
;
;           DoVertex(0, 1,  0, 1, 2); // in[0..1] -> out[0..2]
;           DoVertex(2, 3,  3, 4, 5); // in[2..3] -> out[3..5]
;           DoVertex(4, 5,  6, 7, 8); // in[4..5] -> out[6..8]
;
;           // Whole-triangle guard band reject: if any of the 18 clip
;           // flags of the 3 vertices above is set, adc becomes 0x8000,
;           // i.e. bit 15 - the ADC bit - and the GS skips this
;           // triangle's drawing kick.
;           int adc = 0x7FFF + (clipFlagQueue.last3() != 0 ? 1 : 0);
;           out[2].w = adc; // .w of each of the 3 vertices
;           out[5].w = adc;
;           out[8].w = adc;
;
;           in  += 6;
;           out += 9;
;           vertsLeft -= 3;
;           numVerts  -= 3;
;       }
;       while (numVerts != 0);
;
;       // The last window always holds at least one triangle.
;       win[kWindowPrimTag].nloop = kWindowVerts - vertsLeft;
;       XGKICK(win);
;   }
#vuprog VU1Prog_TexturedTriangles

    LoadFrameConstants{ }

    ; A+D destination address the per-vertex color qwords carry in .z
    ; (0x01 = the RGBAQ register):
    iaddiu iRegRGBAQ, vi00, 1

    ; -1.0, the lower end of the clipper's distance clamp (vf00.w is the
    ; upper one). VU1 has no float immediates.
    sub.x fMinusOne, vf00, vf00[w]

    ; Current double buffer and this batch's pointers:
    ; Which colour form this batch carries; see the emit. Read before the
    ; light block, because only mode 1 has any use for it.
    xtop   iBase
    ilw.w  iNumVerts,  kBatchHeader(iBase)
    ilw.x  iColorMode, kBatchHeader(iBase)

    iaddiu iInPtr, iBase, kVertexData

    ; The first output window, the step that alternates to the other one,
    ; and where this batch's GIF tags are - all parked in VU memory rather
    ; than held in VI registers. See vu_common.i.
    InitOutputWindows{ }

    OpenOutputWindow{ }

    ; One triangle per iteration:
    lTriangleLoop:

        ; The dynamic light block, re-read every triangle rather than held.
        ;
        ; Held, it is twelve float registers live from the top of the program to
        ; the bottom, and with the clipper's own fifteen that is more than the
        ; thirty-two this machine has - openvcl says so, in as many words. Read
        ; here it is live only as far as the third transform below, which is
        ; before the clipper needs anything, so the two sets never overlap.
        ;
        ; Unconditional, although only colour mode 1 reads it: guarding it would
        ; leave the registers undefined on the other path, which openvcl rejects
        ; statically whether or not the read is reachable.
        lq fLightX,     kLightBlock + 0(vi00)
        lq fLightY,     kLightBlock + 1(vi00)
        lq fLightZ,     kLightBlock + 2(vi00)
        lq fNegDiv0,    kLightBlock + 3(vi00)
        lq fNegDiv1,    kLightBlock + 4(vi00)
        lq fNegDiv2,    kLightBlock + 5(vi00)
        lq fNegDiv3,    kLightBlock + 6(vi00)
        lq fColour0,    kLightBlock + 7(vi00)
        lq fColour1,    kLightBlock + 8(vi00)
        lq fColour2,    kLightBlock + 9(vi00)
        lq fColour3,    kLightBlock + 10(vi00)
        lq fLightClamp, kLightBlock + 11(vi00)

        ; Room for everything this triangle can produce, asked once, here.
        ;
        ; Two rules meet at this line. It has to come *before* the corners are
        ; transformed, because OpenOutputWindow reloads the GIF tag block into
        ; seven VF registers and openvcl will put those on top of a
        ; transformed corner that is still live across the call. And it has to
        ; cover the worst case up front - eighteen vertices, since five planes
        ; can each add a corner, so eight corners fan to six triangles -
        ; because asking again from inside the clipper means more window
        ; opens and kicks in there, and that is what stalls VIF1.
        ;
        ; Eighteen of a 45-vertex window is a lot to hold back for the 2% of
        ; triangles that clip, and the measured fan never exceeded six corners
        ; anyway. The cost is paid in window occupancy, not in vertices: a
        ; window closes once fewer than eighteen are left, so it carries
        ; between 27 and 45 instead of always 45, and a full chunk takes two
        ; to four kicks where it took two. GsWait is the canary.
        ; Through a register, not an immediate: iaddi's immediate is five bits
        ; signed, and dvp-as truncates anything larger without a word - -18
        ; assembles to exactly the same instruction as +14, which made this
        ; test always pass and walked the GIF straight out of the window.
        ; check_vu_immediates.py now fails the build on it.
        iaddiu iRoom, vi00,       18
        isub   iRoom, iVertsLeft, iRoom
        ibgez iRoom, lWindowHasRoom
        CloseOutputWindowAndKick{ lKicked1 }
        OpenOutputWindow{ }
        lWindowHasRoom:

        ClipTransform{ fPos0, fStq0, fCol0, 0, 1, lLight0Done }
        ClipTransform{ fPos1, fStq1, fCol1, 2, 3, lLight1Done }
        ClipTransform{ fPos2, fStq2, fCol2, 4, 5, lLight2Done }

        ; Did any corner leave the volume? About one triangle in fifty
        ; does; the rest go straight out below.
        JudgeTriangleAdc{ }
        ibne  vi01, vi00, lClipTriangle

        ; --- every corner inside: emit the triangle as it stands ---
        EmitVertex{ fPos0, fStq0, fCol0, 0, 1, 2, lEmitUn0Packed, lEmitUn0Done }
        EmitVertex{ fPos1, fStq1, fCol1, 3, 4, 5, lEmitUn1Packed, lEmitUn1Done }
        EmitVertex{ fPos2, fStq2, fCol2, 6, 7, 8, lEmitUn2Packed, lEmitUn2Done }
        StoreTriangleAdc{ }

        iaddiu iOutPtr,    iOutPtr,     9
        iaddi  iVertsLeft, iVertsLeft, -3
        b lTriangleAdvance

        ; --- a corner is outside: cut against near, fan what survives ---
    lClipTriangle:

        ClipSeedTriangle{ iWalk }

        ; One plane for now. The other four guard-band sides are the same
        ; macro with a different selector, and go in next.
        ; Five planes: near, then the four guard-band sides. No far plane -
        ; it was never once straddled across 606,931 clipped triangles.
        ;
        ; The buffers ping-pong A->B->A->B->A->B, so an odd number of passes
        ; leaves the survivors in B, which is where the fan looks. A is sized
        ; for the 7 corners pass 4 can write and B for the 8 pass 5 can, each
        ; plus its wrap vertex, and that is the clip scratch exactly full.
        ;
        ; Near goes first because it collapses the most geometry, so the four
        ; passes behind it walk shorter edge lists.
        ClipPlanePass{ fJnC[z], fJnN[z], kClipBufA, kClipBufB, lNearLoop, lNearKept, lNearNoCut, lNearWrap, lNearOut }
        ClipPlanePass{ fJnC[x], fJnN[x], kClipBufB, kClipBufA, lXlLoop,   lXlKept,   lXlNoCut,   lXlWrap,   lXlOut   }
        ClipPlanePass{ fJpC[x], fJpN[x], kClipBufA, kClipBufB, lXrLoop,   lXrKept,   lXrNoCut,   lXrWrap,   lXrOut   }
        ClipPlanePass{ fJnC[y], fJnN[y], kClipBufB, kClipBufA, lYbLoop,   lYbKept,   lYbNoCut,   lYbWrap,   lYbOut   }
        ClipPlanePass{ fJpC[y], fJpN[y], kClipBufA, kClipBufB, lYtLoop,   lYtKept,   lYtNoCut,   lYtWrap,   lYtOut   }

        ilw.x iCount, kClipCount(vi00)

        ; Fan the survivors from corner 0 - (0,1,2), (0,2,3), ... - which is
        ; iCount-2 triangles. Five planes can leave eight corners, so this is
        ; a loop rather than the two triangles one plane could be written out
        ; by hand. It writes the enclosing loop's iOutPtr and iVertsLeft,
        ; which is the shape openvcl miscompiles when a window open joins it;
        ; there is none here, and the two loop-variable checks in the Makefile
        ; are what say so on every build.
        iaddi iLeft, iCount, -2
        ibltz iLeft, lClipDone
        ibeq  iLeft, vi00, lClipDone

        ; Corner 0 is in every triangle of the fan, so it is loaded once.
        lq fPos0, kClipBufB + 0(vi00)
        lq fStq0, kClipBufB + 1(vi00)
        lq fCol0, kClipBufB + 2(vi00)
        iaddiu iFan, vi00, kClipBufB + 3

        lFanLoop:

            lq fPos1, 0(iFan)
            lq fStq1, 1(iFan)
            lq fCol1, 2(iFan)
            lq fPos2, 3(iFan)
            lq fStq2, 4(iFan)
            lq fCol2, 5(iFan)

            ClipJudge{ fPos0 }
            ClipJudge{ fPos1 }
            ClipJudge{ fPos2 }

            EmitVertex{ fPos0, fStq0, fCol0, 0, 1, 2, lEmitFan0Packed, lEmitFan0Done }
            EmitVertex{ fPos1, fStq1, fCol1, 3, 4, 5, lEmitFan1Packed, lEmitFan1Done }
            EmitVertex{ fPos2, fStq2, fCol2, 6, 7, 8, lEmitFan2Packed, lEmitFan2Done }
            WholeTriangleReject{ }

            iaddiu iOutPtr,    iOutPtr,     9
            iaddi  iVertsLeft, iVertsLeft, -3

            iaddiu iFan,  iFan,  3
            iaddi  iLeft, iLeft, -1
            ibgtz  iLeft, lFanLoop

        lClipDone:

    lTriangleAdvance:

        iaddiu iInPtr,    iInPtr,     6
        iaddi  iNumVerts, iNumVerts, -3
        ibgtz  iNumVerts, lTriangleLoop

    ; The last window always holds at least one triangle: a window is
    ; closed only when a triangle will not fit, and that triangle goes
    ; straight into the fresh one. So this never kicks an empty packet.
    CloseOutputWindowAndKick{ lKicked3 }

#endvuprog
