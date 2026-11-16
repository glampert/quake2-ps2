;--------------------------------------------------------------------
; textured_triangles.vcl
;
; A VU1 microprogram to draw a batch of gouraud-shaded, textured
; triangles (triangle list), clipped against the near plane and the
; guard band. Every triangle the renderer draws comes through here but
; particles and sky - world surfaces, their lightmap pass, turbulent
; water and MD2 models - and the batch header says which work applies.
; Built by oxide-vcl, whose preprocessor (-G) runs first;
; --vcl-boilerplate injects the VCL boilerplate (.init_*, --enter/--exit
; blocks).
;
; VU data memory layout (qwords; must match vu1.h):
;   0-3  MVP matrix rows (row-vector convention; for keyframes, row 3
;        carries the MD2 lerp's 'move')
;   4    GS scale  (2048, 2048, zScale)
;   5    GS offset (2048 + width/2, 2048 + height/2, zScale)
;   6    clip-judgement scale (guard band for x/y, 1.0 for z)
;   7    colour clamp (255, 255, 255, 255), for keyframes
;   8+   XTOP double buffers (VIF1 BASE/OFFSET)
;   1010 the per-draw block: the dynamic lights, or the lerp constants
;
; Batch layout at XTOP:
;   +0    header: colour mode .x, warp flag .y, vertex format .z,
;         vertex count .w
;   +1    parameters for the warp
;   +2    7 GIF tag qwords (set tag, MIPTBP1/TEX1/TEX0/ALPHA/ZBUF A+D, prim tag)
;   +9    vertices, in one of two formats:
;           DrawVertex  2 qwords: position, then (rgba, s, t, q)
;           keyframes   3 qwords: two keyframes' bytes, then the
;                       model's (index, s, t, q) - see LerpTransform
;   +189  output window A: a copy of the 7 tags, then 3 qwords per
;   +331  output window B:   vertex - ST, RGBAQ, XYZ2 - up to 45 of them
;
; The GS packet goes to one of the two output windows rather than to
; the space after the input vertices, because how many vertices a batch
; produces is not known until the clipper has run. The program fills a
; window, patches its drawing tag with the vertex count it actually
; wrote, sends it with XGKICK and carries on in the other one.
;
; A vertex is transformed to clip space first, in one of two forms picked
; by the vertex format - a DrawVertex read as it lies, or two MD2
; keyframes lerped - and from there on the two are the same vertex:
; judged, clipped, fanned and emitted by the same code. A keyframe
; triangle is also backface culled, and dropped outright rather than
; sent with its ADC bit set.
;
; The colour comes in one of two forms too. Packed, it arrives in the .x
; word of the vertex's second qword and is raw-copied into an A+D qword:
; the native RGBAQ register layout is exactly that u32 with Q in the word
; above, so the bytes never need spreading apart. Computed - from the
; dynamic lights, or from the entity's light and the shade term - it is
; four floats, emitted through ftoi0 as PACKED RGBAQ.
;--------------------------------------------------------------------

#include "vu_common.i"
#include "vu_clip.i"

; Batch offsets, relative to XTOP. The window values must match the
; kOutputWindow* / kMaxVertsPerWindow constants in vu1.h.
#define kBatchHeader 0
#define kBatchParams 1
#define kGifTags     2
#define kVertexData  9

; The two output windows: where they start, how far apart they are, how
; many vertices each holds, and where the drawing tag sits inside one.
#define kWindowA       189
#define kWindowB       331
#define kWindowQwords  142
#define kWindowVerts   45
#define kWindowPrimTag 6

; The most vertices one triangle can write: five planes can each add a corner,
; so a clipped triangle fans out to at most eight corners, six triangles,
; eighteen vertices. A window closes when it has less room than this left.
#define kWindowReserve 18

; The per-draw block, uploaded once per draw chain: the dynamic lights for a
; DrawVertex batch, which only colour mode 1 reads, or the lerp constants for
; a keyframe batch. Must match vu1::kLightBlockAddr and vu1::kLerpBlockAddr.
#define kLightBlock  1010
#define kLerpBlock   1010

; The keyframe colour's ceiling, a frame constant. See vu1::kColorClamp.
#define kColorClamp  7

; The turbulent surface constants, uploaded once by vu1::Init and never
; rewritten. Only a warped batch reads them. Must match vu1::kWarpConstBlockAddr.
#define kWarpConsts  1022

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
;   void ClipTransform(int offPos, int offStq, vec4& pos, vec4& stq, vec4& col)
;   {
;       pos = in[offPos];
;       stq = in[offStq]; // (rgba, s, t, q); raw packed color in .x
;
;       // Colour mode 1: the four dynamic lights, one per lane, summed at
;       // the world position - which the transform below overwrites:
;       col = (0, 0, 0, 128);
;       if (colorMode == Computed)
;           col.xyz = min(sum_i(max(colour_i - distSqr_i * colour_i / radius_i^2, 0)), 255);
;
;       // Object space to clip space (row-vector MVP); the translation row
;       // is scaled by a constant 1, since pos.w carries the lightmap S:
;       pos = pos.x * mvp[0] + pos.y * mvp[1]
;           + pos.z * mvp[2] + 1.0 * mvp[3];
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
    ; Defined on both paths before it is read: the compiler rejects a read of a
    ; register that some path leaves unwritten, and in mode 0 nothing below this
    ; branch writes it, while the clipper's seed still stores it. Black, with the
    ; alpha the lightmap pass needs: 128, since an alpha of 1 scales the
    ; framebuffer by 1/128 - black, yet non-zero, so it still passes the
    ; NOTEQUAL-0 alpha test instead of being skipped. The alpha does not depend on
    ; the lights, so it is set here, ahead of the branch, and not on the lit side.
    ;
    ; Mode 0 pays two instructions for a colour it never emits.
    move.xyz vCol, vf00
    move.w   vCol, fLightClamp

    ibeq iColorMode, vi00, lblNoLight

    ; Four light vectors at once, one lane per light, then squared distances.
    sub fLx, fLightX, vPos[x]
    sub fLy, fLightY, vPos[y]
    sub fLz, fLightZ, vPos[z]
    mul  acc,   fLx, fLx
    madd acc,   fLy, fLy
    madd fDist, fLz, fLz

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
    ; producer writes a 1.0 that this no longer needs. LerpTransform does the
    ; same, for its own reason - see the note on mod::PolyVertex.
    madd vPos, fMVP3, vf00[w]

    ; Guard-band clip judgement against |w|: scaled x/y, exact z.
    mul.xyz   fJudge, vPos, fClipScale
    clipw.xyz fJudge, vPos[w]

#endmacro

; ClipTransform's counterpart for keyframe batches (MD2 models): reads one
; vertex as two keyframes' bytes plus the model's attribute qword, and
; leaves the same three registers as ClipTransform does, in the same form -
; so from here on a model's vertex is a world vertex, clipped, fanned and
; emitted by the same code. Expects the lerp constants in fFrontV, fBackV,
; fShadeLight and fStScale, and the colour ceiling in fColorClamp.
;
; The keyframe qwords hold integer bit patterns until itof0 converts them -
; they must only ever be touched by raw loads and itof0, never an FMAC op
; (integers look like denormals and would flush to zero). The attribute's
; .x is one too, the model's own keyframe index; it rides into vStq
; untouched, and is only ever lerped by the clipper and discarded by the
; emit, both of which read it as the zero the VU makes of a denormal.
;
; Every register written here is also written by ClipTransform, which runs
; instead of this on the other side of a branch, and each is written the same
; way on both sides - vPos and vStq whole, vCol as .xyz and .w apart - so the
; code after the join sees one shape of value whichever side ran.
;
; C-like pseudo-code ('in' is the qword array at iInPtr):
;
;   void LerpTransform(int offCur, int offOld, int offAttr,
;                      vec4& pos, vec4& stq, vec4& col)
;   {
;       vec4 cur = itof(in[offCur]); // (x, y, z, normalindex), 0-255
;       vec4 old = itof(in[offOld]); // (x, y, z, shade * 128)
;       stq      = in[offAttr];      // (index, s, t, q)
;
;       // The skin's power-of-two correction. Linear, so it commutes with
;       // the clipper's interpolation and the perspective divide after it:
;       stq.yz *= stScale.yz;
;
;       // The shade term broadcast across the entity's light, which carries
;       // the matching 1/128; alpha is the draw's, not the vertex's:
;       col.xyz = clamp(shadeLight.xyz * old.w, 0, 255);
;       col.w   = shadeLight.w;
;
;       // The two-keyframe pose lerp. The uniform 'move' term waits in mvp
;       // row 3, and w comes from vf00's hardwired 1, not from the lerped
;       // normal-index junk in .w:
;       vec4 lerped = cur * frontv + old * backv;
;       pos = lerped.x * mvp[0] + lerped.y * mvp[1]
;           + lerped.z * mvp[2] + 1.0 * mvp[3];
;
;       clipFlagQueue.push(pos.xyz * clipScale.xyz, abs(pos.w));
;   }
#macro LerpTransform: vPos, vStq, vCol, offCur, offOld, offAttr

    lq fCurI, offCur(iInPtr)
    lq fOldI, offOld(iInPtr)
    lq vStq,  offAttr(iInPtr)

    ; Byte lanes to floats (raw integers until here - no FMAC before this):
    itof0 fCur, fCurI
    itof0 fOld, fOldI

    ; Masked to .yz: .x is the model's index and .w is q, and neither is
    ; this multiply's business.
    mul.yz vStq, vStq, fStScale

    ; The colour, from the byte the EE packed the quantized shade into - the
    ; *old* keyframe's 4th, which held a normal index nothing here reads.
    mul.xyz  vCol, fShadeLight, fOld[w]
    move.w   vCol, fShadeLight
    max.xyz  vCol, vCol, vf00
    mini.xyz vCol, vCol, fColorClamp

    ; The two-keyframe pose lerp (componentwise, not broadcast):
    mul  acc,   fFrontV, fCur
    madd fLerp, fBackV,  fOld

    ; Position to clip space (row-vector MVP); w = 1 from vf00, never from
    ; fLerp.w, which holds lerped normal-index junk:
    mul  acc,  fMVP0, fLerp[x]
    madd acc,  fMVP1, fLerp[y]
    madd acc,  fMVP2, fLerp[z]
    madd vPos, fMVP3, vf00[w]

    ; Guard-band clip judgement against |w|: scaled x/y, exact z.
    mul.xyz   fJudge, vPos, fClipScale
    clipw.xyz fJudge, vPos[w]

#endmacro

; ref_gl's EmitWaterPolys on one vertex: its raw texel coordinates, pushed
; around by a sine of the *other* axis plus time, then SURF_FLOWING's scroll
; and the texel-to-image divide. Leaves the warped, normalized (s, t) in
; fUV.xy - not yet divided by w, which is the caller's business.
;
; Run on a triangle's corners only, never on a vertex the clipper made - see
; WarpCorners, its one caller.
;
; The constants load here, not at the top of the loop: read per warped vertex
; they are live inside the warp only, so they cost the clipper no registers.
; The sine is computed rather than looked up - a table wants VU memory there
; is none of, and the VU is not the bottleneck this moves work off.
;
; C-like pseudo-code:
;
;   vec2 WarpTexels(vec4 stq) // (rgba, s, t, q), s and t in texels
;   {
;       vec2 raw   = stq.yz;
;       vec2 phase = raw * turnsPerTexel + framePhase; // turns
;       vec2 ripple = 4.0f * sin(2pi * phase);         // the degree 7 polynomial
;       vec2 uv;
;       uv.x = raw.x + ripple.y + batch.scroll;        // crossed: S takes T's sine
;       uv.y = raw.y + ripple.x;
;       return uv * batch.invTexSize;                  // (1/w, 1/h) of the image
;   }
#macro WarpTexels: vStq

    lq fWarpFold, kWarpConsts + 0(vi00)
    lq fWarpPoly, kWarpConsts + 1(vi00)
    lq fBatch,    kBatchParams(iBase)

    ; VU1 has no float immediates; the fold's two need splatting out of the block.
    add fHalf,    vf00, fWarpFold[y]
    sub fNegHalf, vf00, fHalf

    ; (rgba, s, t, unused) -> (s, t, unused, rgba). Both of the vertex's sines ride
    ; lanes .x/.y the whole way: .x from its S, .y from its T.
    mr32 fRaw, vStq

    ; Phase in turns - the per-vertex part plus the frame's, which the EE pre-wrapped
    ; into [0, 1) so this stays precise however long the session has run.
    mul.xy fPhase, fRaw,   fWarpFold[z]
    add.xy fPhase, fPhase, fClipScale[w]

    ; Reduce to u - round(u) in [-0.5, 0.5). ftoi0 truncates toward zero, so the
    ; bias makes it a floor and a round-to-nearest at once.
    add.xy   fBias,  fPhase, fWarpFold[x]
    ftoi0.xy fTrunc, fBias
    itof0.xy fTrunc, fTrunc
    sub.xy   fFrac,  fBias,  fTrunc
    sub.xy   fFrac,  fFrac,  fWarpFold[y]

    ; Fold onto [-0.25, 0.25], the triangle wave sin() is symmetric under.
    sub.xy  fFoldA, fHalf,    fFrac
    mini.xy fT,     fFrac,    fFoldA
    sub.xy  fFoldB, fNegHalf, fFrac
    max.xy  fT,     fT,       fFoldB

    ; Odd polynomial of degree 7, Horner, 4-texel amplitude folded into the terms.
    mul.xy fTSqr, fT,    fT
    mul.xy fPoly, fTSqr, fWarpPoly[w]
    add.xy fPoly, fPoly, fWarpPoly[z]
    mul.xy fPoly, fPoly, fTSqr
    add.xy fPoly, fPoly, fWarpPoly[y]
    mul.xy fPoly, fPoly, fTSqr
    add.xy fPoly, fPoly, fWarpPoly[x]
    mul.xy fWarp, fPoly, fT

    ; Crossed - S takes the sine of T and vice versa, which is what ripples rather
    ; than merely drifts - then SURF_FLOWING's scroll and the texel-to-image divide.
    add.x  fUV, fRaw, fWarp[y]
    add.y  fUV, fRaw, fWarp[x]
    add.x  fUV, fUV,  fBatch[z]
    mul.xy fUV, fUV,  fBatch

#endmacro

; Projects one clip-space vertex and writes its three output qwords: ST,
; RGBAQ and XYZ2, at offST/offAD/offXyz from iOutPtr. The RGBAQ qword is an
; A+D write of the packed colour, or PACKED RGBAQ of a computed one.
;
; A turbulent batch's coordinates arrive here finished - warped, normalized
; and rebased by WarpCorners - so this never warps anything itself.
;
; The packed color is moved with raw copies only (lq/sq.x): FMAC ops
; would flush denormal color bit patterns (e.g. 0x800000FF) to zero.
;
; vPos is read, never written - the clipper needs the clip-space position
; to survive, so the projection lands in a scratch register instead.
;
; 'lbl' prefixes the two labels placed here, and has to be unique to each
; invocation.
;
; C-like pseudo-code:
;
;   void EmitVertex(vec4 pos, vec4 stq, vec4 col,
;                   int offST, int offAD, int offXyz)
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
;       vec4 st = stqScaled.yzwx;         // ST (.w junk)
;       st.z = q;                         // what PACKED RGBAQ latches Q from
;
;       out[offST]      = st;
;       out[offXyz].xyz = proj;           // XYZ (.w ADC bit set by caller)
;       if (colorMode == Computed)
;           out[offAD]  = ftoi0(col);     // PACKED RGBAQ, one byte per word
;       else
;       {
;           out[offAD].x = stq.x;         // native RGBAQ: packed color...
;           out[offAD].y = q;             // ...with Q in the word above
;           out[offAD].z = 0x01;          // A+D destination: RGBAQ register
;       }
;   }
#macro EmitVertex: vPos, vStq, vCol, offST, offAD, offXyz, lbl

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

    ibeq iColorMode, vi00, lbl##Packed

    ; Mode 1: a computed colour, four floats. ftoi0 lands them one byte per
    ; word, which is exactly what the PACKED RGBAQ descriptor reads.
    ftoi0 fRGBA, vCol
    sq    fRGBA, offAD(iOutPtr)
    b lbl##Done

    lbl##Packed:
    ; Mode 0: the packed u32 straight out of the untouched input register, as an
    ; A+D write to RGBAQ with Q alongside it.
    sq.x   vStq, offAD(iOutPtr)
    sq.y   fQ,   offAD(iOutPtr)
    isw.z  iRegRGBAQ, offAD(iOutPtr)

    lbl##Done:

#endmacro

; Finishes a turbulent triangle's texture coordinates, in the clipper's
; buffer at 'buf' where ClipSeedTriangle laid its corners: each corner's raw
; texels go through WarpTexels, the triangle comes down by whole repeats, and
; the result is written over the (s, t) lanes of the corner's (rgba, s, t, q)
; qword, corner 0's wrap entry included. The packed colour and the unread .w
; stay as the seed left them.
;
; Every turbulent triangle comes through here, cut or not, for two reasons.
;
; The ripple has to be taken at the corners, before any cut. The clipper then
; interpolates finished coordinates, as GL's does, and the piece that survives
; draws exactly as the whole triangle would have. A cut vertex warped on its
; own samples the ripple where no neighbour does, while the neighbours draw
; the straight line between their corners' ripples: on a 64-unit leaf's edges
; that put the two 3.2 texels apart at the median and 8 at worst.
;
; And the coordinates have to be small. Water arrives in raw texels measured
; from the texture's world origin, typically tens of repeats out, and a
; console's GS loses precision on large S/T - the walls' stretching and
; swimming, which the loader's ps2_st_rebase cured for them (see
; mod::SetRebaseWallSt). The loader cannot do that here: the ripple's phase is
; a function of the raw coordinate, with a period of 16pi texels that no whole
; number of 64-texel repeats is a multiple of, so shifting the input would
; move the ripple. Shifting the output by whole repeats moves nothing under
; REPEAT wrapping, mip levels included, so it happens here, after the warp:
; by corner 0's whole repeats, which lands that corner in (-1, 1) and the
; other two within a leaf's extent of it. Per triangle rather than per
; polygon, which is just as exact - every triangle draws on its own.
;
; Into memory rather than the corner registers: the seed has stored the
; corners already, and the fan reads them from there. These stores must land
; after the seed's, which write the same qwords through another address
; register: on the cut path the warp flag's branch puts them in a later block,
; and the uncut path has a --barrier between the two.
;
; C-like pseudo-code ('corner' is the seed's (rgba, s, t, q) qword):
;
;   void WarpCorners(qword* buf)
;   {
;       vec2 uv0   = WarpTexels(stq[0]);
;       vec2 shift = trunc(uv0);            // whole repeats; ftoi0 truncates
;       corner(buf, 0).yz = corner(buf, 3).yz = uv0 - shift; // 3: the wrap
;       corner(buf, 1).yz = WarpTexels(stq[1]) - shift;
;       corner(buf, 2).yz = WarpTexels(stq[2]) - shift;
;   }
#macro WarpCorners: buf

    ; (u, v) up one lane, into the qword's s and t, with two masked writes of
    ; lanes that are both defined: a rotate would read fUV's .zw, which nothing
    ; writes, and the compiler rejects that.
    WarpTexels{ fStq0 }
    add.y    fCutST, vf00, fUV[x]
    add.z    fCutST, vf00, fUV[y]
    ftoi0.yz fShiftI, fCutST
    itof0.yz fShift,  fShiftI
    sub.yz   fCutST,  fCutST, fShift
    sq.yz    fCutST,  buf + 1(vi00)
    sq.yz    fCutST,  buf + 10(vi00)

    WarpTexels{ fStq1 }
    add.y  fCutST, vf00,   fUV[x]
    add.z  fCutST, vf00,   fUV[y]
    sub.yz fCutST, fCutST, fShift
    sq.yz  fCutST, buf + 4(vi00)

    WarpTexels{ fStq2 }
    add.y  fCutST, vf00,   fUV[x]
    add.z  fCutST, vf00,   fUV[y]
    sub.yz fCutST, fCutST, fShift
    sq.yz  fCutST, buf + 7(vi00)

#endmacro

; Emits the three transformed corners as one triangle at iOutPtr, with the ADC
; word JudgeTriangleAdc left in iADC, without advancing anything.
;
; The program invokes this before it knows whether the triangle is culled,
; cut or warped - see the note at the invocations - so it must stay free of
; side effects beyond the nine qwords at iOutPtr. 'lbl' prefixes the labels
; the three emits place, and has to be unique to each invocation.
;
; C-like pseudo-code:
;
;   void EmitTriangle(vec4 pos[3], vec4 stq[3], vec4 col[3])
;   {
;       for (i = 0..2)
;           EmitVertex(pos[i], stq[i], col[i], 3i, 3i + 1, 3i + 2);
;       out[2].w = out[5].w = out[8].w = adc;
;   }
#macro EmitTriangle: lbl
    EmitVertex{ fPos0, fStq0, fCol0, 0, 1, 2, lbl##0 }
    EmitVertex{ fPos1, fStq1, fCol1, 3, 4, 5, lbl##1 }
    EmitVertex{ fPos2, fStq2, fCol2, 6, 7, 8, lbl##2 }
    StoreTriangleAdc{ }
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
;       qword* batch     = &vuMem[XTOP];
;       int    numVerts  = batch[kBatchHeader].w;
;       int    colorMode = batch[kBatchHeader].x;
;       int    warped    = batch[kBatchHeader].y;
;       int    format    = batch[kBatchHeader].z;
;       qword* in        = &batch[kVertexData];
;
;       // Output goes to a window, which is opened by copying the 7
;       // GIF tag qwords the EE prepared to its head. 'room' counts the
;       // vertices left beyond the kWindowReserve one triangle can need:
;       qword* win     = &vuMem[XTOP + kWindowA];
;       int    winStep = kWindowQwords; // flips sign on every kick
;       qword* out     = OpenWindow(win);
;       int    room    = kWindowVerts - kWindowReserve;
;
;       do // One triangle per iteration:
;       {
;           // Room for the most a clipped triangle can fan out to - see
;           // the note at the test. Never asked again inside the clipper.
;           if (room < 0)
;           {
;               win[kWindowPrimTag].nloop = kWindowVerts - kWindowReserve - room;
;               XGKICK(win);
;               win += winStep; winStep = -winStep;
;               out = OpenWindow(win); room = kWindowVerts - kWindowReserve;
;           }
;
;           // Three corners to clip space, with position, (colour or
;           // index, s, t, q) and computed colour each:
;           vec4 p[3], stq[3], col[3];
;           bool culled = false;
;           if (format == DrawVertex)
;           {
;               for (i = 0..2) ClipTransform(in[2i], in[2i + 1], ...);
;               in += 6;
;           }
;           else // Keyframes
;           {
;               for (i = 0..2) LerpTransform(in[3i], in[3i + 1], in[3i + 2], ...);
;               in += 9;
;               culled = BackFacing(p[0], p[1], p[2]);
;           }
;           numVerts -= 3;
;           bool cut = clipFlagQueue.last3() != 0; // about 1 triangle in 50
;
;           // Written before it is judged, and kept only if it goes out as
;           // it stands; otherwise 'out' does not move, and the fan, if
;           // anything, writes over it:
;           EmitTriangle(p, stq, col); // ADC from the judgement
;           if (culled)
;               continue;              // no emit at all
;           if (!cut && !warped)
;           {
;               out += 9; room -= 3;
;               continue;
;           }
;
;           // A turbulent triangle's coordinates are finished first -
;           // warped at its corners and rebased - so the clipper, if it
;           // cuts, interpolates finished values:
;           Seed(cut ? bufA : bufB, p, stq, col);
;           if (warped)
;               WarpCorners(cut ? bufA : bufB);
;
;           // Sutherland-Hodgman against near and the four guard-band
;           // sides (vu_clip.i), then fan the survivors from corner 0:
;           int n = cut ? ClipPolygon(bufA) : 3; // 0, or 3 to 8 corners in bufB
;           for (i = 1; i < n - 1; ++i)
;           {
;               EmitVertex(corner 0, i and i + 1, &out[0..8]);
;               out[2].w = out[5].w = out[8].w = Adc(judged again);
;               out += 9; room -= 3;
;           }
;       }
;       while (numVerts != 0);
;
;       // Skipped if the last triangles were all culled or clipped away.
;       if (room != kWindowVerts - kWindowReserve)
;       {
;           win[kWindowPrimTag].nloop = kWindowVerts - kWindowReserve - room;
;           XGKICK(win);
;       }
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

    ; The warp flag and the vertex format, held for the whole batch. Neither
    ; changes inside the loop, so neither has to be read again, and the
    ; colour mode, written nowhere in the loop either, is what oxide-vcl's
    ; unswitch makes one copy of the loop per value of.
    ilw.y  iWarp,   kBatchHeader(iBase)
    ilw.z  iFormat, kBatchHeader(iBase)

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
        ; Held, it would be twelve float registers live across the whole loop,
        ; the clipper's included. Read here it is live only as far as the third
        ; transform below, which is before the clipper needs anything, so the
        ; two sets never overlap. In the loop's copy for packed colours, where
        ; nothing reads the lights, the compiler leaves out every load but the
        ; clamp's .w, the alpha the colour's initialiser copies.
        ;
        ; Unconditional, although only colour mode 1 reads it: guarding it would
        ; leave the registers undefined on the other path, which the compiler
        ; rejects whether or not the read is reachable.
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
        ; transformed, because the triangle is written into the window as soon
        ; as it is (see below). And it has to cover the worst case up front -
        ; kWindowReserve vertices - because asking again from inside the
        ; clipper means more window opens and kicks in there, and that is what
        ; stalls VIF1.
        ;
        ; Eighteen of a 45-vertex window is a lot to hold back for the 2% of
        ; triangles that clip, and the measured fan never exceeded six corners
        ; anyway. The cost is paid in window occupancy, not in vertices: a
        ; window closes once fewer than eighteen are left, so it carries
        ; between 27 and 45 instead of always 45, and a full chunk takes two
        ; to four kicks where it took two. GsWait is the canary.
        ibgez iRoom, lWindowHasRoom
        CloseOutputWindowAndKick{ lKicked1 }
        OpenOutputWindow{ }
        lWindowHasRoom:

        ; Which vertex format this batch carries. Branched on once a triangle
        ; rather than once a vertex, so each side is one block the scheduler
        ; can interleave three corners through.
        ;
        ; The DrawVertex side comes first, the keyframe side second and the
        ; clipper last, and the order matters to oxide-vcl's unswitch: with the
        ; DrawVertex side placed after the clipper, the loop was not copied per
        ; colour mode, and a world triangle took about twice the cycles.
        ibne  iFormat, vi00, lKeyframes

        ClipTransform{ fPos0, fStq0, fCol0, 0, 1, lLight0Done }
        ClipTransform{ fPos1, fStq1, fCol1, 2, 3, lLight1Done }
        ClipTransform{ fPos2, fStq2, fCol2, 4, 5, lLight2Done }

        ; Did any corner leave the volume? Judged here, in the same block as the
        ; clipw it reads: the clip flags land four cycles after a clipw. Put
        ; across a branch from it, an fcand once ran two cycles after the third
        ; corner's clipw and judged every world triangle without it, so a
        ; triangle whose third corner was behind the eye went out unclipped,
        ; across the screen.
        JudgeTriangleAdc{ }
        iaddiu iInPtr,    iInPtr,     6
        iaddi  iNumVerts, iNumVerts, -3

        ; The triangle is written out before it is known to go out as it
        ; stands, which is what lets this block run straight from the input
        ; loads to the output stores: the transform's chains and the emit's
        ; stores share it, and fill each other's empty slots. Most triangles
        ; do go out as they stand. One that is cut or warped goes through the
        ; fan instead, which starts at the same iOutPtr and writes over it;
        ; one whose fan is empty leaves it past the end of what the window's
        ; tag counts, where nothing sends it.
        ;
        ; So the output advances here too, and the slow path below takes it
        ; back. vi01 is still the judgement and iWarp the batch's flag: either
        ; sends the triangle there, with one branch.
        EmitTriangle{ lEmitDv }
        iaddiu iOutPtr, iOutPtr,  9
        iaddi  iRoom,   iRoom,   -3
        ior    iSlow,   vi01,    iWarp
        ibne   iSlow,   vi00,    lSlow
        b lTriangleBottom

    lKeyframes:

        ; The draw's lerp constants - sent up once per draw in the light block's
        ; place, and read once per triangle for the same reason the lights are.
        ; The last carries the backface cull sign in .x and the ST scale in .yz.
        lq fFrontV,     kLerpBlock + 0(vi00)
        lq fBackV,      kLerpBlock + 1(vi00)
        lq fShadeLight, kLerpBlock + 2(vi00)
        lq fStScale,    kLerpBlock + 3(vi00)
        lq fColorClamp, kColorClamp(vi00)

        LerpTransform{ fPos0, fStq0, fCol0, 0, 1, 2 }
        LerpTransform{ fPos1, fStq1, fCol1, 3, 4, 5 }
        LerpTransform{ fPos2, fStq2, fCol2, 6, 7, 8 }

        ; Judged here for the same reason as on the DrawVertex side.
        JudgeTriangleAdc{ }
        iaddiu iInPtr,    iInPtr,     9
        iaddi  iNumVerts, iNumVerts, -3

        ; Backface cull, ahead of the clipper, which is where it has to be: a
        ; cut does not change a triangle's facing, and a back face should cost
        ; nothing past this point. So it is dropped outright - no clip, no
        ; emit, no GIF traffic - where it used to go to the GS with its ADC bit
        ; set, which for a closed model is about half of every one sent.
        ;
        ; The sign of det[x y w] over the three clip-space corners: the
        ; screen-space signed area times w0*w1*w2. It agrees with the screen
        ; test wherever all three corners are in front of the eye and, needing
        ; no divide, is also right where they are not - which the screen test,
        ; ahead of a clipper, never could be. Taken as p0 . (e1 x e2), each
        ; (x, y, w) triple with its w moved into z for the outer product; edges
        ; rather than the corners themselves, since a small distant triangle has
        ; three nearly equal rows whose determinant would cancel its precision
        ; away before the sign came out.
        sub        fEdge1, fPos1,  fPos0
        sub        fEdge2, fPos2,  fPos0
        add.z      fEdge1, vf00,   fEdge1[w]
        add.z      fEdge2, vf00,   fEdge2[w]
        move.xy    fEye,   fPos0
        add.z      fEye,   vf00,   fPos0[w]
        opmula.xyz acc,    fEdge1, fEdge2
        opmsub.xyz fNorm,  fEdge2, fEdge1
        mul.xyz    fArea,  fNorm,  fEye
        add.x      fArea,  fArea,  fArea[y]
        add.x      fArea,  fArea,  fArea[z]

        ; The sign travels as data: clamped to [-1, +1] so the ftoi4 cannot
        ; overflow the 16 bits mtir moves, leaving the sign in bit 15, which is
        ; what ibltz reads.
        ;
        ; The draw's cull sign goes on first, so the test is always just "is it
        ; negative": a positive sign culls negative determinants, a negative one
        ; positive ones, and zero flattens them all to zero, which is not
        ; negative, so nothing is culled. Its magnitude matters as much as its
        ; sign - see rs::kCullSignScale.
        mul.x      fArea,  fArea,  fStScale[x]
        mini.x     fArea,  fArea,  vf00[w]
        max.x      fArea,  fArea,  fMinusOne[x]
        ftoi4.x    fArea,  fArea
        mtir       iCull,  fArea[x]

        ; Written out ahead of the cull as well as of the judgement - see the
        ; DrawVertex side. The cull's chain is long, and the emit fills it. A
        ; back face leaves the output where it was.
        EmitTriangle{ lEmitKf }
        ibltz  iCull,   lTriangleBottom

        iaddiu iOutPtr, iOutPtr,  9
        iaddi  iRoom,   iRoom,   -3
        ior    iSlow,   vi01,    iWarp
        ibne   iSlow,   vi00,    lSlow
        b lTriangleBottom

        ; --- cut or turbulent: back to the triangle's own output, then the fan ---
    lSlow:

        isubiu iOutPtr, iOutPtr, 9
        iaddi  iRoom,   iRoom,   3

        ; Did any corner leave the volume, as judged above? About one triangle
        ; in fifty does. Otherwise it is a turbulent batch's triangle, which goes
        ; out through the fan even when it needs no cut, because its coordinates
        ; are finished in the clipper's buffer: see WarpCorners. Water is a small
        ; share of any frame.
        ibne  vi01, vi00, lClipTriangle

        ; --- turbulent, every corner inside: straight into B, where the fan reads ---
        ClipSeedTriangle{ iWalk, kClipBufB }

        ; The seed's stores have to land before WarpCorners writes over their s
        ; and t, and nothing here is a block boundary to keep them apart: the
        ; seed writes through iWalk, WarpCorners through vi00, so the two are
        ; not the same address register. On the cut path below, the warp flag's
        ; branch does it.
        --barrier

        WarpCorners{ kClipBufB }
        b lFan

        ; --- a corner is outside: cut against near, fan what survives ---
    lClipTriangle:

        ClipSeedTriangle{ iWalk, kClipBufA }

        ; A turbulent batch's corners are finished before the cut. See WarpCorners.
        ibeq  iWarp, vi00, lSeedWarped
        WarpCorners{ kClipBufA }
        lSeedWarped:

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
        ClipPlanePass{ fJnC[z], fJnN[z], kClipBufA, kClipBufB, lNear }
        ClipPlanePass{ fJnC[x], fJnN[x], kClipBufB, kClipBufA, lXl   }
        ClipPlanePass{ fJpC[x], fJpN[x], kClipBufA, kClipBufB, lXr   }
        ClipPlanePass{ fJnC[y], fJnN[y], kClipBufB, kClipBufA, lYb   }
        ClipPlanePass{ fJpC[y], fJpN[y], kClipBufA, kClipBufB, lYt   }

    lFan:

        ilw.x iCount, kClipCount(vi00)

        ; Fan the survivors from corner 0 - (0,1,2), (0,2,3), ... - which is
        ; iCount-2 triangles. Five planes can leave eight corners, so this is
        ; a loop rather than the two triangles one plane could be written out
        ; by hand. It writes the enclosing loop's iOutPtr and iRoom, and no
        ; window opens inside it: the room test at the top of the triangle
        ; loop reserved enough for all of it.
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

            EmitVertex{ fPos0, fStq0, fCol0, 0, 1, 2, lEmitFan0 }
            EmitVertex{ fPos1, fStq1, fCol1, 3, 4, 5, lEmitFan1 }
            EmitVertex{ fPos2, fStq2, fCol2, 6, 7, 8, lEmitFan2 }
            WholeTriangleReject{ }

            iaddiu iOutPtr, iOutPtr,  9
            iaddi  iRoom,   iRoom,   -3

            iaddiu iFan,  iFan,  3
            iaddi  iLeft, iLeft, -1
            ibgtz  iLeft, lFanLoop

        lClipDone:

    ; Every path through the loop ends here, with the input and the triangle
    ; count already advanced: each format's block moves them by its own
    ; stride, as soon as its loads are issued.
    lTriangleBottom:
        ibgtz  iNumVerts, lTriangleLoop

    ; The last window can be empty - its triangles all culled or clipped
    ; away - and the macro sends nothing then. See its note.
    CloseOutputWindowAndKick{ lKicked3 }

#endvuprog
