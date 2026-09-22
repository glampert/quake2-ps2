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
; VU data memory layout (qwords; must match vu1.cpp):
;   0-3  MVP matrix rows (row-vector convention; row 3 carries 'move')
;   4    GS scale  (2048, 2048, zScale)
;   5    GS offset (2048 + width/2, 2048 + height/2, zScale)
;   6    clip-judgement scale (guard band for x/y, 1.0 for z)
;   7    colour clamp (255, 255, 255, 255)
;   8+   XTOP double buffers (VIF1 BASE/OFFSET)
;
; Batch layout at XTOP - fixed offsets sized for the 78-vertex
; maximum chunk, so short chunks leave gaps rather than move the
; regions (the EE and this program share compile-time addresses):
;   +0    header: backface cull sign in .x (+1 culls negative screen
;         areas, -1 culls positive, 0 culls nothing),
;         texture coordinate scale in .y/.z (the skin's size over
;         its power-of-two TEX0 extent - applied here so the EE
;         does not multiply it onto every vertex), vertex count
;         in .w. Read both as integers (.x/.w) and as floats
;         (.y/.z); the lanes never mix in one operation.
;   +1    frontv: current frame scale * (1 - backlerp), w = 0
;   +2    backv:  old frame scale * backlerp, w = 0
;   +3    shadeLight: the entity's light, vertex alpha in .w. Not
;         in GS units any more - it carries the 1/128 matching the
;         quantized shade byte, so light * shade lands back in the
;         0-255 the clamp expects
;   +4    7 GIF tag qwords (set tag, TEST/TEX1/TEX0/ALPHA/ZBUF A+D,
;         prim tag)
;   +11   positions: 2 qwords per vertex - the current frame's
;         dtrivertx_t then the old frame's, each unpacked by the
;         VIF from V4_8 bytes to four *unsigned integers* per qword.
;         The current frame's 4th lane is its lightnormalindex,
;         which the EE indexes the shade table with and this program
;         never reads; the old frame's is the quantized shade term
;         the EE wrote over it, and is where the colour comes from
;   +155  attributes: 1 qword per vertex: (unused, s, t, q), handed
;         to the DMA straight out of the model hunk - .x is the
;         model's own keyframe index, never a float
;   +227  output window A: a copy of the 7 tags, then 3 qwords per
;   +342  output window B:   vertex - ST, RGBAQ, XYZ2 - up to 36 of
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
#define kFrontV      1
#define kBackV       2
#define kShadeLight  3
#define kGifTags     4
#define kPositions   11
#define kAttributes  155

; The two output windows: where they start, how far apart they are, how
; many vertices each holds, and where the drawing tag sits inside one.
; Must match the kLerp*Window* constants in vu1.h.
#define kWindowA       227
#define kWindowB       342
#define kWindowQwords  115
#define kWindowVerts   36
#define kWindowPrimTag 6

; Transforms one vertex: two position qwords at offCur/offOld from
; iPosPtr (integer byte lanes of the two keyframes) and one attribute
; qword at offStq from iAttrPtr become the ST, RGBAQ (PACKED) and
; XYZ2 output qwords at offST/offRGBA/offXyz from iOutPtr. Leaves this
; vertex's clipw flags as the newest entry in the clip flag register;
; the caller judges whole triangles with fcand after 3 calls and
; writes the XYZ2 .w ADC bit. 'dstScreen' additionally receives the
; float screen-space position (pre-ftoi4), which the caller's
; backface test crosses across the triangle - one register per
; vertex, at no extra instruction (the madd lands there anyway).
;
; The position integers must not reach an FMAC before itof0 - they
; look like denormals and would flush to zero.
;
; C-like pseudo-code ('pos'/'attr'/'out' are the qword arrays at
; iPosPtr/iAttrPtr/iOutPtr):
;
;   void DoVertex(int offCur, int offOld, int offStq,
;                 int offST, int offRGBA, int offXyz)
;   {
;       ivec4 curI = pos[offCur];  // (x, y, z, normalindex) ints, 0-255
;       ivec4 oldI = pos[offOld];
;       vec4  stq  = attr[offStq]; // (unused, s, t, q)
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
;       // vf00's hardwired 1, not from the junk in pos.w:
;       pos = pos.x * mvp[0] + pos.y * mvp[1]
;           + pos.z * mvp[2] + 1.0 * mvp[3];
;
;       // Guard-band clip judgement: compare the scaled position
;       // against |w| and push the 6 outside flags (+x,-x,+y,-y,+z,-z)
;       // onto the clip flag queue for the caller to inspect:
;       vec3 judge = pos.xyz * clipScale.xyz;
;       clipFlagQueue.push(judge, abs(pos.w));
;
;       // Perspective divide; the STQ words share the 1/w so the GS
;       // gets (s/w, t/w, 1/w) for perspective-correct interpolation.
;       // The keyframe index in .x gets scaled too - it reads as
;       // zero (the VU has no denormals) and the rotate below moves
;       // it into the ST qword's ignored .w:
;       float q  = 1.0f / pos.w;
;       pos.xyz *= q;                    // now NDC
;       vec4 stqScaled = stq * q;        // (junk, s/w, t/w, 1/w)
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
;       // NDC to GS window coordinates; the float form feeds the
;       // caller's backface cross product, the 12.4 form the GS:
;       dstScreen = gsOffset.xyz + pos.xyz * gsScale.xyz;
;       pos.xyz   = ftoi4(dstScreen.xyz);
;
;       // ST scaled by the skin's power-of-two correction:
;       stqScaled.yz   *= stScale.yz;
;       out[offST]      = stqScaled.yzwx; // ST (.z carries Q; .w junk)
;       out[offRGBA]    = ftoi0(colour);  // PACKED RGBAQ: one byte per word,
;                                         // Q from the ST write just above
;       out[offXyz].xyz = pos.xyz;        // XYZ (.w ADC bit set by caller)
;   }
#macro DoVertex: offCur, offOld, offStq, offST, offRGBA, offXyz, dstScreen

    lq fCurI, offCur(iPosPtr)
    lq fOldI, offOld(iPosPtr)
    lq fStq,  offStq(iAttrPtr)

    ; Byte lanes to floats (raw integers until here - no FMAC before this):
    itof0 fCur, fCurI
    itof0 fOld, fOldI

    ; The two-keyframe pose lerp (componentwise, not broadcast):
    mul  acc,  fFrontV, fCur
    madd fPos, fBackV,  fOld

    ; Position to clip space (row-vector MVP); w = 1 from vf00, never
    ; from fPos.w, which holds lerped normal-index junk:
    mul  acc,  fMVP0, fPos[x]
    madd acc,  fMVP1, fPos[y]
    madd acc,  fMVP2, fPos[z]
    madd fPos, fMVP3, vf00[w]

    ; Guard-band clip judgement against |w|: scaled x/y, exact z.
    mul.xyz   fJudge, fPos, fClipScale
    clipw.xyz fJudge, fPos[w]

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
    div        q,          vf00[w], fPos[w]
    mul.xyz    fPos,       fPos,    q
    mulq       fStqScaled, fStq,    q

    ; NDC to GS window coordinates: float into dstScreen for the caller's
    ; backface test, then 12.4 fixed point for the GS packet:
    mula.xyz  acc,  fGSOffset, vf00[w]
    madd.xyz  dstScreen, fPos, fGSScale
    ftoi4.xyz fPos, dstScreen

    ; The skin's power-of-two correction, which the EE used to multiply onto
    ; every vertex before handing them over. Masked to .yz so the header
    ; integers sitting in fStScale.x/.w are never computed.
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
    sq.xyz fPos,  offXyz(iOutPtr)

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
;       // This batch, in the current double buffer:
;       qword* batch    = &vuMem[XTOP];
;       int    numVerts = batch[kBatchHeader].w;
;       float  cullSign = batch[kBatchHeader].x;
;       vec4   frontv     = batch[kFrontV];
;       vec4   backv      = batch[kBackV];
;       vec4   shadeLight = batch[kShadeLight];
;       qword* pos        = &batch[kPositions];  // 2 qwords per vertex
;       qword* attr       = &batch[kAttributes]; // 1 qword per vertex
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
;           vec3 scr0, scr1, scr2; // float screen positions
;           DoVertex(0, 1,  0,  0, 1, 2,  scr0); // pos[0..1], attr[0] -> out[0..2]
;           DoVertex(2, 3,  1,  3, 4, 5,  scr1); // pos[2..3], attr[1] -> out[3..5]
;           DoVertex(4, 5,  2,  6, 7, 8,  scr2); // pos[4..5], attr[2] -> out[6..8]
;
;           // Whole-triangle guard band reject: if any of the 18 clip
;           // flags of the 3 vertices above is set, adc becomes 0x8000,
;           // i.e. bit 15 - the ADC bit - and the GS skips this
;           // triangle's drawing kick.
;           int adc = 0x7FFF + (clipFlagQueue.last3() != 0 ? 1 : 0);
;
;           // Backface reject: the sign of the triangle's screen-space
;           // signed area, folded into the same ADC bit. The sign travels
;           // as data, not flags (openvcl reorders around flag reads):
;           // clamp the area to [-1, +1] so ftoi4 cannot overflow 16 bits,
;           // then mtir the low half and mask bit 15. Sub-(1/16)px^2 areas
;           // truncate to 0 = "positive" - at worst a stray subpixel
;           // triangle draws. Guard-band-rejected triangles may cross
;           // garbage positions, but their ADC bit is already set and the
;           // +1 keeps it (and VU floats cannot produce NaN to trip this).
;           // The cull sign goes on before the clamp, so the test is
;           // always just "is it negative". A sign of 0 flattens every
;           // area to zero, which is not negative, so nothing is culled.
;           vec2  e1   = scr1.xy - scr0.xy;
;           vec2  e2   = scr2.xy - scr0.xy;
;           float area = (e1.x * e2.y - e2.x * e1.y) * cullSign;
;           if (ftoi4(clamp(area, -1, +1)) < 0)
;               adc += 1; // bit 15 stays set either way
;
;           out[2].w = adc; // .w of each of the 3 vertices
;           out[5].w = adc;
;           out[8].w = adc;
;
;           pos  += 6;
;           attr += 3;
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

    ; Current double buffer, this batch's counts and lerp constants:
    xtop   iBase
    ilw.w  iNumVerts,   kBatchHeader(iBase)
    lq     fFrontV,     kFrontV(iBase)
    lq     fBackV,      kBackV(iBase)
    lq     fShadeLight, kShadeLight(iBase)

    ; The same header qword as a vector: the ST scale in .y/.z and the
    ; backface cull sign in .x. A raw load, so .w keeps the integer bit
    ; pattern read above - it is only ever used through masks that never
    ; compute that lane.
    lq     fStScale,  kBatchHeader(iBase)

    ; -1.0 for the area clamp below (vf00.w is the +1).
    sub.x  fMinusOne, vf00, vf00[w]

    ; The input regions, all at fixed offsets:
    iaddiu iPosPtr,  iBase, kPositions
    iaddiu iAttrPtr, iBase, kAttributes

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

        DoVertex{ 0, 1, 0, 0, 1, 2, fScr0 }
        DoVertex{ 2, 3, 1, 3, 4, 5, fScr1 }
        DoVertex{ 4, 5, 2, 6, 7, 8, fScr2 }

        ; Judge the whole triangle from the last 3 clipw results: if any
        ; vertex left the guard band, 0x7FFF + flags reaches bit 15 (the
        ; ADC bit) and the GS skips this triangle's drawing kick. Written
        ; to every XYZ2 .w so the kicking vertex always carries it.
        fcand  vi01, 0x3FFFF
        iaddiu iADC, vi01, 0x7FFF

        ; Backface reject, folded into the same ADC bit. The area's sign
        ; travels as data, never flags (openvcl reorders around flag
        ; reads): clamped to [-1, +1] so the ftoi4 cannot overflow the 16
        ; bits mtir moves, leaving the sign in bit 15 of the VI register,
        ; which is exactly what ibgez reads.
        ; area.x = e1.x*e2.y - e2.x*e1.y, e1/e2 = the two edges from scr0.
        ;
        ; The batch's cull sign goes on before the clamp, so the test is
        ; always just "is it negative": +1 rejects negative areas, -1
        ; rejects positive ones, and 0 flattens every area to zero, which
        ; is not negative and so never culls. That is one multiply in
        ; place of a mask register, a compare target and a mode branch -
        ; VI registers this program has none of to spare.
        sub.xy  fEdge1, fScr1, fScr0
        sub.xy  fEdge2, fScr2, fScr0
        mula.x  acc,   fEdge1, fEdge2[y]
        msub.x  fArea, fEdge2, fEdge1[y]
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

        iaddiu iPosPtr,    iPosPtr,     6
        iaddiu iAttrPtr,   iAttrPtr,    3
        iaddiu iOutPtr,    iOutPtr,     9
        iaddi  iVertsLeft, iVertsLeft, -3
        iaddi  iNumVerts,  iNumVerts,  -3
        ibgtz  iNumVerts, lTriangleLoop

    ; The last window always holds at least one triangle: a window is
    ; closed only when a triangle will not fit, and that triangle goes
    ; straight into the fresh one. So this never kicks an empty packet.
    CloseOutputWindowAndKick{ lKicked2 }

#endvuprog
