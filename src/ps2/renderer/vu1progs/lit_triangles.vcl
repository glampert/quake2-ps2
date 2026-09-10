;--------------------------------------------------------------------
; lit_triangles.vcl
;
; textured_triangles.vcl plus per-vertex dynamic point lighting: the
; same transform, clip judgement and GS packet, but the vertex colour
; is *computed* from up to four point lights instead of copied from
; the input. Used by the world's lightmap pass so dynamic lights never
; have to touch a lightmap texture.
;
; The technique is the one Shadowman 2 shipped on PS2: four lights at
; once with no divide and no square root, by storing the light
; positions transposed - all four X's in one register, all four Y's in
; the next, all four Z's in the last - so every SIMD lane carries a
; different light rather than a different axis.
; (https://martinfullerblog.wordpress.com/2023/01/29/ps2-vector-unit-lighting-on-shadowman2)
;
;   Lx = lightX.xyzw - pos.x // four lights, one instruction
;   Ly = lightY.xyzw - pos.y
;   Lz = lightZ.xyzw - pos.z
;   distSqr = Lx*Lx + Ly*Ly + Lz*Lz ; .x .y .z .w = light 0 1 2 3
;
; The attenuation is 1 - d^2/r^2 folded into a single multiply-add per
; light. The EE hands over colour and -(colour / radius^2) already
; scaled to GS colour units, so:
;
;   light += max(distSqr.i * negColDivR2_i + colour_i, 0)
;
; is one mula, one madd, one max and one add per light. No N.L term -
; distance attenuation only, as the original shipped.
;
; VU data memory layout (qwords; must match vu1.cpp):
;   0-3      MVP matrix rows (row-vector convention)
;   4        GS scale  (2048, 2048, zScale)
;   5        GS offset (2048 + width/2, 2048 + height/2, zScale)
;   6        clip-judgement scale (guard band for x/y, 1.0 for z)
;   8+       XTOP double buffers (VIF1 BASE/OFFSET)
;   1000+    the light block (see below), outside the double buffers
;
; Light block at kLightBlock, 12 qwords:
;   +0   light X positions   (x0, x1, x2, x3)
;   +1   light Y positions
;   +2   light Z positions
;   +3   -(colour0 / radius0^2) in .xyz
;   +4   -(colour1 / radius1^2)
;   +5   -(colour2 / radius2^2)
;   +6   -(colour3 / radius3^2)
;   +7   colour0 in .xyz, GS units (0-255)
;   +8   colour1
;   +9   colour2
;   +10  colour3
;   +11  clamp/bias: (255, 255, 255, 128)
;
; An unused light slot is zeroed on the EE, which makes its whole term
; max(0 + 0, 0) = 0 - no branch needed for the common one-or-two light
; case.
;
; Batch layout at XTOP: identical to textured_triangles.vcl.
;
; Output differs in one register: this program emits PACKED RGBAQ
; rather than an A+D write, because the colour it produces is four
; floats rather than a packed u32 - and ftoi0 of a float vector lands
; exactly in the one-byte-per-word layout the PACKED descriptor wants.
; Q comes from the internal register the preceding PACKED ST write
; latches (word 2 of the ST qword carries it), which is why ST must
; stay ahead of RGBAQ in the register list.
;--------------------------------------------------------------------

; Batch offsets, relative to XTOP:
#define kBatchHeader 0
#define kGifTags     1
#define kVertexData  8

; The light block's absolute address (outside the double buffers):
#define kLightBlock  1000

; Transforms and lights one vertex. Input is the same 2 qwords as the
; textured program - position, then (rgba, s, t, q) - but the packed
; colour word is ignored: this program computes its own.
;
; C-like pseudo-code ('in'/'out' are the qword arrays at iInPtr/iOutPtr):
;
;   void DoVertex(int offPos, int offStq, int offST, int offRGBA, int offXyz)
;   {
;       vec4 pos = in[offPos];
;       vec4 stq = in[offStq];
;
;       // Point lighting, in world space - the world pass draws
;       // untransformed, so the input position already is world space:
;       vec4 lx = lightX - pos.x;
;       vec4 ly = lightY - pos.y;
;       vec4 lz = lightZ - pos.z;
;       vec4 d  = lx*lx + ly*ly + lz*lz;
;
;       vec3 light = 0;
;       light += max(d.x * negColDivR2_0 + colour_0, 0);
;       light += max(d.y * negColDivR2_1 + colour_1, 0);
;       light += max(d.z * negColDivR2_2 + colour_2, 0);
;       light += max(d.w * negColDivR2_3 + colour_3, 0);
;       light = mini(light, 255);
;
;       // ...then exactly the textured program's transform:
;       pos = pos.x*mvp[0] + pos.y*mvp[1] + pos.z*mvp[2] + pos.w*mvp[3];
;       clipFlagQueue.push(pos.xyz * clipScale, abs(pos.w));
;       float q = 1.0f / pos.w;
;       pos.xyz *= q;
;       vec4 stqScaled = stq * q;
;       pos.xyz = ftoi4(gsOffset + pos.xyz * gsScale);
;
;       out[offST]      = stqScaled.yzwx;      // ST (.z carries Q)
;       out[offRGBA]    = ftoi0(light);        // PACKED RGBAQ, .w = 128
;       out[offXyz].xyz = pos.xyz;             // XYZ (.w ADC bit set by caller)
;   }
#macro DoVertex: offPos, offStq, offST, offRGBA, offXyz

    lq fPos, offPos(iInPtr)
    lq fStq, offStq(iInPtr)

    ; Four light vectors at once, one lane per light.
    sub fLx, fLightX, fPos[x]
    sub fLy, fLightY, fPos[y]
    sub fLz, fLightZ, fPos[z]

    ; Squared distance to each of the four lights.
    mul  acc,  fLx, fLx
    madd acc,  fLy, fLy
    madd fDist, fLz, fLz

    ; Start at black, with the alpha the lightmap pass needs already in
    ; place: .w stays untouched by the .xyz lighting below and converts
    ; to the GS 1.0 (128).
    move.xyz fLight, vf00
    move.w   fLight, fLightClamp

    ; light += max(distSqr.i * -(colour_i / radius_i^2) + colour_i, 0)
    mula.xyz acc,     fNegDiv0, fDist[x]
    madd.xyz fTerm,   fColour0, vf00[w]
    max.xyz  fTerm,   fTerm,    vf00
    add.xyz  fLight,  fLight,   fTerm

    mula.xyz acc,     fNegDiv1, fDist[y]
    madd.xyz fTerm,   fColour1, vf00[w]
    max.xyz  fTerm,   fTerm,    vf00
    add.xyz  fLight,  fLight,   fTerm

    mula.xyz acc,     fNegDiv2, fDist[z]
    madd.xyz fTerm,   fColour2, vf00[w]
    max.xyz  fTerm,   fTerm,    vf00
    add.xyz  fLight,  fLight,   fTerm

    mula.xyz acc,     fNegDiv3, fDist[w]
    madd.xyz fTerm,   fColour3, vf00[w]
    max.xyz  fTerm,   fTerm,    vf00
    add.xyz  fLight,  fLight,   fTerm

    ; Saturate to the GS byte range and drop to integers, one byte per
    ; word, which is the PACKED RGBAQ layout.
    mini.xyz fLight, fLight, fLightClamp
    ftoi0   fRGBA,  fLight

    ; Position to clip space (row-vector MVP):
    mul  acc,  fMVP0, fPos[x]
    madd acc,  fMVP1, fPos[y]
    madd acc,  fMVP2, fPos[z]
    ; The MVP's translation row is scaled by a hardwired 1.0, not by the vertex's
    ; own .w: PolyVertex parks its lightmap S there, and every other DrawVertex
    ; producer writes a 1.0 that this no longer needs. Same reason
    ; lerped_triangles.vcl does it - see the note on mod::PolyVertex.
    madd fPos, fMVP3, vf00[w]

    ; Guard-band clip judgement against |w|: scaled x/y, exact z.
    mul.xyz   fJudge, fPos, fClipScale
    clipw.xyz fJudge, fPos[w]

    ; Perspective divide, with the same 1/w onto the texture coords.
    div     q,          vf00[w], fPos[w]
    mul.xyz fPos,       fPos,    q
    mulq    fStqScaled, fStq,    q

    ; NDC to GS window coordinates, in 12.4 fixed point:
    mula.xyz  acc,  fGSOffset, vf00[w]
    madd.xyz  fPos, fPos, fGSScale
    ftoi4.xyz fPos, fPos

    ; Rotate (junk, sq, tq, q) into ST order (sq, tq, q, junk):
    mr32 fST, fStqScaled

    ; Q for the PACKED RGBAQ that follows: the GS latches it out of word 2 of the
    ; ST write, so unlike the A+D programs this one cannot let it ride in from the
    ; vertex - PolyVertex keeps its lightmap T in that lane. vf00.z is 0, so this
    ; lands the reciprocal exactly.
    addq.z fST, vf00, q

    sq     fST,   offST(iOutPtr)
    sq     fRGBA, offRGBA(iOutPtr)
    sq.xyz fPos,  offXyz(iOutPtr)

#endmacro

; Identical to VU1Prog_TexturedTriangles apart from loading the
; light block and the per-vertex lighting inside DoVertex.
#vuprog VU1Prog_LitTriangles

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

    ; The light block, uploaded once per draw chain and shared by every
    ; batch in it.
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

    ; Current double buffer and this batch's pointers:
    xtop   iBase
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

        ; Whole-triangle guard band reject, as the textured program.
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
