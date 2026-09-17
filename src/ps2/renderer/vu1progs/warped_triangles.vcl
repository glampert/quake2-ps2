;--------------------------------------------------------------------
; warped_triangles.vcl
;
; A VU1 microprogram to draw a batch of turbulent (warped) surfaces:
; water, lava and slime. Identical to textured_triangles.vcl apart
; from the texture coordinates, which arrive in raw texel units and
; are animated here rather than on the EE - ref_gl's EmitWaterPolys,
; where every vertex's S is pushed around by a sine of its T plus the
; frame time, and its T by a sine of its S.
;
; VU data memory layout (qwords; must match vu1.h):
;   0-3    MVP matrix rows (row-vector convention)
;   4      GS scale  (2048, 2048, zScale)
;   5      GS offset (2048 + width/2, 2048 + height/2, zScale)
;   6      clip-judgement scale in .xyz, animation phase in .w
;   8+     XTOP double buffers (VIF1 BASE/OFFSET)
;   1012-3 the warp constants below, uploaded once by vu1::Init
;
; Batch layout at XTOP - the same as the textured program's, so the
; chunk emitter, the chain budget and kMaxVertsPerBatch are shared:
;   +0   header: 1/texWidth, 1/texHeight, scroll texels, count in .w
;   +1   7 GIF tag qwords (set tag, TEST/TEX1/TEX0/ALPHA/ZBUF, prim tag)
;   +8   vertices, 2 qwords each: position, then (rgba, s, t, unused)
;
; The sine is computed rather than looked up: a table would have to
; live in VU data memory, which has no room for one, and the VU is not
; the bottleneck this moves work off. Both of a vertex's sines ride lanes
; .x and .y of one vector the whole way through, so the pair together
; costs ~25 cycles a vertex - openvcl --cost puts this program at 180
; cycles a triangle against the textured one's 103.
;
; Unlike the other programs this one never reads the vertex's .w or .q:
; the position's translation row is scaled by a hardwired 1.0 and the
; Q that rides with ST is synthesised from the divide. That is what lets
; the EE hand over mod::PolyVertex untouched, lightmap coordinates still
; parked in both those lanes (see view.cpp).
;--------------------------------------------------------------------

; Batch offsets, relative to XTOP:
#define kBatchHeader 0
#define kGifTags     1
#define kVertexData  8

; Absolute VU data address of the warp constant block (see vu1.h):
#define kWarpConsts  1012

; How the texture coordinates are warped. This is written out inline in
; DoVertex below rather than being a #macro of its own: vclpp expands
; macros only where they are written in the program body, not inside
; another macro's expansion.
;
; The sine argument is carried in *turns* (radians / 2pi) rather than
; radians, because the range reduction wants a fraction anyway and the
; conversion is free - it rides the texel scale the EE would otherwise
; have applied. Both of the vertex's two sines are computed at once in
; lanes .x/.y: .x from the vertex's S, .y from its T.
;
; C-like pseudo-code:
;
;   vec2 WarpUV(vec4 stq)          // stq = (rgba, s, t, unused)
;   {
;       vec2 raw = vec2(stq.y, stq.z);          // raw texel S, T
;
;       // Phase in turns: the per-vertex part plus the frame's own,
;       // which the EE pre-wrapped into [0, 1) so this stays precise
;       // however long the session has been running.
;       vec2 u = raw * fold.z + clipScale.w;
;
;       // Reduce to u - round(u) in [-0.5, 0.5). ftoi0 truncates toward
;       // zero, so the +1024.5 bias makes it a floor and a round-to-
;       // nearest in one step; it holds while |u| < 1024 turns, which
;       // texel coordinates would have to pass 51k to break.
;       vec2 b = u + fold.x;
;       vec2 f = (b - itof0(ftoi0(b))) - fold.y;
;
;       // Fold onto [-0.25, 0.25], where an odd polynomial of degree 7
;       // is good to 0.002 texels. Branch-free, and exactly the
;       // triangle wave sin() is symmetric under:
;       //   t = max(min(f, 0.5 - f), -0.5 - f)
;       vec2 t = max(min(f, half - f), negHalf - f);
;
;       // sin(2pi*t) * 4 texels, the amplitude folded into a1..a7:
;       vec2 y = t * t;
;       vec2 w = t * (a1 + y * (a3 + y * (a5 + y * a7)));
;
;       // Each axis is displaced by the *other* axis's sine, which is
;       // what makes the surface ripple rather than merely drift, then
;       // SURF_FLOWING's scroll and the texel-to-image divide:
;       return (vec2(raw.x + w.y + header.z, raw.y + w.x)) * header.xy;
;   }

; Transforms one warped vertex: 2 input qwords at offPos/offStq from
; iInPtr become the ST, RGBAQ (via A+D) and XYZ2 output qwords at
; offST/offAD/offXyz from iOutPtr. Leaves this vertex's clipw flags as
; the newest entry in the clip flag register; the caller judges whole
; triangles with fcand after 3 calls and writes the XYZ2 .w ADC bit.
;
; The packed color is moved with raw copies only (lq/sq.x): FMAC ops
; would flush denormal color bit patterns (e.g. 0x800000FF) to zero.
; Unlike the textured program nothing here even scales the STQ qword the
; color arrives in - WarpUV builds its own - so the packed bits never go
; near an FMAC at all.
;
; C-like pseudo-code ('in'/'out' are the qword arrays at iInPtr/iOutPtr):
;
;   void DoVertex(int offPos, int offStq, int offST, int offAD, int offXyz)
;   {
;       vec4 pos = in[offPos];
;       vec4 stq = in[offStq];           // (rgba, s, t, unused)
;
;       vec2 uv = WarpUV(stq);           // animated, normalized
;
;       // Object space to clip space (row-vector MVP):
;       pos = pos.x * mvp[0] + pos.y * mvp[1]
;           + pos.z * mvp[2] + 1.0f * mvp[3];
;
;       // Guard-band clip judgement: compare the scaled position
;       // against |w| and push the 6 outside flags onto the queue:
;       clipFlagQueue.push(pos.xyz * clipScale.xyz, abs(pos.w));
;
;       float q = 1.0f / pos.w;
;       pos.xyz *= q;                    // now NDC
;
;       // NDC to GS window coordinates, in 12.4 fixed point:
;       pos.xyz = ftoi4(gsOffset.xyz + pos.xyz * gsScale.xyz);
;
;       out[offST].xy   = uv * q;        // perspective-correct ST...
;       out[offST].z    = q;             // ...with Q beside it
;       out[offAD].x    = stq.x;         // native RGBAQ: packed color...
;       out[offAD].y    = q;             // ...with Q in the word above
;       out[offAD].z    = 0x01;          // A+D destination: RGBAQ register
;       out[offXyz].xyz = pos.xyz;       // XYZ (.w ADC bit set by caller)
;   }
#macro DoVertex: offPos, offStq, offST, offAD, offXyz

    lq fPos, offPos(iInPtr)
    lq fStq, offStq(iInPtr)

    ; (rgba, s, t, unused) -> (s, t, unused, rgba)
    mr32 fRaw, fStq

    ; Sine phases in turns, both at once:
    mul.xy fPhase, fRaw,   fWarpFold[z]
    add.xy fPhase, fPhase, fClipScale[w]

    ; Reduce to [-0.5, 0.5):
    add.xy   fBias, fPhase, fWarpFold[x]
    ftoi0.xy fTrunc, fBias
    itof0.xy fTrunc, fTrunc
    sub.xy   fFrac,  fBias, fTrunc
    sub.xy   fFrac,  fFrac, fWarpFold[y]

    ; Fold onto [-0.25, 0.25]:
    sub.xy  fFoldA, fHalf,    fFrac
    mini.xy fT,     fFrac,    fFoldA
    sub.xy  fFoldB, fNegHalf, fFrac
    max.xy  fT,     fT,       fFoldB

    ; Odd polynomial, Horner, 4-texel amplitude folded into the terms:
    mul.xy fTSqr, fT,    fT
    mul.xy fPoly, fTSqr, fWarpPoly[w]
    add.xy fPoly, fPoly, fWarpPoly[z]
    mul.xy fPoly, fPoly, fTSqr
    add.xy fPoly, fPoly, fWarpPoly[y]
    mul.xy fPoly, fPoly, fTSqr
    add.xy fPoly, fPoly, fWarpPoly[x]
    mul.xy fWarp, fPoly, fT

    ; Crossed: S takes the sine of T and vice versa. Then SURF_FLOWING's
    ; scroll (zero for a still surface) and the divide by the texture
    ; size, both from this batch's header.
    add.x  fUV, fRaw, fWarp[y]
    add.y  fUV, fRaw, fWarp[x]
    add.x  fUV, fUV,  fBatch[z]
    mul.xy fUV, fUV,  fBatch

    ; Position to clip space (row-vector MVP):
    mul  acc,  fMVP0, fPos[x]
    madd acc,  fMVP1, fPos[y]
    madd acc,  fMVP2, fPos[z]
    ; The MVP's translation row is scaled by a hardwired 1.0, not by the vertex's
    ; own .w: PolyVertex parks its lightmap S there, and this program is fed
    ; PolyVertex directly. Same reason textured_triangles.vcl does it.
    madd fPos, fMVP3, vf00[w]

    ; Guard-band clip judgement against |w|: scaled x/y, exact z.
    mul.xyz   fJudge, fPos, fClipScale
    clipw.xyz fJudge, fPos[w]

    ; Perspective divide, with the same 1/w multiplied onto the warped
    ; texture coords - the GS wants (s/w, t/w, 1/w) for perspective-correct
    ; interpolation - and captured into .y for the A+D RGBAQ qword.
    div     q,    vf00[w], fPos[w]
    mul.xyz fPos, fPos,    q
    mulq.xy fST,  fUV,     q
    addq.y  fQ,   vf00,    q

    ; NDC to GS window coordinates, in 12.4 fixed point:
    mula.xyz  acc,  fGSOffset, vf00[w]
    madd.xyz  fPos, fPos, fGSScale
    ftoi4.xyz fPos, fPos

    ; Q rides in word 2 of the ST qword. vf00.z is 0, so this lands the
    ; reciprocal exactly. .w goes along for the ride purely so the store
    ; below has every lane defined - the GS never reads it.
    addq.zw fST, vf00, q

    sq     fST,  offST(iOutPtr)
    sq.x   fStq, offAD(iOutPtr)
    sq.y   fQ,   offAD(iOutPtr)
    isw.z  iRegRGBAQ, offAD(iOutPtr)
    sq.xyz fPos, offXyz(iOutPtr)

#endmacro

; Identical to VU1Prog_TexturedTriangles apart from loading the warp
; constants and the per-vertex WarpUV inside DoVertex.
;
; C-like pseudo-code of the program below ('vuMem' is VU1 data memory
; seen as an array of qwords):
;
;   void VU1Prog_WarpedTriangles()
;   {
;       mat4 mvp       = vuMem[0..3];
;       vec4 gsScale   = vuMem[4];
;       vec4 gsOffset  = vuMem[5];
;       vec4 clipScale = vuMem[6];      // .w = animation phase, in turns
;       vec4 fold      = vuMem[1012];   // (phaseBias, 0.5, turnsPerTexel, -)
;       vec4 poly      = vuMem[1013];   // (a1, a3, a5, a7)
;
;       qword* batch    = &vuMem[XTOP];
;       vec4   header   = batch[kBatchHeader];
;       int    numVerts = header.w;
;       qword* in       = &batch[kVertexData];
;
;       qword* kick = in + (numVerts * 2);
;       qword* out  = kick;
;
;       memcpy(out, &batch[kGifTags], 7 * sizeof(qword));
;       out += 7;
;
;       do // One triangle per iteration:
;       {
;           DoVertex(0, 1,  0, 1, 2);
;           DoVertex(2, 3,  3, 4, 5);
;           DoVertex(4, 5,  6, 7, 8);
;
;           int adc = 0x7FFF + (clipFlagQueue.last3() != 0 ? 1 : 0);
;           out[2].w = adc;
;           out[5].w = adc;
;           out[8].w = adc;
;
;           in  += 6;
;           out += 9;
;           numVerts -= 3;
;       }
;       while (numVerts != 0);
;
;       XGKICK(kick);
;   }
#vuprog VU1Prog_WarpedTriangles

    ; VCL requires zeroed clip flags before any CLIP instruction:
    fcset 0x000000

    ; Frame constants from the fixed low addresses:
    lq fMVP0,      0(vi00)
    lq fMVP1,      1(vi00)
    lq fMVP2,      2(vi00)
    lq fMVP3,      3(vi00)
    lq fGSScale,   4(vi00)
    lq fGSOffset,  5(vi00)
    lq fClipScale, 6(vi00)

    ; The warp constants, uploaded once at init and never rewritten:
    lq fWarpFold, kWarpConsts + 0(vi00)
    lq fWarpPoly, kWarpConsts + 1(vi00)

    ; VU1 has no float immediates, so the two constants the fold needs in
    ; every lane are splatted out of the block once, here.
    add fHalf,    vf00, fWarpFold[y]
    sub fNegHalf, vf00, fHalf

    ; A+D destination address the per-vertex color qwords carry in .z
    ; (0x01 = the RGBAQ register):
    iaddiu iRegRGBAQ, vi00, 1

    ; Current double buffer and this batch's pointers. The header qword
    ; carries the warp parameters as well as the count, so it is loaded
    ; both ways - .w holds an integer the FMAC must never touch, which is
    ; why every use of fBatch below is masked to .xyz.
    xtop   iBase
    lq     fBatch,    kBatchHeader(iBase)
    ilw.w  iNumVerts, kBatchHeader(iBase)
    iaddiu iInPtr, iBase, kVertexData

    ; Output (the GS packet) starts right after the input vertices:
    iadd   iKick, iInPtr, iNumVerts
    iadd   iKick, iKick,  iNumVerts

    ; The GIF tags were prepared by the EE; copy them to the packet head:
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

    ; One triangle per iteration:
    lTriangleLoop:

        DoVertex{ 0, 1, 0, 1, 2 }
        DoVertex{ 2, 3, 3, 4, 5 }
        DoVertex{ 4, 5, 6, 7, 8 }

        ; Judge the whole triangle from the last 3 clipw results: if any
        ; vertex left the guard band, 0x7FFF + flags reaches bit 15 (the
        ; ADC bit) and the GS skips this triangle's drawing kick. Written
        ; to every XYZ2 .w so the kicking vertex always carries it.
        fcand  vi01, 0x3FFFF
        iaddiu iADC, vi01, 0x7FFF
        isw.w  iADC, 2(iOutPtr)
        isw.w  iADC, 5(iOutPtr)
        isw.w  iADC, 8(iOutPtr)

        iaddiu iInPtr,    iInPtr,     6
        iaddiu iOutPtr,   iOutPtr,    9
        iaddi  iNumVerts, iNumVerts, -3
        ibne   iNumVerts, vi00, lTriangleLoop

    --barrier

    xgkick iKick

#endvuprog
