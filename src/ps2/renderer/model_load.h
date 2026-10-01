#pragma once
/* ================================================================================================
 * File: model_load.h
 * Brief: Loaders for the Quake 2 on-disk model formats (world map, sprite, md2).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"         // dmdl_t (q_files.h)
#include "ps2/renderer/model.h" // AliasVertex

#include <cstdio> // FILE

namespace ps2::mod {

// Reserves the block the world hunk and the streamed loader's lump scratch are
// carved out of, for the life of the program. Call once at renderer init, before
// any map loads. Neither of those two is ever handed back to the general heap:
// they are the largest and most frequently recycled allocations in the game, and
// leaving them to a non-moving allocator fragments it until no contiguous run big
// enough survives.
void ReserveWorldArena();

// True if 'ptr' is the base of the reserved arena, i.e. memory that must never be
// passed to ps2::heap::Free. ModelCache::Unload checks this before releasing a hunk.
bool IsWorldArenaBlock(const void * ptr);

// The lump scratch half of the reserved arena, handed out so the renderer can keep its
// frame DMA chain there (see cmd_buffer.h).
//
// The two owners never overlap in time: the loader claims this only while it is parsing
// a .bsp, and no frame is being built then. LoadBrushModel drains the chain before it
// takes the memory back, so the handover is one-directional and explicit. Null base
// before ReserveWorldArena has run.
struct ScratchBlock
{
    void * base;
    u32 sizeBytes;
};
ScratchBlock WorldScratchBlock();

// The map's submodel table, handed back rather than parked in the hunk.
//
// It points at the raw dmodel_t array in the lump scratch: ModelCache's inline
// model setup is its only reader and runs immediately after this returns, so a
// converted copy would hold up to 9 KB of world hunk for the life of the map to
// be read exactly once. Valid until the next brush model load.
struct SubModelTable
{
    const void * models; // dmodel_t[count]; the EE reads the file layout directly
    int count;
};

// All three take an open file positioned at the model's first byte, and none of
// them closes it - the caller opened it to read the format tag and owns it.
//
// All three read straight into their final destination rather than staging the
// file first: a sprite is stored in the hunk exactly as it sits on disk, a .bsp
// is read lump by lump into a hunk laid out up front, and an MD2 reads its
// keyframes in place around a conversion pass over its glcmds. None of them ever
// holds a whole model file and a copy of it at once, which for the biggest MD2
// in pak0 would be ~2 MB. The MD2 conversion's one scratch buffer is its glcmds
// block, 32 KB at worst and freed before the keyframes are read.
bool LoadBrushModel(ModelInstance & outModel, FILE * file, const char * fileName, SubModelTable & outSubModels);
bool LoadSpriteModel(ModelInstance & outModel, FILE * file, int fileLen);
bool LoadAliasMD2Model(ModelInstance & outModel, FILE * file, int fileLen);

// ------------------------------------------------------------------------------------------------
// MD2 parsing, shared with the memory card icon (save/mc_icon.cpp), which builds its model
// from one in a buffer rather than through the model cache.
// ------------------------------------------------------------------------------------------------

// Checks an MD2 header against the size of the file it came from: the version, counts within
// the engine's limits, a framesize that matches the vertex count, and the skin names, glcmds
// and keyframes all inside the file. Says what is wrong on the console when it returns false.
bool ValidateMD2Header(const dmdl_t & header, int fileLen, const char * name);

// Expands a model's glcmds - its triangle strips and fans - into a flat list of three
// AliasVertex per triangle, in triangle order: keyframe vertex index and normalised skin
// coordinates. Every vertex index is checked against numXyz and the total against maxTris.
// Returns the triangle count, or -1 (with the reason on the console) if the list is malformed.
int ExpandGLCmdsToTriangles(const s32 * glcmds, int numWords, int numXyz, int maxTris,
                            AliasVertex * out, const char * modelName);

} // namespace ps2::mod
