
;--------------------------------------------------------------------
; vu_utils.inc
;
; Miscellaneous helper macros to ease
; the task of writing VU1 programs
;
; The preprocessor syntax used is from VCLPP
; (https://github.com/glampert/vclpp)
;--------------------------------------------------------------------

; Load a 4x4 matrix into a named variable.
#macro MatrixLoad: matrix, offset, vumem
    lq matrix[0], offset+0(vumem)
    lq matrix[1], offset+1(vumem)
    lq matrix[2], offset+2(vumem)
    lq matrix[3], offset+3(vumem)
#endmacro

; Transform the vector by the given matrix.
#macro MatrixMultiplyVert: result, matrix, vertex
    mul  acc,    matrix[0], vertex[x]
    madd acc,    matrix[1], vertex[y]
    madd acc,    matrix[2], vertex[z]
    madd result, matrix[3], vertex[w]
#endmacro

; Apply scaling and convert to 12:4 GS fixed point format.
#macro VertToGSFormat: vertex, scales
    mula.xyz  acc,    scales, vf00[w]
    madd.xyz  vertex, vertex, scales
    ftoi4.xyz vertex, vertex
#endmacro

; Load a vertex, transform it, clip, perspective-divide, and covert to GS format.
#macro DoVertex: vertoffset, vertptr, vertex, scales, mvpmatrix
    lq vertex, vertoffset(vertptr)

    mul  acc,    mvpmatrix[0], vertex[x]
    madd acc,    mvpmatrix[1], vertex[y]
    madd acc,    mvpmatrix[2], vertex[z]
    madd vertex, mvpmatrix[3], vertex[w]

    clipw.xyz vertex, vertex
    div q, vf00[w], vertex[w]
    mul.xyz vertex, vertex, q

    mula.xyz  acc,    scales, vf00[w]
    madd.xyz  vertex, vertex, scales
    ftoi4.xyz vertex, vertex
#endmacro

