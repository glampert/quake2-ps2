
/* ================================================================================================
 * -*- C -*-
 * File: model_load.h
 * Author: Guilherme R. Lampert
 * Created on: 29/10/15
 * Brief: Declarations related to 3D model loading and handling for entities and world/level.
 *        Structures found here were mostly adapted from 'ref_gl/gl_model.h'
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#ifndef PS2_MODEL_H
#define PS2_MODEL_H

#include "ps2/ref_ps2.h"

enum
{
    // Plane sides:
    SIDE_FRONT          = 0,
    SIDE_BACK           = 1,
    SIDE_ON             = 2,

    // Misc surface flags (same values used by ref_gl):
    SURF_PLANEBACK      = 2,
    SURF_DRAWSKY        = 4,
    SURF_DRAWTURB       = 16,
    SURF_DRAWBACKGROUND = 64,
    SURF_UNDERWATER     = 128,

    // Size in pixels of the lightmap atlases.
    LM_BLOCK_WIDTH      = 128,
    LM_BLOCK_HEIGHT     = 128,

    // Max height in pixels of MD2 skins.
    // This was the original size limit of Quake2,
    // but we can't load textures larger than 256,
    // so anything bigger will get downscaled anyway.
    MAX_MDL_SKIN_HEIGHT = 480
};

/*
==============================================================

In-memory representation of 3D models (world and entities):

==============================================================
*/

/*
 * Model vertex position:
 */
typedef struct ps2_mdl_vertex_s
{
    vec3_t position;
} ps2_mdl_vertex_t;

/*
 * Vertex format used by ps2_mdl_poly_t
 * Has two sets of texture coordinates for lightmapping.
 */
typedef struct ps2_poly_vertex_s
{
    // model vertex position:
    vec3_t position;

    // main tex coords:
    float texture_s;
    float texture_t;

    // lightmap tex coords:
    float lightmap_s;
    float lightmap_t;
} ps2_poly_vertex_t;

/*
 * Model triangle vertex indexes.
 * Limited to 16bits to save space.
 */
typedef struct ps2_mdl_triangle_s
{
    u16 vertexes[3];
} ps2_mdl_triangle_t;

/*
 * Sub-model mesh data:
 */
typedef struct ps2_mdl_submod_s
{
    vec3_t mins;
    vec3_t maxs;
    vec3_t origin;
    float radius;
    int head_node;
    int vis_leafs;
    int first_face;
    int num_faces;
} ps2_mdl_submod_t;

/*
 * Edge description:
 */
typedef struct ps2_mdl_edge_s
{
    u16 v[2]; // vertex numbers/indexes
} ps2_mdl_edge_t;

/*
 * Texture/material description:
 */
typedef struct ps2_mdl_texinfo_s
{
    float vecs[2][4];
    int flags;
    int num_frames;
    ps2_teximage_t * teximage;
    struct ps2_mdl_texinfo_s * next; // animation chain
} ps2_mdl_texinfo_t;

/*
 * Model polygon/face:
 * List links are for draw sorting.
 */
typedef struct ps2_mdl_poly_s
{
    int num_verts;                  // size of vertexes[], since it's dynamically allocated
    ps2_poly_vertex_t  * vertexes;  // array of polygon vertexes. Never null
    ps2_mdl_triangle_t * triangles; // (num_verts - 2) triangles with indexes into vertexes[]
} ps2_mdl_poly_t;

/*
 * Surface description:
 * (holds a set of polygons)
 */
typedef struct ps2_mdl_surface_s
{
    int vis_frame; // should be drawn when node is crossed

    cplane_t * plane;
    int flags;
    int debug_color;

    int first_edge; // look up in model->surf_edges[], negative numbers are backwards edges
    int num_edges;

    s16 texture_mins[2];
    s16 extents[2];

    int light_s, light_t;   // lightmap tex coordinates
    int dlight_s, dlight_t; // lightmap tex coordinates for dynamic lightmaps

    ps2_mdl_poly_t * polys; // multiple if warped
    const struct ps2_mdl_surface_s * texture_chain;
    const struct ps2_mdl_surface_s * lightmap_chain;

    ps2_mdl_texinfo_t * texinfo;

    // dynamic lighting info:
    int dlight_frame;
    int dlight_bits;

    int lightmap_texture_num;
    byte styles[MAXLIGHTMAPS];
    float cached_light[MAXLIGHTMAPS]; // values currently used in lightmap
    byte * samples;                   // [numstyles * surfsize]
} ps2_mdl_surface_t;

/*
 * BSP world node:
 */
typedef struct ps2_mdl_node_s
{
    // common with leaf
    int contents;  // -1, to differentiate from leafs
    int vis_frame; // node needs to be traversed if current

    // for bounding box culling
    float minmaxs[6];

    struct ps2_mdl_node_s * parent;

    // node specific
    cplane_t * plane;
    struct ps2_mdl_node_s * children[2];

    u16 first_surface;
    u16 num_surfaces;
} ps2_mdl_node_t;

/*
 * Special BSP leaf node (draw node):
 */
typedef struct ps2_mdl_leaf_s
{
    // common with node
    int contents;  // will be a negative contents number
    int vis_frame; // node needs to be traversed if current

    // for bounding box culling
    float minmaxs[6];

    struct ps2_mdl_node_s * parent;

    // leaf specific
    int cluster;
    int area;

    ps2_mdl_surface_t ** first_mark_surface;
    int num_mark_surfaces;
} ps2_mdl_leaf_t;

/*
 * Misc model type flags:
 */
typedef enum
{
    MDL_NULL   = 0,        // Uninitialized model. Used internally.
    MDL_BRUSH  = (1 << 1), // World geometry.
    MDL_SPRITE = (1 << 2), // Sprites.
    MDL_ALIAS  = (1 << 3)  // MD2/Entity.
} ps2_mdl_type_t;

/*
==============================================================

Whole model (world or entity/sprite):

==============================================================
*/

typedef struct ps2_model_s
{
    ps2_mdl_type_t type;
    int num_frames;
    int flags;

    // Volume occupied by the model graphics.
    vec3_t mins;
    vec3_t maxs;
    float radius;

    // Solid volume for clipping.
    qboolean clipbox;
    vec3_t clipmins;
    vec3_t clipmaxs;

    // Brush model.
    int first_model_surface;
    int num_model_surfaces;
    int lightmap; // Only for submodels

    int num_submodels;
    ps2_mdl_submod_t * submodels;

    int num_planes;
    cplane_t * planes;

    int num_leafs; // Number of visible leafs, not counting 0
    ps2_mdl_leaf_t * leafs;

    int num_vertexes;
    ps2_mdl_vertex_t * vertexes;

    int num_edges;
    ps2_mdl_edge_t * edges;

    int num_nodes;
    int first_node;
    ps2_mdl_node_t * nodes;

    int num_texinfos;
    ps2_mdl_texinfo_t * texinfos;

    int num_surfaces;
    ps2_mdl_surface_t * surfaces;

    int num_surf_edges;
    int * surf_edges;

    int num_mark_surfaces;
    ps2_mdl_surface_t ** mark_surfaces;

    dvis_t * vis;
    byte   * light_data;

    // For alias models and skins.
    ps2_teximage_t * skins[MAX_MD2SKINS];

    // Registration number, so we know if it is currently referenced by the level being played.
    u32 registration_sequence;

    // Memory hunk backing the model's data.
    mem_hunk_t hunk;

    // Hash of the following name string, for faster lookup.
    u32 hash;

    // File name with path.
    char name[MAX_QPATH];
} ps2_model_t;

/*
==============================================================

Public model loading/management functions:

==============================================================
*/

// Global initialization/shutdown:
void PS2_ModelInit(void);
void PS2_ModelShutdown(void);

// Allocate a blank model from the pool.
// Fails with a Sys_Error if no more slots are available.
ps2_model_t * PS2_ModelAlloc(void);

// Looks up an already loaded model or tries to load it from disk for the first time.
ps2_model_t * PS2_ModelFindOrLoad(const char * name, int flags);

// Loads the world model used by the current level the game wants.
// Only one world model is allowed at any time.
void PS2_ModelLoadWorld(const char * name);

// Get the currently loaded world model instance.
ps2_model_t * PS2_ModelGetWorld(void);

// Frees a model previously acquired from ModelFindOrLoad.
void PS2_ModelFree(ps2_model_t * mdl);

// Called by EndRegistration() to free models not referenced by the new level.
void PS2_ModelFreeUnused(void);

#endif // PS2_MODEL_H
