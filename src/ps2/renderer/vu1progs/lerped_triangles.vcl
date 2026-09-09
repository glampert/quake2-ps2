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
; each vertex carries a scalar shade term and the batch carries the
; entity's light, so the colour is one broadcast multiply, a clamp
; and an ftoi0. That replaces a 162-entry lookup table the EE used
; to rebuild for every entity of every frame.
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
;   +0    header: backface cull mode in .x (0 = keep everything,
;         1 = cull negative screen area, 2 = cull positive),
;         texture coordinate scale in .y/.z (the skin's size over
;         its power-of-two TEX0 extent - applied here so the EE
;         does not multiply it onto every vertex), vertex count
;         in .w. Read both as integers (.x/.w) and as floats
;         (.y/.z); the lanes never mix in one operation.
;   +1    frontv: current frame scale * (1 - backlerp), w = 0
;   +2    backv:  old frame scale * backlerp, w = 0
;   +3    shadeLight: the entity's light in GS units (0-128 per
;         channel) with the vertex alpha in .w
;   +4    7 GIF tag qwords (set tag, TEST/TEX1/TEX0/ALPHA/ZBUF A+D,
;         prim tag)
;   +11   positions: 2 qwords per vertex - the current frame's
;         dtrivertx_t then the old frame's, each unpacked by the
;         VIF from V4_8 bytes to four *unsigned integers* per qword
;         (x, y, z, lightnormalindex; the last is baggage the EE
;         used to index its colour LUT with - never read here)
;   +167  attributes: 1 qword per vertex: (shade, s, t, q)
;   +245  the GS packet built here: 7 tags + 3 qwords per vertex
;
; The position qwords hold integer bit patterns until itof0
; converts them - they must only ever be touched by raw loads and
; itof0, never an FMAC op (integers look like denormals and would
; flush to zero). The attribute qword carries no such thing any
; more: every lane of it is a real float meant for the FMAC.
;--------------------------------------------------------------------

; Batch offsets, relative to XTOP:
#define kBatchHeader 0
#define kFrontV      1
#define kBackV       2
#define kShadeLight  3
#define kGifTags     4
#define kPositions   11
#define kAttributes  167
#define kOutput      245

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
;       vec4  stq  = attr[offStq]; // (shade, s, t, q)
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
;       // The shade term gets scaled too - unused, but the rotate
;       // below moves it into the ST qword's ignored .w:
;       float q  = 1.0f / pos.w;
;       pos.xyz *= q;                    // now NDC
;       vec4 stqScaled = stq * q;        // (junk, s/w, t/w, 1/w)
;
;       // The vertex colour, from the unscaled shade term broadcast
;       // across the entity's light. This is the whole of what the
;       // EE's per-entity colour LUT used to compute, and the clamp
;       // it needed 486 compares for is two instructions here:
;       vec4 colour  = shadeLight * stq.x;   // .w = alpha * shade
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
    ; term, broadcast from the attribute's .x - which is the unscaled fStq,
    ; not the perspective-divided copy above. Alpha is moved in rather than
    ; multiplied; it is the batch's, not the vertex's. The clamp is the one
    ; the EE spent 486 compares an entity on.
    mulx.xyz fColor, fShadeLight, fStq
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
;       int    cullMode = batch[kBatchHeader].x;
;       vec4   frontv     = batch[kFrontV];
;       vec4   backv      = batch[kBackV];
;       vec4   shadeLight = batch[kShadeLight];
;       qword* pos        = &batch[kPositions];  // 2 qwords per vertex
;       qword* attr       = &batch[kAttributes]; // 1 qword per vertex
;
;       // The sign-bit value (see the backface test below) of a culled
;       // face: mode 1 culls negative areas (bit 15 set), mode 2
;       // positive (clear); mode 0 skips the test entirely.
;       int cullTarget = (cullMode == 1) ? 0x8000 : 0;
;
;       // The GS packet at its fixed home past the input regions:
;       qword* kick = &batch[kOutput];
;       qword* out  = kick;
;
;       // Packet head: the 7 GIF tag qwords prepared by the EE:
;       memcpy(out, &batch[kGifTags], 7 * sizeof(qword));
;       out += 7;
;
;       do // One triangle per iteration:
;       {
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
;           if (cullMode != 0)
;           {
;               vec2  e1   = scr1.xy - scr0.xy;
;               vec2  e2   = scr2.xy - scr0.xy;
;               float area = e1.x * e2.y - e2.x * e1.y;
;               int   sign = ftoi4(clamp(area, -1, +1)) & 0x8000;
;               if (sign == cullTarget) // 0x8000 for mode 1, 0 for mode 2
;                   adc += 1;           // bit 15 stays set either way
;           }
;
;           out[2].w = adc; // .w of each of the 3 vertices
;           out[5].w = adc;
;           out[8].w = adc;
;
;           pos  += 6;
;           attr += 3;
;           out  += 9;
;           numVerts -= 3;
;       }
;       while (numVerts != 0);
;
;       XGKICK(kick); // Send the finished GS packet.
;   }
#vuprog VU1Prog_LerpedTriangles

    ; VCL requires zeroed clip flags before any CLIP instruction:
    fcset 0x000000

    ; Frame constants from the fixed low addresses:
    lq fMVP0,       0(vi00)
    lq fMVP1,       1(vi00)
    lq fMVP2,       2(vi00)
    lq fMVP3,       3(vi00)
    lq fGSScale,    4(vi00)
    lq fGSOffset,   5(vi00)
    lq fClipScale,  6(vi00)
    lq fColorClamp, 7(vi00)

    ; Current double buffer, this batch's counts and lerp constants:
    xtop   iBase
    ilw.w  iNumVerts,   kBatchHeader(iBase)
    ilw.x  iCullMode,   kBatchHeader(iBase)
    lq     fFrontV,     kFrontV(iBase)
    lq     fBackV,      kBackV(iBase)
    lq     fShadeLight, kShadeLight(iBase)

    ; The same header qword as a vector, for the ST scale in .y/.z. A raw
    ; load, so .x and .w keep the integer bit patterns read above - only
    ; ever used through a .yz mask, which never computes those lanes.
    lq     fStScale,  kBatchHeader(iBase)

    ; Backface-test constants: -1.0 for the area clamp (vf00.w is the +1),
    ; the 16-bit sign mask, and the masked value a culled face matches -
    ; 0x8000 for mode 1 (cull negative areas), 0 for mode 2 (positive).
    sub.x  fMinusOne, vf00, vf00[w]
    iaddiu iSignMask, vi00, 0x7FFF
    iaddiu iSignMask, iSignMask, 1
    iaddiu iCullTarget, vi00, 0
    iaddi  iTmp, iCullMode, -1
    ibne   iTmp, vi00, lCullTargetDone
    iadd   iCullTarget, iSignMask, vi00
    lCullTargetDone:

    ; The input regions and the GS packet, all at fixed offsets:
    iaddiu iPosPtr,  iBase, kPositions
    iaddiu iAttrPtr, iBase, kAttributes
    iaddiu iKick,    iBase, kOutput

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
        ; bits mtir moves, leaving the sign in bit 15 of the VI register.
        ; area.x = e1.x*e2.y - e2.x*e1.y, e1/e2 = the two edges from scr0.
        ibeq    iCullMode, vi00, lNoFaceCull
        sub.xy  fEdge1, fScr1, fScr0
        sub.xy  fEdge2, fScr2, fScr0
        mula.x  acc,   fEdge1, fEdge2[y]
        msub.x  fArea, fEdge2, fEdge1[y]
        mini.x  fArea, fArea, vf00[w]
        max.x   fArea, fArea, fMinusOne[x]
        ftoi4.x fArea, fArea
        mtir    iAreaInt, fArea[x]
        iand    iAreaSign, iAreaInt, iSignMask
        ibne    iAreaSign, iCullTarget, lNoFaceCull
        iaddiu  iADC, iADC, 1 ; bit 15 stays set either way
    lNoFaceCull:

        isw.w  iADC, 2(iOutPtr)
        isw.w  iADC, 5(iOutPtr)
        isw.w  iADC, 8(iOutPtr)

        iaddiu iPosPtr,   iPosPtr,    6
        iaddiu iAttrPtr,  iAttrPtr,   3
        iaddiu iOutPtr,   iOutPtr,    9
        iaddi  iNumVerts, iNumVerts, -3
        ibne   iNumVerts, vi00, lTriangleLoop

    --barrier

    xgkick iKick

#endvuprog
