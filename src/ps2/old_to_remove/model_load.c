
/* ================================================================================================
 * -*- C -*-
 * File: model_load.c
 * Author: Guilherme R. Lampert
 * Created on: 29/10/15
 * Brief: 3D model loading routines.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include "ps2/model_load.h"
#include "ps2/ref_ps2.h"
#include "ps2/mem_alloc.h"
#include "common/q_files.h"

// d*_t structures are on-disk representation
// m*_t structures are in-memory representation
// c*_t are structures reused from the collision code

//=============================================================================

// Extra debug printing during model load when defined.
//#define PS2_VERBOSE_MODEL_LOADER

// Memory for the model structures is statically allocated.
enum { PS2_MDL_POOL_SIZE = 512 };

// Stats for debug printing:
int ps2_model_pool_used     = 0;
int ps2_model_cache_hits    = 0;
int ps2_unused_models_freed = 0;
int ps2_inline_models_used  = 0;
int ps2_models_failed       = 0;

// Timings for a level-load (registration sequence):
int ps2_model_load_fs_time    = 0; // Total milliseconds spent on FS_LoadFile.
int ps2_model_load_world_time = 0; // Total milliseconds spent on world/brush models.
int ps2_model_load_ents_time  = 0; // Total milliseconds spent on MD2 and sprites.

// If set, maps are always discarded on level load, even if still the same.
static cvar_t * r_ps2_flush_map = NULL;

// If set we don't load the MD2 and sprite models, making them render as null models.
static cvar_t * r_ps2_force_null_entity_models = NULL;

// World instance. Usually a reference to ps2_model_pool[0].
static ps2_model_t * ps2_world_model = NULL;

// Pool of models used by world/entities/sprites:
static ps2_model_t ps2_model_pool[PS2_MDL_POOL_SIZE];

// The inline * models from the current map are kept separate.
// These are only referenced by the world geometry.
static ps2_model_t ps2_inline_models[PS2_MDL_POOL_SIZE];

// Used to hash the model filenames.
extern u32 Sys_HashString(const char * str);

// For the fixed-size world chunk.
#define MEGABYTES(n) ((n) * 1024 * 1024)

//=============================================================================

/*
==============
PS2_ModelInit
==============
*/
void PS2_ModelInit(void)
{
    if (ps2_model_pool_used != 0 || ps2_inline_models_used != 0)
    {
        Sys_Error("Invalid PS2_ModelInit call!");
    }

    r_ps2_force_null_entity_models = Cvar_Get("r_ps2_force_null_entity_models", "1", 0);
    r_ps2_flush_map = Cvar_Get("r_ps2_flush_map", "0", 0);
}

/*
==============
PS2_ModelShutdown
==============
*/
void PS2_ModelShutdown(void)
{
    int i;
    ps2_model_t * model_iter = ps2_model_pool;
    for (i = 0; i < PS2_MDL_POOL_SIZE; ++i, ++model_iter)
    {
        if (model_iter->type != MDL_NULL)
        {
            PS2_ModelFree(model_iter);
        }
    }

    memset(ps2_model_pool,    0, sizeof(ps2_model_pool));
    memset(ps2_inline_models, 0, sizeof(ps2_inline_models));

    ps2_model_pool_used    = 0;
    ps2_inline_models_used = 0;
}

/*
==============
PS2_ModelAlloc
==============
*/
ps2_model_t * PS2_ModelAlloc(void)
{
    if (ps2_model_pool_used == PS2_MDL_POOL_SIZE)
    {
        Sys_Error("Out of model objects!!!");
    }

    //
    // Find a free slot in the model pool:
    //
    int i;
    ps2_model_t * model_iter = ps2_model_pool;
    for (i = 0; i < PS2_MDL_POOL_SIZE; ++i, ++model_iter)
    {
        if (model_iter->type == MDL_NULL)
        {
            ++ps2_model_pool_used;
            return model_iter;
        }
    }

    Sys_Error("Out of model objects! Can't find a free slot!");
    return NULL;
}

/*
==============
PS2_ModelFree
==============
*/
void PS2_ModelFree(ps2_model_t * mdl)
{
    if (mdl == NULL)
    {
        return;
    }

    Hunk_Free(&mdl->hunk);
    PS2_MemClearObj(mdl);
    --ps2_model_pool_used;
}

/*
==============
PS2_ModelFreeUnused
==============
*/
void PS2_ModelFreeUnused(void)
{
    int i;
    ps2_model_t * model_iter = ps2_model_pool;
    for (i = 0; i < PS2_MDL_POOL_SIZE; ++i, ++model_iter)
    {
        if (model_iter->type == MDL_NULL)
        {
            continue;
        }
        if (model_iter->registration_sequence != ps2ref.registration_sequence)
        {
            PS2_ModelFree(model_iter);
            ++ps2_unused_models_freed;
        }
    }
}

//=============================================================================
//
// MD2 (AKA Alias Models) loading:
//
//=============================================================================

/*
==============
PS2_LoadAliasMD2Model

Remarks: Local function.
Fails with a Sys_Error if the data is invalid.
Adapted from ref_gl.
==============
*/
static void PS2_LoadAliasMD2Model(ps2_model_t * mdl, const void * mdl_data)
{
    int i, j;
    const dmdl_t * p_mdl_data_in = (const dmdl_t *)mdl_data;
    const int version = LittleLong(p_mdl_data_in->version);

    if (version != ALIAS_VERSION)
    {
        Sys_Error("Model '%s' has wrong version number (%i should be %i)",
                  mdl->name, version, ALIAS_VERSION);
    }

    dmdl_t * p_header_out = (dmdl_t *)Hunk_BlockAlloc(&mdl->hunk, LittleLong(p_mdl_data_in->ofs_end));

    //
    // Byte swap the header fields and validate:
    //
    for (i = 0; i < sizeof(dmdl_t) / 4; ++i)
    {
        ((int *)p_header_out)[i] = LittleLong(((const int *)mdl_data)[i]);
    }

    if (p_header_out->skinheight > MAX_MDL_SKIN_HEIGHT)
    {
        Sys_Error("Model '%s' has a skin taller than %d.", mdl->name, MAX_MDL_SKIN_HEIGHT);
    }
    if (p_header_out->num_xyz <= 0)
    {
        Sys_Error("Model '%s' has no vertices!", mdl->name);
    }
    if (p_header_out->num_xyz > MAX_VERTS)
    {
        Sys_Error("Model '%s' has too many vertices!", mdl->name);
    }
    if (p_header_out->num_st <= 0)
    {
        Sys_Error("Model '%s' has no st vertices!", mdl->name);
    }
    if (p_header_out->num_tris <= 0)
    {
        Sys_Error("Model '%s' has no triangles!", mdl->name);
    }
    if (p_header_out->num_frames <= 0)
    {
        Sys_Error("Model '%s' has no frames!", mdl->name);
    }

    //
    // S and T texture coordinates:
    //
    const dstvert_t * p_st_in = (const dstvert_t *)((const byte *)p_mdl_data_in + p_header_out->ofs_st);
    dstvert_t * p_st_out = (dstvert_t *)((byte *)p_header_out + p_header_out->ofs_st);

    for (i = 0; i < p_header_out->num_st; ++i)
    {
        p_st_out[i].s = LittleShort(p_st_in[i].s);
        p_st_out[i].t = LittleShort(p_st_in[i].t);
    }

    //
    // Triangle lists:
    //
    const dtriangle_t * p_tris_in = (const dtriangle_t *)((const byte *)p_mdl_data_in + p_header_out->ofs_tris);
    dtriangle_t * p_tris_out = (dtriangle_t *)((byte *)p_header_out + p_header_out->ofs_tris);

    for (i = 0; i < p_header_out->num_tris; ++i)
    {
        for (j = 0; j < 3; j++)
        {
            p_tris_out[i].index_xyz[j] = LittleShort(p_tris_in[i].index_xyz[j]);
            p_tris_out[i].index_st[j]  = LittleShort(p_tris_in[i].index_st[j]);
        }
    }

    //
    // Animation frames:
    //
    for (i = 0; i < p_header_out->num_frames; ++i)
    {
        const daliasframe_t * p_frame_in;
        daliasframe_t * p_frame_out;

        p_frame_in = (const daliasframe_t *)((const byte *)p_mdl_data_in +
                                             p_header_out->ofs_frames +
                                             i * p_header_out->framesize);

        p_frame_out = (daliasframe_t *)((byte *)p_header_out +
                                        p_header_out->ofs_frames +
                                        i * p_header_out->framesize);

        memcpy(p_frame_out->name, p_frame_in->name, sizeof(p_frame_out->name));

        for (j = 0; j < 3; ++j)
        {
            p_frame_out->scale[j]     = LittleFloat(p_frame_in->scale[j]);
            p_frame_out->translate[j] = LittleFloat(p_frame_in->translate[j]);
        }

        // Verts are all 8 bit, so no swapping needed.
        memcpy(p_frame_out->verts, p_frame_in->verts,
               p_header_out->num_xyz * sizeof(dtrivertx_t));
    }

    //
    // The GL Cmds:
    //
    const int * p_cmds_in = (const int *)((const byte *)p_mdl_data_in + p_header_out->ofs_glcmds);
    int * p_cmds_out = (int *)((byte *)p_header_out + p_header_out->ofs_glcmds);

    for (i = 0; i < p_header_out->num_glcmds; ++i)
    {
        p_cmds_out[i] = LittleLong(p_cmds_in[i]);
    }

    // Set defaults for these:
    mdl->mins[0] = -32;
    mdl->mins[1] = -32;
    mdl->mins[2] = -32;
    mdl->maxs[0] =  32;
    mdl->maxs[1] =  32;
    mdl->maxs[2] =  32;

    mdl->type = MDL_ALIAS;
    mdl->num_frames = p_header_out->num_frames;

    //
    // Register all skins:
    //
    memcpy((char *)p_header_out + p_header_out->ofs_skins,
           (const char *)p_mdl_data_in + p_header_out->ofs_skins,
           p_header_out->num_skins * MAX_SKINNAME);

    for (i = 0; i < p_header_out->num_skins; ++i)
    {
        const char * p_skin_name = (const char *)p_header_out + p_header_out->ofs_skins + (i * MAX_SKINNAME);
        mdl->skins[i] = PS2_TexImageFindOrLoad(p_skin_name, IT_SKIN);
    }

    #ifdef PS2_VERBOSE_MODEL_LOADER
    Com_DPrintf("New Alias model '%s' loaded!\n", mdl->name);
    #endif // PS2_VERBOSE_MODEL_LOADER
}

//=============================================================================
//
// Sprite model loading:
//
//=============================================================================

/*
==============
PS2_LoadSpriteModel

Remarks: Local function.
Based on code from ref_gl.
==============
*/
static void PS2_LoadSpriteModel(ps2_model_t * mdl, const void * mdl_data, int file_len)
{
    const dsprite_t * p_sprite_in = (const dsprite_t *)mdl_data;
    dsprite_t * p_sprite_out = (dsprite_t *)Hunk_BlockAlloc(&mdl->hunk, file_len);

    p_sprite_out->ident     = LittleLong(p_sprite_in->ident);
    p_sprite_out->version   = LittleLong(p_sprite_in->version);
    p_sprite_out->numframes = LittleLong(p_sprite_in->numframes);

    if (p_sprite_out->version != SPRITE_VERSION)
    {
        Sys_Error("Sprite %s has wrong version number (%i should be %i)",
                  mdl->name, p_sprite_out->version, SPRITE_VERSION);
    }

    if (p_sprite_out->numframes > MAX_MD2SKINS)
    {
        Sys_Error("Sprite %s has too many frames (%i > %i)",
                  mdl->name, p_sprite_out->numframes, MAX_MD2SKINS);
    }

    // Byte swap everything:
    int i;
    for (i = 0; i < p_sprite_out->numframes; ++i)
    {
        p_sprite_out->frames[i].width    = LittleLong(p_sprite_in->frames[i].width);
        p_sprite_out->frames[i].height   = LittleLong(p_sprite_in->frames[i].height);
        p_sprite_out->frames[i].origin_x = LittleLong(p_sprite_in->frames[i].origin_x);
        p_sprite_out->frames[i].origin_y = LittleLong(p_sprite_in->frames[i].origin_y);

        // Reference the texture images:
        memcpy(p_sprite_out->frames[i].name, p_sprite_in->frames[i].name, MAX_SKINNAME);
        mdl->skins[i] = PS2_TexImageFindOrLoad(p_sprite_out->frames[i].name, IT_SPRITE);
    }

    mdl->type = MDL_SPRITE;

    #ifdef PS2_VERBOSE_MODEL_LOADER
    Com_DPrintf("New Sprite model '%s' loaded!\n", mdl->name);
    #endif // PS2_VERBOSE_MODEL_LOADER
}

//=============================================================================
//
// Brush/world model loading:
// The BMod_* prefix is for the local Brush Model loading helpers.
//
//=============================================================================

/*
==============
BMod_LoadVertexes
==============
*/
static void BMod_LoadVertexes(ps2_model_t * mdl, const byte * mdl_data, const lump_t * l)
{
    const dvertex_t * in = (const dvertex_t *)(mdl_data + l->fileofs);
    if (l->filelen % sizeof(*in))
    {
        Sys_Error("BMod_LoadVertexes: Funny lump size in '%s'", mdl->name);
    }

    const int count = l->filelen / sizeof(*in);
    ps2_mdl_vertex_t * out = (ps2_mdl_vertex_t *)Hunk_BlockAlloc(&mdl->hunk, count * sizeof(*out));

    mdl->vertexes     = out;
    mdl->num_vertexes = count;

    int i;
    for (i = 0; i < count; ++i, ++in, ++out)
    {
        out->position[0] = LittleFloat(in->point[0]);
        out->position[1] = LittleFloat(in->point[1]);
        out->position[2] = LittleFloat(in->point[2]);
    }
}

/*
==============
BMod_LoadEdges
==============
*/
static void BMod_LoadEdges(ps2_model_t * mdl, const byte * mdl_data, const lump_t * l)
{
    const dedge_t * in = (const dedge_t *)(mdl_data + l->fileofs);
    if (l->filelen % sizeof(*in))
    {
        Sys_Error("BMod_LoadEdges: Funny lump size in '%s'", mdl->name);
    }

    const int count = l->filelen / sizeof(*in);
    ps2_mdl_edge_t * out = (ps2_mdl_edge_t *)Hunk_BlockAlloc(&mdl->hunk, (count + 1) * sizeof(*out));

    mdl->edges     = out;
    mdl->num_edges = count;

    int i;
    for (i = 0; i < count; ++i, ++in, ++out)
    {
        out->v[0] = (u16)LittleShort(in->v[0]);
        out->v[1] = (u16)LittleShort(in->v[1]);
    }
}

/*
==============
BMod_LoadSurfEdges
==============
*/
static void BMod_LoadSurfEdges(ps2_model_t * mdl, const byte * mdl_data, const lump_t * l)
{
    const int * in = (const int *)(mdl_data + l->fileofs);
    if (l->filelen % sizeof(*in))
    {
        Sys_Error("BMod_LoadSurfEdges: Funny lump size in '%s'", mdl->name);
    }

    const int count = l->filelen / sizeof(*in);
    if (count < 1 || count >= MAX_MAP_SURFEDGES)
    {
        Sys_Error("BMod_LoadSurfEdges: Bad surf edges count in '%s': %i", mdl->name, count);
    }

    int * out = (int *)Hunk_BlockAlloc(&mdl->hunk, count * sizeof(*out));
    mdl->surf_edges     = out;
    mdl->num_surf_edges = count;

    int i;
    for (i = 0; i < count; ++i)
    {
        out[i] = LittleLong(in[i]);
    }
}

/*
==============
BMod_LoadLighting
==============
*/
static inline void BMod_LoadLighting(ps2_model_t * mdl, const byte * mdl_data, const lump_t * l)
{
    if (l->filelen <= 0)
    {
        mdl->light_data = NULL;
        return;
    }

    mdl->light_data = Hunk_BlockAlloc(&mdl->hunk,  l->filelen);
    memcpy(mdl->light_data, mdl_data + l->fileofs, l->filelen);
}

/*
==============
BMod_LoadPlanes
==============
*/
static void BMod_LoadPlanes(ps2_model_t * mdl, const byte * mdl_data, const lump_t * l)
{
    const dplane_t * in = (const dplane_t *)(mdl_data + l->fileofs);
    if (l->filelen % sizeof(*in))
    {
        Sys_Error("BMod_LoadPlanes: Funny lump size in '%s'", mdl->name);
    }

    const int count = l->filelen / sizeof(*in);
    cplane_t * out  = (cplane_t *)Hunk_BlockAlloc(&mdl->hunk, count * 2 * sizeof(*out));

    mdl->planes     = out;
    mdl->num_planes = count;

    int i, j;
    for (i = 0; i < count; ++i, ++in, ++out)
    {
        int bits = 0;
        for (j = 0; j < 3; ++j)
        {
            out->normal[j] = LittleFloat(in->normal[j]);
            if (out->normal[j] < 0)
            {
                bits |= (1 << j); // Negative vertex normals will set a bit
            }
        }
        out->dist = LittleFloat(in->dist);
        out->type = LittleLong(in->type);
        out->signbits = bits;
    }
}

/*
==============
BMod_LoadTexInfo
==============
*/
static void BMod_LoadTexInfo(ps2_model_t * mdl, const byte * mdl_data, const lump_t * l)
{
    // Set as the fall-back texture if loading fails.
    extern ps2_teximage_t * ps2_builtin_tex_debug;

    const textureinfo_t * in = (const textureinfo_t *)(mdl_data + l->fileofs);
    if (l->filelen % sizeof(*in))
    {
        Sys_Error("BMod_LoadTexInfo: Funny lump size in '%s'", mdl->name);
    }

    const int count = l->filelen / sizeof(*in);
    ps2_mdl_texinfo_t * out = (ps2_mdl_texinfo_t *)Hunk_BlockAlloc(&mdl->hunk, count * sizeof(*out));

    mdl->texinfos     = out;
    mdl->num_texinfos = count;

    int i, j;
    for (i = 0; i < count; ++i, ++in, ++out)
    {
        for (j = 0; j < 8; ++j)
        {
            out->vecs[0][j] = LittleFloat(in->vecs[0][j]);
        }

        out->flags = LittleLong(in->flags);
        const int next = LittleLong(in->nexttexinfo);

        if (next > 0)
        {
            out->next = mdl->texinfos + next;
        }
        else
        {
            out->next = NULL;
        }

        char name[MAX_QPATH];
        Com_sprintf(name, sizeof(name), "textures/%s.wal", in->texture);
        out->teximage = PS2_TexImageFindOrLoad(name, IT_WALL);

        // You'll get a nice and visible checker pattern if the texture can't be loaded.
        if (out->teximage == NULL)
        {
            out->teximage = ps2_builtin_tex_debug;
        }
    }

    // Count animation frames:
    for (i = 0; i < count; ++i)
    {
        out = &mdl->texinfos[i];
        out->num_frames = 1;

        ps2_mdl_texinfo_t * step;
        for (step = out->next; step && step != out; step = step->next)
        {
            out->num_frames++;
        }
    }
}

/*
==============
BMod_CalcSurfaceExtents
==============
*/
static void BMod_CalcSurfaceExtents(ps2_model_t * mdl, ps2_mdl_surface_t * s)
{
    float mins[2];
    float maxs[2];
    mins[0] = mins[1] = 999999;
    maxs[0] = maxs[1] = -99999;

    ps2_mdl_vertex_t  * v;
    ps2_mdl_texinfo_t * tex = s->texinfo;

    int i, j, e;
    for (i = 0; i < s->num_edges; ++i)
    {
        e = mdl->surf_edges[s->first_edge + i];
        if (e >= 0)
        {
            v = &mdl->vertexes[mdl->edges[e].v[0]];
        }
        else
        {
            v = &mdl->vertexes[mdl->edges[-e].v[1]];
        }

        for (j = 0; j < 2; ++j)
        {
            const float val = (v->position[0] * tex->vecs[j][0] +
                               v->position[1] * tex->vecs[j][1] +
                               v->position[2] * tex->vecs[j][2] +
                               tex->vecs[j][3]);

            if (val < mins[j]) { mins[j] = val; }
            if (val > maxs[j]) { maxs[j] = val; }
        }
    }

    int bmins[2];
    int bmaxs[2];
    for (i = 0; i < 2; ++i)
    {
        bmins[i] = floor(mins[i] / 16);
        bmaxs[i] = ceil(maxs[i]  / 16);

        s->texture_mins[i] = bmins[i] * 16;
        s->extents[i] = (bmaxs[i] - bmins[i]) * 16;
    }
}

/*
==============
BMod_ComputePolygonNormal

Computing the normal of an arbitrary polygon is
as simple as taking the cross product or each
pair of vertexes, from the first to the last
and wrapping around back to the first one if
needed. A more detailed mathematical explanation
of why this works can be found at:
http://www.iquilezles.org/www/articles/areas/areas.htm
==============
*/
static void BMod_ComputePolygonNormal(const ps2_mdl_poly_t * poly, vec3_t normal)
{
    int v;
    vec3_t cross, p0, p1;

    VectorClear(normal);
    for (v = 0; v < poly->num_verts; ++v)
    {
        const int v_next = (v + 1) % poly->num_verts;

        VectorCopy(poly->vertexes[v].position, p0);
        VectorCopy(poly->vertexes[v_next].position, p1);

        CrossProduct(p0, p1, cross);
        VectorAdd(normal, cross, normal);
    }
    VectorNormalize(normal);
}

/*
==============
GetNextActive

Remarks:
BMod_TriangulatePolygon helper
==============
*/
static inline int GetNextActive(int x, int num_verts, const byte * active)
{
    for (;;)
    {
        if (++x == num_verts)
        {
            x = 0;
        }
        if (active[x])
        {
            return x;
        }
    }
}

/*
==============
GetPrevActive

Remarks:
BMod_TriangulatePolygon helper
==============
*/
static inline int GetPrevActive(int x, int num_verts, const byte * active)
{
    for (;;)
    {
        if (--x == -1)
        {
            x = num_verts - 1;
        }
        if (active[x])
        {
            return x;
        }
    }
}

/*
==============
TestTriangle

Remarks:
BMod_TriangulatePolygon helper
==============
*/
static qboolean TestTriangle(int pi1, int pi2, int pi3, vec3_t p1, vec3_t p2, vec3_t p3, vec3_t normal,
                             const byte * active, const ps2_mdl_poly_t * poly, float epsilon)
{
    int v;
    vec3_t n1, n2, n3, pv;
    vec3_t temp0, temp1, temp2;
    qboolean result = false;

    VectorSubtract(p2, p1, temp0);
    VectorSubtract(p3, p1, temp1);

    VectorNormalize(temp0);
    CrossProduct(normal, temp0, n1);

    if (DotProduct(n1, temp1) > epsilon)
    {
        VectorSubtract(p3, p2, temp0);
        VectorSubtract(p1, p3, temp1);

        VectorNormalize(temp0);
        VectorNormalize(temp1);

        CrossProduct(normal, temp0, n2);
        CrossProduct(normal, temp1, n3);

        result = true;
        for (v = 0; v < poly->num_verts; ++v)
        {
            // Look for other vertexes inside the triangle:
            if (active[v] && v != pi1 && v != pi2 && v != pi3)
            {
                VectorCopy(poly->vertexes[v].position, pv);

                VectorSubtract(pv, p1, temp0);
                VectorSubtract(pv, p2, temp1);
                VectorSubtract(pv, p3, temp2);

                VectorNormalize(temp0);
                VectorNormalize(temp1);
                VectorNormalize(temp2);

                if (DotProduct(n1, temp0) > -epsilon &&
                    DotProduct(n2, temp1) > -epsilon &&
                    DotProduct(n3, temp2) > -epsilon)
                {
                    result = false;
                    break;
                }
            }
        }
    }

    return result;
}

/*
==============
BMod_TriangulatePolygon

Algorithm used below is an "Ear clipping"-based triangulation algorithm,
adapted from sample code presented in the "Mathematics for 3D Game Programming and Computer Graphics"
book by Eric Lengyel (available at http://www.mathfor3dgameprogramming.com/code/Listing9.2.cpp).
==============
*/
static void BMod_TriangulatePolygon(ps2_mdl_poly_t * poly)
{
    // -------------------------------------
    // Local helpers:
    #define TRIANGULATION_EPSILON 0.001f
    #define TRIANGULATION_MAX_VERTS 128
    #define EMIT_TRI(ptr, v0, v1, v2)                   \
        do                                              \
        {                                               \
            if (triangles_done == num_triangles)        \
            {                                           \
                Sys_Error("Triangle list overflowed!"); \
            }                                           \
            (ptr)->vertexes[0] = v0;                    \
            (ptr)->vertexes[1] = v1;                    \
            (ptr)->vertexes[2] = v2;                    \
            (ptr)++;                                    \
            triangles_done++;                           \
        } while (0)
    // -------------------------------------

    if (poly->num_verts <= 3)
    {
        // Already a triangle or an broken polygon.
        if (poly->num_verts == 3)
        {
            if (poly->triangles == NULL)
            {
                Sys_Error("Null triangle list in polygon!");
            }
            poly->triangles->vertexes[0] = 0;
            poly->triangles->vertexes[1] = 1;
            poly->triangles->vertexes[2] = 2;
        }
        else
        {
            // Broken polygons will be ignored by the view draw.
            Com_DPrintf("WARNING: Broken polygon found!\n");
        }
        return;
    }

    const int num_verts = poly->num_verts;
    const int num_triangles = num_verts - 2;

    // Just make it bigger if you hit this. We only require 1 byte per entry.
    if (num_verts > TRIANGULATION_MAX_VERTS)
    {
        Sys_Error("TRIANGULATION_MAX_VERTS exceeded!");
    }

    // We need a normal to properly judge the winding of the triangles.
    vec3_t normal;
    BMod_ComputePolygonNormal(poly, normal);

    int i;
    int start = 0;
    int p1 = 0;
    int p2 = 1;
    int m1 = num_verts - 1;
    int m2 = num_verts - 2;
    qboolean last_positive = false;

    int triangles_done = 0;
    ps2_mdl_triangle_t * tris_ptr = poly->triangles;

    vec3_t temp0, temp1;
    vec3_t vp1, vp2, vm1, vm2;

    // BSP polygons are generally small, under 20 verts or so.
    // We can get away with a local stack buffer and avoid a malloc.
    byte active[TRIANGULATION_MAX_VERTS];
    for (i = 0; i < num_verts; ++i)
    {
        active[i] = true;
    }

    // Triangulation loop:
    for (;;)
    {
        if (p2 == m2)
        {
            // Only three vertexes remain. We're done.
            EMIT_TRI(tris_ptr, m1, p1, p2);
            break;
        }

        VectorCopy(poly->vertexes[p1].position, vp1);
        VectorCopy(poly->vertexes[p2].position, vp2);
        VectorCopy(poly->vertexes[m1].position, vm1);
        VectorCopy(poly->vertexes[m2].position, vm2);

        // Determine whether vp1, vp2, and vm1 form a valid triangle:
        qboolean positive = TestTriangle(p1, p2, m1, vp2, vm1, vp1, normal, active, poly, TRIANGULATION_EPSILON);

        // Determine whether vm1, vm2, and vp1 form a valid triangle:
        qboolean negative = TestTriangle(m1, m2, p1, vp1, vm2, vm1, normal, active, poly, TRIANGULATION_EPSILON);

        // If both triangles are valid, choose the
        // one having the larger smallest angle.
        if (positive && negative)
        {
            VectorSubtract(vp2, vm1, temp0);
            VectorSubtract(vm2, vm1, temp1);
            VectorNormalize(temp0);
            VectorNormalize(temp1);
            const float pDot = DotProduct(temp0, temp1);

            VectorSubtract(vm2, vp1, temp0);
            VectorSubtract(vp2, vp1, temp1);
            VectorNormalize(temp0);
            VectorNormalize(temp1);
            const float mDot = DotProduct(temp0, temp1);

            if (ps2_fabsf(pDot - mDot) < TRIANGULATION_EPSILON)
            {
                if (last_positive) { positive = false; }
                else               { negative = false; }
            }
            else
            {
                if (pDot < mDot)   { negative = false; }
                else               { positive = false; }
            }
        }

        if (positive)
        {
            // Output the triangle m1, p1, p2:
            active[p1] = false;
            EMIT_TRI(tris_ptr, m1, p1, p2);
            p1 = GetNextActive(p1, num_verts, active);
            p2 = GetNextActive(p2, num_verts, active);
            last_positive = true;
            start = -1;
        }
        else if (negative)
        {
            // Output the triangle m2, m1, p1:
            active[m1] = false;
            EMIT_TRI(tris_ptr, m2, m1, p1);
            m1 = GetPrevActive(m1, num_verts, active);
            m2 = GetPrevActive(m2, num_verts, active);
            last_positive = false;
            start = -1;
        }
        else // Not a valid triangle yet.
        {
            if (start == -1)
            {
                start = p2;
            }
            else if (p2 == start)
            {
                // Exit if we've gone all the way around the
                // polygon without finding a valid triangle.
                break;
            }

            // Advance working set of vertexes:
            m2 = m1;
            m1 = p1;
            p1 = p2;
            p2 = GetNextActive(p2, num_verts, active);
        }
    }

    // FIXME I don't think this is a hard error...
    // we should be outputting at most num_verts - 2 triangles
    // but the algorithm might still fail to produce that many tris.
    // better to keep a num_triangles member in the polygon struct instead!
    if (triangles_done != num_triangles)
    {
        Sys_Error("BMod_TriangulatePolygon: Unexpected triangle count!");
    }

    // We are done with these macros now.
    #undef TRIANGULATION_EPSILON
    #undef TRIANGULATION_MAX_VERTS
    #undef EMIT_TRI
}

/*
==============
BMod_BuildPolygonFromSurface
==============
*/
static void BMod_BuildPolygonFromSurface(ps2_model_t * mdl, ps2_mdl_surface_t * surf)
{
    int i;
    float * vec;
    float s, t;

    ps2_mdl_edge_t * edges  = mdl->edges;
    const int num_verts     = surf->num_edges;
    const int num_triangles = num_verts - 2;

    vec3_t total;
    VectorClear(total);

    ps2_mdl_poly_t * poly = (ps2_mdl_poly_t *)Hunk_BlockAlloc(&mdl->hunk, sizeof(*poly));
    surf->polys = poly;

    poly->num_verts = num_verts;
    poly->vertexes  = (ps2_poly_vertex_t  *)Hunk_BlockAlloc(&mdl->hunk, sizeof(ps2_poly_vertex_t)  * num_verts);
    poly->triangles = (ps2_mdl_triangle_t *)Hunk_BlockAlloc(&mdl->hunk, sizeof(ps2_mdl_triangle_t) * num_triangles);

    // Reconstruct the polygon from edges:
    for (i = 0; i < num_verts; ++i)
    {
        ps2_mdl_edge_t * other_edge;
        const int index = mdl->surf_edges[surf->first_edge + i];

        if (index > 0)
        {
            other_edge = &edges[index];
            vec = mdl->vertexes[other_edge->v[0]].position;
        }
        else
        {
            other_edge = &edges[-index];
            vec = mdl->vertexes[other_edge->v[1]].position;
        }

        s = DotProduct(vec, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3];
        s /= surf->texinfo->teximage->width;

        t = DotProduct(vec, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3];
        t /= surf->texinfo->teximage->height;

        // Vertex position:
        VectorAdd(total, vec, total);
        VectorCopy(vec, poly->vertexes[i].position);

        // Color texture tex coordinates:
        poly->vertexes[i].texture_s = s;
        poly->vertexes[i].texture_t = t;

        // Lightmap texture coordinates:
        s = DotProduct(vec, surf->texinfo->vecs[0]) + surf->texinfo->vecs[0][3];
        s -= surf->texture_mins[0];
        s += surf->light_s * 16;
        s += 8;
        s /= LM_BLOCK_WIDTH * 16;

        t = DotProduct(vec, surf->texinfo->vecs[1]) + surf->texinfo->vecs[1][3];
        t -= surf->texture_mins[1];
        t += surf->light_t * 16;
        t += 8;
        t /= LM_BLOCK_HEIGHT * 16;

        poly->vertexes[i].lightmap_s = s;
        poly->vertexes[i].lightmap_t = t;
    }

    // We need triangles to render with the PS2.
    BMod_TriangulatePolygon(poly);
}

/*
==============
BMod_LoadFaces
==============
*/
static void BMod_LoadFaces(ps2_model_t * mdl, const byte * mdl_data, const lump_t * l)
{
    extern int Dbg_GetDebugColorIndex(void);

    const dface_t * in = (const dface_t *)(mdl_data + l->fileofs);
    if (l->filelen % sizeof(*in))
    {
        Sys_Error("BMod_LoadFaces: Funny lump size in %s", mdl->name);
    }

    const int count = l->filelen / sizeof(*in);
    ps2_mdl_surface_t * out = (ps2_mdl_surface_t *)Hunk_BlockAlloc(&mdl->hunk, count * sizeof(*out));

    mdl->surfaces     = out;
    mdl->num_surfaces = count;

    //TODO needed?
    //GL_BeginBuildingLightmaps(mdl);

    int surf_num;
    for (surf_num = 0; surf_num < count; ++surf_num, ++in, ++out)
    {
        out->first_edge  = LittleLong(in->firstedge);
        out->num_edges   = LittleShort(in->numedges);
        out->debug_color = Dbg_GetDebugColorIndex();
        out->flags       = 0;
        out->polys       = NULL;

        const int plane_num = LittleShort(in->planenum);
        const int side = LittleShort(in->side);
        if (side)
        {
            out->flags |= SURF_PLANEBACK;
        }

        out->plane = mdl->planes + plane_num;

        const int tex_num = LittleShort(in->texinfo);
        if (tex_num < 0 || tex_num >= mdl->num_texinfos)
        {
            Sys_Error("BMod_LoadFaces: Bad texinfo number: %i", tex_num);
        }
        out->texinfo = mdl->texinfos + tex_num;

        //
        // Fill out->texturemins[] and out->extents[]:
        //
        BMod_CalcSurfaceExtents(mdl, out);

        //
        // Lighting info:
        //
        int i;
        for (i = 0; i < MAXLIGHTMAPS; ++i)
        {
            out->styles[i] = in->styles[i];
        }

        i = LittleLong(in->lightofs);
        if (i == -1)
        {
            out->samples = NULL;
        }
        else
        {
            out->samples = mdl->light_data + i;
        }

        //
        // Set the drawing flags:
        //
        if (out->texinfo->flags & SURF_WARP)
        {
            out->flags |= SURF_DRAWTURB;
            for (i = 0; i < 2; ++i)
            {
                out->extents[i] = 16384;
                out->texture_mins[i] = -8192;
            }

            //TODO
            //GL_SubdivideSurface(out); // cut up polygon for warps
        }

        //
        // Create lightmaps and polygons:
        //
        /*
        if (!(out->texinfo->flags & (SURF_SKY | SURF_TRANS33 | SURF_TRANS66 | SURF_WARP)))
        {
            //TODO
            GL_CreateSurfaceLightmap(out);
        }
        */
        if (!(out->texinfo->flags & SURF_WARP))
        {
            BMod_BuildPolygonFromSurface(mdl, out);
        }
    }

    //TODO needed?
    //GL_EndBuildingLightmaps();
}

/*
==============
BMod_LoadMarkSurfaces
==============
*/
static void BMod_LoadMarkSurfaces(ps2_model_t * mdl, const byte * mdl_data, const lump_t * l)
{
    const s16 * in = (const s16 *)(mdl_data + l->fileofs);
    if (l->filelen % sizeof(*in))
    {
        Sys_Error("BMod_LoadMarkSurfaces: Funny lump size in '%s'", mdl->name);
    }

    const int count = l->filelen / sizeof(*in);
    ps2_mdl_surface_t ** out = (ps2_mdl_surface_t **)Hunk_BlockAlloc(&mdl->hunk, count * sizeof(*out));

    mdl->mark_surfaces     = out;
    mdl->num_mark_surfaces = count;

    int i, j;
    for (i = 0; i < count; ++i)
    {
        j = LittleShort(in[i]);
        if (j < 0 || j >= mdl->num_surfaces)
        {
            Sys_Error("BMod_LoadMarkSurfaces: Bad surface number: %i", j);
        }
        out[i] = mdl->surfaces + j;
    }
}

/*
==============
BMod_LoadVisibility
==============
*/
static void BMod_LoadVisibility(ps2_model_t * mdl, const byte * mdl_data, const lump_t * l)
{
    if (l->filelen <= 0)
    {
        mdl->vis = NULL;
        return;
    }

    int i;

    mdl->vis = (dvis_t *)Hunk_BlockAlloc(&mdl->hunk, l->filelen);
    memcpy(mdl->vis, mdl_data + l->fileofs, l->filelen);

    mdl->vis->numclusters = LittleLong(mdl->vis->numclusters);
    for (i = 0; i < mdl->vis->numclusters; ++i)
    {
        mdl->vis->bitofs[i][0] = LittleLong(mdl->vis->bitofs[i][0]);
        mdl->vis->bitofs[i][1] = LittleLong(mdl->vis->bitofs[i][1]);
    }
}

/*
==============
BMod_LoadLeafs
==============
*/
static void BMod_LoadLeafs(ps2_model_t * mdl, const byte * mdl_data, const lump_t * l)
{
    const dleaf_t * in = (const dleaf_t *)(mdl_data + l->fileofs);
    if (l->filelen % sizeof(*in))
    {
        Sys_Error("BMod_LoadLeafs: Funny lump size in '%s'", mdl->name);
    }

    const int count = l->filelen / sizeof(*in);
    ps2_mdl_leaf_t * out = (ps2_mdl_leaf_t *)Hunk_BlockAlloc(&mdl->hunk, count * sizeof(*out));

    mdl->leafs     = out;
    mdl->num_leafs = count;

    int i, j;
    for (i = 0; i < count; ++i, ++in, ++out)
    {
        for (j = 0; j < 3; ++j)
        {
            out->minmaxs[j] = LittleShort(in->mins[j]);
            out->minmaxs[j + 3] = LittleShort(in->maxs[j]);
        }

        out->contents = LittleLong(in->contents);
        out->cluster  = LittleShort(in->cluster);
        out->area     = LittleShort(in->area);

        out->first_mark_surface = mdl->mark_surfaces + LittleShort(in->firstleafface);
        out->num_mark_surfaces = LittleShort(in->numleaffaces);
    }
}

/*
==============
BMod_SetParentRecursive
==============
*/
static void BMod_SetParentRecursive(ps2_mdl_node_t * node, ps2_mdl_node_t * parent)
{
    node->parent = parent;
    if (node->contents != -1)
    {
        return;
    }
    BMod_SetParentRecursive(node->children[0], node);
    BMod_SetParentRecursive(node->children[1], node);
}

/*
==============
BMod_LoadNodes
==============
*/
static void BMod_LoadNodes(ps2_model_t * mdl, const byte * mdl_data, const lump_t * l)
{
    const dnode_t * in = (const dnode_t *)(mdl_data + l->fileofs);
    if (l->filelen % sizeof(*in))
    {
        Sys_Error("BMod_LoadNodes: Funny lump size in '%s'", mdl->name);
    }

    const int count = l->filelen / sizeof(*in);
    ps2_mdl_node_t * out = (ps2_mdl_node_t *)Hunk_BlockAlloc(&mdl->hunk, count * sizeof(*out));

    mdl->nodes     = out;
    mdl->num_nodes = count;

    int i, j, p;
    for (i = 0; i < count; ++i, ++in, ++out)
    {
        for (j = 0; j < 3; ++j)
        {
            out->minmaxs[j] = LittleShort(in->mins[j]);
            out->minmaxs[j + 3] = LittleShort(in->maxs[j]);
        }

        p = LittleLong(in->planenum);
        out->plane = mdl->planes + p;

        out->first_surface = LittleShort(in->firstface);
        out->num_surfaces  = LittleShort(in->numfaces);
        out->contents      = -1; // differentiate from leafs

        for (j = 0; j < 2; ++j)
        {
            p = LittleLong(in->children[j]);
            if (p >= 0)
            {
                out->children[j] = mdl->nodes + p;
            }
            else
            {
                out->children[j] = (ps2_mdl_node_t *)(mdl->leafs + (-1 - p));
            }
        }
    }

    BMod_SetParentRecursive(mdl->nodes, NULL); // Also sets nodes and leafs
}

/*
==============
BMod_RadiusFromBounds
==============
*/
static inline float BMod_RadiusFromBounds(const vec3_t mins, const vec3_t maxs)
{
    int i;
    vec3_t corner;
    for (i = 0; i < 3; ++i)
    {
        const float abs_min = ps2_fabsf(mins[i]);
        const float abs_max = ps2_fabsf(maxs[i]);
        corner[i] = (abs_min > abs_max) ? abs_min : abs_max;
    }
    return VectorLength(corner);
}

/*
==============
BMod_LoadSubmodels
==============
*/
static void BMod_LoadSubmodels(ps2_model_t * mdl, const byte * mdl_data, const lump_t * l)
{
    const dmodel_t * in = (const dmodel_t *)(mdl_data + l->fileofs);
    if (l->filelen % sizeof(*in))
    {
        Sys_Error("BMod_LoadSubmodels: Funny lump size in '%s'", mdl->name);
    }

    const int count = l->filelen / sizeof(*in);
    ps2_mdl_submod_t * out = (ps2_mdl_submod_t *)Hunk_BlockAlloc(&mdl->hunk, count * sizeof(*out));

    mdl->submodels     = out;
    mdl->num_submodels = count;

    int i, j;
    for (i = 0; i < count; ++i, ++in, ++out)
    {
        for (j = 0; j < 3; ++j)
        {
            // spread the mins/maxs by a unit
            out->mins[j]   = LittleFloat(in->mins[j]) - 1;
            out->maxs[j]   = LittleFloat(in->maxs[j]) + 1;
            out->origin[j] = LittleFloat(in->origin[j]);
        }
        out->radius     = BMod_RadiusFromBounds(out->mins, out->maxs);
        out->head_node  = LittleLong(in->headnode);
        out->first_face = LittleLong(in->firstface);
        out->num_faces  = LittleLong(in->numfaces);
    }
}

/*
==============
PS2_LoadBrushModel

Remarks: Local function.
Fails with a Sys_Error if the data is invalid.
Adapted from ref_gl.
==============
*/
static void PS2_LoadBrushModel(ps2_model_t * mdl, void * mdl_data)
{
    if (mdl != &ps2_model_pool[0])
    {
        Sys_Error("Loaded a brush model after the world!");
    }

    int i;
    dheader_t * header = (dheader_t *)mdl_data;
    const int version  = LittleLong(header->version);

    if (version != BSPVERSION)
    {
        Sys_Error("PS2_LoadBrushModel: '%s' has wrong version number (%i should be %i)",
                  mdl->name, version, BSPVERSION);
    }

    // Byte-swap the header fields:
    for (i = 0; i < sizeof(dheader_t) / 4; ++i)
    {
        ((int *)header)[i] = LittleLong(((int *)header)[i]);
    }

    // Load file contents into the in-memory model structure:
    BMod_LoadVertexes(mdl, mdl_data, &header->lumps[LUMP_VERTEXES]);
    BMod_LoadEdges(mdl, mdl_data, &header->lumps[LUMP_EDGES]);
    BMod_LoadSurfEdges(mdl, mdl_data, &header->lumps[LUMP_SURFEDGES]);
    BMod_LoadLighting(mdl, mdl_data, &header->lumps[LUMP_LIGHTING]);
    BMod_LoadPlanes(mdl, mdl_data, &header->lumps[LUMP_PLANES]);
    BMod_LoadTexInfo(mdl, mdl_data, &header->lumps[LUMP_TEXINFO]);
    BMod_LoadFaces(mdl, mdl_data, &header->lumps[LUMP_FACES]);
    BMod_LoadMarkSurfaces(mdl, mdl_data, &header->lumps[LUMP_LEAFFACES]);
    BMod_LoadVisibility(mdl, mdl_data, &header->lumps[LUMP_VISIBILITY]);
    BMod_LoadLeafs(mdl, mdl_data, &header->lumps[LUMP_LEAFS]);
    BMod_LoadNodes(mdl, mdl_data, &header->lumps[LUMP_NODES]);
    BMod_LoadSubmodels(mdl, mdl_data, &header->lumps[LUMP_MODELS]);

    mdl->num_frames = 2; // regular and alternate animation
    mdl->type = MDL_BRUSH;

    // Set up the submodels:
    for (i = 0; i < mdl->num_submodels; ++i)
    {
        ps2_mdl_submod_t * submodel = &mdl->submodels[i];
        ps2_model_t * inline_mdl = &ps2_inline_models[i];

        *inline_mdl = *mdl;
        inline_mdl->first_model_surface = submodel->first_face;
        inline_mdl->num_model_surfaces  = submodel->num_faces;
        inline_mdl->first_node          = submodel->head_node;

        if (inline_mdl->first_node >= mdl->num_nodes)
        {
            Sys_Error("Inline model %i has bad first_node!", i);
        }

        VectorCopy(submodel->maxs, inline_mdl->maxs);
        VectorCopy(submodel->mins, inline_mdl->mins);
        inline_mdl->radius = submodel->radius;

        if (i == 0)
        {
            *mdl = *inline_mdl;
        }

        inline_mdl->num_leafs = submodel->vis_leafs;
    }

    // Make sure all images are referenced now.
    for (i = 0; i < mdl->num_texinfos; ++i)
    {
        if (mdl->texinfos[i].teximage == NULL)
        {
            Sys_Error("Null teximage at %i for model '%s'!", i, mdl->name);
        }
        mdl->texinfos[i].teximage->registration_sequence = ps2ref.registration_sequence;
    }

    #ifdef PS2_VERBOSE_MODEL_LOADER
    Com_DPrintf("New Brush model '%s' loaded!\n", mdl->name);
    #endif // PS2_VERBOSE_MODEL_LOADER
}

/*
==============
PS2_FindInlineModel

Remarks: Local function.
==============
*/
static inline ps2_model_t * PS2_FindInlineModel(const char * name)
{
    int i = atoi(name + 1);
    if (i < 1 || ps2_world_model == NULL || i >= ps2_world_model->num_submodels)
    {
        Sys_Error("Bad inline model number or null world model!");
    }

    ++ps2_inline_models_used;
    return &ps2_inline_models[i];
}

//=============================================================================
//
// Public model/world loaders:
//
//=============================================================================

/*
==============
PS2_ReferenceAllTextures

Remarks: Local function.
==============
*/
static void PS2_ReferenceAllTextures(ps2_model_t * mdl)
{
    int i;
    dsprite_t * p_sprite;
    dmdl_t    * p_md2;

    switch (mdl->type)
    {
    case MDL_BRUSH :
        for (i = 0; i < mdl->num_texinfos; ++i)
        {
            if (mdl->texinfos[i].teximage == NULL)
            {
                continue;
            }
            mdl->texinfos[i].teximage->registration_sequence = ps2ref.registration_sequence;
        }
        break;

    case MDL_SPRITE :
        p_sprite = (dsprite_t *)mdl->hunk.base_ptr;
        for (i = 0; i < p_sprite->numframes; ++i)
        {
            mdl->skins[i] = PS2_TexImageFindOrLoad(p_sprite->frames[i].name, IT_SPRITE);
        }
        break;

    case MDL_ALIAS :
        p_md2 = (dmdl_t *)mdl->hunk.base_ptr;
        for (i = 0; i < p_md2->num_skins; ++i)
        {
            mdl->skins[i] = PS2_TexImageFindOrLoad((const char *)p_md2 + p_md2->ofs_skins + (i * MAX_SKINNAME), IT_SKIN);
        }
        mdl->num_frames = p_md2->num_frames;
        break;

    default :
        Sys_Error("PS2_ReferenceAllTextures: Bad model type for '%s'!", mdl->name);
    } // switch (mdl->type)
}

/*
==============
PS2_ModelFindOrLoad
==============
*/
ps2_model_t * PS2_ModelFindOrLoad(const char * name, int flags)
{
    if (name == NULL || *name == '\0')
    {
        Com_DPrintf("FindModel: Null/empty model name!\n");
        ++ps2_models_failed;
        return NULL;
    }

    //
    // Inline models are grabbed from a separate pool:
    //
    if (name[0] == '*')
    {
        return PS2_FindInlineModel(name);
    }

    //
    // Search the currently loaded models first:
    //
    int i;
    const u32 name_hash = Sys_HashString(name); // Compare by hash code, much cheaper.
    ps2_model_t * model_iter = ps2_model_pool;
    for (i = 0; i < PS2_MDL_POOL_SIZE; ++i, ++model_iter)
    {
        if (model_iter->type == MDL_NULL)
        {
            continue;
        }
        if ((name_hash == model_iter->hash) && (flags & model_iter->type))
        {
            if (ps2ref.registration_started)
            {
                ++ps2_model_cache_hits;
            }

            #ifdef PS2_VERBOSE_MODEL_LOADER
            Com_DPrintf("Model '%s' already in cache.\n", name);
            #endif // PS2_VERBOSE_MODEL_LOADER

            model_iter->registration_sequence = ps2ref.registration_sequence;
            PS2_ReferenceAllTextures(model_iter); // Ensures they are not discarded by EndRegistration.
            return model_iter;
        }
    }

    //
    // Else, load from file for the first time:
    //
    ps2_model_t * new_model = PS2_ModelAlloc();
    strncpy(new_model->name, name, MAX_QPATH); // Save the name string for console printing
    new_model->hash = name_hash;               // We've already computed the name hash above!

    // Optionally skip loading entity and sprite models
    // (useful for quick loading of just the map during development)
    if (r_ps2_force_null_entity_models->value)
    {
        // If not a map, skip.
        if (strcmp(name + strlen(name) - 4, ".bsp") != 0)
        {
            Com_DPrintf("Skipping load for model '%s'\n", name);
            new_model->registration_sequence = ps2ref.registration_sequence;
            new_model->type = MDL_NULL;
            return new_model;
        }
    }

    int start_time;
    int end_time;
    int file_len;
    void * file_data = NULL;

    //
    // Load raw file data:
    //
    start_time = Sys_Milliseconds();
    {
        file_len = FS_LoadFile(name, &file_data);
        if (file_data == NULL || file_len <= 0)
        {
            Com_DPrintf("WARNING: Unable to find model '%s'! Failed to open file.\n", name);

            // Put it back into the pool.
            PS2_ModelFree(new_model);
            ++ps2_models_failed;
            return NULL;
        }
    }
    end_time = Sys_Milliseconds();
    ps2_model_load_fs_time += end_time - start_time;

    //
    // Call the appropriate loader:
    //
    const u32 id = LittleLong(*(u32 *)file_data);
    switch (id)
    {
    case IDALIASHEADER :
        start_time = Sys_Milliseconds();
        {
            Hunk_New(&new_model->hunk, file_len + 128, MEMTAG_MDL_ALIAS); // Plus some extra bytes for rounding
            PS2_LoadAliasMD2Model(new_model, file_data);
        }
        end_time = Sys_Milliseconds();
        ps2_model_load_ents_time += end_time - start_time;
        break;

    case IDSPRITEHEADER :
        start_time = Sys_Milliseconds();
        {
            Hunk_New(&new_model->hunk, file_len + 128, MEMTAG_MDL_SPRITE); // Plus some extra bytes for rounding
            PS2_LoadSpriteModel(new_model, file_data, file_len);
        }
        end_time = Sys_Milliseconds();
        ps2_model_load_ents_time += end_time - start_time;
        break;

    case IDBSPHEADER :
        start_time = Sys_Milliseconds();
        {
            Hunk_New(&new_model->hunk, MEGABYTES(5), MEMTAG_MDL_WORLD); // Fixed size for the largest map in the game
            PS2_LoadBrushModel(new_model, file_data);
        }
        end_time = Sys_Milliseconds();
        ps2_model_load_world_time += end_time - start_time;
        break;

    default :
        Sys_Error("FindModel: Unknown file id (0x%X) for '%s'!", id, name);
    } // switch (id)

    // Done with the original file.
    FS_FreeFile(file_data);

    // Reference it:
    new_model->registration_sequence = ps2ref.registration_sequence;
    return new_model;
}

/*
==============
PS2_ModelLoadWorld

Remarks: Fails with a Sys_Error if the world model cannot be loaded.
==============
*/
void PS2_ModelLoadWorld(const char * name)
{
    if (name == NULL || *name == '\0')
    {
        Sys_Error("LoadWorld: Null/empty map name!\n");
    }

    // This function is only called by BeginRegistration,
    // so it's a good place to reset these counters.
    ps2_unused_models_freed   = 0;
    ps2_model_cache_hits      = 0;
    ps2_inline_models_used    = 0;
    ps2_models_failed         = 0;
    ps2_model_load_fs_time    = 0;
    ps2_model_load_world_time = 0;
    ps2_model_load_ents_time  = 0;

    char fullname[MAX_QPATH];
    Com_sprintf(fullname, sizeof(fullname), "maps/%s.bsp", name);

    // Explicitly free the old map if different.
    // This guarantees that that the first model is the world map.
    if (strcmp(ps2_model_pool[0].name, fullname) != 0 || r_ps2_flush_map->value)
    {
        PS2_ModelFree(&ps2_model_pool[0]);
    }

    ps2_world_model = PS2_ModelFindOrLoad(fullname, MDL_BRUSH);
    if (ps2_world_model == NULL)
    {
        Sys_Error("Unable to load level '%s'!", fullname);
    }
}

/*
==============
PS2_ModelGetWorld
==============
*/
ps2_model_t * PS2_ModelGetWorld(void)
{
    return ps2_world_model;
}
