#pragma once
/* ================================================================================================
 * File: vram.h
 * Brief: GS VRAM texture heap: tracks which textures are resident in the VRAM left
 *        over after the framebuffers and z-buffer, handing out space on demand and
 *        evicting the least-recently-bound textures when full. Pure bookkeeping -
 *        the DMA uploads and GS synchronisation stay with gs::EnsureTextureResident.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::tex { struct Texture; }

namespace ps2::vram {

// 32-bits type to represent VRAM addresses.
enum struct Address : int
{
    Invalid = -1,
};

// Takes ownership of GS VRAM from 'heapBaseWords' (a word address) up to the 4 MB
// end. Call once, from gs::Init(), after the framebuffer/z-buffer allocations.
void Init(int heapBaseWords);

// Advances the LRU clock. Call once per frame, from gs::BeginFrame()/EndFrame().
void BeginFrame();
void EndFrame();

// VRAM words the texture occupies: the whole GS page grid it covers. libgraph's
// graph_vram_size undercounts here - see the .cpp for why.
int TextureFootprintWords(int width, int height, int psm);

// Allocates 'sizeWords' for 'texture', evicting least-recently-bound textures as
// needed (never ones bound this frame; asserts if the frame's working set cannot
// fit). Evicted textures get vramAddr = kNotResident and self-heal on their next
// bind. Returns the block's word address; sets *outEvicted when anything was
// evicted - the caller must sync the GS before writing over reused VRAM.
Address Allocate(const tex::Texture & texture, int sizeWords, bool * outEvicted);

// Marks the (resident) texture as bound this frame, protecting it from eviction
// until the next frame.
void Touch(const tex::Texture & texture);

// True when the (resident) texture was already bound this frame - its draws may
// still be queued in the unsent frame packet, so overwriting its VRAM (dynamic
// texture re-upload) must sync the GS first.
bool BoundThisFrame(const tex::Texture & texture);

// Returns the texture's block to the heap and marks it non-resident (no-op when
// not resident). The freed range may be handed out without an eviction, so the
// caller must treat it like evicted VRAM: sync the GS before writing over it.
void Free(const tex::Texture & texture);

} // namespace ps2::vram
