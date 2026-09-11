#pragma once
/* ================================================================================================
 * File: model_load.h
 * Brief: Loaders for the Quake 2 on-disk model formats (world map, sprite, md2).
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <cstdio> // FILE

namespace ps2::mod {

struct ModelInstance;

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
// frame DMA chain there (see frame_chain.h).
//
// The two owners never overlap in time: the loader claims this only while it is parsing
// a .bsp, and no frame is being built then. LoadBrushModel drains the chain before it
// takes the memory back, so the handover is one-directional and explicit. Null base
// before ReserveWorldArena has run.
struct ScratchBlock
{
    void * base;
    unsigned int sizeBytes;
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

} // namespace ps2::mod
