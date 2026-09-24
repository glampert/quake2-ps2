/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "common/q_common.h"

//
// model loading
//

typedef struct
{
    cplane_t * plane;
    int children[2]; // negative numbers are leafs
} cnode_t;

typedef struct
{
    cplane_t * plane;
    mapsurface_t * surface;
} cbrushside_t;

typedef struct
{
    int contents;
    int cluster;
    int area;
    unsigned short firstleafbrush;
    unsigned short numleafbrushes;
} cleaf_t;

typedef struct
{
    int contents;
    int numsides;
    int firstbrushside;
    int checkcount; // to avoid repeated testings
} cbrush_t;

typedef struct
{
    int numareaportals;
    int firstareaportal;
    int floodnum; // if two areas have equal floodnums, they are connected
    int floodvalid;
} carea_t;

//
// [PS2_QUAKE] 2015-10-27
// Made a bunch of local data in this file 'static',
// since most global variables in here are never exported.
//

// Referenced by cl_main.c
int numtexinfo;
mapsurface_t map_surfaces[MAX_MAP_TEXINFO];

static int checkcount;
static char map_name[MAX_QPATH];

static int numbrushsides;
static cbrushside_t map_brushsides[MAX_MAP_BRUSHSIDES];

static int numplanes;
static cplane_t map_planes[MAX_MAP_PLANES + 6]; // extra for box hull

static int numnodes;
static cnode_t map_nodes[MAX_MAP_NODES + 6]; // extra for box hull

static int numleafs = 1; // allow leaf funcs to be called without a map
static cleaf_t map_leafs[MAX_MAP_LEAFS];
static int emptyleaf, solidleaf;

static int numleafbrushes;
static unsigned short map_leafbrushes[MAX_MAP_LEAFBRUSHES];

static int numcmodels;
static cmodel_t map_cmodels[MAX_MAP_MODELS];

static int numbrushes;
static cbrush_t map_brushes[MAX_MAP_BRUSHES];

static int numvisibility;
static byte map_visibility[MAX_MAP_VISIBILITY];
static dvis_t * map_vis = (dvis_t *)map_visibility;

static int numentitychars;
static char map_entitystring[MAX_MAP_ENTSTRING];

static int numareas = 1;
static carea_t map_areas[MAX_MAP_AREAS];

static int numareaportals;
static dareaportal_t map_areaportals[MAX_MAP_AREAPORTALS];

static int numclusters = 1;
static int floodvalid;

static mapsurface_t nullsurface;
static qboolean portalopen[MAX_MAP_AREAPORTALS];

//
// [PS2_QUAKE] 2026-08-29
// Was 'cmod_base', a pointer to the whole .bsp held in memory, with every loader
// indexing it as cmod_base + l->fileofs. CM_LoadMap now streams the file a lump at
// a time (see bspreader_t below), so this points at the one lump being loaded
// instead and the loaders read it from offset zero.
//
static byte * cmod_lump;
static cvar_t * map_noareas;

// These counters are referenced by Qcommon_Frame().
int c_pointcontents;
int c_traces;
int c_brush_traces;

void CM_InitBoxHull(void);
void FloodAreaConnections(void);

/*
===============================================================================

                    MAP LOADING

===============================================================================
*/

/*
=================
CMod_LoadSubmodels
=================
*/
void CMod_LoadSubmodels(lump_t * l)
{
    dmodel_t * in;
    cmodel_t * out;
    int i, j, count;

    in = (void *)cmod_lump;
    if (l->filelen % sizeof(*in))
        Com_Error(ERR_DROP, "MOD_LoadBmodel: funny lump size");

    count = l->filelen / sizeof(*in);

    if (count < 1)
        Com_Error(ERR_DROP, "Map with no models");

    if (count > MAX_MAP_MODELS)
        Com_Error(ERR_DROP, "Map has too many models");

    numcmodels = count;

    for (i = 0; i < count; i++, in++, out++)
    {
        out = &map_cmodels[i];

        for (j = 0; j < 3; j++)
        { // spread the mins / maxs by a pixel
            out->mins[j] = LittleFloat(in->mins[j]) - 1;
            out->maxs[j] = LittleFloat(in->maxs[j]) + 1;
            out->origin[j] = LittleFloat(in->origin[j]);
        }
        out->headnode = LittleLong(in->headnode);
    }
}

/*
=================
CMod_LoadSurfaces
=================
*/
void CMod_LoadSurfaces(lump_t * l)
{
    textureinfo_t * in;
    mapsurface_t * out;
    int i, count;

    in = (void *)cmod_lump;
    if (l->filelen % sizeof(*in))
        Com_Error(ERR_DROP, "MOD_LoadBmodel: funny lump size");

    count = l->filelen / sizeof(*in);
    if (count < 1)
        Com_Error(ERR_DROP, "Map with no surfaces");

    if (count > MAX_MAP_TEXINFO)
        Com_Error(ERR_DROP, "Map has too many surfaces");

    numtexinfo = count;
    out = map_surfaces;

    for (i = 0; i < count; i++, in++, out++)
    {
        strncpy(out->c.name, in->texture, sizeof(out->c.name) - 1);
        strncpy(out->rname, in->texture, sizeof(out->rname) - 1);
        out->c.flags = LittleLong(in->flags);
        out->c.value = LittleLong(in->value);
    }
}

/*
=================
CMod_LoadNodes
=================
*/
void CMod_LoadNodes(lump_t * l)
{
    dnode_t * in;
    int child;
    cnode_t * out;
    int i, j, count;

    in = (void *)cmod_lump;
    if (l->filelen % sizeof(*in))
        Com_Error(ERR_DROP, "MOD_LoadBmodel: funny lump size");

    count = l->filelen / sizeof(*in);

    if (count < 1)
        Com_Error(ERR_DROP, "Map has no nodes");

    if (count > MAX_MAP_NODES)
        Com_Error(ERR_DROP, "Map has too many nodes");

    out = map_nodes;

    numnodes = count;

    for (i = 0; i < count; i++, out++, in++)
    {
        out->plane = map_planes + LittleLong(in->planenum);
        for (j = 0; j < 2; j++)
        {
            child = LittleLong(in->children[j]);
            out->children[j] = child;
        }
    }
}

/*
=================
CMod_LoadBrushes
=================
*/
void CMod_LoadBrushes(lump_t * l)
{
    dbrush_t * in;
    cbrush_t * out;
    int i, count;

    in = (void *)cmod_lump;
    if (l->filelen % sizeof(*in))
        Com_Error(ERR_DROP, "MOD_LoadBmodel: funny lump size");

    count = l->filelen / sizeof(*in);

    if (count > MAX_MAP_BRUSHES)
        Com_Error(ERR_DROP, "Map has too many brushes");

    out = map_brushes;

    numbrushes = count;

    for (i = 0; i < count; i++, out++, in++)
    {
        out->firstbrushside = LittleLong(in->firstside);
        out->numsides = LittleLong(in->numsides);
        out->contents = LittleLong(in->contents);
    }
}

/*
=================
CMod_LoadLeafs
=================
*/
void CMod_LoadLeafs(lump_t * l)
{
    int i;
    cleaf_t * out;
    dleaf_t * in;
    int count;

    in = (void *)cmod_lump;
    if (l->filelen % sizeof(*in))
        Com_Error(ERR_DROP, "MOD_LoadBmodel: funny lump size");

    count = l->filelen / sizeof(*in);

    if (count < 1)
        Com_Error(ERR_DROP, "Map with no leafs");

    // [PS2_QUAKE] 2026-08-27
    // id tested MAX_MAP_PLANES here while filling map_leafs[MAX_MAP_LEAFS]. It was
    // harmless while every MAX_MAP_* was 65536, but the PS2 port sizes them apart,
    // so the wrong constant would let a map overrun the array.
    if (count > MAX_MAP_LEAFS)
        Com_Error(ERR_DROP, "Map has too many leafs");

    out = map_leafs;
    numleafs = count;
    numclusters = 0;

    for (i = 0; i < count; i++, in++, out++)
    {
        out->contents = LittleLong(in->contents);
        out->cluster = LittleShort(in->cluster);
        out->area = LittleShort(in->area);
        out->firstleafbrush = LittleShort(in->firstleafbrush);
        out->numleafbrushes = LittleShort(in->numleafbrushes);

        if (out->cluster >= numclusters)
            numclusters = out->cluster + 1;
    }

    if (map_leafs[0].contents != CONTENTS_SOLID)
        Com_Error(ERR_DROP, "Map leaf 0 is not CONTENTS_SOLID");

    solidleaf = 0;
    emptyleaf = -1;
    for (i = 1; i < numleafs; i++)
    {
        if (!map_leafs[i].contents)
        {
            emptyleaf = i;
            break;
        }
    }

    if (emptyleaf == -1)
        Com_Error(ERR_DROP, "Map does not have an empty leaf");
}

/*
=================
CMod_LoadPlanes
=================
*/
void CMod_LoadPlanes(lump_t * l)
{
    int i, j;
    cplane_t * out;
    dplane_t * in;
    int count;
    int bits;

    in = (void *)cmod_lump;
    if (l->filelen % sizeof(*in))
        Com_Error(ERR_DROP, "MOD_LoadBmodel: funny lump size");

    count = l->filelen / sizeof(*in);

    if (count < 1)
        Com_Error(ERR_DROP, "Map with no planes");

    // need to save space for box planes
    if (count > MAX_MAP_PLANES)
        Com_Error(ERR_DROP, "Map has too many planes");

    out = map_planes;
    numplanes = count;

    for (i = 0; i < count; i++, in++, out++)
    {
        bits = 0;
        for (j = 0; j < 3; j++)
        {
            out->normal[j] = LittleFloat(in->normal[j]);
            if (out->normal[j] < 0)
                bits |= 1 << j;
        }

        out->dist = LittleFloat(in->dist);
        out->type = LittleLong(in->type);
        out->signbits = bits;
    }
}

/*
=================
CMod_LoadLeafBrushes
=================
*/
void CMod_LoadLeafBrushes(lump_t * l)
{
    int i;
    unsigned short * out;
    unsigned short * in;
    int count;

    in = (void *)cmod_lump;
    if (l->filelen % sizeof(*in))
        Com_Error(ERR_DROP, "MOD_LoadBmodel: funny lump size");

    count = l->filelen / sizeof(*in);

    if (count < 1)
        Com_Error(ERR_DROP, "Map with no planes");

    // need to save space for box planes
    if (count > MAX_MAP_LEAFBRUSHES)
        Com_Error(ERR_DROP, "Map has too many leafbrushes");

    out = map_leafbrushes;
    numleafbrushes = count;

    for (i = 0; i < count; i++, in++, out++)
        *out = LittleShort(*in);
}

/*
=================
CMod_LoadBrushSides
=================
*/
void CMod_LoadBrushSides(lump_t * l)
{
    int i, j;
    cbrushside_t * out;
    dbrushside_t * in;
    int count;
    int num;

    in = (void *)cmod_lump;
    if (l->filelen % sizeof(*in))
        Com_Error(ERR_DROP, "MOD_LoadBmodel: funny lump size");

    count = l->filelen / sizeof(*in);

    // need to save space for box planes
    if (count > MAX_MAP_BRUSHSIDES)
        Com_Error(ERR_DROP, "Map has too many planes");

    out = map_brushsides;
    numbrushsides = count;

    for (i = 0; i < count; i++, in++, out++)
    {
        num = LittleShort(in->planenum);
        out->plane = &map_planes[num];
        j = LittleShort(in->texinfo);

        if (j >= numtexinfo)
            Com_Error(ERR_DROP, "Bad brushside texinfo");

        out->surface = &map_surfaces[j];
    }
}

/*
=================
CMod_LoadAreas
=================
*/
void CMod_LoadAreas(lump_t * l)
{
    int i;
    carea_t * out;
    darea_t * in;
    int count;

    in = (void *)cmod_lump;
    if (l->filelen % sizeof(*in))
        Com_Error(ERR_DROP, "MOD_LoadBmodel: funny lump size");

    count = l->filelen / sizeof(*in);

    if (count > MAX_MAP_AREAS)
        Com_Error(ERR_DROP, "Map has too many areas");

    out = map_areas;
    numareas = count;

    for (i = 0; i < count; i++, in++, out++)
    {
        out->numareaportals = LittleLong(in->numareaportals);
        out->firstareaportal = LittleLong(in->firstareaportal);
        out->floodvalid = 0;
        out->floodnum = 0;
    }
}

/*
=================
CMod_LoadAreaPortals
=================
*/
void CMod_LoadAreaPortals(lump_t * l)
{
    int i;
    dareaportal_t * out;
    dareaportal_t * in;
    int count;

    in = (void *)cmod_lump;
    if (l->filelen % sizeof(*in))
        Com_Error(ERR_DROP, "MOD_LoadBmodel: funny lump size");

    count = l->filelen / sizeof(*in);

    // [PS2_QUAKE] 2026-08-27
    // Same copy-paste as CMod_LoadLeafs above: this fills map_areaportals, not map_areas.
    if (count > MAX_MAP_AREAPORTALS)
        Com_Error(ERR_DROP, "Map has too many areaportals");

    out = map_areaportals;
    numareaportals = count;

    for (i = 0; i < count; i++, in++, out++)
    {
        out->portalnum = LittleLong(in->portalnum);
        out->otherarea = LittleLong(in->otherarea);
    }
}

/*
=================
CMod_LoadVisibility
=================
*/
void CMod_LoadVisibility(lump_t * l)
{
    int i;

    // [PS2_QUAKE] 2026-08-29
    // Read straight into map_visibility by the caller rather than copied out of
    // the file image, so this lump never passes through the lump scratch buffer.
    // The bounds check above the (former) memcpy is now the caller's, done before
    // the read rather than after it.
    numvisibility = l->filelen;

    // [PS2_QUAKE] 2026-08-29: bail out on a map with no vis data. id's version
    // fell through and byte-swapped whatever numclusters and bitofs[] the previous
    // map had left in the buffer. Harmless in practice - every shipped map has vis,
    // and CM_DecompressVis checks numvisibility before it reads any of it - but
    // CM_HasVisibility now exports "does this map have vis" to the refresh, so the
    // state behind that answer should be consistent rather than stale.
    if (numvisibility <= 0)
        return;

    map_vis->numclusters = LittleLong(map_vis->numclusters);
    for (i = 0; i < map_vis->numclusters; i++)
    {
        map_vis->bitofs[i][0] = LittleLong(map_vis->bitofs[i][0]);
        map_vis->bitofs[i][1] = LittleLong(map_vis->bitofs[i][1]);
    }
}

/*
=================
CMod_LoadEntityString
=================
*/
void CMod_LoadEntityString(lump_t * l)
{
    // [PS2_QUAKE] 2026-08-29
    // Read straight into map_entitystring by the caller, like the visibility lump
    // above; nothing left to do here but record the length.
    numentitychars = l->filelen;
}

/*
===============================================================================

[PS2_QUAKE] 2026-08-29
STREAMED BSP READING

CM_LoadMap used to FS_LoadFile the whole .bsp - up to 3.1 MB (space.bsp) - and
hold it while it filled the static map_* arrays. On a 32 MB console that was
fatal, because it happens during SV_SpawnServer while the *previous* map's
renderer geometry is still resident: leaving power2 for cool1 needed 2.7 MB and
there were 2.0 MB left.

Nothing here ever wants two lumps at once, so the file is read one lump at a time
into a scratch buffer sized from the header (the largest single transformed lump
across every stock map is 291,880 bytes, strike.bsp). VISIBILITY and ENTITIES are
verbatim copies, so they skip the scratch and are read straight into
map_visibility[] / map_entitystring[].

The loose end is the checksum. Com_BlockChecksum covered the whole file, but the
loaders now touch under a third of it, so CM_BspChecksum makes its own sequential
pass over every byte through the same scratch, feeding the streaming
Com_BlockChecksum* API. That costs one extra read of the file per map load (~25%
more I/O: the lump reads are the small half), and it is deliberate - keeping the
value identical to stock means this change cannot alter behaviour anywhere the
checksum is used.

It is worth knowing how little that is, before anyone is tempted to trade it away.
CS_MAPCHECKSUM is compared in exactly one place, cl_main.c's CL_RequestNextDownload,
against a string SV_SpawnServer wrote from this same file moments earlier - so any
deterministic function of the map would do. Demo playback does *not* check it: old
demos stuff a bare "precache", which takes CL_Precache_f's argc < 2 branch and
skips the comparison entirely. Hashing only the lumps we read would therefore work
today, and would save the extra pass; it is not done because it buys load time, not
memory, and this change is about memory.

===============================================================================
*/

typedef struct
{
    FILE * file;
    long base;   // where the .bsp starts in the stream (nonzero inside a pak)
    int length;  // whole-file length, for the checksum pass
    byte * scratch;
    int scratchSize;
} bspreader_t;

// The lumps loaded element by element, each read into the scratch buffer on its
// own. VISIBILITY and ENTITIES are deliberately absent - see above.
static const int cm_streamedLumps[] = {
    LUMP_TEXINFO, LUMP_LEAFS, LUMP_LEAFBRUSHES, LUMP_PLANES, LUMP_BRUSHES,
    LUMP_BRUSHSIDES, LUMP_MODELS, LUMP_NODES, LUMP_AREAS, LUMP_AREAPORTALS
};

#define CM_NUM_STREAMED_LUMPS ((int)(sizeof(cm_streamedLumps) / sizeof(cm_streamedLumps[0])))

// Enough for the largest lump we transform. Floored so that the checksum pass,
// which reuses this buffer, does not end up doing hundreds of tiny reads on a map
// whose lumps all happen to be small.
#define CM_MIN_SCRATCH_BYTES (64 * 1024)

static int CM_BspScratchBytes(const dheader_t * header)
{
    int i, len;
    int most = CM_MIN_SCRATCH_BYTES;

    for (i = 0; i < CM_NUM_STREAMED_LUMPS; i++)
    {
        len = header->lumps[cm_streamedLumps[i]].filelen;
        if (len > most)
            most = len;
    }
    return most;
}

static void CM_BspClose(bspreader_t * r)
{
    if (r->scratch)
    {
        Z_Free(r->scratch);
        r->scratch = NULL;
    }
    if (r->file)
    {
        FS_FCloseFile(r->file);
        r->file = NULL;
    }
    cmod_lump = NULL;
}

static void CM_BspSeek(bspreader_t * r, const lump_t * l)
{
    if (fseek(r->file, r->base + l->fileofs, SEEK_SET) != 0)
    {
        int ofs = l->fileofs;
        CM_BspClose(r);
        Com_Error(ERR_DROP, "CM_BspSeek: seek to lump offset %i failed", ofs);
    }
}

// Reads one lump into the scratch buffer and points cmod_lump at it for the
// CMod_Load* function that follows.
static void CM_BspReadLump(bspreader_t * r, const lump_t * l)
{
    // The scratch was sized from these same lengths, so this can only trip if the
    // header is inconsistent - better here than as a silent heap overrun, since
    // the CMod_Load* bounds checks all happen after the bytes have landed.
    if (l->filelen > r->scratchSize)
    {
        int len = l->filelen, cap = r->scratchSize;
        CM_BspClose(r);
        Com_Error(ERR_DROP, "CM_BspReadLump: lump of %i bytes overruns the %i byte scratch", len, cap);
    }

    cmod_lump = r->scratch;

    if (l->filelen <= 0)
        return;

    CM_BspSeek(r, l);
    FS_Read(r->scratch, l->filelen, r->file);
}

// Reads a lump straight to 'dest', bypassing the scratch. For the two the loaders
// copy verbatim; 'destSize' is the destination array's capacity.
static void CM_BspReadLumpInto(bspreader_t * r, const lump_t * l, void * dest, int destSize, const char * what)
{
    if (l->filelen > destSize)
    {
        int len = l->filelen;
        CM_BspClose(r);
        Com_Error(ERR_DROP, "CM_BspReadLumpInto: Map has too large %s lump (%i bytes)", what, len);
    }

    if (l->filelen <= 0)
        return;

    CM_BspSeek(r, l);
    FS_Read(dest, l->filelen, r->file);
}

// Hashes every byte of the file, so the value matches what the old whole-file
// Com_BlockChecksum produced. Reuses the scratch as the read buffer.
static unsigned CM_BspChecksum(bspreader_t * r)
{
    blockchecksum_t ctx;
    int remaining, chunk;

    Com_BlockChecksumBegin(&ctx);

    if (fseek(r->file, r->base, SEEK_SET) != 0)
    {
        CM_BspClose(r);
        Com_Error(ERR_DROP, "CM_BspChecksum: seek to start of file failed");
    }

    for (remaining = r->length; remaining > 0; remaining -= chunk)
    {
        chunk = (remaining < r->scratchSize) ? remaining : r->scratchSize;
        FS_Read(r->scratch, chunk, r->file);
        Com_BlockChecksumUpdate(&ctx, r->scratch, chunk);
    }

    return Com_BlockChecksumEnd(&ctx);
}

static void CM_BspOpen(bspreader_t * r, const char * name, dheader_t * header)
{
    int i;

    memset(r, 0, sizeof(*r));

    r->length = FS_FOpenFile(name, &r->file);
    if (r->length <= 0 || !r->file)
        Com_Error(ERR_DROP, "CM_BspOpen: Couldn't load %s", name);

    // Lump offsets are relative to the start of the .bsp, which inside a pak is
    // not the start of the stream - FS_FOpenFile leaves us seeked there.
    r->base = ftell(r->file);
    if (r->base < 0)
    {
        CM_BspClose(r);
        Com_Error(ERR_DROP, "CM_BspOpen: cannot tell position in %s", name);
    }

    FS_Read(header, sizeof(*header), r->file);
    for (i = 0; i < sizeof(dheader_t) / 4; i++)
        ((int *)header)[i] = LittleLong(((int *)header)[i]);

    if (header->version != BSPVERSION)
    {
        int version = header->version;
        CM_BspClose(r);
        Com_Error(ERR_DROP, "CM_BspOpen: %s has wrong version number (%i should be %i)",
                  name, version, BSPVERSION);
    }

    // Nothing validated the lump table before, because it indexed a buffer that
    // was already known to be 'length' bytes long. Now the offsets drive seeks and
    // the lengths drive reads, so a corrupt header has to be caught up front.
    for (i = 0; i < HEADER_LUMPS; i++)
    {
        if (header->lumps[i].fileofs < 0 || header->lumps[i].filelen < 0 ||
            header->lumps[i].fileofs + header->lumps[i].filelen > r->length)
        {
            CM_BspClose(r);
            Com_Error(ERR_DROP, "CM_BspOpen: %s has a bad lump table (lump %i)", name, i);
        }
    }

    r->scratchSize = CM_BspScratchBytes(header);
    r->scratch = Z_Malloc(r->scratchSize);
}

/*
==================
CM_LoadMap

Loads in the map and all submodels
==================
*/
cmodel_t * CM_LoadMap(char * name, qboolean clientload, unsigned * checksum)
{
    bspreader_t bsp;
    dheader_t header;
    static unsigned last_checksum;

    map_noareas = Cvar_Get("map_noareas", "0", 0);

    if (!strcmp(map_name, name) && (clientload || !Cvar_VariableValue("flushmap")))
    {
        *checksum = last_checksum;
        if (!clientload)
        {
            memset(portalopen, 0, sizeof(portalopen));
            FloodAreaConnections();
        }
        return &map_cmodels[0]; // still have the right version
    }

    // free old stuff
    numplanes = 0;
    numnodes = 0;
    numleafs = 0;
    numcmodels = 0;
    numvisibility = 0;
    numentitychars = 0;
    map_entitystring[0] = 0;
    map_name[0] = 0;

    if (!name || !name[0])
    {
        numleafs = 1;
        numclusters = 1;
        numareas = 1;
        *checksum = 0;
        return &map_cmodels[0]; // cinematic servers won't have anything at all
    }

    //
    // [PS2_QUAKE] 2026-08-29
    // Stream the file rather than loading it whole - see the STREAMED BSP READING
    // block above. CM_BspOpen validates the header and allocates the one scratch
    // buffer every lump below is read through; CM_BspClose frees it.
    //
    CM_BspOpen(&bsp, name, &header);

    last_checksum = LittleLong(CM_BspChecksum(&bsp));
    *checksum = last_checksum;

    // Load into the static map_* arrays, one lump at a time - each read overwrites
    // the scratch, so only one lump is ever in memory.
    //
    // Nothing here actually depends on the order: the cross-references (nodes and
    // brush sides pointing into map_planes, leafs into map_leafbrushes) are offsets
    // into fixed static arrays, which exist whether or not they have been filled
    // yet. id's order is kept anyway, so this change cannot shift behaviour.
#define CM_LOAD_LUMP(lump, loader)                 \
    do {                                           \
        CM_BspReadLump(&bsp, &header.lumps[lump]); \
        loader(&header.lumps[lump]);               \
    } while (0)

    CM_LOAD_LUMP(LUMP_TEXINFO,     CMod_LoadSurfaces);
    CM_LOAD_LUMP(LUMP_LEAFS,       CMod_LoadLeafs);
    CM_LOAD_LUMP(LUMP_LEAFBRUSHES, CMod_LoadLeafBrushes);
    CM_LOAD_LUMP(LUMP_PLANES,      CMod_LoadPlanes);
    CM_LOAD_LUMP(LUMP_BRUSHES,     CMod_LoadBrushes);
    CM_LOAD_LUMP(LUMP_BRUSHSIDES,  CMod_LoadBrushSides);
    CM_LOAD_LUMP(LUMP_MODELS,      CMod_LoadSubmodels);
    CM_LOAD_LUMP(LUMP_NODES,       CMod_LoadNodes);
    CM_LOAD_LUMP(LUMP_AREAS,       CMod_LoadAreas);
    CM_LOAD_LUMP(LUMP_AREAPORTALS, CMod_LoadAreaPortals);

#undef CM_LOAD_LUMP

    // These two are verbatim copies, so they are read straight into their arrays
    // and never occupy the scratch. The size check the loaders used to do after
    // the memcpy is now done by CM_BspReadLumpInto before the read.
    CM_BspReadLumpInto(&bsp, &header.lumps[LUMP_VISIBILITY],
                       map_visibility, MAX_MAP_VISIBILITY, "visibility");
    CMod_LoadVisibility(&header.lumps[LUMP_VISIBILITY]);

    CM_BspReadLumpInto(&bsp, &header.lumps[LUMP_ENTITIES],
                       map_entitystring, MAX_MAP_ENTSTRING, "entity");
    CMod_LoadEntityString(&header.lumps[LUMP_ENTITIES]);

    CM_BspClose(&bsp);

    CM_InitBoxHull();

    memset(portalopen, 0, sizeof(portalopen));
    FloodAreaConnections();

    strcpy(map_name, name);

    return &map_cmodels[0];
}

/*
==================
CM_InlineModel
==================
*/
cmodel_t * CM_InlineModel(char * name)
{
    int num;

    if (!name || name[0] != '*')
        Com_Error(ERR_DROP, "CM_InlineModel: bad name");

    num = atoi(name + 1);

    if (num < 1 || num >= numcmodels)
        Com_Error(ERR_DROP, "CM_InlineModel: bad number");

    return &map_cmodels[num];
}

int CM_NumClusters(void)
{
    return numclusters;
}

// [PS2_QUAKE] 2026-08-29
// For the renderer, which shares these PVS rows and needs the same "no vis data,
// everything is visible" shortcut CM_DecompressVis takes internally - it wants to
// mark every leaf and node at once rather than ask per cluster.
qboolean CM_HasVisibility(void)
{
    return numvisibility > 0;
}

// [PS2_QUAKE] 2026-08-29
// The .bsp currently loaded here, or "" for none. Also for the renderer: it reads
// this map's PVS through CM_ClusterPVS, so its world model has to be the same one,
// and this lets it say so up front instead of finding out from mismatched data.
const char * CM_MapName(void)
{
    return map_name;
}

int CM_NumInlineModels(void)
{
    return numcmodels;
}

char * CM_EntityString(void)
{
    return map_entitystring;
}

int CM_LeafContents(int leafnum)
{
    if (leafnum < 0 || leafnum >= numleafs)
        Com_Error(ERR_DROP, "CM_LeafContents: bad number");
    return map_leafs[leafnum].contents;
}

int CM_LeafCluster(int leafnum)
{
    if (leafnum < 0 || leafnum >= numleafs)
        Com_Error(ERR_DROP, "CM_LeafCluster: bad number");
    return map_leafs[leafnum].cluster;
}

int CM_LeafArea(int leafnum)
{
    if (leafnum < 0 || leafnum >= numleafs)
        Com_Error(ERR_DROP, "CM_LeafArea: bad number");
    return map_leafs[leafnum].area;
}

//=======================================================================

static int box_headnode;
static cplane_t * box_planes;
static cbrush_t * box_brush;
static cleaf_t * box_leaf;

/*
===================
CM_InitBoxHull

Set up the planes and nodes so that the six floats of a bounding box
can just be stored out and get a proper clipping hull structure.
===================
*/
void CM_InitBoxHull(void)
{
    int i;
    int side;
    cnode_t * c;
    cplane_t * p;
    cbrushside_t * s;

    box_headnode = numnodes;
    box_planes = &map_planes[numplanes];
    if (numnodes + 6 > MAX_MAP_NODES || numbrushes + 1 > MAX_MAP_BRUSHES || numleafbrushes + 1 > MAX_MAP_LEAFBRUSHES || numbrushsides + 6 > MAX_MAP_BRUSHSIDES || numplanes + 12 > MAX_MAP_PLANES)
        Com_Error(ERR_DROP, "Not enough room for box tree");

    box_brush = &map_brushes[numbrushes];
    box_brush->numsides = 6;
    box_brush->firstbrushside = numbrushsides;
    box_brush->contents = CONTENTS_MONSTER;

    box_leaf = &map_leafs[numleafs];
    box_leaf->contents = CONTENTS_MONSTER;
    box_leaf->firstleafbrush = numleafbrushes;
    box_leaf->numleafbrushes = 1;

    map_leafbrushes[numleafbrushes] = numbrushes;

    for (i = 0; i < 6; i++)
    {
        side = i & 1;

        // brush sides
        s = &map_brushsides[numbrushsides + i];
        s->plane = map_planes + (numplanes + i * 2 + side);
        s->surface = &nullsurface;

        // nodes
        c = &map_nodes[box_headnode + i];
        c->plane = map_planes + (numplanes + i * 2);
        c->children[side] = -1 - emptyleaf;
        if (i != 5)
            c->children[side ^ 1] = box_headnode + i + 1;
        else
            c->children[side ^ 1] = -1 - numleafs;

        // planes
        p = &box_planes[i * 2];
        p->type = i >> 1;
        p->signbits = 0;
        VectorClear(p->normal);
        p->normal[i >> 1] = 1;

        p = &box_planes[i * 2 + 1];
        p->type = 3 + (i >> 1);
        p->signbits = 0;
        VectorClear(p->normal);
        p->normal[i >> 1] = -1;
    }
}

/*
===================
CM_HeadnodeForBox

To keep everything totally uniform, bounding boxes are turned into small
BSP trees instead of being compared directly.
===================
*/
int CM_HeadnodeForBox(vec3_t mins, vec3_t maxs)
{
    box_planes[0].dist = maxs[0];
    box_planes[1].dist = -maxs[0];
    box_planes[2].dist = mins[0];
    box_planes[3].dist = -mins[0];
    box_planes[4].dist = maxs[1];
    box_planes[5].dist = -maxs[1];
    box_planes[6].dist = mins[1];
    box_planes[7].dist = -mins[1];
    box_planes[8].dist = maxs[2];
    box_planes[9].dist = -maxs[2];
    box_planes[10].dist = mins[2];
    box_planes[11].dist = -mins[2];

    return box_headnode;
}

/*
==================
CM_PointLeafnum_r
==================
*/
int CM_PointLeafnum_r(vec3_t p, int num)
{
    float d;
    cnode_t * node;
    cplane_t * plane;

    while (num >= 0)
    {
        node = map_nodes + num;
        plane = node->plane;

        if (plane->type < 3)
            d = p[plane->type] - plane->dist;
        else
            d = DotProduct(plane->normal, p) - plane->dist;
        if (d < 0)
            num = node->children[1];
        else
            num = node->children[0];
    }

    c_pointcontents++; // optimize counter

    return -1 - num;
}

int CM_PointLeafnum(vec3_t p)
{
    if (!numplanes)
        return 0; // sound may call this without map loaded
    return CM_PointLeafnum_r(p, 0);
}

static int leaf_topnode;
static int leaf_count;
static int leaf_maxcount;
static int * leaf_list;
static float * leaf_mins;
static float * leaf_maxs;

/*
=============
CM_BoxLeafnums_r

Fills in a list of all the leafs touched
=============
*/
void CM_BoxLeafnums_r(int nodenum)
{
    cplane_t * plane;
    cnode_t * node;
    int s;

    while (1)
    {
        if (nodenum < 0)
        {
            if (leaf_count >= leaf_maxcount)
            {
                //              Com_Printf ("CM_BoxLeafnums_r: overflow\n");
                return;
            }
            leaf_list[leaf_count++] = -1 - nodenum;
            return;
        }

        node = &map_nodes[nodenum];
        plane = node->plane;
        //      s = BoxOnPlaneSide (leaf_mins, leaf_maxs, plane);
        s = BOX_ON_PLANE_SIDE(leaf_mins, leaf_maxs, plane);
        if (s == 1)
            nodenum = node->children[0];
        else if (s == 2)
            nodenum = node->children[1];
        else
        { // go down both
            if (leaf_topnode == -1)
                leaf_topnode = nodenum;
            CM_BoxLeafnums_r(node->children[0]);
            nodenum = node->children[1];
        }
    }
}

int CM_BoxLeafnums_headnode(vec3_t mins, vec3_t maxs, int * list, int listsize, int headnode, int * topnode)
{
    leaf_list = list;
    leaf_count = 0;
    leaf_maxcount = listsize;
    leaf_mins = mins;
    leaf_maxs = maxs;

    leaf_topnode = -1;

    CM_BoxLeafnums_r(headnode);

    if (topnode)
        *topnode = leaf_topnode;

    return leaf_count;
}

int CM_BoxLeafnums(vec3_t mins, vec3_t maxs, int * list, int listsize, int * topnode)
{
    return CM_BoxLeafnums_headnode(mins, maxs, list, listsize, map_cmodels[0].headnode, topnode);
}

/*
==================
CM_PointContents
==================
*/
int CM_PointContents(vec3_t p, int headnode)
{
    int l;

    if (!numnodes) // map not loaded
        return 0;

    l = CM_PointLeafnum_r(p, headnode);

    return map_leafs[l].contents;
}

/*
==================
CM_TransformedPointContents

Handles offseting and rotation of the end points for moving and
rotating entities
==================
*/
int CM_TransformedPointContents(vec3_t p, int headnode, vec3_t origin, vec3_t angles)
{
    vec3_t p_l;
    vec3_t temp;
    vec3_t forward, right, up;
    int l;

    // subtract origin offset
    VectorSubtract(p, origin, p_l);

    // rotate start and end into the models frame of reference
    if (headnode != box_headnode &&
        (angles[0] || angles[1] || angles[2]))
    {
        AngleVectors(angles, forward, right, up);

        VectorCopy(p_l, temp);
        p_l[0] = DotProduct(temp, forward);
        p_l[1] = -DotProduct(temp, right);
        p_l[2] = DotProduct(temp, up);
    }

    l = CM_PointLeafnum_r(p_l, headnode);

    return map_leafs[l].contents;
}

/*
===============================================================================

BOX TRACING

===============================================================================
*/

// 1/32 epsilon to keep floating point happy
#define DIST_EPSILON (0.03125)

static vec3_t trace_start;
static vec3_t trace_end;
static vec3_t trace_mins;
static vec3_t trace_maxs;
static vec3_t trace_extents;

static trace_t trace_trace;
static int trace_contents;
static qboolean trace_ispoint; // optimized case

/*
================
CM_ClipBoxToBrush
================
*/
void CM_ClipBoxToBrush(vec3_t mins, vec3_t maxs, vec3_t p1, vec3_t p2, trace_t * trace, cbrush_t * brush)
{
    int i, j;
    cplane_t *plane, *clipplane;
    float dist;
    float enterfrac, leavefrac;
    vec3_t ofs;
    float d1, d2;
    qboolean getout, startout;
    float f;
    cbrushside_t *side, *leadside;

    enterfrac = -1;
    leavefrac = 1;
    clipplane = NULL;

    if (!brush->numsides)
        return;

    c_brush_traces++;

    getout = false;
    startout = false;
    leadside = NULL;

    for (i = 0; i < brush->numsides; i++)
    {
        side = &map_brushsides[brush->firstbrushside + i];
        plane = side->plane;

        // FIXME: special case for axial

        if (!trace_ispoint)
        { // general box case

            // push the plane out apropriately for mins/maxs

            // FIXME: use signbits into 8 way lookup for each mins/maxs
            for (j = 0; j < 3; j++)
            {
                if (plane->normal[j] < 0)
                    ofs[j] = maxs[j];
                else
                    ofs[j] = mins[j];
            }
            dist = DotProduct(ofs, plane->normal);
            dist = plane->dist - dist;
        }
        else
        { // special point case
            dist = plane->dist;
        }

        d1 = DotProduct(p1, plane->normal) - dist;
        d2 = DotProduct(p2, plane->normal) - dist;

        if (d2 > 0)
            getout = true; // endpoint is not in solid
        if (d1 > 0)
            startout = true;

        // if completely in front of face, no intersection
        if (d1 > 0 && d2 >= d1)
            return;

        if (d1 <= 0 && d2 <= 0)
            continue;

        // crosses face
        if (d1 > d2)
        { // enter
            f = (d1 - DIST_EPSILON) / (d1 - d2);
            if (f > enterfrac)
            {
                enterfrac = f;
                clipplane = plane;
                leadside = side;
            }
        }
        else
        { // leave
            f = (d1 + DIST_EPSILON) / (d1 - d2);
            if (f < leavefrac)
                leavefrac = f;
        }
    }

    if (!startout)
    { // original point was inside brush
        trace->startsolid = true;
        if (!getout)
            trace->allsolid = true;
        return;
    }
    if (enterfrac < leavefrac)
    {
        if (enterfrac > -1 && enterfrac < trace->fraction)
        {
            if (enterfrac < 0)
                enterfrac = 0;
            trace->fraction = enterfrac;
            trace->plane = *clipplane;
            trace->surface = &(leadside->surface->c);
            trace->contents = brush->contents;
        }
    }
}

/*
================
CM_TestBoxInBrush
================
*/
void CM_TestBoxInBrush(vec3_t mins, vec3_t maxs, vec3_t p1, trace_t * trace, cbrush_t * brush)
{
    int i, j;
    cplane_t * plane;
    float dist;
    vec3_t ofs;
    float d1;
    cbrushside_t * side;

    if (!brush->numsides)
        return;

    for (i = 0; i < brush->numsides; i++)
    {
        side = &map_brushsides[brush->firstbrushside + i];
        plane = side->plane;

        // FIXME: special case for axial

        // general box case

        // push the plane out apropriately for mins/maxs

        // FIXME: use signbits into 8 way lookup for each mins/maxs
        for (j = 0; j < 3; j++)
        {
            if (plane->normal[j] < 0)
                ofs[j] = maxs[j];
            else
                ofs[j] = mins[j];
        }
        dist = DotProduct(ofs, plane->normal);
        dist = plane->dist - dist;

        d1 = DotProduct(p1, plane->normal) - dist;

        // if completely in front of face, no intersection
        if (d1 > 0)
            return;
    }

    // inside this brush
    trace->startsolid = trace->allsolid = true;
    trace->fraction = 0;
    trace->contents = brush->contents;
}

/*
================
CM_TraceToLeaf
================
*/
void CM_TraceToLeaf(int leafnum)
{
    int k;
    int brushnum;
    cleaf_t * leaf;
    cbrush_t * b;

    leaf = &map_leafs[leafnum];
    if (!(leaf->contents & trace_contents))
        return;
    // trace line against all brushes in the leaf
    for (k = 0; k < leaf->numleafbrushes; k++)
    {
        brushnum = map_leafbrushes[leaf->firstleafbrush + k];
        b = &map_brushes[brushnum];
        if (b->checkcount == checkcount)
            continue; // already checked this brush in another leaf

        b->checkcount = checkcount;

        if (!(b->contents & trace_contents))
            continue;

        CM_ClipBoxToBrush(trace_mins, trace_maxs, trace_start, trace_end, &trace_trace, b);
        if (!trace_trace.fraction)
            return;
    }
}

/*
================
CM_TestInLeaf
================
*/
void CM_TestInLeaf(int leafnum)
{
    int k;
    int brushnum;
    cleaf_t * leaf;
    cbrush_t * b;

    leaf = &map_leafs[leafnum];
    if (!(leaf->contents & trace_contents))
        return;
    // trace line against all brushes in the leaf
    for (k = 0; k < leaf->numleafbrushes; k++)
    {
        brushnum = map_leafbrushes[leaf->firstleafbrush + k];
        b = &map_brushes[brushnum];
        if (b->checkcount == checkcount)
            continue; // already checked this brush in another leaf

        b->checkcount = checkcount;

        if (!(b->contents & trace_contents))
            continue;

        CM_TestBoxInBrush(trace_mins, trace_maxs, trace_start, &trace_trace, b);
        if (!trace_trace.fraction)
            return;
    }
}

/*
==================
CM_RecursiveHullCheck
==================
*/
void CM_RecursiveHullCheck(int num, float p1f, float p2f, vec3_t p1, vec3_t p2)
{
    cnode_t * node;
    cplane_t * plane;
    float t1, t2, offset;
    float frac, frac2;
    float idist;
    int i;
    vec3_t mid;
    int side;
    float midf;

    if (trace_trace.fraction <= p1f)
        return; // already hit something nearer

    // if < 0, we are in a leaf node
    if (num < 0)
    {
        CM_TraceToLeaf(-1 - num);
        return;
    }

    //
    // find the point distances to the seperating plane
    // and the offset for the size of the box
    //
    node = map_nodes + num;
    plane = node->plane;

    if (plane->type < 3)
    {
        t1 = p1[plane->type] - plane->dist;
        t2 = p2[plane->type] - plane->dist;
        offset = trace_extents[plane->type];
    }
    else
    {
        t1 = DotProduct(plane->normal, p1) - plane->dist;
        t2 = DotProduct(plane->normal, p2) - plane->dist;
        if (trace_ispoint)
        {
            offset = 0;
        }
        else
        {
            offset = PS2Quake_Fabsf(trace_extents[0] * plane->normal[0]) +
                     PS2Quake_Fabsf(trace_extents[1] * plane->normal[1]) +
                     PS2Quake_Fabsf(trace_extents[2] * plane->normal[2]);
        }
    }

#if 0
CM_RecursiveHullCheck (node->children[0], p1f, p2f, p1, p2);
CM_RecursiveHullCheck (node->children[1], p1f, p2f, p1, p2);
return;
#endif

    // see which sides we need to consider
    if (t1 >= offset && t2 >= offset)
    {
        CM_RecursiveHullCheck(node->children[0], p1f, p2f, p1, p2);
        return;
    }
    if (t1 < -offset && t2 < -offset)
    {
        CM_RecursiveHullCheck(node->children[1], p1f, p2f, p1, p2);
        return;
    }

    // put the crosspoint DIST_EPSILON pixels on the near side
    if (t1 < t2)
    {
        idist = 1.0 / (t1 - t2);
        side = 1;
        frac2 = (t1 + offset + DIST_EPSILON) * idist;
        frac = (t1 - offset + DIST_EPSILON) * idist;
    }
    else if (t1 > t2)
    {
        idist = 1.0 / (t1 - t2);
        side = 0;
        frac2 = (t1 - offset - DIST_EPSILON) * idist;
        frac = (t1 + offset + DIST_EPSILON) * idist;
    }
    else
    {
        side = 0;
        frac = 1;
        frac2 = 0;
    }

    // move up to the node
    if (frac < 0)
        frac = 0;
    if (frac > 1)
        frac = 1;

    midf = p1f + (p2f - p1f) * frac;
    for (i = 0; i < 3; i++)
        mid[i] = p1[i] + frac * (p2[i] - p1[i]);

    CM_RecursiveHullCheck(node->children[side], p1f, midf, p1, mid);

    // go past the node
    if (frac2 < 0)
        frac2 = 0;
    if (frac2 > 1)
        frac2 = 1;

    midf = p1f + (p2f - p1f) * frac2;
    for (i = 0; i < 3; i++)
        mid[i] = p1[i] + frac2 * (p2[i] - p1[i]);

    CM_RecursiveHullCheck(node->children[side ^ 1], midf, p2f, mid, p2);
}

//======================================================================

/*
==================
CM_BoxTrace
==================
*/
trace_t CM_BoxTrace(vec3_t start, vec3_t end,
                    vec3_t mins, vec3_t maxs,
                    int headnode, int brushmask)
{
    int i;

    checkcount++; // for multi-check avoidance
    c_traces++;   // for statistics, may be zeroed

    // fill in a default trace
    memset(&trace_trace, 0, sizeof(trace_trace));
    trace_trace.fraction = 1;
    trace_trace.surface = &(nullsurface.c);

    if (!numnodes) // map not loaded
        return trace_trace;

    trace_contents = brushmask;
    VectorCopy(start, trace_start);
    VectorCopy(end, trace_end);
    VectorCopy(mins, trace_mins);
    VectorCopy(maxs, trace_maxs);

    //
    // check for position test special case
    //
    if (start[0] == end[0] && start[1] == end[1] && start[2] == end[2])
    {
        int leafs[1024];
        int i, numleafs;
        vec3_t c1, c2;
        int topnode;

        VectorAdd(start, mins, c1);
        VectorAdd(start, maxs, c2);
        for (i = 0; i < 3; i++)
        {
            c1[i] -= 1;
            c2[i] += 1;
        }

        numleafs = CM_BoxLeafnums_headnode(c1, c2, leafs, 1024, headnode, &topnode);
        for (i = 0; i < numleafs; i++)
        {
            CM_TestInLeaf(leafs[i]);
            if (trace_trace.allsolid)
                break;
        }
        VectorCopy(start, trace_trace.endpos);
        return trace_trace;
    }

    //
    // check for point special case
    //
    if (mins[0] == 0 && mins[1] == 0 && mins[2] == 0 && maxs[0] == 0 && maxs[1] == 0 && maxs[2] == 0)
    {
        trace_ispoint = true;
        VectorClear(trace_extents);
    }
    else
    {
        trace_ispoint = false;
        trace_extents[0] = -mins[0] > maxs[0] ? -mins[0] : maxs[0];
        trace_extents[1] = -mins[1] > maxs[1] ? -mins[1] : maxs[1];
        trace_extents[2] = -mins[2] > maxs[2] ? -mins[2] : maxs[2];
    }

    //
    // general sweeping through world
    //
    CM_RecursiveHullCheck(headnode, 0, 1, start, end);

    if (trace_trace.fraction == 1)
    {
        VectorCopy(end, trace_trace.endpos);
    }
    else
    {
        for (i = 0; i < 3; i++)
            trace_trace.endpos[i] = start[i] + trace_trace.fraction * (end[i] - start[i]);
    }
    return trace_trace;
}

/*
==================
CM_TransformedBoxTrace

Handles offseting and rotation of the end points for moving and
rotating entities
==================
*/
trace_t CM_TransformedBoxTrace(vec3_t start, vec3_t end,
                               vec3_t mins, vec3_t maxs,
                               int headnode, int brushmask,
                               vec3_t origin, vec3_t angles)
{
    trace_t trace;
    vec3_t start_l, end_l;
    vec3_t a;
    vec3_t forward, right, up;
    vec3_t temp;
    qboolean rotated;

    // subtract origin offset
    VectorSubtract(start, origin, start_l);
    VectorSubtract(end, origin, end_l);

    // rotate start and end into the models frame of reference
    if (headnode != box_headnode &&
        (angles[0] || angles[1] || angles[2]))
    {
        rotated = true;
    }
    else
    {
        rotated = false;
    }

    if (rotated)
    {
        AngleVectors(angles, forward, right, up);

        VectorCopy(start_l, temp);
        start_l[0] = DotProduct(temp, forward);
        start_l[1] = -DotProduct(temp, right);
        start_l[2] = DotProduct(temp, up);

        VectorCopy(end_l, temp);
        end_l[0] = DotProduct(temp, forward);
        end_l[1] = -DotProduct(temp, right);
        end_l[2] = DotProduct(temp, up);
    }

    // sweep the box through the model
    trace = CM_BoxTrace(start_l, end_l, mins, maxs, headnode, brushmask);

    if (rotated && trace.fraction != 1.0)
    {
        // FIXME: figure out how to do this with existing angles
        VectorNegate(angles, a);
        AngleVectors(a, forward, right, up);

        VectorCopy(trace.plane.normal, temp);
        trace.plane.normal[0] = DotProduct(temp, forward);
        trace.plane.normal[1] = -DotProduct(temp, right);
        trace.plane.normal[2] = DotProduct(temp, up);
    }

    trace.endpos[0] = start[0] + trace.fraction * (end[0] - start[0]);
    trace.endpos[1] = start[1] + trace.fraction * (end[1] - start[1]);
    trace.endpos[2] = start[2] + trace.fraction * (end[2] - start[2]);

    return trace;
}

/*
===============================================================================

PVS / PHS

===============================================================================
*/

static void CM_DecompressVis(byte * in, byte * out)
{
    int c;
    byte * out_p;
    int row;

    row = (numclusters + 7) >> 3;
    out_p = out;

    if (!in || !numvisibility)
    { // no vis info, so make all visible
        while (row)
        {
            *out_p++ = 0xff;
            row--;
        }
        return;
    }

    do
    {
        if (*in)
        {
            *out_p++ = *in++;
            continue;
        }

        c = in[1];
        in += 2;
        if ((out_p - out) + c > row)
        {
            c = row - (out_p - out);
            Com_DPrintf("warning: Vis decompression overrun\n");
        }
        while (c)
        {
            *out_p++ = 0;
            c--;
        }
    } while (out_p - out < row);
}

//
// [PS2_QUAKE] 2026-08-29
// 16-byte aligned because the renderer shares these rows now (it dropped its own
// copy of the visibility lump and its own decompressor - see MarkLeaves in
// ps2/renderer/render_view.cpp). Combining two clusters' PVS there ORs the rows a
// word at a time, and MIPS will not load a word from an odd address.
//
static byte pvsrow[MAX_MAP_LEAFS / 8] __attribute__((aligned(16)));
static byte phsrow[MAX_MAP_LEAFS / 8] __attribute__((aligned(16)));

byte * CM_ClusterPVS(int cluster)
{
    if (cluster == -1)
        memset(pvsrow, 0, (numclusters + 7) >> 3);
    else
        CM_DecompressVis(map_visibility + map_vis->bitofs[cluster][DVIS_PVS], pvsrow);
    return pvsrow;
}

byte * CM_ClusterPHS(int cluster)
{
    if (cluster == -1)
        memset(phsrow, 0, (numclusters + 7) >> 3);
    else
        CM_DecompressVis(map_visibility + map_vis->bitofs[cluster][DVIS_PHS], phsrow);
    return phsrow;
}

/*
===============================================================================

AREAPORTALS

===============================================================================
*/

void FloodArea_r(carea_t * area, int floodnum)
{
    int i;
    dareaportal_t * p;

    if (area->floodvalid == floodvalid)
    {
        if (area->floodnum == floodnum)
            return;
        Com_Error(ERR_DROP, "FloodArea_r: reflooded");
    }

    area->floodnum = floodnum;
    area->floodvalid = floodvalid;
    p = &map_areaportals[area->firstareaportal];
    for (i = 0; i < area->numareaportals; i++, p++)
    {
        if (portalopen[p->portalnum])
            FloodArea_r(&map_areas[p->otherarea], floodnum);
    }
}

/*
====================
FloodAreaConnections
====================
*/
void FloodAreaConnections(void)
{
    int i;
    carea_t * area;
    int floodnum;

    // all current floods are now invalid
    floodvalid++;
    floodnum = 0;

    // area 0 is not used
    for (i = 1; i < numareas; i++)
    {
        area = &map_areas[i];
        if (area->floodvalid == floodvalid)
            continue; // already flooded into
        floodnum++;
        FloodArea_r(area, floodnum);
    }
}

void CM_SetAreaPortalState(int portalnum, qboolean open)
{
    if (portalnum > numareaportals)
        Com_Error(ERR_DROP, "areaportal > numareaportals");

    portalopen[portalnum] = open;
    FloodAreaConnections();
}

qboolean CM_AreasConnected(int area1, int area2)
{
    if (map_noareas->value)
        return true;

    if (area1 > numareas || area2 > numareas)
        Com_Error(ERR_DROP, "area > numareas");

    if (map_areas[area1].floodnum == map_areas[area2].floodnum)
        return true;
    return false;
}

/*
=================
CM_WriteAreaBits

Writes a length byte followed by a bit vector of all the areas
that area in the same flood as the area parameter

This is used by the client refreshes to cull visibility
=================
*/
int CM_WriteAreaBits(byte * buffer, int area)
{
    int i;
    int floodnum;
    int bytes;

    bytes = (numareas + 7) >> 3;

    if (map_noareas->value)
    { // for debugging, send everything
        memset(buffer, 255, bytes);
    }
    else
    {
        memset(buffer, 0, bytes);

        floodnum = map_areas[area].floodnum;
        for (i = 0; i < numareas; i++)
        {
            if (map_areas[i].floodnum == floodnum || !area)
                buffer[i >> 3] |= 1 << (i & 7);
        }
    }

    return bytes;
}

/*
===================
CM_WritePortalState

Writes the portal state to a savegame file
===================
*/
void CM_WritePortalState(FILE * f)
{
    fwrite(portalopen, sizeof(portalopen), 1, f);
}

/*
===================
CM_ReadPortalState

Reads the portal state from a savegame file
and recalculates the area connections
===================
*/
void CM_ReadPortalState(FILE * f)
{
    FS_Read(portalopen, sizeof(portalopen), f);
    FloodAreaConnections();
}

/*
=============
CM_HeadnodeVisible

Returns true if any leaf under headnode has a cluster that
is potentially visible
=============
*/
qboolean CM_HeadnodeVisible(int nodenum, byte * visbits)
{
    int leafnum;
    int cluster;
    cnode_t * node;

    if (nodenum < 0)
    {
        leafnum = -1 - nodenum;
        cluster = map_leafs[leafnum].cluster;

        if (cluster == -1)
        {
            return false;
        }
        if (visbits[cluster >> 3] & (1 << (cluster & 7)))
        {
            return true;
        }
        return false;
    }

    node = &map_nodes[nodenum];
    if (CM_HeadnodeVisible(node->children[0], visbits))
    {
        return true;
    }
    return CM_HeadnodeVisible(node->children[1], visbits);
}
