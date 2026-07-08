
;--------------------------------------------------------------------
; color_triangles_clip_verts.vcl
;
; A VU1 microprogram to draw a batch of colored triangles.
; - Vertex format: RGBAQ | XYZ2
; - Writes the output in-place.
; - Performs clipping (per vertex only).
;--------------------------------------------------------------------

#include "src/ps2/vu1progs/vu_utils.inc"

; Data offsets in the VU memory (quadword units):
#define kMVPMatrix    0
#define kScaleFactors 4
#define kVertexCount  4
#define kGIFTag       5
#define kStartColor   6
#define kStartVert    7

#vuprog VU1Prog_Color_Triangles

    ; Clear the clip flag so we can use the CLIP instruction below:
    fcset 0

    ; Number of vertexes we need to process here:
    ; (W component of the quadword used by the scale factors)
    ilw.w iNumVerts, kVertexCount(vi00)

    ; Loop counter / vertex ptr:
    iaddiu iVert,    vi00, 0 ; Start vertex counter
    iaddiu iVertPtr, vi00, 0 ; Point to the first vertex (0=color-qword, 1=position-qword)

    ; Rasterizer scaling factors:
    lq fScales, kScaleFactors(vi00)

    ; Model View Projection matrix:
    MatrixLoad{ fMVPMatrix, kMVPMatrix, vi00 }

    ; Loop for each vertex in the batch:
    lVertexLoop:
        ; Load the vert (currently in object space and floating point format):
        lq fVert, kStartVert(iVertPtr)

        ; Transform the vertex by the MVP matrix:
        MatrixMultiplyVert{ fVert, fMVPMatrix, fVert }

        ; Clipping for the triangle being processed:
        clipw.xyz fVert, fVert
        fcand     vi01,  0x3FFFF
        iaddiu    iADC,  vi01, 0x7FFF

        ; Divide by W (perspective divide):
        div     q,     vf00[w], fVert[w]
        mul.xyz fVert, fVert,   q

        ; Apply scaling and convert to FP:
        VertToGSFormat{ fVert, fScales }

        ; Store:
        sq.xyz fVert, kStartVert(iVertPtr) ; Write the vertex back to VU memory
        isw.w  iADC,  kStartVert(iVertPtr) ; Write the ADC bit back to memory to clip the vert if outside the screen

        ; Increment the vertex counter and pointer and jump back to lVertexLoop if not done.
        iaddiu iVert,    iVert,     1 ; One vertex done
        iaddiu iVertPtr, iVertPtr,  2 ; Advance 2 Quadwords (color+position) per vertex
        ibne   iVert,    iNumVerts, lVertexLoop
    ; END lVertexLoop

    iaddiu iGIFTag, vi00, kGIFTag ; Load the position of the GIF tag
    xgkick iGIFTag                ; and tell the VU to send that to the GS

#endvuprog
