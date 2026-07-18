#pragma once
/* ================================================================================================
 * File: vu1.h
 * Brief: VU1-accelerated 3D drawing: microprogram upload and triangle batch submission.
 *        The microprogram transforms vertices by the caller's MVP matrix, performs
 *        guard-band clipping (whole triangles) and XGKICKs the result to the GS.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/math/vec_mat.h"
#include "ps2/renderer/vif_packet.h"

namespace ps2::tex { struct Texture; }

namespace ps2::vu1 {

// Declares the linker symbols bracketing an assembled VU microprogram in the
// ELF's .vudata section. 'progName' must match the #vuprog name in the .vcl.
#define PS2_DECLARE_VU_MICROPROGRAM(progName) \
    extern "C" u32 progName##_CodeStart __attribute__((section(".vudata"))); \
    extern "C" u32 progName##_CodeEnd   __attribute__((section(".vudata"))); \
    inline u32 progName##_InstructionQwordCount() { return u32((&progName##_CodeEnd - &progName##_CodeStart) / 2); } \
    inline ps2::vu1::VUCode progName##_Code() { return { &progName##_CodeStart, &progName##_CodeEnd }; }

// One triangle vertex, 2 qwords, matching the microprogram's input layout.
// The packed color must sit in the first word of its qword: the microprogram
// raw-copies it into a GS A+D RGBAQ qword with a single masked store, and
// only word 0 is reachable that way (swizzling the raw bits through the FMAC
// instead would flush denormal color patterns to zero).
struct alignas(16) DrawVertex
{
    float x, y, z, w; // model-space position; w must be 1.0f
    u32   rgba;       // packed color, use PackColorRGBA()
    float s, t, q;    // texture coords; q must be 1.0f
};
static_assert(sizeof(DrawVertex) == 32, "DrawVertex must be exactly 2 qwords");

// Packs 0-255 channels into DrawVertex::rgba, the GS native RGBAQ byte order
// (r in the low byte). Alpha 0x80 = 1.0 on the GS.
constexpr u32 PackColorRGBA(u32 r, u32 g, u32 b, u32 a)
{
    return r | (g << 8) | (b << 16) | (a << 24);
}

// Brings up the VIF1 DMA channel, uploads the microprogram to VU1 micro memory
// and programs the double-buffer registers. Call once, after gs::Init().
void Init();

// Draws a batch of triangles (3 verts each, triangle list) through VU1 with
// the given transform and texture (uploaded to GS VRAM on demand). Any whole-
// triangle count works: draws beyond kMaxVertsPerBatch are split into chunks
// submitted back to back in the same DMA chain, overlapping each chunk's
// upload with the previous one's transform. Synchronous for now: returns once
// the GS has consumed the batch, so the vertex data only needs to stay valid
// for the duration of the call. Call between gs::Begin/EndFrame but outside
// the gs::Begin2D/End2D section.
void DrawTriangles(const math::Mat4 & mvp, const tex::Texture & texture,
                   const DrawVertex * verts, int vertCount);

} // namespace ps2::vu1
