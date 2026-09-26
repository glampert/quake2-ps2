#pragma once
/* ================================================================================================
 * File: image_load.h
 * Brief: Loaders for the Quake II on-disk image formats (PCX/WAL/TGA), decoding
 *        into pixel buffers the GS texture pipeline consumes directly. The 8-bit
 *        formats stay as palette indices sampled through the shared global-palette
 *        CLUT (PixelFormat::Palette8); TGA expands to RGBA32.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>

namespace ps2::tex {

// On success every loader hands back a pixel buffer allocated with
// ps2::heap::AllocAligned(16, ...) - DMA-ready - that the caller
// owns and frees with ps2::heap::Free (size = width * height * bytes per texel).
// On failure they warn via Com_DPrintf and return false with nothing allocated.
//
// The image comes back at the size the file gives it, powers of two or not.
// Making a non-power-of-two image sampleable belongs to the texture cache,
// which knows what the image is for: the tiling ones are resampled up to the
// GS's TEX0 extent, the rest take a coordinate scale (see tex::StScaleFor).

// PCX: 8-bit palette indices, 1 byte/texel. The palette embedded in the file is
// ignored - all Quake II art indexes the shared global palette, the same way
// ref_gl decoded through d_8to24table.
bool LoadPcx(const char * filename, u8 ** outPic, int * outWidth, int * outHeight);

// TGA (types 2 and 10, 24/32 bpp, no colormaps - all Quake II ever shipped):
// RGBA32 texels, 4 bytes/texel. *outHasAlpha is set when the file carried an
// alpha channel (32 bpp source); 24 bpp texels get alpha 255.
bool LoadTga(const char * filename, u8 ** outPic, int * outWidth, int * outHeight, bool * outHasAlpha);

// WAL: 8-bit palette indices, 1 byte/texel, followed in the file by three mip levels
// the tools that wrote it box-filtered down from it, each half the size of the one
// before. Unlike the loaders above, this one hands back the file itself, with the
// levels checked and located in it, and leaves the pixel buffer to the caller: only
// the texture cache knows how many levels it wants and at what size, and building from
// here copies each level straight into place. Release it with FreeWal.
constexpr int kWalLevels = 4; // q_files.h's MIPLEVELS

struct WalFile
{
    void *     fileData;           // FS_LoadFile's buffer, which 'levels' point into
    const u8 * levels[kWalLevels]; // level 0 first; level L is (width >> L) x (height >> L)
    int        width, height;      // level 0's
    int        numLevels;          // 1 + the mip levels the file holds intact, in order
};

bool LoadWal(const char * filename, WalFile * out);
void FreeWal(WalFile & wal);

} // namespace ps2::tex
