/* ================================================================================================
 * File: vu1.cpp
 * Brief: Microprogram upload. See vu1.h.
 *
 *  All four microprograms stay resident in VU1 micro memory at once, uploaded here and never
 *  swapped: a draw picks one by entry point in its MSCAL. The upload itself is built into the
 *  command buffer like any other VIF1 transfer, which is why this has to run after cmdbuf::Init.
 *
 *  What feeds them - the chunk emitters, the batch GIF tags, the chain budget - is in
 *  render_context.cpp. What they read is declared in vu1.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/cmd_buffer.h"
#include "ps2/renderer/render_context.h"

#include <dma.h>

namespace ps2::vu1 {

PS2_DECLARE_VU_MICROPROGRAM(VU1Prog_TexturedTriangles);
PS2_DECLARE_VU_MICROPROGRAM(VU1Prog_LerpedTriangles);
PS2_DECLARE_VU_MICROPROGRAM(VU1Prog_Particles);
PS2_DECLARE_VU_MICROPROGRAM(VU1Prog_LitTriangles);

namespace {

static bool s_initialized = false;

// Micro memory entry point of each program, indexed by Program. Set by Init().
static ProgramAddr s_progAddr[4] = {};

} // namespace

ProgramAddr ProgramAddress(const Program prog)
{
    // The one assert standing in for the old per-draw "vu1::Init not called!" checks: every chunk
    // emitted for every draw path comes through here for its MSCAL entry point.
    PS2_AssertMsg(s_initialized, "vu1::Init not called!");
    return s_progAddr[static_cast<int>(prog)];
}

void Init()
{
    PS2_AssertMsg(!s_initialized, "vu1::Init called twice!");
    s_initialized = true;

    dma_channel_initialize(DMA_CHANNEL_VIF1, nullptr, 0);
    dma_channel_fast_waits(DMA_CHANNEL_VIF1);

    // The textured program sits at micro address 0, then the lerped one, the particle one and the
    // lit one. MPG uploads round an odd instruction count up to even, so each base rounds up too.
    const u32 texturedInstructions  = VU1Prog_TexturedTriangles_InstructionCount();
    const u32 lerpedInstructions    = VU1Prog_LerpedTriangles_InstructionCount();
    const u32 particlesInstructions = VU1Prog_Particles_InstructionCount();
    const u32 litInstructions       = VU1Prog_LitTriangles_InstructionCount();

    const u32 texturedAddr  = 0;
    const u32 lerpedAddr    = (texturedInstructions + 1u) & ~1u;
    const u32 particlesAddr = (lerpedAddr + lerpedInstructions + 1u) & ~1u;
    const u32 litAddr       = (particlesAddr + particlesInstructions + 1u) & ~1u;

    PS2_AssertMsg(litAddr + litInstructions <= 2048,
                  "Microprograms overflow VU1 micro memory!");

    s_progAddr[static_cast<int>(Program::Textured)]  = ProgramAddr(texturedAddr);
    s_progAddr[static_cast<int>(Program::Lerped)]    = ProgramAddr(lerpedAddr);
    s_progAddr[static_cast<int>(Program::Particles)] = ProgramAddr(particlesAddr);
    s_progAddr[static_cast<int>(Program::Lit)]       = ProgramAddr(litAddr);

    // Built into the command buffer like every other VIF1 transfer, which is why vu1::Init has to
    // run after cmdbuf::Init - see the ordering note in PS2_RefInit. Synchronous: the Drain
    // terminates, kicks and waits, so VU1 is ready once it returns.
    rc::RenderContext & ctx = rc::Ctx();
    ctx.AddMicroProgram(ProgramAddr(texturedAddr),  VU1Prog_TexturedTriangles_Code());
    ctx.AddMicroProgram(ProgramAddr(lerpedAddr),    VU1Prog_LerpedTriangles_Code());
    ctx.AddMicroProgram(ProgramAddr(particlesAddr), VU1Prog_Particles_Code());
    ctx.AddMicroProgram(ProgramAddr(litAddr),       VU1Prog_LitTriangles_Code());
    ctx.AddDoubleBufferSettings(kDoubleBufferBase, kDoubleBufferOffset);
    cmdbuf::Drain();
}

} // namespace ps2::vu1
