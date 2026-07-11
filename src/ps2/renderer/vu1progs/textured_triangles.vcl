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
;   +0   header: vertex count in .w
;   +1   5 GIF tag qwords (set tag, TEST A+D, TEX1 A+D, TEX0 A+D, prim tag)
;   +6   vertices, 3 qwords each: position, RGBA, STQ
;
; The GS packet (the 5 GIF tags + 3 output qwords per vertex: ST,
; RGBAQ, XYZ2) is built right after the input vertices in the same
; buffer and sent with XGKICK. Clipping is a whole-triangle guard
; band reject: clipw flags outside the scaled |w| range set the ADC
; bit on all 3 vertices so the GS skips the drawing kick.
;--------------------------------------------------------------------

; Batch offsets, relative to XTOP:
#define kBatchHeader 0
#define kGifTags     1
#define kVertexData  6

; Transforms one vertex: 3 input qwords at offPos/offColor/offStq from
; iInPtr become the ST, RGBAQ and XYZ2 output qwords at the same offsets
; from iOutPtr (the offsets coincide because both formats are 3 qwords).
; Leaves this vertex's clipw flags as the newest entry in the clip flag
; register; the caller judges whole triangles with fcand after 3 calls
; and writes the XYZ2 .w ADC bit.
;
; C-like pseudo-code ('in'/'out' are the qword arrays at iInPtr/iOutPtr):
;
;   void DoVertex(int offPos, int offColor, int offStq)
;   {
;       vec4 pos   = in[offPos];
;       vec4 color = in[offColor];
;       vec4 stq   = in[offStq];
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
;
;       // Perspective divide; STQ shares the 1/w so the GS gets
;       // (s/w, t/w, 1/w) for perspective-correct interpolation:
;       float q  = 1.0f / pos.w;
;       pos.xyz *= q; // now NDC
;       stq     *= q;
;
;       // NDC to GS window coordinates, in 12.4 fixed point:
;       pos.xyz = ftoi4(gsOffset.xyz + pos.xyz * gsScale.xyz);
;
;       out[offPos]     = stq;     // ST    (.z carries Q = 1/w)
;       out[offColor]   = color;   // RGBAQ
;       out[offStq].xyz = pos.xyz; // XYZ   (.w ADC bit set by caller)
;   }
#macro DoVertex: offPos, offColor, offStq

    lq fPos,   offPos(iInPtr)
    lq fColor, offColor(iInPtr)
    lq fStq,   offStq(iInPtr)

    ; Position to clip space (row-vector MVP):
    mul  acc,  fMVP0, fPos[x]
    madd acc,  fMVP1, fPos[y]
    madd acc,  fMVP2, fPos[z]
    madd fPos, fMVP3, fPos[w]

    ; Guard-band clip judgement against |w|: scaled x/y, exact z.
    mul.xyz   fJudge, fPos, fClipScale
    clipw.xyz fJudge, fPos[w]

    ; Perspective divide, with the same 1/w multiplied onto the texture
    ; coords - the GS wants (s/w, t/w, 1/w) for perspective-correct
    ; interpolation.
    div     q,    vf00[w], fPos[w]
    mul.xyz fPos, fPos,    q
    mulq    fStq, fStq,    q

    ; NDC to GS window coordinates, in 12.4 fixed point:
    mula.xyz  acc,  fGSOffset, vf00[w]
    madd.xyz  fPos, fPos, fGSScale
    ftoi4.xyz fPos, fPos

    sq     fStq,   offPos(iOutPtr)
    sq     fColor, offColor(iOutPtr)
    sq.xyz fPos,   offStq(iOutPtr)

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
;       qword* in       = &batch[kVertexData]; // 3 qwords per vertex
;
;       // The GS packet starts right after the input vertices:
;       qword* kick = in + (numVerts * 3);
;       qword* out  = kick;
;
;       // Packet head: the 5 GIF tag qwords prepared by the EE:
;       memcpy(out, &batch[kGifTags], 5 * sizeof(qword));
;       out += 5;
;
;       do // One triangle per iteration:
;       {
;           DoVertex(0, 1, 2); // in[0..2] -> out[0..2]
;           DoVertex(3, 4, 5); // in[3..5] -> out[3..5]
;           DoVertex(6, 7, 8); // in[6..8] -> out[6..8]
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
;           in  += 9;
;           out += 9;
;           numVerts -= 3;
;       }
;       while (numVerts != 0);
;
;       XGKICK(kick); // Send the finished GS packet.
;   }
#vuprog VU1Prog_TexturedTriangles

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

    ; Current double buffer and this batch's pointers:
    xtop   iBase
    ilw.w  iNumVerts, kBatchHeader(iBase)
    iaddiu iInPtr, iBase, kVertexData

    ; Output (the GS packet) starts right after the input vertices:
    iadd   iKick, iInPtr, iNumVerts
    iadd   iKick, iKick,  iNumVerts
    iadd   iKick, iKick,  iNumVerts

    ; The GIF tags were prepared by the EE; copy them to the packet head:
    iaddiu iTagPtr, iBase, kGifTags
    iaddiu iOutPtr, iKick, 0
    lqi fTag0, (iTagPtr++)
    lqi fTag1, (iTagPtr++)
    lqi fTag2, (iTagPtr++)
    lqi fTag3, (iTagPtr++)
    lqi fTag4, (iTagPtr++)
    sqi fTag0, (iOutPtr++)
    sqi fTag1, (iOutPtr++)
    sqi fTag2, (iOutPtr++)
    sqi fTag3, (iOutPtr++)
    sqi fTag4, (iOutPtr++)

    ; One triangle per iteration:
    lTriangleLoop:

        DoVertex{ 0, 1, 2 }
        DoVertex{ 3, 4, 5 }
        DoVertex{ 6, 7, 8 }

        ; Judge the whole triangle from the last 3 clipw results: if any
        ; vertex left the guard band, 0x7FFF + flags reaches bit 15 (the
        ; ADC bit) and the GS skips this triangle's drawing kick. Written
        ; to every XYZ2 .w so the kicking vertex always carries it.
        fcand  vi01, 0x3FFFF
        iaddiu iADC, vi01, 0x7FFF
        isw.w  iADC, 2(iOutPtr)
        isw.w  iADC, 5(iOutPtr)
        isw.w  iADC, 8(iOutPtr)

        iaddiu iInPtr,    iInPtr,     9
        iaddiu iOutPtr,   iOutPtr,    9
        iaddi  iNumVerts, iNumVerts, -3
        ibne   iNumVerts, vi00, lTriangleLoop

    --barrier

    xgkick iKick

#endvuprog
