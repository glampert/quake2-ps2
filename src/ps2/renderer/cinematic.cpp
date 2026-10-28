/* ================================================================================================
 * File: cinematic.cpp
 * Brief: Cinematic frame rendering. See cinematic.h.
 *
 *  Palette expansion and RGBA5551 packing happen on the EE, so the GS samples a plain
 *  16-bit texture - no CLUT involvement, so the palette takes the brightness curve
 *  (gamma.h) here. The frame texture goes through the regular
 *  dynamic-texture pipeline (MarkPixelsDirty + bind re-uploads it) and its VRAM returns
 *  to the heap when playback ends.
 *
 *  The engine draws every client frame (50/60 Hz) but decodes at 14 fps, so the conversion
 *  and the 128 KB upload run only when something they depend on changed: the frame
 *  (PS2_CinematicFrameChanged, or a different pointer), the palette or brightness curve, the
 *  size, or a fresh buffer. In between, the resident texture is drawn again. On the console
 *  this was 3.7 ms of Ui on every cinematic frame.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/cinematic.h"
#include "ps2/renderer/gamma.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/render_system.h"
#include "ps2/renderer/texture.h"
#include "ps2/system/heap.h"
#include "ps2/builtin/builtin.h" // global_palette

#include <cstring> // memcpy, memset

namespace ps2::cin {
namespace {

// Fixed frame texture dimensions: big enough for the stock 320x240 cinematics
// (taller sources downsample), power-of-two for the GS, and small enough that
// the per-frame conversion + upload stay cheap.
constexpr int kFrameDim = 256;

// The expanded frame, allocated on first use and released when playback stops.
// 128 KB is worth reclaiming: cinematics play *between* levels, so as .bss this
// was reserved for the whole run, competing with the map that has just loaded.
// The allocation is zeroed so rows past a short frame stay black.
constexpr size_t kFrameBufferBytes = kFrameDim * kFrameDim * sizeof(u16);
static u16 * s_frameBuffer = nullptr;

// The palette frames are expanded with: SetPalette's, kept as it came, and
// through the brightness curve as packed RGBA5551 texels, rebuilt when the curve
// changes. Small, and SetPalette can arrive before the first frame does, so these
// stay static.
alignas(16) static u32 s_sourcePalette[256];
alignas(16) static u16 s_palette16[256];
static u32 s_paletteGeneration = 0; // gamma::Generation() when s_palette16 was made

// What the texture in s_frameBuffer was converted from. DrawFrame converts again when any of
// it moved; s_converted false (a new buffer) forces it.
static u32        s_frameVersion     = 0; // bumped by PS2_CinematicFrameChanged
static u32        s_paletteVersion   = 0; // bumped by every CurvePalette
static bool       s_converted        = false;
static const u8 * s_convertedData    = nullptr; // compared, never read: the engine may have freed it
static u32        s_convertedFrame   = 0;
static u32        s_convertedPalette = 0;
static int        s_convertedCols    = 0;
static int        s_convertedRows    = 0;

// The frame texture, drawn through the regular 2D texture path. Linear
// filtering smooths the stretch to screen size (the GS does the upscale).
// 'pixels' is patched to the frame buffer by EnsureFrameBuffer below.
static tex::Texture s_frameTexture = {
    .name         = "cinematic_frame",
    .regSequence  = 0, // never in the texture cache; not part of the registration cycle.
    .pixels       = nullptr,
    .width        = kFrameDim,
    .height       = kFrameDim,
    .srcWidth     = kFrameDim, // never resampled: already a power of two
    .srcHeight    = kFrameDim,
    .dirtyPixels  = false,
    .type         = tex::ImageType::Pic,
    .flags        = tex::TexFlags::None,
    .format       = tex::PixelFormat::RGB16,
    .components   = tex::TexComponents::RGB,
    .function     = tex::TexFunction::Modulate,
    .magFilter    = tex::TexFilter::Linear,
    .minFilter    = tex::TexFilter::Linear,
    .textureChain = nullptr,
    .atlas        = nullptr, // owns its own VRAM; far too big for a scrap anyway
    .atlasX       = 0,
    .atlasY       = 0,
    .vramAddr     = tex::Texture::kNotResident
};

// Allocates the frame buffer on the first frame of a cinematic. Zeroed, so the
// rows a short frame never writes draw black instead of the previous movie's
// leftovers. ps2::heap::AllocAligned is fatal on failure, so there is no error path.
void EnsureFrameBuffer()
{
    if (s_frameBuffer != nullptr)
    {
        return;
    }

    void * const mem = ps2::heap::AllocAligned(ps2::heap::MemAlign(16), kFrameBufferBytes, ps2::heap::MemTag::Renderer);
    std::memset(mem, 0, kFrameBufferBytes);

    s_frameBuffer         = static_cast<u16 *>(mem);
    s_frameTexture.pixels = s_frameBuffer;
    s_converted           = false;
}

// Makes s_palette16 from s_sourcePalette, as the curve stands: RGBA5551, of which only the
// alpha MSB survives (always set, since SetPalette forces alpha opaque).
void CurvePalette()
{
    for (int i = 0; i < 256; ++i)
    {
        const u32 color = gamma::CurveColor(s_sourcePalette[i]);
        const u32 r = color         & 0xFF;
        const u32 g = (color >>  8) & 0xFF;
        const u32 b = (color >> 16) & 0xFF;
        const u32 a = (color >> 24) & 0xFF;

        s_palette16[i] = static_cast<u16>(((a & 0x1) << 15) | ((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
    }
    s_paletteGeneration = gamma::Generation();
    ++s_paletteVersion;
}

// Drops the frame buffer at end of playback. Safe to call when nothing is
// allocated, which is the common case - the engine resets the palette on every
// SCR_StopCinematic, including ones that never drew a frame.
void ReleaseFrameBuffer()
{
    if (s_frameBuffer == nullptr)
    {
        return;
    }

    ps2::heap::Free(s_frameBuffer, kFrameBufferBytes, ps2::heap::MemTag::Renderer);
    s_frameBuffer         = nullptr;
    s_frameTexture.pixels = nullptr;
    s_converted           = false;
}

// Whether the texture no longer shows what DrawFrame was handed.
bool NeedsConversion(const u8 * data, const int cols, const int rows)
{
    return !s_converted ||
           data != s_convertedData ||
           s_frameVersion != s_convertedFrame ||
           s_paletteVersion != s_convertedPalette ||
           cols != s_convertedCols ||
           rows != s_convertedRows;
}

// Expands indices through the palette into RGBA5551, resampling columns with a 16.16
// fixed-point step (the original Quake ref_gl upsample algorithm) and rows by skipping.
void ConvertFrame(const u8 * data, const int cols, const int rows, const int trows, const float hscale)
{
    const int fracStep = cols * 0x10000 / kFrameDim;
    for (int i = 0; i < trows; ++i)
    {
        const int row = static_cast<int>(static_cast<float>(i) * hscale);
        if (row >= rows)
        {
            break;
        }

        const u8 * source = data + (cols * row);
        u16 *      dest   = &s_frameBuffer[i * kFrameDim];

        int frac = fracStep >> 1;
        for (int j = 0; j < kFrameDim; ++j)
        {
            dest[j] = s_palette16[source[frac >> 16]];
            frac += fracStep;
        }
    }

    // Guard row: the quad samples v up to trows, and bilinear filtering at
    // that edge reads texel row trows - duplicate the last valid row so the
    // bottom of the picture doesn't blend into black.
    if (trows < kFrameDim)
    {
        std::memcpy(&s_frameBuffer[trows * kFrameDim],
                    &s_frameBuffer[(trows - 1) * kFrameDim],
                    kFrameDim * sizeof(u16));
    }

    s_converted        = true;
    s_convertedData    = data;
    s_convertedFrame   = s_frameVersion;
    s_convertedPalette = s_paletteVersion;
    s_convertedCols    = cols;
    s_convertedRows    = rows;
    s_frameTexture.MarkPixelsDirty();
}

} // namespace

void SetPalette(const u8 * palette)
{
    if (palette == nullptr)
    {
        std::memcpy(s_sourcePalette, global_palette, sizeof(s_sourcePalette));
        CurvePalette();

        // The engine resets the palette when playback stops (SCR_StopCinematic),
        // so the frame texture's VRAM can go back to the texture heap. Harmless
        // if a cinematic draws again later - the texture re-uploads on bind.
        gs::ReleaseTexture(s_frameTexture);

        // ...and the EE-side frame buffer with it; the next cinematic re-allocates.
        ReleaseFrameBuffer();
        return;
    }

    // 768-byte RGB triplets -> RGBA u32 entries, alpha forced opaque (only the
    // alpha MSB survives CurvePalette's RGBA5551 packing).
    u8 * dest = reinterpret_cast<u8 *>(s_sourcePalette);
    for (int i = 0; i < 256; ++i)
    {
        dest[(i * 4) + 0] = palette[(i * 3) + 0];
        dest[(i * 4) + 1] = palette[(i * 3) + 1];
        dest[(i * 4) + 2] = palette[(i * 3) + 2];
        dest[(i * 4) + 3] = 0xFF;
    }
    CurvePalette();
}

void DrawFrame(int x, int y, int w, int h, int cols, int rows, const u8 * data)
{
    PS2_Assert(data != nullptr);
    PS2_Assert(cols > 0 && rows > 0);

    EnsureFrameBuffer();

    // The brightness or contrast changed since the palette was made (the console, mid-movie).
    if (s_paletteGeneration != gamma::Generation())
    {
        CurvePalette();
    }

    // Rows map 1:1 when the source fits vertically; taller sources downsample
    // by row skipping. Columns always resample to the fixed 256-px width.
    int trows;
    float hscale;
    if (rows <= kFrameDim)
    {
        hscale = 1.0f;
        trows  = rows;
    }
    else
    {
        hscale = static_cast<float>(rows) / static_cast<float>(kFrameDim);
        trows  = kFrameDim;
    }

    if (NeedsConversion(data, cols, rows))
    {
        ConvertFrame(data, cols, rows, trows, hscale);
    }

    // Drawn stretched over the caller's rect (the engine passes the full screen). v stops at
    // trows, because rows past the source are not part of the picture - sampling the full 256
    // puts a black band along the bottom.
    constexpr u8 kUiBrightness[3] = { 128, 128, 128 };
    rs::DrawTexturedRect(s_frameTexture, x, y, w, h, 0, 0, kFrameDim, trows, kUiBrightness);
}

} // namespace ps2::cin

void PS2_CinematicFrameChanged()
{
    ++ps2::cin::s_frameVersion;
}
