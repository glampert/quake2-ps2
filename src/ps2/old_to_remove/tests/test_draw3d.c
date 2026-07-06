
/* ================================================================================================
 * -*- C -*-
 * File: test_draw3d.c
 * Author: Guilherme R. Lampert
 * Created on: 19/01/16
 * Brief: Tests for the 3D drawing functions of the Refresh module and the Vector Unit 1.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include "client/client.h"
#include "ps2/ref_ps2.h"
#include "ps2/mem_alloc.h"
#include "ps2/math_funcs.h"
#include "ps2/vec_mat.h"

#include "ps2/vu1.h"
#include "ps2/gs_defs.h"

#include "ps2/dma_mgr.h"
#include "ps2/vu_prog_mgr.h"

// Functions exported from this file:
void Test_PS2_VU1Triangle(void);
void Test_PS2_VU1Cubes(void);

//=============================================================================
//
// Local shared data / helpers:
//
//=============================================================================

// The VU1 program generated a .VCL source file:
DECLARE_VU_MICROPROGRAM(VU1Prog_Color_Triangles);

// GS vertex format for our triangles (reglist):
static const u64 VERTEX_FORMAT = (((u64)GS_REG_RGBAQ) << 0) | (((u64)GS_REG_XYZ2) << 4);

// Number of elements in a vertex; (color + position) in our case:
static const int NUM_VERTEX_ELEMENTS = 2;

// Simple struct to hold the temporary DMA buffer we need
// to send per draw list info to the Vector Unit 1.
typedef struct
{
    // We send one matrix and 1 quadword (vec4) for each draw list.
    byte buffer[sizeof(m_mat4_t) + sizeof(m_vec4_t)] PS2_ALIGN(16);
    byte * ptr;
} draw_data_t;

// ---------------------
//  draw_data_t helpers
// ---------------------

static void DrawDataReset(draw_data_t * dd)
{
    dd->ptr = dd->buffer;
}

static int DrawDataGetQWordSize(const draw_data_t * dd)
{
    return (dd->ptr - dd->buffer) >> 4;
}

static void DrawDataAddMatrix(draw_data_t * dd, const m_mat4_t * m)
{
    // Copy the matrix as-is:
    memcpy(dd->ptr, m, sizeof(*m));
    dd->ptr += sizeof(*m);
}

static void DrawDataAddScaleFactorsAndVertCount(draw_data_t * dd, u32 vert_count)
{
    // GS rasterizer scale factors follow the MVP matrix:
    float * scale_vec = (float *)dd->ptr;
    scale_vec[0] = 2048.0f;
    scale_vec[1] = 2048.0f;
    scale_vec[2] = ((float)0xFFFFFF) / 32.0f;
    dd->ptr += sizeof(float) * 3;

    // And lastly the vertex count in the W component of the quadword.
    u32 * count = (u32 *)dd->ptr;
    *count = vert_count;
    dd->ptr += sizeof(u32);
}

static int CountVertexLoops(float vertex_qwords, float num_regs)
{
    const float loops_per_qw = 2.0f / num_regs;
    return (int)(vertex_qwords * loops_per_qw);
}

//=============================================================================
//
// Test_PS2_VU1Triangle -- Draws a rotating triangle using the
// Vector Unit 1 (VU1) hardware accelerated rendering path.
//
//=============================================================================

static void DrawVU1Triangle(draw_data_t * dd, const m_mat4_t * mvp)
{
    DrawDataReset(dd);
    DrawDataAddMatrix(dd, mvp);
    DrawDataAddScaleFactorsAndVertCount(dd, 3); // 1 triangle = 3 verts

    VU1_Begin();

    // Matrix/scales will be uploaded at address 0 in VU memory.
    const int draw_data_qwsize = DrawDataGetQWordSize(dd);
    VU1_ListData(0, dd->buffer, draw_data_qwsize);

    // Data added from here will begin at address 6, right after the MVP, fScales and vert count:
    VU1_ListAddBegin(draw_data_qwsize);

    // The GIF Tag and primitive description:
    const int vert_loops = CountVertexLoops(/* vertex_qwords = */ 6, NUM_VERTEX_ELEMENTS);
    const u64 prim_info  = GS_PRIM(GS_PRIM_TRIANGLE, GS_PRIM_SFLAT, GS_PRIM_TOFF, GS_PRIM_FOFF, GS_PRIM_ABOFF, GS_PRIM_AAON, GS_PRIM_FSTQ, GS_PRIM_C1, 0);
    const u64 gif_tag    = GS_GIFTAG(vert_loops, 1, 1, prim_info, GS_GIFTAG_PACKED, NUM_VERTEX_ELEMENTS);
    VU1_ListAdd128(gif_tag, VERTEX_FORMAT);

    //
    // Vertex format:
    // Color (RGBA bytes)
    // Position (X,Y,Z,W floats)
    //

    // Vertex 0:
    VU1_ListAdd32(50);
    VU1_ListAdd32(50);
    VU1_ListAdd32(127);
    VU1_ListAdd32(127);
    VU1_ListAddFloat(-1.0f);
    VU1_ListAddFloat(1.0f);
    VU1_ListAddFloat(3.0f);
    VU1_ListAddFloat(1.0f);

    // Vertex 1:
    VU1_ListAdd32(50);
    VU1_ListAdd32(50);
    VU1_ListAdd32(127);
    VU1_ListAdd32(127);
    VU1_ListAddFloat(-1.0f);
    VU1_ListAddFloat(-1.0f);
    VU1_ListAddFloat(3.0f);
    VU1_ListAddFloat(1.0f);

    // Vertex 2:
    VU1_ListAdd32(50);
    VU1_ListAdd32(50);
    VU1_ListAdd32(127);
    VU1_ListAdd32(127);
    VU1_ListAddFloat(0.5f);
    VU1_ListAddFloat(0.5f);
    VU1_ListAddFloat(3.0f);
    VU1_ListAddFloat(1.0f);

    // 13 qwords total so far (verts + matrix/scales draw data)
    VU1_ListAddEnd();

    // End the list and start the VU program (located in micromem address 0)
    VU1_End(0);
}

#define XYZ2 ((u64)0x05)
#define XYZ2_SET(X, Y, Z) ((PS2_AS_U128(Z) << 32) | (PS2_AS_U128(Y) << 16) | (PS2_AS_U128(X) << 0))

#define TEST_1 ((u64)0x47)
#define TEST_SET(ATE, ATST, AREF, AFAIL, DATE, DATM, ZTE, ZTST) (((u64)(ZTST) << 17) | ((u64)(ZTE) << 16) | ((u64)(DATM) << 15) | ((u64)(DATE) << 14) | ((u64)(AFAIL) << 12) | ((u64)(AREF) << 4) | ((u64)(ATST) << 1) | ((u64)(ATE) << 0))

#define RGBAQ ((u64)0x01)
#define RGBAQ_SET(R, G, B, A, Q) (((u64)(Q) << 32) | ((u64)(A) << 24) | ((u64)(B) << 16) | ((u64)(G) << 8) | ((u64)(R) << 0))

#define PRMODE ((u64)0x1B)
#define PRMODE_SET(IIP, TME, FGE, ABE, AA1, FST, CTXT, FIX) ((u64)((FIX << 10) | (CTXT << 9) | (FST << 8) | (AA1 << 7) | (ABE << 6) | (FGE << 5) | (TME << 4) | (IIP << 3)))

#define PRIM ((u64)0x00)
#define PRIM_SET(PRIM, IIP, TME, FGE, ABE, AA1, FST, CTXT, FIX) (PRMODE_SET(IIP, TME, FGE, ABE, AA1, FST, CTXT, FIX) | PRIM)

static int InitScreenClear(ps2_vif_static_dma_t * vif_static_dma, int R, int G, int B)
{
    int x0 = (2048 - (viddef.width >> 1)) << 4;
    int y0 = (2048 - (viddef.height >> 1)) << 4;
    int x1 = (2048 + (viddef.width >> 1)) << 4;
    int y1 = (2048 + (viddef.height >> 1)) << 4;

    // Get the address of the screen clear packet so we can CALL it
    // from the dynamic packet.
    const u32 screen_clear_dma = VIFDMA_GetPointer(vif_static_dma);

    // Start the VIF direct mode.
    VIFDMA_StartDirect(vif_static_dma);

    VIFDMA_AddU128(vif_static_dma, PS2_GS_GIFTAG_BATCH(4 + (20 * 2), 1, 0, 0,
                                                       PS2_GIFTAG_PACKED, PS2_GS_BATCH_1(PS2_GIFTAG_AD)));

    VIFDMA_AddU64(vif_static_dma, TEST_SET(0, 0, 0, 0, 0, 0, 1, 1));
    VIFDMA_AddU64(vif_static_dma, TEST_1);

    VIFDMA_AddU64(vif_static_dma, PRIM_SET(0x6, 0, 0, 0, 0, 0, 0, 0, 0));
    VIFDMA_AddU64(vif_static_dma, PRIM);

    VIFDMA_AddU64(vif_static_dma, RGBAQ_SET(R, G, B, 0x80, 0x3f800000));
    VIFDMA_AddU64(vif_static_dma, RGBAQ);

    int i;
    for (i = 0; i < 20; i++)
    {
        VIFDMA_AddU64(vif_static_dma, XYZ2_SET(x0, y0, 0));
        VIFDMA_AddU64(vif_static_dma, XYZ2);

        VIFDMA_AddU64(vif_static_dma, XYZ2_SET(x0 + (32 << 4), y1, 0));
        VIFDMA_AddU64(vif_static_dma, XYZ2);

        x0 += (32 << 4);
    }

    VIFDMA_AddU64(vif_static_dma, TEST_SET(0, 0, 0, 0, 0, 0, 1, 3));
    VIFDMA_AddU64(vif_static_dma, TEST_1);

    VIFDMA_EndDirect(vif_static_dma);
    VIFDMA_DMARet(vif_static_dma);

    return screen_clear_dma;
}

static u32 SetUpTriangleDMA(ps2_vif_static_dma_t * vif_static_dma)
{
    // Get the address of static data that we can call to later.
    u32 iStaticAddr = VIFDMA_GetPointer(vif_static_dma);

    // Drawing 2 triangles.
    int iTriangles = 2;
    // There are three vertices per triangle
    int iVerts = iTriangles * 3;

    FU32_t i2f;
    i2f.asI32 = iVerts;

    // We want to unpack 2 quad words of misc data, and then 2QW per vertex starting at
    // VU mem location 4 (we will upload the MVP matrix to the first 4QW).
    // Unpack at location 4
    // Each vertex is 2 qwords in size
    VIFDMA_AddUnpack(vif_static_dma, VIF_V4_32, 4, 2 + (iVerts * 2));
    VIFDMA_AddVector4F(vif_static_dma, 2048.0f, 2048.0f, ((float)0xFFFFFF) / 32.0f, i2f.asFloat);

    // Add the GIFTag
    VIFDMA_AddU128(vif_static_dma, PS2_GS_GIFTAG_BATCH(iVerts,               // NLOOP
                                                       1,                    // EOP
                                                       1,                    // PRE
                                                       PS2_GS_PRIM(PS2_PRIM_TRIANGLE, // PRIM
                                                                   PS2_PRIM_IIP_GOURAUD,
                                                                   PS2_PRIM_TME_OFF,
                                                                   PS2_PRIM_FGE_OFF,
                                                                   PS2_PRIM_ABE_OFF,
                                                                   PS2_PRIM_AA1_OFF,
                                                                   PS2_PRIM_FST_UV,
                                                                   PS2_PRIM_CTXT_CONTEXT1,
                                                                   PS2_PRIM_FIX_NOFIXDDA),
                                                       PS2_GIFTAG_PACKED,               // FLG
                                                       PS2_GS_BATCH_2(PS2_GIFTAG_RGBAQ, // BATCH
                                                                      PS2_GIFTAG_XYZ2)));

    // Add the quad
    VIFDMA_AddU128(vif_static_dma, PS2_PACKED_RGBA(0x80, 0x00, 0x00, 0x80));  // Colour
    VIFDMA_AddVector4F(vif_static_dma, -3.0f, 3.0f, 3.0f, 1.0f);  // Vert

    VIFDMA_AddU128(vif_static_dma, PS2_PACKED_RGBA(0x80, 0x00, 0x00, 0x80));  // Colour
    VIFDMA_AddVector4F(vif_static_dma, 3.0f, 3.0f, 3.0f, 1.0f);   // Vert

    VIFDMA_AddU128(vif_static_dma, PS2_PACKED_RGBA(0x80, 0x00, 0x00, 0x80));  // Colour
    VIFDMA_AddVector4F(vif_static_dma, -3.0f, -3.0f, 3.0f, 1.0f); // Vert

    VIFDMA_AddU128(vif_static_dma, PS2_PACKED_RGBA(0x00, 0x00, 0x80, 0x80));  // Colour
    VIFDMA_AddVector4F(vif_static_dma, -3.0f, -3.0f, 3.0f, 1.0f); // Vert

    VIFDMA_AddU128(vif_static_dma, PS2_PACKED_RGBA(0x00, 0x00, 0x80, 0x80));  // Colour
    VIFDMA_AddVector4F(vif_static_dma, 3.0f, 3.0f, 3.0f, 1.0f);   // Vert

    VIFDMA_AddU128(vif_static_dma, PS2_PACKED_RGBA(0x00, 0x00, 0x80, 0x80));  // Colour
    VIFDMA_AddVector4F(vif_static_dma, 3.0f, -3.0f, 3.0f, 1.0f);  // Vert

    // Flush to make sure VU1 isn't doing anything.
    VIFDMA_AddU32(vif_static_dma, VIF_FLUSH);

    // Then run the microcode.
    VIFDMA_AddU32(vif_static_dma, VIF_MSCALL(0));

    // Return back to the dynamic chain (note that we aren't aligned on a QWORD boundary, but
    // it doesn't matter because the packet class will sort that out for us).
    VIFDMA_DMARet(vif_static_dma);

    return iStaticAddr;
}

void Test_PS2_VU1Triangle(void)
{
    Com_Printf("====== QPS2 - Test_PS2_VU1Triangle ======\n");

    // The scene viewer (not moving):
    const m_vec4_t camera_origin = { 0.0,  0.0f, -1.0f,  1.0f };
    const m_vec4_t camera_lookat = { 0.0,  0.0f,  1.0f,  1.0f };
    const m_vec4_t camera_up     = { 0.0,  1.0f,  0.0f,  1.0f };

    // Render matrices:
    m_mat4_t proj_matrix;
    m_mat4_t view_matrix;
    m_mat4_t view_proj_matrix;
    m_mat4_t mvp_matrix;
    m_mat4_t translation_matrix;
    m_mat4_t rotation_matrix;
    m_mat4_t model_matrix;

    // Clear the screen to dark gray. Default color is black.
    PS2_SetClearColor(120, 120, 120);

    // Upload the VU1 microprogram:
//    VU1_UploadProg(0, &VU1Prog_Color_Triangles_CodeStart, &VU1Prog_Color_Triangles_CodeEnd);

    ps2_vu_prog_manager_t vu_prog_mgr;
    ps2_vif_dynamic_dma_t vif_dynamic_dma;
    ps2_vif_static_dma_t  vif_static_dma;

    VIFDMA_Initialize(&vif_dynamic_dma, /* pages = */ 8, VIF_DYNAMIC_DMA);
    VIFDMA_Initialize(&vif_static_dma,  /* pages = */ 8, VIF_STATIC_DMA);

    VU_ProgManagerInit(&vu_prog_mgr);
    VU_InitMicroprogram(&vif_static_dma, &VU1Prog_Color_Triangles, VU1_MICROPROGRAM, /* offset = */ 0);
    VU_UploadMicroprogram(&vu_prog_mgr, &vif_dynamic_dma, &VU1Prog_Color_Triangles,  /* index  = */ 0, /* force = */ true);
    VIFDMA_Fire(&vif_dynamic_dma);

    // Set up the matrices; these never change in this test program:
    Mat4_MakeLookAt(&view_matrix, &camera_origin, &camera_lookat, &camera_up);
    Mat4_MakePerspProjection(&proj_matrix, ps2_deg_to_rad(60.0f), 4.0f / 3.0f, viddef.width, viddef.height, 2.0f, 2000.0f, 4096.0f);
    Mat4_Multiply(&view_proj_matrix, &view_matrix, &proj_matrix);

    // Temp buffer for DMA upload of the renderer matrix, etc.
    draw_data_t dd = {0};

    // Copy the geometry into a static DMA buffer that we'll resubmit repeated times.
    const u32 triangle_dma_addr = SetUpTriangleDMA(&vif_static_dma);
    const u32 screen_clear_dma_addr = InitScreenClear(&vif_static_dma, 120, 120, 120);

    // Rotate the model, so it looks a little less boring.
    float rotation_angle = 0.0f; // In radians

    for (;;)
    {
        PS2_BeginFrame(0);

        // TODO WORK IN PROGRESS
        /*
        Mat4_MakeRotationZ(&rotation_matrix, rotation_angle);
        Mat4_MakeTranslation(&translation_matrix, 1.0f, 0.0f, 0.0f);
        Mat4_Multiply(&model_matrix, &rotation_matrix, &translation_matrix);
        Mat4_Multiply(&mvp_matrix, &model_matrix, &view_proj_matrix);

        //VIFDMA_DMACall(&vif_dynamic_dma, screen_clear_dma_addr);

        VIFDMA_AddUnpack(&vif_dynamic_dma, VIF_V4_32, 0, 4); // Unpack 4QW to VU memory addr 0
        VIFDMA_AddMatrix4F(&vif_dynamic_dma, (float *)&mvp_matrix);

        VIFDMA_DMACall(&vif_dynamic_dma, triangle_dma_addr);

        VIFDMA_Fire(&vif_dynamic_dma);

        //#define D1_CHCR *((volatile u32 *)(0x10009000))
        //while (D1_CHCR & 256) { }
        //*/

        //*
        Mat4_MakeRotationZ(&rotation_matrix, rotation_angle);
        Mat4_MakeTranslation(&translation_matrix, 1.0f, 0.0f, 0.0f);
        Mat4_Multiply(&model_matrix, &rotation_matrix, &translation_matrix);
        Mat4_Multiply(&mvp_matrix, &model_matrix, &view_proj_matrix);

        DrawVU1Triangle(&dd, &mvp_matrix);
        //*/

        rotation_angle += 0.02f;
        PS2_EndFrame();
    }
}

//=============================================================================
//
// Test_PS2_VU1Cubes -- Draws a couple rotating cubes using
// Vector Unit 1 (VU1) hardware accelerated rendering path.
//
//=============================================================================

static const int CUBE_VERT_COUNT  = 24;
static const int CUBE_INDEX_COUNT = 36;
typedef struct
{
    m_vec4_t * vertexes;
    u16      * indexes;
} cube_t;

static void MakeCubeGeometry(cube_t * cube, const float scale)
{
    // Face indexes:
    const u16 cubeF[6][4] = {
        { 0, 1, 5, 4 },
        { 4, 5, 6, 7 },
        { 7, 6, 2, 3 },
        { 1, 0, 3, 2 },
        { 1, 2, 6, 5 },
        { 0, 4, 7, 3 }
    };

    // Positions/vertexes:
    const float cubeV[8][3] = {
        { -0.5f, -0.5f, -0.5f },
        { -0.5f, -0.5f,  0.5f },
        {  0.5f, -0.5f,  0.5f },
        {  0.5f, -0.5f, -0.5f },
        { -0.5f,  0.5f, -0.5f },
        { -0.5f,  0.5f,  0.5f },
        {  0.5f,  0.5f,  0.5f },
        {  0.5f,  0.5f, -0.5f }
    };

    cube->vertexes = (m_vec4_t *)PS2_MemAllocAligned(16, CUBE_VERT_COUNT  * sizeof(m_vec4_t), MEMTAG_MISC);
    cube->indexes  = (u16      *)PS2_MemAllocAligned(16, CUBE_INDEX_COUNT * sizeof(u16),      MEMTAG_MISC);

    // Fill in the data:
    m_vec4_t * vertex_ptr = cube->vertexes;
    u16      * face_ptr   = cube->indexes;

    // 'i' iterates over the faces, 2 triangles per face:
    int i, j;
    u16 vert_index = 0;
    for (i = 0; i < 6; ++i, vert_index += 4)
    {
        for (j = 0; j < 4; ++j)
        {
            vertex_ptr->x = cubeV[cubeF[i][j]][0] * scale;
            vertex_ptr->y = cubeV[cubeF[i][j]][1] * scale;
            vertex_ptr->z = cubeV[cubeF[i][j]][2] * scale;
            vertex_ptr->w = 1.0f;
            ++vertex_ptr;
        }

        face_ptr[0] = vert_index;
        face_ptr[1] = vert_index + 1;
        face_ptr[2] = vert_index + 2;
        face_ptr += 3;
        face_ptr[0] = vert_index + 2;
        face_ptr[1] = vert_index + 3;
        face_ptr[2] = vert_index;
        face_ptr += 3;
    }
}

static void DrawVU1Cube(draw_data_t * dd, const m_mat4_t * mvp, const cube_t * cube)
{
    DrawDataReset(dd);
    DrawDataAddMatrix(dd, mvp);
    DrawDataAddScaleFactorsAndVertCount(dd, CUBE_INDEX_COUNT); // 1 vertex per index

    VU1_Begin();

    // Matrix/scales will be uploaded at address 0 in VU memory.
    const int draw_data_qwsize = DrawDataGetQWordSize(dd);
    VU1_ListData(0, dd->buffer, draw_data_qwsize);

    // Data added from here will begin at address 6, right after the MVP, fScales and vert count:
    VU1_ListAddBegin(draw_data_qwsize);

    // The GIF Tag and primitive description:
    const int vertex_qws = CUBE_INDEX_COUNT * 2; // 2 qwords per vertex (color + position)
    const int vert_loops = CountVertexLoops(vertex_qws, NUM_VERTEX_ELEMENTS);
    const u64 prim_info  = GS_PRIM(GS_PRIM_TRIANGLE, GS_PRIM_SFLAT, GS_PRIM_TOFF, GS_PRIM_FOFF, GS_PRIM_ABOFF, GS_PRIM_AAON, GS_PRIM_FSTQ, GS_PRIM_C1, 0);
    const u64 gif_tag    = GS_GIFTAG(vert_loops, 1, 1, prim_info, GS_GIFTAG_PACKED, NUM_VERTEX_ELEMENTS);
    VU1_ListAdd128(gif_tag, VERTEX_FORMAT);

    // NOTE: alpha cannot be > 0x80 (128)!
    const byte colors[4][4] = {
        { 0x80, 0x00, 0x00, 0x80 }, // red
        { 0x00, 0x80, 0x00, 0x80 }, // green
        { 0x00, 0x00, 0x80, 0x80 }, // blue
        { 0x80, 0x80, 0x00, 0x80 }  // yellow
    };

    // Push one vertex per cube index:
    int i, j, c;
    for (i = 0, j = 0, c = 0; i < CUBE_INDEX_COUNT; ++i, ++j)
    {
        const m_vec4_t * vert = &cube->vertexes[cube->indexes[i]];

        VU1_ListAdd32(colors[c][0]);
        VU1_ListAdd32(colors[c][1]);
        VU1_ListAdd32(colors[c][2]);
        VU1_ListAdd32(colors[c][3]);

        // Each triangle will get a different color.
        if (j == 3)
        {
            c = (c + 1) & 3;
            j = 0;
        }

        VU1_ListAddFloat(vert->x);
        VU1_ListAddFloat(vert->y);
        VU1_ListAddFloat(vert->z);
        VU1_ListAddFloat(vert->w);
    }
    // We have (6 + 36*2 = 78) qwords of draw data for a cube

    VU1_ListAddEnd();

    // End the list and start the VU program (located in micromem address 0)
    VU1_End(0);

    // Since we are going to draw multiple cubes, we need to synchronize before
    // writing to the same VU1 memory address. Waiting for the GS to finish draw does the job.
    PS2_WaitGSDrawFinish();
}

static u32 SetUpCubeDMA(const cube_t * cube, ps2_vif_static_dma_t * vif_static_dma)
{
    const u32 static_addr = VIFDMA_GetPointer(vif_static_dma);
    const int num_verts   = CUBE_INDEX_COUNT;

    FU32_t i2f;
    i2f.asI32 = num_verts;

    //
    // We want to unpack 2 quadwords of misc data, and then 2QWs per vertex starting
    // as VU mem location 4 (we will upload the MVP matrix to the first 4QWs).
    //
    // Unpack at location 4
    // Each vertex is 2 qwords in size
    //
    VIFDMA_AddUnpack(vif_static_dma, VIF_V4_32, 4, 2 + (num_verts * 2));
    VIFDMA_AddVector4F(vif_static_dma, 2048.0f, 2048.0f, ((float)0xFFFFFF) / 32.0f, i2f.asFloat);

    //
    // Add the GIF Tag:
    //
    VIFDMA_AddU128(vif_static_dma, PS2_GS_GIFTAG_BATCH(num_verts,                     // NLOOP
                                                       1,                             // EOP
                                                       1,                             // PRE
                                                       PS2_GS_PRIM(PS2_PRIM_TRIANGLE, // PRIM
                                                                   PS2_PRIM_IIP_GOURAUD,
                                                                   PS2_PRIM_TME_OFF,
                                                                   PS2_PRIM_FGE_OFF,
                                                                   PS2_PRIM_ABE_OFF,
                                                                   PS2_PRIM_AA1_ON,
                                                                   PS2_PRIM_FST_STQ,
                                                                   PS2_PRIM_CTXT_CONTEXT1,
                                                                   PS2_PRIM_FIX_NOFIXDDA),
                                                       PS2_GIFTAG_PACKED,
                                                       PS2_GS_BATCH_2(PS2_GIFTAG_RGBAQ, PS2_GIFTAG_XYZ2)));

    //
    // Add the cube data to the static buffer:
    //
    const byte colors[4][4] = {
        { 0x80, 0x00, 0x00, 0x80 }, // red
        { 0x00, 0x80, 0x00, 0x80 }, // green
        { 0x00, 0x00, 0x80, 0x80 }, // blue
        { 0x80, 0x80, 0x00, 0x80 }  // yellow
    };

    // Push one vertex per cube index:
    int i, j, c;
    for (i = 0, j = 0, c = 0; i < num_verts; ++i, ++j)
    {
        const m_vec4_t * vert = &cube->vertexes[cube->indexes[i]];

        VIFDMA_AddVector4I(vif_static_dma, colors[c][0], colors[c][1], colors[c][2], colors[c][3]);
        VIFDMA_AddVector4F(vif_static_dma, vert->x, vert->y, vert->z, vert->w);

        // Each triangle will get a different color.
        if (j == 3)
        {
            c = (c + 1) & 3;
            j = 0;
        }
    }

    // Flush to make sure VU1 isn't doing anything.
    VIFDMA_AddU32(vif_static_dma, VIF_FLUSH);

    // Then run the microcode at addr 0.
    VIFDMA_AddU32(vif_static_dma, VIF_MSCALL(0));

    // Return back to the dynamic chain.
    VIFDMA_DMARet(vif_static_dma);

    // Address that we can resubmit for many cubes.
    return static_addr;
}

void Test_PS2_VU1Cubes(void)
{
    Com_Printf("====== QPS2 - Test_PS2_VU1Cubes ======\n");

    // The scene viewer (not moving):
    const m_vec4_t camera_origin = { 0.0,  0.0f, -1.0f,  1.0f };
    const m_vec4_t camera_lookat = { 0.0,  0.0f,  1.0f,  1.0f };
    const m_vec4_t camera_up     = { 0.0,  1.0f,  0.0f,  1.0f };

    // Render matrices:
    m_mat4_t proj_matrix;
    m_mat4_t view_matrix;
    m_mat4_t view_proj_matrix;
    m_mat4_t mvp_matrix;
    m_mat4_t translation_matrix;
    m_mat4_t rotation_matrix;
    m_mat4_t model_matrix;

    // Clear the screen to dark gray. Default color is black.
    PS2_SetClearColor(120, 120, 120);

    // Upload the VU1 microprogram:
//    VU1_UploadProg(0, &VU1Prog_Color_Triangles_CodeStart, &VU1Prog_Color_Triangles_CodeEnd);

    ps2_vu_prog_manager_t vu_prog_mgr;
    ps2_vif_dynamic_dma_t vif_dynamic_dma;
    ps2_vif_static_dma_t  vif_static_dma;

    VIFDMA_Initialize(&vif_dynamic_dma, /* pages = */ 8, VIF_DYNAMIC_DMA);
    VIFDMA_Initialize(&vif_static_dma,  /* pages = */ 8, VIF_STATIC_DMA);

    VU_ProgManagerInit(&vu_prog_mgr);
    VU_InitMicroprogram(&vif_static_dma, &VU1Prog_Color_Triangles, VU1_MICROPROGRAM, /* offset = */ 0);
    VU_UploadMicroprogram(&vu_prog_mgr, &vif_dynamic_dma, &VU1Prog_Color_Triangles,  /* index  = */ 0, /* force = */ true);
    VIFDMA_Fire(&vif_dynamic_dma);

    // Set up the matrices; these never change in this test program:
    Mat4_MakeLookAt(&view_matrix, &camera_origin, &camera_lookat, &camera_up);
    Mat4_MakePerspProjection(&proj_matrix, ps2_deg_to_rad(60.0f), 4.0f / 3.0f, viddef.width, viddef.height, 2.0f, 2000.0f, 4096.0f);
    Mat4_Multiply(&view_proj_matrix, &view_matrix, &proj_matrix);

    // Rotate the model, so it looks a little less boring.
    float rotation_angle = 0.0f; // In radians

    // Create the geometry for a cube made of triangles:
    cube_t cube = {0};
    MakeCubeGeometry(&cube, 1.0f);

    // Copy the geometry into a static DMA buffer that we'll resubmit repeated times.
    const u32 cube_dma_addr = SetUpCubeDMA(&cube, &vif_static_dma);

    // Each cube has its own draw_data_t.
    draw_data_t dd0 = {0};
    draw_data_t dd1 = {0};
    draw_data_t dd2 = {0};

    for (;;)
    {
        PS2_BeginFrame(0);

        // TODO WORK IN PROGRESS
        /*
        {
            Mat4_MakeRotationX(&rotation_matrix, rotation_angle);
            Mat4_MakeTranslation(&translation_matrix, 1.5f, -1.0f, 4.0f);

            Mat4_Multiply(&model_matrix, &rotation_matrix, &translation_matrix);
            Mat4_Multiply(&mvp_matrix, &model_matrix, &view_proj_matrix);

            VIFDMA_AddUnpack(&vif_dynamic_dma, VIF_V4_32, 0, 4); // Unpack 4QW to VU memory addr 0

            //VIFDMA_AddMatrix4F(&vif_dynamic_dma, (float *)&mvp_matrix);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[0][0]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[0][1]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[0][2]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[0][3]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[1][0]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[1][1]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[1][2]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[1][3]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[2][0]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[2][1]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[2][2]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[2][3]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[3][0]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[3][1]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[3][2]);
            VIFDMA_AddFloat(&vif_dynamic_dma, mvp_matrix.m[3][3]);

            // We can simply call the rest, which means the CPU doesn't have to do any work.
            VIFDMA_DMACall(&vif_dynamic_dma, cube_dma_addr);

            VIFDMA_Fire(&vif_dynamic_dma);
        }
        */

        //*
        Mat4_MakeRotationX(&rotation_matrix, rotation_angle);
        Mat4_MakeTranslation(&translation_matrix, 1.5f, -1.0f, 4.0f);

        Mat4_Multiply(&model_matrix, &rotation_matrix, &translation_matrix);
        Mat4_Multiply(&mvp_matrix, &model_matrix, &view_proj_matrix);

        DrawVU1Cube(&dd0, &mvp_matrix, &cube);

        Mat4_MakeRotationY(&rotation_matrix, rotation_angle);
        Mat4_MakeTranslation(&translation_matrix, -1.0f, -1.0f, 4.0f);

        Mat4_Multiply(&model_matrix, &rotation_matrix, &translation_matrix);
        Mat4_Multiply(&mvp_matrix, &model_matrix, &view_proj_matrix);

        DrawVU1Cube(&dd1, &mvp_matrix, &cube);

        Mat4_MakeRotationZ(&rotation_matrix, rotation_angle);
        Mat4_MakeTranslation(&translation_matrix, 0.0f, 0.5f, 4.0f);

        Mat4_Multiply(&model_matrix, &rotation_matrix, &translation_matrix);
        Mat4_Multiply(&mvp_matrix, &model_matrix, &view_proj_matrix);

        DrawVU1Cube(&dd2, &mvp_matrix, &cube);
        //*/

        rotation_angle += 0.02f;
        PS2_EndFrame();
    }
}
