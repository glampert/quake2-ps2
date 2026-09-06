;--------------------------------------------------------------------
; particles.vcl
;
; A VU1 microprogram to draw a batch of camera-facing particle
; billboards. Preprocessed with vclpp; the -j flag injects the VCL
; boilerplate (.init_*, --enter/--exit blocks).
;
; The EE submits one quadword per particle - packed colour and world
; origin - and this program builds the whole billboard. Nothing is
; transformed on the EE.
;
; Why a GS sprite rather than two triangles: the billboard spans the
; camera's 'up' and 'right', which are orthogonal to 'forward', so
; sliding along them cannot change view-space depth. Every corner
; therefore shares the centre's clip w, and the quad projects to an
; exactly axis-aligned screen rectangle - which is precisely what
; PRIM_SPRITE draws from two vertices. Two consequences:
;
;   - The corner offset needs no per-particle transform. The EE
;     transforms (up + right) * 1.5 once per batch as a direction
;     (w = 0), and this program scales that constant by 1/w.
;   - Output is 5 qwords per particle instead of the 18 that six
;     gouraud vertices would take.
;
; VU data memory layout (qwords; must match vu1.cpp):
;   0-3  MVP matrix rows (row-vector convention)
;   4    GS scale  (2048, 2048, zScale)
;   5    GS offset (2048 + width/2, 2048 + height/2, zScale)
;   6    clip-judgement scale (guard band for x/y, 1.0 for z)
;   8+   XTOP double buffers (VIF1 BASE/OFFSET)
;
; Batch layout at XTOP:
;   +0   header: particle count in .w
;   +1   quad offset: clip-space (up + right) * 1.5 in .xyz (z is 0,
;        the offset is orthogonal to the view axis), and the distance
;        blow-up rate in .w
;   +2   UV of the billboard's anchor corner
;   +3   UV of its opposite corner
;   +4   7 GIF tag qwords (set tag, TEST/TEX1/TEX0/ALPHA/ZBUF A+D, prim tag)
;   +11  particles, 1 qword each: (rgba, x, y, z)
;
; The GS packet (the 7 GIF tags + 5 output qwords per particle: the
; A+D colour, then a UV/XYZ2 pair per corner) is built right after the
; input particles in the same buffer and sent with XGKICK.
;
; The colour arrives packed in the .x word - first, as in DrawVertex,
; because it is raw-copied (lq/sq.x) into an A+D qword rather than
; spread a byte per word: FMAC ops would flush denormal colour bit
; patterns (e.g. 0x800000FF) to zero. The position consequently lives
; in .yzw and the transform below reads it from there.
;
; Clipping is a whole-billboard guard band reject on the centre: a
; billboard is small in screen space, so if its centre leaves the band
; the ADC bit is set on both corners and the GS skips the drawing kick.
;--------------------------------------------------------------------

; Batch offsets, relative to XTOP:
#define kBatchHeader   0
#define kQuadOffset    1
#define kUV0           2
#define kUV1           3
#define kGifTags       4
#define kParticleData 11

; C-like pseudo-code of the program below ('vuMem' is VU1 data memory
; seen as an array of qwords):
;
;   void VU1Prog_Particles()
;   {
;       // Frame constants at the fixed low addresses:
;       mat4 mvp       = vuMem[0..3];
;       vec4 gsScale   = vuMem[4];
;       vec4 gsOffset  = vuMem[5];
;       vec4 clipScale = vuMem[6];
;
;       // This batch, in the current double buffer:
;       qword* batch    = &vuMem[XTOP];
;       int    numPrts  = batch[kBatchHeader].w;
;       vec4   quadOff  = batch[kQuadOffset];  // .xyz offset, .w blow-up rate
;       vec4   uv0      = batch[kUV0];
;       vec4   uv1      = batch[kUV1];
;       qword* in       = &batch[kParticleData]; // 1 qword per particle
;
;       // The GS packet starts right after the input particles:
;       qword* kick = in + numPrts;
;       qword* out  = kick;
;
;       // Packet head: the 7 GIF tag qwords prepared by the EE:
;       memcpy(out, &batch[kGifTags], 7 * sizeof(qword));
;       out += 7;
;
;       do // One particle per iteration:
;       {
;           vec4 p = in[0]; // (rgba, x, y, z); raw packed colour in .x
;
;           // World space to clip space. The position is in .yzw and the
;           // w term is a literal 1, so vf00[w] supplies it:
;           vec4 c = p.y * mvp[0] + p.z * mvp[1]
;                  + p.w * mvp[2] + 1.0f * mvp[3];
;
;           // Guard-band clip judgement on the centre; the caller-side
;           // equivalent of the triangle programs' per-vertex test:
;           vec3 judge = c.xyz * clipScale.xyz;
;           clipFlagQueue.push(judge, abs(c.w));
;
;           float q = 1.0f / c.w;
;
;           // ref_gl's "hack a scale up to keep particles from
;           // disappearing": the billboard grows with distance so it
;           // stays wide enough to cover a pixel. c.w *is* that
;           // distance - it is the view depth along 'forward' - so the
;           // dot product the EE used to compute disappears here.
;           float scale = 1.0f + c.w * quadOff.w;
;
;           // Corner offset in NDC: the constant clip-space offset
;           // scaled and put through the same perspective divide. Its
;           // w is 0 by construction, so both corners keep c.w.
;           float k       = scale * q;
;           vec3  offset  = quadOff.xyz * k;
;
;           vec3 ndc0 = c.xyz * q;
;           vec3 ndc1 = ndc0 + offset;
;
;           // NDC to GS window coordinates, in 12.4 fixed point:
;           vec3 s0 = ftoi4(gsOffset.xyz + ndc0 * gsScale.xyz);
;           vec3 s1 = ftoi4(gsOffset.xyz + ndc1 * gsScale.xyz);
;
;           // Whole-billboard reject: if any of the centre's 6 clip
;           // flags is set, adc becomes 0x8000 - bit 15, the ADC bit -
;           // and the GS skips this sprite's drawing kick.
;           int adc = 0x7FFF + (clipFlagQueue.last() != 0 ? 1 : 0);
;
;           out[0].x    = p.x;    // native RGBAQ: packed colour...
;           out[0].y    = q;      // ...with Q in the word above
;           out[0].z    = 0x01;   // A+D destination: RGBAQ register
;           out[1]      = uv0;
;           out[2].xyz  = s0;
;           out[2].w    = adc;
;           out[3]      = uv1;
;           out[4].xyz  = s1;
;           out[4].w    = adc;
;
;           in  += 1;
;           out += 5;
;           numPrts -= 1;
;       }
;       while (numPrts != 0);
;
;       XGKICK(kick); // Send the finished GS packet.
;   }
#vuprog VU1Prog_Particles

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

    ; A+D destination address the per-particle color qwords carry in .z
    ; (0x01 = the RGBAQ register):
    iaddiu iRegRGBAQ, vi00, 1

    ; Current double buffer and this batch's constants:
    xtop   iBase
    ilw.w  iNumPrts,  kBatchHeader(iBase)
    lq     fQuadOff,  kQuadOffset(iBase)
    lq     fUV0,      kUV0(iBase)
    lq     fUV1,      kUV1(iBase)
    iaddiu iInPtr,    iBase, kParticleData

    ; Output (the GS packet) starts right after the input particles,
    ; which are one qword each:
    iadd   iKick, iInPtr, iNumPrts

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

    ; One particle per iteration:
    lParticleLoop:

        lq fP, 0(iInPtr)

        ; World to clip space. The packed color occupies .x, so the
        ; position sits in .yzw and the w term is a literal 1 from vf00.
        mul  acc, fMVP0, fP[y]
        madd acc, fMVP1, fP[z]
        madd acc, fMVP2, fP[w]
        madd fC,  fMVP3, vf00[w]

        ; Guard-band clip judgement on the billboard centre: scaled x/y,
        ; exact z, against |w|.
        mul.xyz   fJudge, fC, fClipScale
        clipw.xyz fJudge, fC[w]

        div q, vf00[w], fC[w]

        ; ref_gl's distance blow-up. fC.w is the view depth along the
        ; camera's forward axis - the very distance ref_gl dots out - so
        ; scale = 1 + rate * distance needs no dot product here.
        mul.w  fScale, fC,     fQuadOff[w]
        add.w  fScale, fScale, vf00[w]

        ; Fold the blow-up into the perspective divide, then take the
        ; constant clip-space corner offset through both at once. Its z
        ; is 0 (orthogonal to the view axis), so both corners keep the
        ; centre's depth as well as its w.
        mulq.w   fK,    fScale,   q
        mul.xyz  fOff,  fQuadOff, fK[w]

        ; The two opposite corners, in NDC:
        mulq.xyz fNdc0, fC,    q
        add.xyz  fNdc1, fNdc0, fOff

        ; NDC to GS window coordinates, in 12.4 fixed point:
        mula.xyz  acc, fGSOffset, vf00[w]
        madd.xyz  fS0, fNdc0, fGSScale
        ftoi4.xyz fS0, fS0

        mula.xyz  acc, fGSOffset, vf00[w]
        madd.xyz  fS1, fNdc1, fGSScale
        ftoi4.xyz fS1, fS1

        ; Q for the A+D RGBAQ qword. Unused while the batch samples
        ; through UV (PRIM's FST bit), but it must stay a finite float.
        addq.y fQ, vf00, q

        ; Judge the whole billboard from the centre's 6 clip flags: if
        ; any is set, 0x7FFF + flags reaches bit 15 (the ADC bit) and the
        ; GS skips this sprite's drawing kick. Written to both corners so
        ; the kicking vertex always carries it.
        fcand  vi01, 0x3F
        iaddiu iADC, vi01, 0x7FFF

        sq.x   fP,        0(iOutPtr)
        sq.y   fQ,        0(iOutPtr)
        isw.z  iRegRGBAQ, 0(iOutPtr)
        sq     fUV0,      1(iOutPtr)
        sq.xyz fS0,       2(iOutPtr)
        isw.w  iADC,      2(iOutPtr)
        sq     fUV1,      3(iOutPtr)
        sq.xyz fS1,       4(iOutPtr)
        isw.w  iADC,      4(iOutPtr)

        iaddiu iInPtr,   iInPtr,   1
        iaddiu iOutPtr,  iOutPtr,  5
        iaddi  iNumPrts, iNumPrts, -1
        ibne   iNumPrts, vi00, lParticleLoop

    --barrier

    xgkick iKick

#endvuprog
