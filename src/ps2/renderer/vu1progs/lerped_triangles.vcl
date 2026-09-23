;--------------------------------------------------------------------
; lerped_triangles.vcl
;
; A VU1 microprogram to draw a batch of gouraud-shaded, textured
; triangles whose positions are the interpolation of two MD2
; keyframes: pos = cur * frontv + old * backv, over byte-quantized
; frame vertices (dtrivertx_t). The uniform 'move' translation of
; the MD2 lerp is folded into the MVP's row 3 by the EE, so only
; the two componentwise scale vectors ride with the batch. From
; the clip-space transform onward this is textured_triangles.vcl
; unchanged. Preprocessed with vclpp; -j injects the boilerplate.
;
; Vertex colour is computed here rather than handed over packed:
; each vertex carries a shade term and the batch carries the
; entity's light, so the colour is one broadcast multiply, a clamp
; and an ftoi0. That replaces a 162-entry lookup table the EE used
; to rebuild for every entity of every frame.
;
; The shade term rides in the *old keyframe's* 4th byte, quantized
; to shade * 128, rather than in a lane of the attribute qword. The
; EE was storing that word anyway and the byte held a
; lightnormalindex nothing here reads, so putting it there leaves
; the attribute qword with nothing the model does not already have
; baked - and the EE stopped gathering that stream at all. It is
; now the model's own vertexes, referenced where they lie.
;
; VU data memory layout (qwords; must match vu1.h):
;   0-3  MVP matrix rows (row-vector convention; row 3 carries 'move')
;   4    GS scale  (2048, 2048, zScale)
;   5    GS offset (2048 + width/2, 2048 + height/2, zScale)
;   6    clip-judgement scale (guard band for x/y, 1.0 for z)
;   7    colour clamp (255, 255, 255, 255)
;   8+   XTOP double buffers (VIF1 BASE/OFFSET)
;   1010 the draw's lerp constants, in the dynamic light block's place
;        (vu1::LerpConstants), uploaded once per draw:
;          +0  frontv: current frame scale * (1 - backlerp), w = 0
;          +1  backv:  old frame scale * backlerp, w = 0
;          +2  shadeLight: the entity's light, vertex alpha in .w. Not
;              in GS units - it carries the 1/128 matching the
;              quantized shade byte, so light * shade lands back in
;              the 0-255 the clamp expects
;          +3  backface cull sign in .x (+1 culls negative screen
;              areas, -1 culls positive, 0 culls nothing), texture
;              coordinate scale in .y/.z (the skin's size over its
;              power-of-two TEX0 extent - applied here so the EE does
;              not multiply it onto every vertex)
;
; Batch layout at XTOP - a world batch's head, and fixed offsets sized
; for the 72-vertex maximum chunk, so short chunks leave gaps rather
; than move the regions (the EE and this program share compile-time
; addresses):
;   +0    header: vertex count in .w (.x/.y/.z are the colour mode,
;         warp flag and vertex format, which only matter to a program
;         that has more than one of each)
;   +1    parameters: unused
;   +2    7 GIF tag qwords (set tag, TEST/TEX1/TEX0/ALPHA/ZBUF A+D,
;         prim tag)
;   +9    vertices, 3 qwords each, interleaved by the VIF as the two
;         streams unpack:
;           +0  the current frame's dtrivertx_t, unpacked from V4_8
;               bytes to four *unsigned integers*. Its 4th lane is
;               its lightnormalindex, which the EE indexes the shade
;               table with and this program never reads
;           +1  the old frame's, the same way. Its 4th lane is the
;               quantized shade term the EE wrote over it, and is
;               where the colour comes from
;           +2  attributes: (unused, s, t, q), handed to the DMA
;               straight out of the model hunk - .x is the model's
;               own keyframe index, never a float
;   +225  output window A: a copy of the 7 tags, then 3 qwords per
;   +340  output window B:   vertex - ST, RGBAQ, XYZ2 - up to 36 of
;         them. Filled, tagged with the count actually written and
;         kicked in turn, so a full 72-vertex chunk is two kicks
;
; The position qwords hold integer bit patterns until itof0
; converts them - they must only ever be touched by raw loads and
; itof0, never an FMAC op (integers look like denormals and would
; flush to zero). The attribute qword's .x is one of those too, now
; that it comes from the model: the single FMAC that touches it
; discards the result, and says so where it happens.
;--------------------------------------------------------------------

#include "vu_common.i"

; Batch offsets, relative to XTOP:
#define kBatchHeader 0
#define kGifTags     2
#define kVertexData  9

; The draw's lerp constants, above the double buffers. Must match
; vu1::kLerpBlockAddr and the field order of vu1::LerpConstants.
#define kLerpBlock   1010

; The two output windows: where they start, how far apart they are, how
; many vertices each holds, and where the drawing tag sits inside one.
; Must match the kLerp*Window* constants in vu1.h.
#define kWindowA       225
#define kWindowB       340
#define kWindowQwords  115
#define kWindowVerts   36
#define kWindowPrimTag 6

; Transforms one vertex: the three qwords at offCur/offOld/offStq from
; iInPtr (integer byte lanes of the two keyframes, then the attribute
; qword) become the ST, RGBAQ (PACKED) and
; XYZ2 output qwords at offST/offRGBA/offXyz from iOutPtr. Leaves this
; vertex's clipw flags as the newest entry in the clip flag register;
; the caller judges whole triangles with fcand after 3 calls and
; writes the XYZ2 .w ADC bit. 'vClip' additionally keeps the vertex's
; clip-space position, which the caller's backface test takes the
; determinant of across the triangle - one register per vertex, at no
; extra instruction (the transform's last madd lands there anyway).
;
; The position integers must not reach an FMAC before itof0 - they
; look like denormals and would flush to zero.
;
; C-like pseudo-code ('in'/'out' are the qword arrays at iInPtr/iOutPtr):
;
;   void DoVertex(int offCur, int offOld, int offStq,
;                 int offST, int offRGBA, int offXyz)
;   {
;       ivec4 curI = in[offCur];  // (x, y, z, normalindex) ints, 0-255
;       ivec4 oldI = in[offOld];
;       vec4  stq  = in[offStq];  // (unused, s, t, q)
;
;       vec4 cur = itof(curI);
;       vec4 old = itof(oldI);
;
;       // The two-keyframe pose lerp (componentwise multiplies; the
;       // uniform 'move' term waits in mvp row 3). The .w lanes carry
;       // normal-index junk - finite, and overwritten just below:
;       vec4 pos = cur * frontv + old * backv;
;
;       // Model space to clip space (row-vector MVP); w comes from
;       // vf00's hardwired 1, not from the junk in pos.w. Kept, for
;       // the caller's backface test:
;       clip = pos.x * mvp[0] + pos.y * mvp[1]
;            + pos.z * mvp[2] + 1.0 * mvp[3];
;
;       // Guard-band clip judgement: compare the scaled position
;       // against |w| and push the 6 outside flags (+x,-x,+y,-y,+z,-z)
;       // onto the clip flag queue for the caller to inspect:
;       vec3 judge = clip.xyz * clipScale.xyz;
;       clipFlagQueue.push(judge, abs(clip.w));
;
;       // Perspective divide; the STQ words share the 1/w so the GS
;       // gets (s/w, t/w, 1/w) for perspective-correct interpolation.
;       // The keyframe index in .x gets scaled too - it reads as
;       // zero (the VU has no denormals) and the rotate below moves
;       // it into the ST qword's ignored .w:
;       float q    = 1.0f / clip.w;
;       vec3  proj = clip.xyz * q;       // now NDC
;       vec4  stqScaled = stq * q;       // (junk, s/w, t/w, 1/w)
;
;       // The vertex colour, from the quantized shade term in the
;       // old keyframe's .w broadcast across the entity's light.
;       // This is the whole of what the EE's per-entity colour LUT
;       // used to compute, and the clamp it needed 486 compares for
;       // is two instructions here:
;       vec4 colour  = shadeLight * old.w;   // .w = alpha * shade
;       colour.xyz   = clamp(colour.xyz, 0, 255);
;       colour.w     = shadeLight.w;         // alpha, untouched
;
;       // NDC to GS window coordinates, in 12.4 fixed point:
;       proj = ftoi4(gsOffset.xyz + proj * gsScale.xyz);
;
;       // ST scaled by the skin's power-of-two correction:
;       stqScaled.yz   *= stScale.yz;
;       out[offST]      = stqScaled.yzwx; // ST (.z carries Q; .w junk)
;       out[offRGBA]    = ftoi0(colour);  // PACKED RGBAQ: one byte per word,
;                                         // Q from the ST write just above
;       out[offXyz].xyz = proj;           // XYZ (.w ADC bit set by caller)
;   }
#macro DoVertex: offCur, offOld, offStq, offST, offRGBA, offXyz, vClip

    lq fCurI, offCur(iInPtr)
    lq fOldI, offOld(iInPtr)
    lq fStq,  offStq(iInPtr)

    ; Byte lanes to floats (raw integers until here - no FMAC before this):
    itof0 fCur, fCurI
    itof0 fOld, fOldI

    ; The two-keyframe pose lerp (componentwise, not broadcast):
    mul  acc,  fFrontV, fCur
    madd fPos, fBackV,  fOld

    ; Position to clip space (row-vector MVP); w = 1 from vf00, never
    ; from fPos.w, which holds lerped normal-index junk:
    mul  acc,   fMVP0, fPos[x]
    madd acc,   fMVP1, fPos[y]
    madd acc,   fMVP2, fPos[z]
    madd vClip, fMVP3, vf00[w]

    ; Guard-band clip judgement against |w|: scaled x/y, exact z.
    mul.xyz   fJudge, vClip, fClipScale
    clipw.xyz fJudge, vClip[w]

    ; Perspective divide, with the same 1/w multiplied onto the texture
    ; coords - the GS wants (s/w, t/w, 1/w) for perspective-correct
    ; interpolation. The rotate below lands that 1/w in the ST qword's
    ; third word, which is where PACKED RGBAQ latches its Q from.
    ;
    ; Unmasked, and .x is the one place in this program an integer bit pattern
    ; deliberately reaches an FMAC. The attribute qword now arrives verbatim from
    ; the model and its .x is the model's own keyframe index; the VU has no
    ; denormals, so it reads as zero and the product is zero. That is fine here
    ; and nowhere else: the lane it rotates into is the ST qword's unread fourth
    ; word. Masking to .yzw instead would leave .x uninitialised for the mr32
    ; below, which openvcl rejects outright.
    div        q,          vf00[w], vClip[w]
    mul.xyz    fProj,      vClip,   q
    mulq       fStqScaled, fStq,    q

    ; NDC to GS window coordinates, in 12.4 fixed point:
    mula.xyz  acc,   fGSOffset, vf00[w]
    madd.xyz  fProj, fProj, fGSScale
    ftoi4.xyz fProj, fProj

    ; The skin's power-of-two correction, which the EE used to multiply onto
    ; every vertex before handing them over. Masked to .yz: the cull sign
    ; shares that qword, in .x.
    mul.yz fStqScaled, fStqScaled, fStScale

    ; The vertex colour: the entity's light scaled by this vertex's shade
    ; term, broadcast from the *old* keyframe's .w - the byte the EE packed the
    ; quantized shade into (shade * 128), already widened by the VIF and
    ; converted by the itof0 above. The batch's light carries the matching 1/128,
    ; so the product is the colour the float shade used to give.
    ;
    ; Reading it here rather than from the attribute qword is what lets that
    ; qword come straight out of the model hunk with no EE gather at all.
    ; Alpha is moved in rather than multiplied; it is the batch's, not the
    ; vertex's. The clamp is the one the EE spent 486 compares an entity on.
    mulw.xyz fColor, fShadeLight, fOld
    move.w   fColor, fShadeLight
    max.xyz  fColor, fColor, vf00
    mini.xyz fColor, fColor, fColorClamp
    ftoi0    fRGBA,  fColor

    ; Rotate (junk, sq, tq, q) into ST order (sq, tq, q, junk):
    mr32 fST, fStqScaled

    ; ST first: PACKED RGBAQ takes its Q from the internal register the ST
    ; write latches, which is why the two cannot be reordered.
    sq     fST,   offST(iOutPtr)
    sq     fRGBA, offRGBA(iOutPtr)
    sq.xyz fProj, offXyz(iOutPtr)

#endmacro

; C-like pseudo-code of the program below ('vuMem' is VU1 data memory
; seen as an array of qwords):
;
;   void VU1Prog_LerpedTriangles()
;   {
;       // Frame constants at the fixed low addresses:
;       mat4 mvp        = vuMem[0..3];
;       vec4 gsScale    = vuMem[4];
;       vec4 gsOffset   = vuMem[5];
;       vec4 clipScale  = vuMem[6];
;       vec4 colorClamp = vuMem[7];
;
;       // The draw's constants, in the per-draw block:
;       vec4  frontv     = vuMem[kLerpBlock + 0];
;       vec4  backv      = vuMem[kLerpBlock + 1];
;       vec4  shadeLight = vuMem[kLerpBlock + 2];
;       float cullSign   = vuMem[kLerpBlock + 3].x;
;
;       // This batch, in the current double buffer:
;       qword* batch    = &vuMem[XTOP];
;       int    numVerts = batch[kBatchHeader].w;
;       qword* in       = &batch[kVertexData]; // 3 qwords per vertex
;
;       // Output goes to a window, opened by copying the 7 GIF tag
;       // qwords the EE prepared to its head:
;       qword* win       = &batch[kWindowA];
;       int    winStep   = kWindowQwords; // flips sign on every kick
;       qword* out       = OpenWindow(win);
;       int    vertsLeft = kWindowVerts;
;       qword* out  = kick;
;
;       // Packet head: the 7 GIF tag qwords prepared by the EE:
;       memcpy(out, &batch[kGifTags], 7 * sizeof(qword));
;       out += 7;
;
;       do // One triangle per iteration:
;       {
;           if (vertsLeft == 0) // No room: send this window, start the other.
;           {
;               win[kWindowPrimTag].nloop = kWindowVerts - vertsLeft;
;               XGKICK(win);
;               win += winStep; winStep = -winStep;
;               out = OpenWindow(win); vertsLeft = kWindowVerts;
;           }
;
;           vec4 clip0, clip1, clip2; // clip-space positions
;           DoVertex(0, 1, 2,  0, 1, 2,  clip0); // in[0..2] -> out[0..2]
;           DoVertex(3, 4, 5,  3, 4, 5,  clip1); // in[3..5] -> out[3..5]
;           DoVertex(6, 7, 8,  6, 7, 8,  clip2); // in[6..8] -> out[6..8]
;
;           // Whole-triangle guard band reject: if any of the 18 clip
;           // flags of the 3 vertices above is set, adc becomes 0x8000,
;           // i.e. bit 15 - the ADC bit - and the GS skips this
;           // triangle's drawing kick.
;           int adc = 0x7FFF + (clipFlagQueue.last3() != 0 ? 1 : 0);
;
;           // Backface reject, folded into the same ADC bit: the sign of
;           // det[x y w] over the three clip-space corners, which is the
;           // screen-space signed area times w0*w1*w2. Taken as
;           // p0 . (e1 x e2) over (x, y, w) triples, e1/e2 the edges from
;           // corner 0. The sign travels as data, not flags (openvcl
;           // reorders around flag reads): clamp to [-1, +1] so ftoi4
;           // cannot overflow 16 bits, then mtir the low half and read
;           // bit 15. The cull sign goes on before the clamp, so the test
;           // is always just "is it negative"; a sign of 0 flattens every
;           // determinant to zero, which is not negative, so nothing is
;           // culled. Its magnitude is what keeps a small triangle close to
;           // the eye from truncating to zero - see rs::kCullSignScale.
;           vec3  p0   = clip0.xyw;
;           vec3  e1   = clip1.xyw - clip0.xyw;
;           vec3  e2   = clip2.xyw - clip0.xyw;
;           float det  = dot(p0, cross(e1, e2)) * cullSign;
;           if (ftoi4(clamp(det, -1, +1)) < 0)
;               adc += 1; // bit 15 stays set either way
;
;           out[2].w = adc; // .w of each of the 3 vertices
;           out[5].w = adc;
;           out[8].w = adc;
;
;           in   += 9;
;           out  += 9;
;           vertsLeft -= 3;
;           numVerts  -= 3;
;       }
;       while (numVerts != 0);
;
;       // The last window always holds at least one triangle.
;       win[kWindowPrimTag].nloop = kWindowVerts - vertsLeft;
;       XGKICK(win);
;   }
#vuprog VU1Prog_LerpedTriangles

    LoadFrameConstants{ }
    lq fColorClamp, 7(vi00)

    ; The draw's lerp constants, sent up once with the draw rather than with
    ; every batch. The last qword carries the backface cull sign in .x and the
    ; ST scale in .y/.z; its .w is never computed.
    lq fFrontV,     kLerpBlock + 0(vi00)
    lq fBackV,      kLerpBlock + 1(vi00)
    lq fShadeLight, kLerpBlock + 2(vi00)
    lq fStScale,    kLerpBlock + 3(vi00)

    ; Current double buffer and this batch's count:
    xtop   iBase
    ilw.w  iNumVerts, kBatchHeader(iBase)

    ; -1.0 for the area clamp below (vf00.w is the +1).
    sub.x  fMinusOne, vf00, vf00[w]

    ; The vertices: both keyframes and the attribute qword, interleaved, so
    ; one pointer walks all three.
    iaddiu iInPtr, iBase, kVertexData

    ; The first output window, the step that alternates to the other one,
    ; and where this batch's GIF tags are - all parked in VU memory rather
    ; than held in VI registers. See vu_common.i.
    InitOutputWindows{ }

    OpenOutputWindow{ }

    ; One triangle per iteration:
    lTriangleLoop:

        ; Room for another triangle in this window? If not, send what is
        ; there and start the other one. iVertsLeft only ever steps by
        ; three, so "greater than zero" is "at least one triangle" and
        ; the test needs no scratch register.
        ibgtz iVertsLeft, lWindowHasRoom

        CloseOutputWindowAndKick{ lKicked1 }
        OpenOutputWindow{ }

        lWindowHasRoom:

        DoVertex{ 0, 1, 2, 0, 1, 2, fClip0 }
        DoVertex{ 3, 4, 5, 3, 4, 5, fClip1 }
        DoVertex{ 6, 7, 8, 6, 7, 8, fClip2 }

        ; Judge the whole triangle from the last 3 clipw results: if any
        ; vertex left the guard band, 0x7FFF + flags reaches bit 15 (the
        ; ADC bit) and the GS skips this triangle's drawing kick. Written
        ; to every XYZ2 .w so the kicking vertex always carries it.
        fcand  vi01, 0x3FFFF
        iaddiu iADC, vi01, 0x7FFF

        ; Backface reject, folded into the same ADC bit, from the clip-space
        ; corners rather than the screen positions: the sign of det[x y w]
        ; over the three. That is the screen-space signed area times
        ; w0*w1*w2, so it agrees with the screen test wherever all three
        ; corners are in front of the eye - and it needs no divide, so it is
        ; also right where they are not. That is what will let it run ahead
        ; of a clipper, which the screen test cannot.
        ;
        ; Taken as p0 . (e1 x e2), each (x, y, w) triple with its w moved
        ; into z for the outer product. Edges rather than the corners
        ; themselves because a small triangle far away has three nearly equal
        ; rows, and a determinant of those would cancel away most of its
        ; precision before the sign came out.
        sub        fEdge1, fClip1, fClip0
        sub        fEdge2, fClip2, fClip0
        add.z      fEdge1, vf00,   fEdge1[w]
        add.z      fEdge2, vf00,   fEdge2[w]
        move.xy    fEye,   fClip0
        add.z      fEye,   vf00,   fClip0[w]
        opmula.xyz acc,    fEdge1, fEdge2
        opmsub.xyz fNorm,  fEdge2, fEdge1
        mul.xyz    fArea,  fNorm,  fEye
        add.x      fArea,  fArea,  fArea[y]
        add.x      fArea,  fArea,  fArea[z]

        ; The sign travels as data, never flags (openvcl reorders around flag
        ; reads): clamped to [-1, +1] so the ftoi4 cannot overflow the 16 bits
        ; mtir moves, leaving the sign in bit 15 of the VI register, which is
        ; exactly what ibgez reads.
        ;
        ; The draw's cull sign goes on before the clamp, so the test is always
        ; just "is it negative": positive rejects negative determinants,
        ; negative rejects positive ones, and 0 flattens every one to zero,
        ; which is not negative and so never culls. That is one multiply in
        ; place of a mask register, a compare target and a mode branch - VI
        ; registers this program has none of to spare. Its magnitude matters
        ; too: see rs::kCullSignScale.
        mul.x   fArea, fArea, fStScale[x]
        mini.x  fArea, fArea, vf00[w]
        max.x   fArea, fArea, fMinusOne[x]
        ftoi4.x fArea, fArea
        mtir    iAreaInt, fArea[x]
        ibgez   iAreaInt, lNoFaceCull
        iaddiu  iADC, iADC, 1 ; bit 15 stays set either way
    lNoFaceCull:

        isw.w  iADC, 2(iOutPtr)
        isw.w  iADC, 5(iOutPtr)
        isw.w  iADC, 8(iOutPtr)

        iaddiu iInPtr,     iInPtr,      9
        iaddiu iOutPtr,    iOutPtr,     9
        iaddi  iVertsLeft, iVertsLeft, -3
        iaddi  iNumVerts,  iNumVerts,  -3
        ibgtz  iNumVerts, lTriangleLoop

    ; The last window always holds at least one triangle: a window is
    ; closed only when a triangle will not fit, and that triangle goes
    ; straight into the fresh one. So this never kicks an empty packet.
    CloseOutputWindowAndKick{ lKicked2 }

#endvuprog
