/* ================================================================================================
 * File: bspinfo.cpp
 * Created on: 27/08/26
 * Brief: Reports the largest value each BSP lump reaches across a set of maps, next to the
 *        MAX_MAP_* cap the engine reserves a static array for.
 *
 * The PS2 port sizes cmodel.c's collision arrays to what the shipped maps actually use rather
 * than to id's design bounds (see the note above the MAX_MAP_* block in src/common/q_files.h).
 * Those caps are tight, so this tool exists to re-derive them: point it at a pak or a directory
 * of .bsp files and it prints the worst case per lump, which map set it, and how much slack is
 * left over the cap. Anything at or over 100% would fail to load with a Com_Error.
 *
 *   build/tools/bspinfo baseq2/pak0.pak
 *   build/tools/bspinfo baseq2/pak0/maps
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>

/*
 * From Quake2:
 */

// 4CC 'PACK'
#define PAK_HEADER_IDENT (('K' << 24) + ('C' << 16) + ('A' << 8) + 'P')

// 4CC 'IBSP'
#define BSP_HEADER_IDENT (('P' << 24) + ('S' << 16) + ('B' << 8) + 'I')

#define BSP_VERSION   38
#define BSP_NUM_LUMPS 19

typedef struct
{
    char name[56];
    int filepos;
    int filelen;
} pak_file_t;

typedef struct
{
    int ident;
    int dirofs;
    int dirlen;
} pak_header_t;

typedef struct
{
    int fileofs;
    int filelen;
} bsp_lump_t;

typedef struct
{
    int ident;
    int version;
    bsp_lump_t lumps[BSP_NUM_LUMPS];
} bsp_header_t;

/*
 * Lump table:
 *
 * 'elem_size' is the on-disk size of one element, so filelen/elem_size is the count the engine
 * bounds-checks. The byte lumps (entities, visibility, lighting) use 1 and are counted in bytes.
 * 'cap' mirrors the MAX_MAP_* value in src/common/q_files.h; keep the two in sync. A cap of 0
 * means the engine does not bound that lump with a constant of its own.
 */
typedef struct
{
    const char * name;
    int elem_size;
    long cap;
    const char * cap_name;
    bool reserves_array; // true when cmodel.c sizes a static array with the cap
} lump_info_t;

static const lump_info_t lump_info[BSP_NUM_LUMPS] = {
    /*  0 */ { "ENTITIES",    1,  0x28000, "MAX_MAP_ENTSTRING",   true  },
    /*  1 */ { "PLANES",      20, 16384,   "MAX_MAP_PLANES",      true  },
    /*  2 */ { "VERTEXES",    12, 65536,   "MAX_MAP_VERTS",       false },
    /*  3 */ { "VISIBILITY",  1,  0x70000, "MAX_MAP_VISIBILITY",  true  },
    /*  4 */ { "NODES",       28, 12288,   "MAX_MAP_NODES",       true  },
    /*  5 */ { "TEXINFO",     76, 1536,    "MAX_MAP_TEXINFO",     true  },
    /*  6 */ { "FACES",       20, 65536,   "MAX_MAP_FACES",       false },
    /*  7 */ { "LIGHTING",    1,  0x200000,"MAX_MAP_LIGHTING",    false },
    /*  8 */ { "LEAFS",       28, 12288,   "MAX_MAP_LEAFS",       true  },
    /*  9 */ { "LEAFFACES",   2,  65536,   "MAX_MAP_LEAFFACES",   false },
    /* 10 */ { "LEAFBRUSHES", 2,  16384,   "MAX_MAP_LEAFBRUSHES", true  },
    /* 11 */ { "EDGES",       4,  128000,  "MAX_MAP_EDGES",       false },
    /* 12 */ { "SURFEDGES",   4,  256000,  "MAX_MAP_SURFEDGES",   false },
    /* 13 */ { "MODELS",      48, 224,     "MAX_MAP_MODELS",      true  },
    /* 14 */ { "BRUSHES",     12, 6144,    "MAX_MAP_BRUSHES",     true  },
    /* 15 */ { "BRUSHSIDES",  4,  40960,   "MAX_MAP_BRUSHSIDES",  true  },
    /* 16 */ { "POP",         1,  0,       "",                    false },
    /* 17 */ { "AREAS",       8,  256,     "MAX_MAP_AREAS",       true  },
    /* 18 */ { "AREAPORTALS", 8,  64,      "MAX_MAP_AREAPORTALS", true  },
};


/*
 * ================================================================================================
 * World hunk sizing
 *
 * The renderer packs a whole brush model into one contiguous allocation, sized up front by
 * ComputeBrushHunkSize() in src/ps2/renderer/model_load.cpp and then filled by loaders that must
 * agree with it to the byte. On a 32MB console that single block is the largest allocation the
 * program ever makes, so the number below - the worst it reaches across a map set - is what any
 * up-front reservation has to be sized against.
 *
 * This mirrors ComputeBrushHunkSize(), including the SubdividePolygon() recursion that warp
 * (SURF_WARP) faces go through, because that recursion is the dominant per-map variable: on
 * power2 it produces ~1.5MB of polygons against ~0.16MB on base2. Estimating it instead of
 * running it is off by more than 20%.
 *
 * KEEP IN SYNC with model_load.cpp. The struct sizes below are the PS2 build's, read out of the
 * debug ELF's DWARF; a layout change there silently invalidates this tool's numbers.
 * ================================================================================================
 */

#define HUNK_ALIGN 16

/* sizeof() on the EE (32-bit pointers), from model.h via DWARF. */
/* TODO/FIXME: Use actual sizeof(Struct) here rather than this, now that this tool is C++ and
   could include model.h - it needs the EE include chain untangled from the host build first. */
#define SZ_MODEL_VERTEX     12
#define SZ_MODEL_EDGE        4
#define SZ_SURF_EDGE         4  /* int */
#define SZ_CPLANE           20
#define SZ_MODEL_TEXINFO    48
#define SZ_MODEL_SURFACE    88
#define SZ_MODEL_POLY       16
#define SZ_POLY_VERTEX      28
#define SZ_MODEL_TRIANGLE    3
#define SZ_MARK_SURFACE      4  /* ModelSurface * */
#define SZ_MODEL_LEAF       52
#define SZ_MODEL_NODE       52
#define SZ_SUBMODEL_INFO    52

/* From model.h / q_files.h. */
#define SUBDIVIDE_SIZE      64
#define SUBDIVIDE_SIZE_F    64.0f
#define SURF_WARP_FLAG      0x8

/* On-disk lump element sizes used by the sizer (see lump_info above for the rest). */
typedef struct { float point[3]; } d_vertex_t;
typedef struct { unsigned short v[2]; } d_edge_t;

typedef struct
{
    unsigned short planenum;
    short          side;
    int            firstedge;
    short          numedges;
    short          texinfo;
    unsigned char  styles[4];
    int            lightofs;
} d_face_t;

typedef struct
{
    float vecs[2][4];
    int   flags;
    int   value;
    char  texture[32];
    int   nexttexinfo;
} d_texinfo_t;

typedef struct { float x, y, z; } vec3_t3;

static long align_up(long v, long a)
{
    return (v + (a - 1)) / a * a;
}

/* Mirrors PolyBlockBytes(): one block per polygon holding the header, its vertices and its
   triangles, rounded once instead of three times. */
static long poly_block_bytes(int num_verts, int num_tris)
{
    return align_up((long)SZ_MODEL_POLY
                    + (long)num_verts * SZ_POLY_VERTEX
                    + (long)num_tris * SZ_MODEL_TRIANGLE, HUNK_ALIGN);
}

static float vec_component(const vec3_t3 * v, int axis)
{
    return (axis == 0) ? v->x : ((axis == 1) ? v->y : v->z);
}

/* Mirrors SubdividePolygon(). Accumulates into *out_bytes rather than emitting geometry.
   Returns false if the polygon exceeds what the real loader accepts, which is a hard error
   there (Sys_Error) and so must be reported here rather than silently mis-sized. */
static bool subdivide_polygon(int num_verts, const vec3_t3 * verts, long * out_bytes)
{
    vec3_t3 mins, maxs;
    int axis, i;

    if (num_verts > SUBDIVIDE_SIZE - 4)
    {
        return false; /* SubdividePolygon() calls Sys_Error here. */
    }

    mins = maxs = verts[0];
    for (i = 1; i < num_verts; ++i)
    {
        if (verts[i].x < mins.x) mins.x = verts[i].x;
        if (verts[i].y < mins.y) mins.y = verts[i].y;
        if (verts[i].z < mins.z) mins.z = verts[i].z;
        if (verts[i].x > maxs.x) maxs.x = verts[i].x;
        if (verts[i].y > maxs.y) maxs.y = verts[i].y;
        if (verts[i].z > maxs.z) maxs.z = verts[i].z;
    }

    for (axis = 0; axis < 3; ++axis)
    {
        float dist[SUBDIVIDE_SIZE + 1];
        vec3_t3 wrapped[SUBDIVIDE_SIZE + 1];
        vec3_t3 front[SUBDIVIDE_SIZE];
        vec3_t3 back[SUBDIVIDE_SIZE];
        int f = 0, b = 0;

        const float lo  = vec_component(&mins, axis);
        const float hi  = vec_component(&maxs, axis);
        const float mid = SUBDIVIDE_SIZE_F * floorf(((lo + hi) * 0.5f) / SUBDIVIDE_SIZE_F + 0.5f);

        if (hi - mid < 8.0f) continue;
        if (mid - lo < 8.0f) continue;

        for (i = 0; i < num_verts; ++i)
        {
            dist[i] = vec_component(&verts[i], axis) - mid;
            wrapped[i] = verts[i];
        }
        dist[num_verts] = dist[0];
        wrapped[num_verts] = verts[0];

        for (i = 0; i < num_verts; ++i)
        {
            if (dist[i] >= 0.0f) front[f++] = wrapped[i];
            if (dist[i] <= 0.0f) back[b++]  = wrapped[i];

            if (dist[i] == 0.0f || dist[i + 1] == 0.0f) continue;

            if ((dist[i] > 0.0f) != (dist[i + 1] > 0.0f))
            {
                const float frac = dist[i] / (dist[i] - dist[i + 1]);
                vec3_t3 m;
                m.x = wrapped[i].x + (wrapped[i + 1].x - wrapped[i].x) * frac;
                m.y = wrapped[i].y + (wrapped[i + 1].y - wrapped[i].y) * frac;
                m.z = wrapped[i].z + (wrapped[i + 1].z - wrapped[i].z) * frac;
                front[f++] = m;
                back[b++]  = m;
            }
        }

        return subdivide_polygon(f, front, out_bytes) &&
               subdivide_polygon(b, back, out_bytes);
    }

    /* A leaf of the recursion: the filler emits a fan of numVerts + 2 vertices, no triangles. */
    *out_bytes += poly_block_bytes(num_verts + 2, 0);
    return true;
}


/* Mirrors BspFileReader::RequiredScratchBytes(). The streamed loader stages one lump at a time
   through a single buffer, except for the five the hunk pre-pass needs held together, so the
   buffer has to be the larger of those two demands. Like the hunk, this is allocated per map
   load today and is a candidate for the same up-front reservation. */
static long compute_scratch_size(const bsp_header_t * hdr)
{
    /* LUMP_FACES, TEXINFO, SURFEDGES, EDGES, VERTEXES */
    static const int pre_pass[]  = { 6, 5, 12, 11, 2 };
    /* the above, plus PLANES, LEAFFACES, LEAFS, NODES, MODELS */
    static const int streamed[]  = { 6, 5, 12, 11, 2, 1, 9, 8, 4, 13 };

    long pre_total = 0, largest = 0, needed;
    size_t i;

    for (i = 0; i < sizeof(pre_pass) / sizeof(pre_pass[0]); ++i)
    {
        pre_total += align_up(hdr->lumps[pre_pass[i]].filelen, HUNK_ALIGN);
    }
    for (i = 0; i < sizeof(streamed) / sizeof(streamed[0]); ++i)
    {
        long len = align_up(hdr->lumps[streamed[i]].filelen, HUNK_ALIGN);
        if (len > largest) { largest = len; }
    }

    needed = (pre_total > largest) ? pre_total : largest;
    return (needed != 0) ? needed : HUNK_ALIGN;
}

/* Computes what ComputeBrushHunkSize() would return for this map. 'data' must be the whole
   .bsp. Returns -1 if the map has something the real loader would reject. */
static long compute_brush_hunk_size(const bsp_header_t * hdr, const unsigned char * data)
{
    const d_face_t    * faces      = (const d_face_t *)   (data + hdr->lumps[6].fileofs);
    const d_texinfo_t * texinfos   = (const d_texinfo_t *)(data + hdr->lumps[5].fileofs);
    const int         * surf_edges = (const int *)        (data + hdr->lumps[12].fileofs);
    const d_edge_t    * edges      = (const d_edge_t *)   (data + hdr->lumps[11].fileofs);
    const d_vertex_t  * vertexes   = (const d_vertex_t *) (data + hdr->lumps[2].fileofs);

    const int num_vertexes = hdr->lumps[2].filelen  / (int)sizeof(d_vertex_t);
    const int num_edges    = hdr->lumps[11].filelen / (int)sizeof(d_edge_t);
    const int num_surfedge = hdr->lumps[12].filelen / (int)sizeof(int);
    const int num_planes   = hdr->lumps[1].filelen  / 20;
    const int num_texinfo  = hdr->lumps[5].filelen  / (int)sizeof(d_texinfo_t);
    const int num_faces    = hdr->lumps[6].filelen  / (int)sizeof(d_face_t);
    const int num_marksurf = hdr->lumps[9].filelen  / 2;
    const int num_leafs    = hdr->lumps[8].filelen  / 28;
    const int num_nodes    = hdr->lumps[4].filelen  / 28;
    const int num_models   = hdr->lumps[13].filelen / 48;
    const int lighting_len = hdr->lumps[7].filelen;

    long total = 0;
    int f, i;

    total += align_up((long)num_vertexes * SZ_MODEL_VERTEX, HUNK_ALIGN);
    total += align_up((long)(num_edges + 1) * SZ_MODEL_EDGE, HUNK_ALIGN);
    total += align_up((long)num_surfedge * SZ_SURF_EDGE, HUNK_ALIGN);

    if (lighting_len > 0)
    {
        total += align_up(lighting_len, HUNK_ALIGN);
    }

    total += align_up((long)num_planes * SZ_CPLANE, HUNK_ALIGN);
    total += align_up((long)num_texinfo * SZ_MODEL_TEXINFO, HUNK_ALIGN);
    total += align_up((long)num_faces * SZ_MODEL_SURFACE, HUNK_ALIGN);

    /* Per-face polygons: the largest term, and the only one that needs the lump data. */
    for (f = 0; f < num_faces; ++f)
    {
        const int ne = faces[f].numedges;
        const int tn = faces[f].texinfo;
        const bool warp = (tn >= 0 && tn < num_texinfo) && (texinfos[tn].flags & SURF_WARP_FLAG);

        if (!warp)
        {
            total += poly_block_bytes(ne, ne >= 3 ? ne - 2 : 0);
            continue;
        }

        {
            vec3_t3 poly[SUBDIVIDE_SIZE];
            int count = 0;

            for (i = 0; i < ne; ++i)
            {
                int e, vi;

                if (count >= SUBDIVIDE_SIZE)
                {
                    return -1; /* "Warp surface too large" in the real loader. */
                }

                e  = surf_edges[faces[f].firstedge + i];
                vi = (e > 0) ? edges[e].v[0] : edges[-e].v[1];

                poly[count].x = vertexes[vi].point[0];
                poly[count].y = vertexes[vi].point[1];
                poly[count].z = vertexes[vi].point[2];
                ++count;
            }

            if (!subdivide_polygon(count, poly, &total))
            {
                return -1;
            }
        }
    }

    total += align_up((long)num_marksurf * SZ_MARK_SURFACE, HUNK_ALIGN);
    /* No VISIBILITY term: cmodel.c owns that lump, the view walk reads it via CM_ClusterPVS. */
    total += align_up((long)num_leafs * SZ_MODEL_LEAF, HUNK_ALIGN);
    total += align_up((long)num_nodes * SZ_MODEL_NODE, HUNK_ALIGN);
    total += align_up((long)num_models * SZ_SUBMODEL_INFO, HUNK_ALIGN);

    return total;
}

typedef struct
{
    long count;
    char map[64];
} lump_max_t;

static lump_max_t maxima[BSP_NUM_LUMPS];
static int num_maps_scanned;

/* Every map's world hunk, so the report can rank them rather than just name the worst. */
typedef struct
{
    char map[64];
    long hunk_bytes;    /* -1 when the sizer rejected the map */
    long scratch_bytes; /* the streaming loader's lump staging buffer */
} map_hunk_t;

#define MAX_MAPS_TRACKED 512
static map_hunk_t map_hunks[MAX_MAPS_TRACKED];
static int num_map_hunks;

/*
 * Scanning:
 */

// Folds one map's header into the running maxima. 'data' is the first sizeof(bsp_header_t)
// bytes of the BSP; anything that isn't an IBSP v38 header is skipped with a warning.
static void account_bsp(const char * map_name, const unsigned char * data, size_t data_len)
{
    bsp_header_t hdr;
    int i;

    if (data_len < sizeof(hdr))
    {
        fprintf(stderr, "warning: '%s' is too small to hold a BSP header, skipping.\n", map_name);
        return;
    }

    memcpy(&hdr, data, sizeof(hdr));

    if (hdr.ident != BSP_HEADER_IDENT)
    {
        fprintf(stderr, "warning: '%s' is not an IBSP file, skipping.\n", map_name);
        return;
    }
    if (hdr.version != BSP_VERSION)
    {
        fprintf(stderr, "warning: '%s' is BSP version %d, expected %d, skipping.\n",
                map_name, hdr.version, BSP_VERSION);
        return;
    }

    for (i = 0; i < BSP_NUM_LUMPS; ++i)
    {
        long count = (long)hdr.lumps[i].filelen / lump_info[i].elem_size;
        if (count > maxima[i].count)
        {
            maxima[i].count = count;
            snprintf(maxima[i].map, sizeof(maxima[i].map), "%s", map_name);
        }
    }

    /* The hunk sizer walks the face/texinfo/surfedge/edge/vertex lumps, so it needs the whole
       file rather than just the header. Callers that only handed us a header get counted for
       the lump table and skipped here. */
    if (data_len >= sizeof(hdr) && num_map_hunks < MAX_MAPS_TRACKED)
    {
        map_hunk_t * rec = &map_hunks[num_map_hunks++];
        snprintf(rec->map, sizeof(rec->map), "%s", map_name);
        rec->hunk_bytes    = compute_brush_hunk_size(&hdr, data);
        rec->scratch_bytes = compute_scratch_size(&hdr);

        if (rec->hunk_bytes < 0)
        {
            fprintf(stderr, "warning: '%s' has geometry the renderer would reject.\n", map_name);
        }
    }

    ++num_maps_scanned;
}

// Strips directories off a path so the report shows 'lab.bsp' rather than 'maps/lab.bsp'.
static const char * base_name(const char * path)
{
    const char * slash = strrchr(path, '/');
    return slash ? (slash + 1) : path;
}

static bool scan_pak(const char * pak_path)
{
    pak_header_t hdr;
    pak_file_t * dir;
    int num_files, i;
    FILE * fp = fopen(pak_path, "rb");

    if (fp == NULL)
    {
        fprintf(stderr, "error: unable to open '%s'.\n", pak_path);
        return false;
    }

    if (fread(&hdr, sizeof(hdr), 1, fp) != 1 || hdr.ident != PAK_HEADER_IDENT)
    {
        fprintf(stderr, "error: '%s' is not a Quake 2 PAK archive.\n", pak_path);
        fclose(fp);
        return false;
    }

    num_files = hdr.dirlen / (int)sizeof(pak_file_t);
    if (num_files <= 0)
    {
        fprintf(stderr, "error: '%s' has an empty directory.\n", pak_path);
        fclose(fp);
        return false;
    }

    dir = (pak_file_t *)malloc((size_t)num_files * sizeof(pak_file_t));
    if (dir == NULL)
    {
        fprintf(stderr, "error: out of memory reading the PAK directory.\n");
        fclose(fp);
        return false;
    }

    if (fseek(fp, hdr.dirofs, SEEK_SET) != 0 ||
        fread(dir, sizeof(pak_file_t), (size_t)num_files, fp) != (size_t)num_files)
    {
        fprintf(stderr, "error: truncated PAK directory in '%s'.\n", pak_path);
        free(dir);
        fclose(fp);
        return false;
    }

    for (i = 0; i < num_files; ++i)
    {
        const char * name = dir[i].name;
        size_t name_len = strlen(name);
        unsigned char * map_bytes;

        if (name_len < 4 || strcasecmp(name + name_len - 4, ".bsp") != 0)
        {
            continue;
        }

        // The whole map, not just the header: the hunk sizer walks the geometry lumps.
        map_bytes = (unsigned char *)malloc((size_t)dir[i].filelen);
        if (map_bytes == NULL)
        {
            fprintf(stderr, "warning: out of memory reading '%s', skipping.\n", name);
            continue;
        }

        if (fseek(fp, dir[i].filepos, SEEK_SET) != 0 ||
            fread(map_bytes, 1, (size_t)dir[i].filelen, fp) != (size_t)dir[i].filelen)
        {
            fprintf(stderr, "warning: unable to read '%s', skipping.\n", name);
            free(map_bytes);
            continue;
        }

        account_bsp(base_name(name), map_bytes, (size_t)dir[i].filelen);
        free(map_bytes);
    }

    free(dir);
    fclose(fp);
    return true;
}

static bool scan_directory(const char * dir_path)
{
    struct dirent * ent;
    DIR * dir = opendir(dir_path);

    if (dir == NULL)
    {
        fprintf(stderr, "error: unable to open directory '%s'.\n", dir_path);
        return false;
    }

    while ((ent = readdir(dir)) != NULL)
    {
        char full_path[1024];
        size_t name_len = strlen(ent->d_name);
        unsigned char * map_bytes;
        long file_len;
        FILE * fp;

        if (name_len < 4 || strcasecmp(ent->d_name + name_len - 4, ".bsp") != 0)
        {
            continue;
        }

        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);

        fp = fopen(full_path, "rb");
        if (fp == NULL)
        {
            fprintf(stderr, "warning: unable to open '%s', skipping.\n", full_path);
            continue;
        }

        // The whole map, not just the header: the hunk sizer walks the geometry lumps.
        if (fseek(fp, 0, SEEK_END) != 0 || (file_len = ftell(fp)) <= 0 || fseek(fp, 0, SEEK_SET) != 0)
        {
            fprintf(stderr, "warning: unable to size '%s', skipping.\n", full_path);
            fclose(fp);
            continue;
        }

        map_bytes = (unsigned char *)malloc((size_t)file_len);
        if (map_bytes == NULL)
        {
            fprintf(stderr, "warning: out of memory reading '%s', skipping.\n", full_path);
            fclose(fp);
            continue;
        }

        if (fread(map_bytes, 1, (size_t)file_len, fp) == (size_t)file_len)
        {
            account_bsp(ent->d_name, map_bytes, (size_t)file_len);
        }
        else
        {
            fprintf(stderr, "warning: unable to read '%s', skipping.\n", full_path);
        }

        free(map_bytes);
        fclose(fp);
    }

    closedir(dir);
    return true;
}

/*
 * Reporting:
 */

static void print_report()
{
    int i;
    int over_cap = 0;

    printf("\n%d map(s) scanned.\n\n", num_maps_scanned);
    printf("%-13s %10s  %-16s %10s  %6s  %s\n",
           "LUMP", "WORST", "SET BY", "CAP", "USED", "CONSTANT");
    printf("--------------------------------------------------------------------------------\n");

    for (i = 0; i < BSP_NUM_LUMPS; ++i)
    {
        const lump_info_t * info = &lump_info[i];

        printf("%-13s %10ld  %-16s ", info->name, maxima[i].count,
               maxima[i].count > 0 ? maxima[i].map : "-");

        if (info->cap > 0)
        {
            double used = 100.0 * (double)maxima[i].count / (double)info->cap;
            printf("%10ld  %5.1f%%  %s%s\n", info->cap, used, info->cap_name,
                   info->reserves_array ? "" : " (bound only)");

            if (maxima[i].count > info->cap)
            {
                ++over_cap;
            }
        }
        else
        {
            printf("%10s  %6s  %s\n", "-", "-", "unbounded");
        }
    }

    printf("--------------------------------------------------------------------------------\n");
    printf("'USED' is the worst map as a percentage of the cap; at 100%% or above the map\n");
    printf("fails to load with a Com_Error. Lumps marked '(bound only)' are validated but\n");
    printf("do not reserve a static array, so lowering them saves nothing.\n");

    if (over_cap > 0)
    {
        printf("\n*** %d lump(s) EXCEED their cap - raise them in src/common/q_files.h ***\n", over_cap);
    }
}

/* Descending by hunk size, so the report leads with the map that sets the reservation. */
static int compare_hunks(const void * a, const void * b)
{
    const map_hunk_t * x = (const map_hunk_t *)a;
    const map_hunk_t * y = (const map_hunk_t *)b;

    if (x->hunk_bytes < y->hunk_bytes) return 1;
    if (x->hunk_bytes > y->hunk_bytes) return -1;
    return strcmp(x->map, y->map);
}

static void print_hunk_report(void)
{
    const int kShow = 10;
    long worst = 0, worst_scratch = 0;
    const char * worst_map = "-";
    const char * worst_scratch_map = "-";
    int i, shown, rejected = 0;

    if (num_map_hunks == 0)
    {
        return;
    }

    qsort(map_hunks, (size_t)num_map_hunks, sizeof(map_hunks[0]), compare_hunks);

    for (i = 0; i < num_map_hunks; ++i)
    {
        if (map_hunks[i].hunk_bytes < 0) { ++rejected; continue; }
        if (map_hunks[i].hunk_bytes > worst)
        {
            worst = map_hunks[i].hunk_bytes;
            worst_map = map_hunks[i].map;
        }
        if (map_hunks[i].scratch_bytes > worst_scratch)
        {
            worst_scratch = map_hunks[i].scratch_bytes;
            worst_scratch_map = map_hunks[i].map;
        }
    }

    printf("\n\nWORLD HUNK - the single contiguous block the renderer allocates per map\n");
    printf("--------------------------------------------------------------------------------\n");
    printf("%-16s %12s  %10s  %12s\n", "MAP", "HUNK", "MB", "SCRATCH");

    shown = (num_map_hunks < kShow) ? num_map_hunks : kShow;
    for (i = 0; i < shown; ++i)
    {
        if (map_hunks[i].hunk_bytes < 0)
        {
            printf("%-16s %12s  %10s  %12ld\n", map_hunks[i].map, "REJECTED", "-",
                   map_hunks[i].scratch_bytes);
            continue;
        }
        printf("%-16s %12ld  %10.2f  %12ld\n", map_hunks[i].map, map_hunks[i].hunk_bytes,
               (double)map_hunks[i].hunk_bytes / (1024.0 * 1024.0),
               map_hunks[i].scratch_bytes);
    }
    if (num_map_hunks > shown)
    {
        printf("%-16s %12s  %10s  %12s   (%d more)\n", "...", "", "", "", num_map_hunks - shown);
    }

    printf("--------------------------------------------------------------------------------\n");
    printf("WORST HUNK   : %s needs %ld bytes (%.2f MB).\n",
           worst_map, worst, (double)worst / (1024.0 * 1024.0));
    printf("WORST SCRATCH: %s needs %ld bytes (%.2f MB).\n",
           worst_scratch_map, worst_scratch, (double)worst_scratch / (1024.0 * 1024.0));
    printf("\n");
    printf("Those are the figures the up-front reservations have to be sized to. Round them up\n");
    printf("for headroom - custom maps and the mission packs are not in this set:\n");
    printf("  hunk    +5%%  = %8ld bytes (%.2f MB)   +10%% = %8ld bytes (%.2f MB)\n",
           (long)(worst * 1.05), (double)worst * 1.05 / (1024.0 * 1024.0),
           (long)(worst * 1.10), (double)worst * 1.10 / (1024.0 * 1024.0));
    printf("  scratch +5%%  = %8ld bytes (%.2f MB)   +10%% = %8ld bytes (%.2f MB)\n",
           (long)(worst_scratch * 1.05), (double)worst_scratch * 1.05 / (1024.0 * 1024.0),
           (long)(worst_scratch * 1.10), (double)worst_scratch * 1.10 / (1024.0 * 1024.0));
    printf("  combined at +10%% = %ld bytes (%.2f MB)\n",
           (long)((worst + worst_scratch) * 1.10),
           (double)(worst + worst_scratch) * 1.10 / (1024.0 * 1024.0));
    printf("\n");
    printf("For reference, id's own ref_gl and ref_soft called Hunk_Begin(0x1000000) - 16 MB -\n");
    printf("for every brush model. That was a virtual-address RESERVATION on Win32/Linux, with\n");
    printf("Hunk_End() committing only the bytes actually used, so it was an upper bound rather\n");
    printf("than an allocation. The PS2 has no MMU trick to borrow, which is why the real\n");
    printf("maximum above is the number that matters. (id used 0x200000 for alias models and\n");
    printf("0x10000 for sprites on the same scheme.)\n");

    if (rejected > 0)
    {
        printf("\n*** %d map(s) REJECTED by the sizer - see the warnings above ***\n", rejected);
    }
}

int main(int argc, char * argv[])
{
    struct stat path_stat;
    const char * path;
    bool ok;

    if (argc != 2)
    {
        printf("Usage:\n  %s <pak-file | directory-of-bsps>\n\n", argv[0]);
        printf("Reports the largest value each BSP lump reaches across every map found,\n");
        printf("next to the MAX_MAP_* cap from src/common/q_files.h. Use it to re-derive\n");
        printf("those caps after adding a mission pack or custom maps.\n\n");
        printf("Also reports the world hunk each map needs - the one contiguous block the\n");
        printf("renderer allocates per map - and the worst case across the set.\n\n");
        printf("Examples:\n");
        printf("  %s baseq2/pak0.pak\n", argv[0]);
        printf("  %s baseq2/pak0/maps\n", argv[0]);
        return EXIT_FAILURE;
    }

    path = argv[1];

    if (stat(path, &path_stat) != 0)
    {
        fprintf(stderr, "error: '%s' does not exist.\n", path);
        return EXIT_FAILURE;
    }

    ok = S_ISDIR(path_stat.st_mode) ? scan_directory(path) : scan_pak(path);
    if (!ok)
    {
        return EXIT_FAILURE;
    }

    if (num_maps_scanned == 0)
    {
        fprintf(stderr, "error: no usable BSP files found in '%s'.\n", path);
        return EXIT_FAILURE;
    }

    print_report();
    print_hunk_report();
    return EXIT_SUCCESS;
}
