;--------------------------------------------------------------------
; particles.vcl
;
; A VU1 microprogram to draw a batch of camera-facing particle
; billboards. Built by oxide-vcl, whose preprocessor (-G) runs first;
; --vcl-boilerplate injects the VCL boilerplate (.init_*, --enter/--exit
; blocks).
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
;   +4   7 GIF tag qwords (set tag, MIPTBP1/TEX1/TEX0/ALPHA/ZBUF A+D, prim tag)
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

#include "vu_common.i"

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
;       // The GS scale, folded once a batch into everything the loop
;       // scales by it: the matrix's x, y and z columns, the corner offset,
;       // and - divided out again - the clip judgement's scale. Its x and y
;       // are 2048, a power of two, so on those axes the fold is exact:
;       mvp[0..3].xyz *= gsScale.xyz;
;       quadOff.xyz   *= gsScale.xyz;
;       vec3 judgeScale = clipScale.xyz / gsScale.xyz;
;
;       do // One particle per iteration:
;       {
;           vec4 p = in[0]; // (rgba, x, y, z); raw packed colour in .x
;
;           // World space to clip space, scaled to the GS on x, y and z. The
;           // position is in .yzw and the w term is a literal 1, so vf00[w]
;           // supplies it:
;           vec4 c = p.y * mvp[0] + p.z * mvp[1]
;                  + p.w * mvp[2] + 1.0f * mvp[3];
;
;           // Guard-band clip judgement on the centre; the caller-side
;           // equivalent of the triangle programs' per-vertex test. The
;           // scale undoes the fold: c.xyz * judgeScale is c's clip-space
;           // position times clipScale:
;           vec3 judge = c.xyz * judgeScale;
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
;           // Corner offset in GS units: the constant offset scaled and
;           // put through the same perspective divide. Its w is 0 by
;           // construction, so both corners keep c.w.
;           float k       = scale * q;
;           vec3  offset  = quadOff.xyz * k;
;
;           // The two opposite corners in GS window coordinates, 12.4 fixed
;           // point. With the GS scale in c already, the perspective divide
;           // and the move to the window are one multiply-add:
;           vec3 w0 = gsOffset.xyz + c.xyz * q;
;           vec3 s0 = ftoi4(w0);
;           vec3 s1 = ftoi4(w0 + offset);
;
;           // Whole-billboard reject: if any of the centre's 6 clip
;           // flags is set, adc becomes 0x8000 - bit 15, the ADC bit -
;           // and the GS skips this sprite's drawing kick.
;           int adc = 0x7FFF + (clipFlagQueue.last() != 0 ? 1 : 0);
;
;           out[0].x    = p.x;    // native RGBAQ: packed colour...
;           out[0].yz   = (q, 0x01); // ...with Q in the word above, and
;                                    // the A+D destination: RGBAQ register
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

    LoadFrameConstants{ }

    ; A+D destination address the per-particle color qwords carry in .z
    ; (0x01 = the RGBAQ register). It goes into fQ's .z once, as the integer
    ; it is - mfir copies the bits, no FMAC op touches them - and stays there:
    ; the loop writes only fQ's .y, so one sq.yz stores Q and the address.
    iaddiu iRegRGBAQ, vi00, 1
    mfir.z fQ, iRegRGBAQ

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

    ; The GS scale, folded into what the loop multiplies by it, once a batch:
    ; the matrix's x, y and z columns, so the transform lands in GS units
    ; (w, the depth the blow-up and the divide read, is untouched), and the
    ; corner offset. The clip judgement's scale is divided by it to undo the
    ; fold there. Its x and y are 2048, so on those axes both are exact; its z
    ; is the depth scale, where the result can differ in the last bit.
    ;
    ; Only the registers change: the frame constants in VU memory stay as the
    ; EE wrote them, for the triangle program.
    mul.xyz fMVP0,    fMVP0,    fGSScale
    mul.xyz fMVP1,    fMVP1,    fGSScale
    mul.xyz fMVP2,    fMVP2,    fGSScale
    mul.xyz fMVP3,    fMVP3,    fGSScale
    mul.xyz fQuadOff, fQuadOff, fGSScale

    div    q,       fClipScale[x], fGSScale[x]
    addq.x fJScale, vf00,          q
    div    q,       fClipScale[y], fGSScale[y]
    addq.y fJScale, vf00,          q
    div    q,       fClipScale[z], fGSScale[z]
    addq.z fJScale, vf00,          q

    CopyGifTags{ }

    ; One particle per iteration:
    lParticleLoop:

        lq fP, 0(iInPtr)

        ; World to clip space, in GS units on x, y and z (see the fold
        ; above). The packed color occupies .x, so the position sits in .yzw
        ; and the w term is a literal 1 from vf00.
        mul  acc, fMVP0, fP[y]
        madd acc, fMVP1, fP[z]
        madd acc, fMVP2, fP[w]
        madd fC,  fMVP3, vf00[w]

        ; Guard-band clip judgement on the billboard centre: scaled x/y,
        ; exact z, against |w|. fJScale takes the GS scale back out.
        mul.xyz   fJudge, fC, fJScale
        clipw.xyz fJudge, fC[w]

        div q, vf00[w], fC[w]

        ; ref_gl's distance blow-up. fC.w is the view depth along the
        ; camera's forward axis - the very distance ref_gl dots out - so
        ; scale = 1 + rate * distance needs no dot product here.
        mul.w  fScale, fC,     fQuadOff[w]
        add.w  fScale, fScale, vf00[w]

        ; Fold the blow-up into the perspective divide, then take the
        ; constant corner offset through both at once. Its z is 0
        ; (orthogonal to the view axis), so both corners keep the centre's
        ; depth as well as its w.
        mulq.w   fK,    fScale,   q
        mul.xyz  fOff,  fQuadOff, fK[w]

        ; The two opposite corners, in GS window coordinates, 12.4 fixed
        ; point. With the GS scale in fC already, the perspective divide and
        ; the move to the window are one multiply-add, and the far corner is
        ; the near one plus the offset.
        mulaw.xyz acc,  fGSOffset, vf00[w]
        maddq.xyz fW0,  fC,        q
        add.xyz   fW1,  fW0,       fOff
        ftoi4.xyz fS0,  fW0
        ftoi4.xyz fS1,  fW1

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
        sq.yz  fQ,        0(iOutPtr)
        sq     fUV0,      1(iOutPtr)
        sq.xyz fS0,       2(iOutPtr)
        isw.w  iADC,      2(iOutPtr)
        sq     fUV1,      3(iOutPtr)
        sq.xyz fS1,       4(iOutPtr)
        isw.w  iADC,      4(iOutPtr)

        iaddiu iInPtr,   iInPtr,   1
        iaddiu iOutPtr,  iOutPtr,  5
        iaddi  iNumPrts, iNumPrts, -1
        ibgtz  iNumPrts, lParticleLoop

    --barrier

    xgkick iKick

#endvuprog
