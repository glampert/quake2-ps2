#pragma once
/* ================================================================================================
 * File: clut.h
 * Brief: The 256-entry Color Lookup Tables the GS's indexed pixel formats sample
 *        through. Ours are built at startup, rebuilt only when the brightness curve
 *        changes (gamma.h), and live at fixed VRAM addresses outside the texture
 *        heap - see gs.cpp, which owns the instances and their upload; this header
 *        is just their layout and the entry-order arithmetic the GS imposes.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/vram.h"
#include <tamtypes.h>

namespace ps2::tex {

// A CLUT: 256 RGBA entries plus where they live in GS VRAM.
//
// The entries are held in the arrangement the GS *reads* them in, not the
// arrangement they are written in - see Csm1Index - so the buffer is ready to
// upload verbatim and an index into it is not the palette index. Build it
// through the helpers below rather than assigning entries directly.
struct Clut final
{
    static constexpr int kNumEntries = 256;

    // A CLUT uploads as a 16x16 PSMCT32 image (256 words = 4 GS blocks), with
    // the transfer's destination buffer at the minimum TBW granularity.
    static constexpr int kImageWidth    = 16;
    static constexpr int kImageHeight   = 16;
    static constexpr int kTransferWidth = 64;

    alignas(16) u32 entries[kNumEntries];
    vram::Address vramAddr = vram::Address::Invalid;

    // Where the GS reads palette entry 'index' from in CSM1 storage mode: within
    // each 32-entry group the two middle 8-entry blocks swap, i.e. index bits 3
    // and 4 exchange (see ps2stuff GS::ReorderClut).
    static constexpr int Csm1Index(int index)
    {
        return (index & ~0x18) | ((index & 0x08) << 1) | ((index & 0x10) >> 1);
    }

    // Fills from 256 linear RGBA entries - a palette in the order everything
    // outside the GS thinks of it in.
    void BuildFromPalette(const u32 * palette)
    {
        for (int i = 0; i < kNumEntries; ++i)
        {
            entries[Csm1Index(i)] = palette[i];
        }
    }

    // Fills with the alpha ramp sampled by PixelFormat::Alpha8: the index *is*
    // the alpha, and the colour is pinned at the GS modulate identity (128) so
    // the texture leaves the primitive's own colour untouched. Alpha 0x80 is 1.0
    // on the GS, so the ramp tops out at 128 rather than 255. Index 0 maps to
    // alpha 0 on purpose - the batch alpha test drops those texels, which is what
    // cuts the particle images out.
    void BuildAlphaRamp()
    {
        for (int i = 0; i < kNumEntries; ++i)
        {
            const u32 alpha = static_cast<u32>((i + 1) >> 1);
            entries[Csm1Index(i)] = 128u | (128u << 8) | (128u << 16) | (alpha << 24);
        }
    }

    // Fills with the light ramp sampled by PixelFormat::Light8, the lightmap atlases: as the
    // alpha ramp, but the alpha is 'alphas[index]', the luxel's light through the brightness
    // curve (gamma.h), which is the alpha ramp itself at gamma 1. Index 0 still reads as alpha 0,
    // which the alpha test drops, so the atlases floor their luxels at 1.
    void BuildLightRamp(const u8 * alphas)
    {
        for (int i = 0; i < kNumEntries; ++i)
        {
            entries[Csm1Index(i)] = 128u | (128u << 8) | (128u << 16) | (static_cast<u32>(alphas[i]) << 24);
        }
    }
};

} // namespace ps2::tex
