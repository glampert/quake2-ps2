
/* ================================================================================================
 * -*- C -*-
 * File: view_draw.c
 * Author: Guilherme R. Lampert
 * Created on: 23/01/16
 * Brief: 3D view drawing for Refresh::RenderFrame().
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include "common/q_common.h"
#include "ps2/ref_ps2.h"
#include "ps2/mem_alloc.h"
#include "ps2/model_load.h"
#include "ps2/math_funcs.h"
#include "ps2/vec_mat.h"
#include "ps2/vu1.h"
#include "ps2/gs_defs.h"

//
// ---------------------------
// NOTES ON THE VIEW RENDERING
// ---------------------------
//
// This file contains the entry points for the 3D view rendering of the game.
// 2D overlay drawing is not done here (check ref_ps2.c).
//
// PS2_RenderFrame() / refexport_t::RenderFrame() will start a view rendering
// form the client code. RenderFrame will then forward to this file, drawing
// first the static world geometry and then entities (MD2/sprites) and brush
// models (scene props).
//

//=============================================================================
//
// Local view_draw data:
//
//=============================================================================

// Frame counters used to mark drawable surfaces:
int ps2_vis_frame_count   = 0; // Bumped when going to a new PVS
int ps2_frame_count       = 0; // Used for dlight push checking

// Quake 2 viewcluster stuff:
// PS2_BeginRegistration() has to reset them to -1.
int ps2_view_cluster      = -1;
int ps2_view_cluster2     = -1;
int ps2_old_view_cluster  = -1;
int ps2_old_view_cluster2 = -1;

// Scene viewer/camera:
static m_vec4_t ps2_camera_origin;
static m_vec4_t ps2_camera_lookat;
static m_vec4_t ps2_forward_vec;
static m_vec4_t ps2_right_vec;
static m_vec4_t ps2_up_vec;

// Render matrices:
static m_mat4_t ps2_model_to_world_matrix;
static m_mat4_t ps2_proj_matrix;
static m_mat4_t ps2_view_matrix;
static m_mat4_t ps2_view_proj_matrix;
static m_mat4_t ps2_mvp_matrix;

// View frustum for the frame, so we can cull bounding boxes out of view.
static cplane_t ps2_frustum[4];

// Buffer to decompress a cluster PVS.
// Alignment not strictly necessary, but might help the compiler since PS2 likes aligned data.
static byte ps2_dvis_pvs[MAX_MAP_LEAFS / 8] PS2_ALIGN(16);

// Color table used for debug coloring of surfaces.
static const int NUM_DEBUG_COLORS = 25;
static const byte ps2_debug_color_table[25][4] = {
    //  R     G     B     A
    {   0,    0,  255,  128 }, // Blue
    { 165,   42,   42,  128 }, // Brown
    { 127,   31,    0,  128 }, // Copper
    {   0,  255,  255,  128 }, // Cyan
    {   0,    0,  139,  128 }, // DarkBlue
    { 255,  215,    0,  128 }, // Gold
    { 128,  128,  128,  128 }, // Gray
    {   0,  255,    0,  128 }, // Green
    { 195,  223,  223,  128 }, // Ice
    { 173,  216,  230,  128 }, // LightBlue
    { 175,  175,  175,  128 }, // LightGray
    { 135,  206,  250,  128 }, // LightSkyBlue
    { 210,  105,   30,  128 }, // Lime
    { 255,    0,  255,  128 }, // Magenta
    { 128,    0,    0,  128 }, // Maroon
    { 128,  128,    0,  128 }, // Olive
    { 255,  165,    0,  128 }, // Orange
    { 255,  192,  203,  128 }, // Pink
    { 128,    0,  128,  128 }, // Purple
    { 255,    0,    0,  128 }, // Red
    { 192,  192,  192,  128 }, // Silver
    {   0,  128,  128,  128 }, // Teal
    { 238,  130,  238,  128 }, // Violet
    { 255,  255,  255,  128 }, // White
    { 255,  255,    0,  128 }  // Yellow
};

//=============================================================================
//
// view_draw helper functions:
//
//=============================================================================

/*
================
Dbg_GetDebugColorIndex
================
*/
int Dbg_GetDebugColorIndex(void)
{
    static int next_color = 0;
    next_color = (next_color + 1) % NUM_DEBUG_COLORS;
    return next_color;
}

/*
================
Dbg_GetDebugColor
================
*/
const byte * Dbg_GetDebugColor(int index)
{
    if (index < 0)
    {
        index = 0;
    }
    else if (index >= NUM_DEBUG_COLORS)
    {
        index = NUM_DEBUG_COLORS - 1;
    }
    return ps2_debug_color_table[index];
}

/*
================
PS2_ShouldCullBBox

Returns true if the bounding box is completely outside the frustum
and should be culled. False if visible and allowed to draw.
================
*/
static inline qboolean PS2_ShouldCullBBox(vec3_t mins, vec3_t maxs)
{
    int i;
    for (i = 0; i < 4; ++i)
    {
        if (BOX_ON_PLANE_SIDE(mins, maxs, &ps2_frustum[i]) == 2)
        {
            return true;
        }
    }
    return false;
}

/*
================
PS2_DrawNullModel

Remarks: Local function.
================
*/
static void PS2_DrawNullModel(const entity_t * ent)
{
    //TODO
}

/*
================
PS2_DrawBeamModel

Remarks: Local function.
================
*/
static void PS2_DrawBeamModel(const entity_t * ent)
{
    //TODO
}

/*
================
PS2_DrawSpriteModel

Remarks: Local function.
================
*/
static void PS2_DrawSpriteModel(const entity_t * ent)
{
    //TODO
}

/*
================
PS2_DrawAliasMD2Model

Remarks: Local function.
================
*/
static void PS2_DrawAliasMD2Model(const entity_t * ent)
{
    //TODO
}

//=============================================================================
//
// World rendering and visibility/PVS handling:
//
//=============================================================================

/*
================
PS2_TextureAnimation

Returns the proper texture for a given time and base texture
================
*/
static ps2_teximage_t * PS2_TextureAnimation(ps2_mdl_texinfo_t * tex)
{
    if (tex == NULL)
    {
        Sys_Error("PS2_TextureAnimation: Null tex info!");
    }

    return tex->teximage;
    /*TODO
    if (tex->next == NULL)
    {
        return tex->teximage;
    }

    int c = currententity->frame % tex->num_frames;
    while (c)
    {
        tex = tex->next;
        c--;
    }
    return tex->teximage;
    */
}

/*
================
PS2_FindLeafNodeForPoint

Remarks: Local function.
================
*/
static const ps2_mdl_leaf_t * PS2_FindLeafNodeForPoint(const vec3_t p, const ps2_model_t * model)
{
    if (model == NULL || model->nodes == NULL)
    {
        Sys_Error("PS2_FindLeafNodeForPoint: Bad model!");
    }

    const ps2_mdl_node_t * node = model->nodes;
    for (;;)
    {
        if (node->contents != -1)
        {
            return (const ps2_mdl_leaf_t *)node;
        }

        const cplane_t * plane = node->plane;
        const float d = DotProduct(p, plane->normal) - plane->dist;

        if (d > 0.0f)
        {
            node = node->children[0];
        }
        else
        {
            node = node->children[1];
        }
    }

    // Never reached if everything works as it should...
    Sys_Error("PS2_FindLeafNodeForPoint: Failed to find point!");
    return NULL;
}

/*
================
PS2_DecompressModelVis

Remarks: Local function.
Returned pointer is a local shared buffer, so don't hold on to it!
================
*/
static byte * PS2_DecompressModelVis(const byte * in, const ps2_model_t * model)
{
    int row = (model->vis->numclusters + 7) >> 3;
    byte * out = ps2_dvis_pvs;

    if (in == NULL)
    {
        // No vis info, so make all visible:
        while (row)
        {
            *out++ = 0xFF;
            row--;
        }
        return ps2_dvis_pvs;
    }

    do
    {
        if (*in)
        {
            *out++ = *in++;
            continue;
        }

        int c = in[1];
        in += 2;
        while (c)
        {
            *out++ = 0;
            c--;
        }
    } while (out - ps2_dvis_pvs < row);

    return ps2_dvis_pvs;
}

/*
================
PS2_GetClusterPVS

Remarks: Local function.
================
*/
static inline byte * PS2_GetClusterPVS(int cluster, const ps2_model_t * model)
{
    if (cluster == -1 || model->vis == NULL)
    {
        memset(ps2_dvis_pvs, 0xFF, sizeof(ps2_dvis_pvs)); // All visible.
        return ps2_dvis_pvs;
    }
    return PS2_DecompressModelVis((const byte *)model->vis + model->vis->bitofs[cluster][DVIS_PVS], model);
}

/*
================
PS2_MarkLeaves

Mark the leaves and nodes that are in the PVS for the current cluster.
================
*/
static void PS2_MarkLeaves(ps2_model_t * world_mdl)
{
    if (ps2_old_view_cluster  == ps2_view_cluster  &&
        ps2_old_view_cluster2 == ps2_view_cluster2 &&
        ps2_view_cluster != -1)
    {
        return;
    }

    ++ps2_vis_frame_count;
    ps2_old_view_cluster  = ps2_view_cluster;
    ps2_old_view_cluster2 = ps2_view_cluster2;

    int i;
    if (ps2_view_cluster == -1 || world_mdl->vis == NULL)
    {
        // Mark everything:
        for (i = 0; i < world_mdl->num_leafs; ++i)
        {
            world_mdl->leafs[i].vis_frame = ps2_vis_frame_count;
        }
        for (i = 0; i < world_mdl->num_nodes; ++i)
        {
            world_mdl->nodes[i].vis_frame = ps2_vis_frame_count;
        }
        return;
    }

    byte * vis = PS2_GetClusterPVS(ps2_view_cluster, world_mdl);
    byte fat_vis[MAX_MAP_LEAFS / 8] PS2_ALIGN(16);

    // May have to combine two clusters because of solid water boundaries:
    if (ps2_view_cluster2 != ps2_view_cluster)
    {
        memcpy(fat_vis, vis, (world_mdl->num_leafs + 7) / 8);
        vis = PS2_GetClusterPVS(ps2_view_cluster2, world_mdl);

        const int c = (world_mdl->num_leafs + 31) / 32;
        for (i = 0; i < c; ++i)
        {
            ((int *)fat_vis)[i] |= ((int *)vis)[i];
        }
        vis = fat_vis;
    }

    ps2_mdl_leaf_t * leaf;
    for (i = 0, leaf = world_mdl->leafs; i < world_mdl->num_leafs; ++i, ++leaf)
    {
        const int cluster = leaf->cluster;
        if (cluster == -1)
        {
            continue;
        }

        if (vis[cluster >> 3] & (1 << (cluster & 7)))
        {
            ps2_mdl_node_t * node = (ps2_mdl_node_t *)leaf;
            do
            {
                if (node->vis_frame == ps2_vis_frame_count)
                {
                    break;
                }
                node->vis_frame = ps2_vis_frame_count;
                node = node->parent;
            } while (node);
        }
    }
}

/*
================
PS2_RecursiveWorldNode

This function will recursively mark all surfaces that should
be drawn and add them to the appropriate draw chain, so the
next call to PS2_DrawTextureChains() will actually render what
was marked for draw in here.
================
*/
static void PS2_RecursiveWorldNode(const refdef_t * view_def, ps2_model_t * world_mdl, ps2_mdl_node_t * node)
{
    if (node->contents == CONTENTS_SOLID)
    {
        return;
    }
    if (node->vis_frame != ps2_vis_frame_count)
    {
        return;
    }
    if (PS2_ShouldCullBBox(node->minmaxs, node->minmaxs + 3))
    {
        return;
    }

    // If a leaf node, it can draw if visible.
    if (node->contents != -1)
    {
        ps2_mdl_leaf_t * leaf = (ps2_mdl_leaf_t *)node;

        // Check for door connected areas:
        if (view_def->areabits)
        {
            if (!(view_def->areabits[leaf->area >> 3] & (1 << (leaf->area & 7))))
            {
                return; // Not visible.
            }
        }

        ps2_mdl_surface_t ** mark = leaf->first_mark_surface;
        int num_surfs = leaf->num_mark_surfaces;
        if (num_surfs)
        {
            do
            {
                (*mark)->vis_frame = ps2_frame_count;
                ++mark;
            } while (--num_surfs);
        }

        return;
    }

    //
    // Node is just a decision point, so go down the appropriate sides:
    //

    float dot;
    int side, sidebit;
    const cplane_t * plane = node->plane;

    // Find which side of the node we are on:
    switch (plane->type)
    {
    case PLANE_X:
        dot = view_def->vieworg[0] - plane->dist;
        break;

    case PLANE_Y:
        dot = view_def->vieworg[1] - plane->dist;
        break;

    case PLANE_Z:
        dot = view_def->vieworg[2] - plane->dist;
        break;

    default:
        dot = DotProduct(view_def->vieworg, plane->normal) - plane->dist;
        break;
    } // switch (plane->type)

    if (dot >= 0.0f)
    {
        side    = 0;
        sidebit = 0;
    }
    else
    {
        side    = 1;
        sidebit = SURF_PLANEBACK;
    }

    // Recurse down the children, front side first:
    PS2_RecursiveWorldNode(view_def, world_mdl, node->children[side]);

    //
    // Add stuff to the draw lists:
    //
    int i;
    ps2_mdl_surface_t * surf;
    for (i = node->num_surfaces, surf = world_mdl->surfaces + node->first_surface; i; --i, ++surf)
    {
        if (surf->vis_frame != ps2_frame_count)
        {
            continue;
        }
        if ((surf->flags & SURF_PLANEBACK) != sidebit)
        {
            continue; // wrong side
        }

        if (surf->texinfo->flags & SURF_SKY)
        {
            // Just adds to visible sky bounds.
            // TODO
        }
        else if (surf->texinfo->flags & (SURF_TRANS33 | SURF_TRANS66))
        {
            // Add to the translucent draw chain.
            // TODO
        }
        else
        {
            //TODO: How are we going to handle the lightmaps?
            /*
            if (qglMTexCoord2fSGIS && !(surf->flags & SURF_DRAWTURB))
            {
                GL_RenderLightmappedPoly(surf);
            }
            else
            {
                // The polygon is visible, so add it to the texture sorted chain:
                // FIXME: this is a hack for animation
                //image = R_TextureAnimation(surf->texinfo);
                //surf->texturechain = image->texturechain;
                //image->texturechain = surf;
            }
            */

            ps2_teximage_t * image = PS2_TextureAnimation(surf->texinfo);
            if (image == NULL)
            {
                Sys_Error("PS2_RecursiveWorldNode: Null tex image!");
            }

            surf->texture_chain  = image->texture_chain;
            image->texture_chain = surf;
        }
    }

    // Finally recurse down the back side:
    PS2_RecursiveWorldNode(view_def, world_mdl, node->children[!side]);
}

/*
================
PS2_DrawBrushModel

Remarks: Local function.
================
*/
static void PS2_DrawBrushModel(const entity_t * ent)
{
    //TODO
}

//=============================================================================
//FIXME TEMP BEGIN

extern void VU1Prog_Color_Triangles_CodeStart VU_DATA_SECTION;
extern void VU1Prog_Color_Triangles_CodeEnd   VU_DATA_SECTION;

static qboolean vu_prog_set = false;
void SetVUProg(void)
{
    if (vu_prog_set) { return; }
    VU1_UploadProg(0, &VU1Prog_Color_Triangles_CodeStart, &VU1Prog_Color_Triangles_CodeEnd);
    vu_prog_set = true;
}

// GS vertex format for our triangles (reglist):
static const u64 VERTEX_FORMAT = (((u64)GS_REG_RGBAQ) << 0) | (((u64)GS_REG_XYZ2) << 4);

// Number of elements in a vertex; (color + position) in our case:
static const int NUM_VERTEX_ELEMENTS = 2;

// Simple struct to hold the temporary DMA buffer we need
// to send per draw list info to the Vector Unit 1.
typedef struct
{
    m_mat4_t mvp_matrix;
    float    gs_scale_x;
    float    gs_scale_y;
    float    gs_scale_z;
    int      vert_count;
} vu_batch_data_t PS2_ALIGN(16);

static inline int CountVertexLoops(float vertex_qwords, float num_regs)
{
    const float loops_per_qw = 2.0f / num_regs;
    return (int)(vertex_qwords * loops_per_qw);
}

enum
{
// largest value attempted so far
    MAX_TRIS_PER_VU_BATCH = 40,
    MAX_VERTS_PER_VU_BATCH = MAX_TRIS_PER_VU_BATCH * 3
};

extern u32 vu1_buffer_index;
static vu_batch_data_t ps2_batch_data_buffers[2];

static vu_batch_data_t * ps2_current_batch_data = NULL;
static u64 * ps2_current_giftag = NULL;

static int ps2_vu_batch_vert_count = 0;
static int ps2_num_vu_batches = 0; // for printing

//FIXME END TEMP
//=============================================================================

/*
================
PS2_BeginNewVUBatch

Remarks: Local function.
================
*/
static void PS2_BeginNewVUBatch(void)
{
    VU1_Begin();

    ++ps2_num_vu_batches;
    ps2_current_batch_data = &ps2_batch_data_buffers[vu1_buffer_index];

    // Copy the MVP matrix as-is:
    ps2_current_batch_data->mvp_matrix = ps2_mvp_matrix;

    // GS rasterizer scale factors follow the MVP matrix:
    ps2_current_batch_data->gs_scale_x = 2048.0f;
    ps2_current_batch_data->gs_scale_y = 2048.0f;
    ps2_current_batch_data->gs_scale_z = ((float)0xFFFFFF) / 32.0f;

    // The vertex count is in the W component of the previous quadword.
    // We only actually set the count before the batch is closed, since
    // we don't know the number of vertexes beforehand.
    ps2_current_batch_data->vert_count = 0;
    ps2_vu_batch_vert_count = 0;

    // Batch/list data will be uploaded at address 0 in VU memory.
    const int batch_data_qwsize = sizeof(*ps2_current_batch_data) >> 4;
    VU1_ListData(0, ps2_current_batch_data, batch_data_qwsize);

    // Data added from here will follow the batch/list data (matrix, scales, etc).
    VU1_ListAddBegin(batch_data_qwsize);

    // Filled before we close the draw list.
    ps2_current_giftag = VU1_ListAddGIFTag();
}

/*
================
PS2_FlushVUBatch

Remarks: Local function.
================
*/
static void PS2_FlushVUBatch(void)
{
    // Now we can set the vertex count.
    ps2_current_batch_data->vert_count = ps2_vu_batch_vert_count;

    // Finish the GIF tag now that we know the vertex count.
    const int vertex_qws = ps2_vu_batch_vert_count * 2; // 2 qwords per vertex (color + position)
    const int vert_loops = CountVertexLoops(vertex_qws, NUM_VERTEX_ELEMENTS);
    const u64 prim_desc  = GS_PRIM(GS_PRIM_TRIANGLE, GS_PRIM_SFLAT, GS_PRIM_TOFF, GS_PRIM_FOFF, GS_PRIM_ABOFF, GS_PRIM_AAON, GS_PRIM_FSTQ, GS_PRIM_C1, 0);

    // Update it:
    *ps2_current_giftag++ = GS_GIFTAG(vert_loops, 1, 1, prim_desc, GS_GIFTAG_PACKED, NUM_VERTEX_ELEMENTS);
    *ps2_current_giftag++ = VERTEX_FORMAT;

    // Close the draw list:
    VU1_ListAddEnd();

    // Send the batch and start the VU program (located in micromem address 0):
    VU1_End(0);

    //FIXME PROBABLY actually synchronize before VU1_End() call...
    PS2_WaitGSDrawFinish();
}

/*
================
PS2_VUBatchAddSurfaceTris

Remarks: Local function.
Actually sends the surface triangles to a VU1 draw list/batch.
================
*/
static void PS2_VUBatchAddSurfaceTris(const ps2_mdl_surface_t * surf)
{
    const ps2_mdl_poly_t * poly = surf->polys;
    const int num_triangles = poly->num_verts - 2;
    const byte * color = Dbg_GetDebugColor(surf->debug_color);

    ps2_vu_batch_vert_count += num_triangles * 3;

    //TEMP shouldn't need once we stabilize...
    //if (ps2_vu_batch_vert_count > MAX_VERTS_PER_VU_BATCH)
    //{
        //Sys_Error("MAX_VERTS_PER_VU_BATCH overflow!");
    //}

    int t, v;
    for (t = 0; t < num_triangles; ++t)
    {
        const ps2_mdl_triangle_t * tri = &poly->triangles[t];

        for (v = 0; v < 3; ++v)
        {
            const ps2_poly_vertex_t * vert = &poly->vertexes[tri->vertexes[v]];

            //TODO probably define a new set of functions that takes a whole
            //vertex instead of one element at a time. Reduce the number of API calls.

            // Color:
            VU1_ListAdd32(color[0]); // R
            VU1_ListAdd32(color[1]); // G
            VU1_ListAdd32(color[2]); // B
            VU1_ListAdd32(color[3]); // A

            // Position:
            VU1_ListAddFloat(vert->position[0]); // X
            VU1_ListAddFloat(vert->position[1]); // Y
            VU1_ListAddFloat(vert->position[2]); // Z
            VU1_ListAddFloat(1.0f);              // W
        }
    }
}

/*
================
PS2_DrawTextureChains

Remarks: Local function.
================
*/
static void PS2_DrawTextureChains(void)
{
    PS2_BeginNewVUBatch();

    int i;
    int verts_done = 0;
    ps2_teximage_t * teximage_iter = ps2ref.teximages;

    for (i = 0; i < MAX_TEXIMAGES; ++i, ++teximage_iter)
    {
        if (teximage_iter->type == IT_NULL)
        {
            continue;
        }

        const ps2_mdl_surface_t * surf = teximage_iter->texture_chain;
        if (surf == NULL)
        {
            continue;
        }

        for (; surf != NULL; surf = surf->texture_chain)
        {
            const ps2_mdl_poly_t * poly = surf->polys;

            // Need at least one triangle.
            if (poly == NULL || poly->num_verts < 3)
            {
                continue;
            }

            //FIXME TEMP WORK IN PROGRESS
            const int num_triangles = poly->num_verts - 2;
            if (num_triangles >= MAX_TRIS_PER_VU_BATCH)
            {
                Sys_Error("num_verts >= MAX_TRIS_PER_VU_BATCH");
            }

            if ((ps2_vu_batch_vert_count + poly->num_verts) > MAX_VERTS_PER_VU_BATCH)
            {
                PS2_FlushVUBatch();    // Close current
                PS2_BeginNewVUBatch(); // Open a new one
            }

            PS2_VUBatchAddSurfaceTris(surf);
        }

        teximage_iter->texture_chain = NULL;
    }

    PS2_FlushVUBatch();
}

/*
================
PS2_SetUpViewClusters

Remarks: Local function.
Resets the ps2_view_cluster globals for each new frame.
================
*/
static void PS2_SetUpViewClusters(const refdef_t * view_def)
{
    if (view_def->rdflags & RDF_NOWORLDMODEL)
    {
        return;
    }

    const ps2_model_t * world_mdl = PS2_ModelGetWorld();
    const ps2_mdl_leaf_t * leaf   = PS2_FindLeafNodeForPoint(view_def->vieworg, world_mdl);

    ps2_old_view_cluster  = ps2_view_cluster;
    ps2_old_view_cluster2 = ps2_view_cluster2;
    ps2_view_cluster = ps2_view_cluster2 = leaf->cluster;

    // Check above and below so crossing solid water doesn't draw wrong:
    vec3_t temp;
    if (!leaf->contents)
    {
        // Look down a bit:
        VectorCopy(view_def->vieworg, temp);
        temp[2] -= 16.0f;
    }
    else
    {
        // Look up a bit:
        VectorCopy(view_def->vieworg, temp);
        temp[2] += 16.0f;
    }

    leaf = PS2_FindLeafNodeForPoint(temp, world_mdl);
    if (!(leaf->contents & CONTENTS_SOLID) && (leaf->cluster != ps2_view_cluster2))
    {
        ps2_view_cluster2 = leaf->cluster;
    }
}

/*
================
SignBitsForPlane

Remarks: Local function.
Sign bits are used for fast box-on-plane-side tests.
================
*/
static inline int SignBitsForPlane(const cplane_t * plane)
{
    int bits, j;
    bits = 0;
    for (j = 0; j < 3; ++j)
    {
        // If the value is negative, set a bit for it.
        if (plane->normal[j] < 0.0f)
        {
            bits |= 1 << j;
        }
    }
    return bits;
}

/*
================
PS2_SetUpFrustum

Remarks: Local function.
Code reused from ref_gl (R_SetFrustum)
================
*/
static void PS2_SetUpFrustum(const refdef_t * view_def)
{
    // Rotate VPN right by FOV_X/2 degrees
    RotatePointAroundVector(ps2_frustum[0].normal,
                            (float *)&ps2_up_vec,
                            (float *)&ps2_forward_vec,
                            -(90.0f - view_def->fov_x / 2.0f));

    // Rotate VPN left by FOV_X/2 degrees
    RotatePointAroundVector(ps2_frustum[1].normal,
                            (float *)&ps2_up_vec,
                            (float *)&ps2_forward_vec,
                            (90.0f - view_def->fov_x / 2.0f));

    // Rotate VPN up by FOV_X/2 degrees
    RotatePointAroundVector(ps2_frustum[2].normal,
                            (float *)&ps2_right_vec,
                            (float *)&ps2_forward_vec,
                            (90.0f - view_def->fov_y / 2.0f));

    // Rotate VPN down by FOV_X/2 degrees
    RotatePointAroundVector(ps2_frustum[3].normal,
                            (float *)&ps2_right_vec,
                            (float *)&ps2_forward_vec,
                            -(90.0f - view_def->fov_y / 2.0f));

    int i;
    for (i = 0; i < 4; ++i)
    {
        ps2_frustum[i].type = PLANE_ANYZ;
        ps2_frustum[i].dist = DotProduct(view_def->vieworg, ps2_frustum[i].normal);
        ps2_frustum[i].signbits = SignBitsForPlane(&ps2_frustum[i]);
    }
}

//=============================================================================
//
// Public view_draw functions:
//
//=============================================================================

/*
================
PS2_DrawFrameSetup

Called by refexport_t::RenderFrame / PS2_RenderFrame.
================
*/
void PS2_DrawFrameSetup(const refdef_t * view_def)
{
    ++ps2_frame_count;
    PS2_SetUpViewClusters(view_def);

    // Eye position:
    Vec4_Set3(&ps2_camera_origin, view_def->vieworg[0], view_def->vieworg[1], view_def->vieworg[2]);

    // Camera view vectors:
    AngleVectors(view_def->viewangles, (float *)&ps2_forward_vec, (float *)&ps2_right_vec, (float *)&ps2_up_vec);
    Vec4_Add3(&ps2_camera_lookat, &ps2_camera_origin, &ps2_forward_vec);

    //Vec4_Set4(&ps2_camera_origin, 0.0f,  0.0f, -2400.0f,  1.0f);
    //Vec4_Set4(&ps2_camera_lookat, 0.0f,  0.0f,  2400.0f,  1.0f);
    //Vec4_Set4(&ps2_up_vec,        0.0f,  1.0f,  0.0f,  1.0f);

    //TODO get these from view_def
    const float fov_y  = ps2_deg_to_rad(60.0f);
    const float aspect = 4.0f / 3.0f;
    const float z_near = 4.0f;
    const float z_far  = 4096.0f;

    // Set projection and view matrices for the frame:
    Mat4_MakeLookAt(&ps2_view_matrix, &ps2_camera_origin, &ps2_camera_lookat, &ps2_up_vec);
    Mat4_MakePerspProjection(&ps2_proj_matrix, fov_y, aspect, viddef.width, viddef.height, z_near, z_far, 4096.0f);

    Mat4_Multiply(&ps2_view_proj_matrix, &ps2_view_matrix, &ps2_proj_matrix);
    Mat4_Copy(&ps2_mvp_matrix, &ps2_view_proj_matrix);
    Mat4_Identity(&ps2_model_to_world_matrix);

    //TEMP makes the word rotate around the camera
    static float rotation_angle = 0.0f;
    Mat4_MakeRotationZ(&ps2_model_to_world_matrix, rotation_angle);
    Mat4_Multiply(&ps2_mvp_matrix, &ps2_model_to_world_matrix, &ps2_view_proj_matrix);
    rotation_angle += 0.01f;

    // Update the frustum planes:
    PS2_SetUpFrustum(view_def);

    ///////////////////////////////
    //TEST
    SetVUProg();

    ps2_num_vu_batches = 0;
    ps2_vu_batch_vert_count = 0;
    ps2_current_giftag = NULL;
    ps2_current_batch_data = NULL;
}

/*
================
PS2_DrawWorldModel

Called by refexport_t::RenderFrame / PS2_RenderFrame.
================
*/
void PS2_DrawWorldModel(refdef_t * view_def)
{
    if (view_def->rdflags & RDF_NOWORLDMODEL)
    {
        return;
    }

    ps2_model_t * world_mdl = PS2_ModelGetWorld();
    PS2_MarkLeaves(world_mdl);
    PS2_RecursiveWorldNode(view_def, world_mdl, world_mdl->nodes);
    PS2_DrawTextureChains();

    PS2_DrawAltString(10, viddef.height - 30, va("batches: %d", ps2_num_vu_batches));
}

/*
================
PS2_DrawViewEntities

Called by refexport_t::RenderFrame / PS2_RenderFrame.
================
*/
void PS2_DrawViewEntities(refdef_t * view_def)
{
    int i;
    const ps2_model_t * model;
    const entity_t    * entity;
    const entity_t    * entities_list = view_def->entities;
    const int           num_entities  = view_def->num_entities;

    //
    // Draw solid entities first:
    //
    for (i = 0; i < num_entities; ++i)
    {
        entity = &entities_list[i];
        if (entity->flags & RF_TRANSLUCENT)
        {
            continue; // Drawn later.
        }

        if (entity->flags & RF_BEAM)
        {
            PS2_DrawBeamModel(entity);
            continue;
        }

        // entity_t::model is an opaque pointer outside
        // the Refresh module, so we need the cast.
        model = (const ps2_model_t *)entity->model;
        if (model == NULL)
        {
            PS2_DrawNullModel(entity);
            continue;
        }

        switch (model->type)
        {
        case MDL_NULL :
            PS2_DrawNullModel(entity);
            break;

        case MDL_BRUSH :
            PS2_DrawBrushModel(entity);
            break;

        case MDL_SPRITE :
            PS2_DrawSpriteModel(entity);
            break;

        case MDL_ALIAS :
            PS2_DrawAliasMD2Model(entity);
            break;

        default:
            Sys_Error("PS2_DrawViewEntities: Bad model type for '%s'!", model->name);
        } // switch (model->type)
    }

    //
    // Now draw the translucent/transparent ones:
    //
    //TODO!
}
