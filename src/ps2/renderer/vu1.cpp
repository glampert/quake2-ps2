/* ================================================================================================
 * File: vu1.cpp
 * Brief: Microprogram upload. See vu1.h.
 *
 *  All five microprograms stay resident in VU1 micro memory at once, uploaded here and never
 *  swapped: a draw picks one by entry point in its MSCAL. The upload itself is built into the
 *  command buffer like any other VIF1 transfer, which is why this has to run after cmdbuf::Init.
 *
 *  What feeds them - the chunk emitters, the batch GIF tags, the chain budget - is in
 *  render_system.cpp. What they read is declared in vu1.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/vu1.h"
#include "ps2/renderer/cmd_buffer.h"
#include "ps2/renderer/render_system.h"

namespace ps2::vu1 {

PS2_DECLARE_VU_MICROPROGRAM(VU1Prog_TexturedTriangles);
PS2_DECLARE_VU_MICROPROGRAM(VU1Prog_Particles);

namespace {

// VU1 micro memory, in the 64-bit instruction units MPG destinations count in (16 KB / 8 bytes).
constexpr u32 kMicroMemMaxInstructions = 2048;

// Micro memory entry point of each program, indexed by Program. Set by Init().
static ProgramAddr s_progAddr[static_cast<int>(Program::Count)];

static bool s_initialized = false;

} // namespace

ProgramAddr ProgramAddress(const Program prog)
{
    // Every chunk of every draw path comes through here for its MSCAL entry point,
    // so this one assert covers them all.
    PS2_AssertMsg(s_initialized, "vu1::Init not called!");
    PS2_Assert(prog < Program::Count);
    return s_progAddr[static_cast<int>(prog)];
}

void Init()
{
    PS2_AssertMsg(!s_initialized, "vu1::Init called twice!");
    s_initialized = true;

    // Uploaded back to back from micro address 0, in this array's order - which is Program's
    // order, since the loop below indexes s_progAddr by position. MPG rounds an odd instruction
    // count up to even, so each program's base rounds up too.
    const struct { VUCode code; u32 instructionCount; } programs[] = {
        { VU1Prog_TexturedTriangles_Code(), VU1Prog_TexturedTriangles_InstructionCount() },
        { VU1Prog_Particles_Code(),         VU1Prog_Particles_InstructionCount()         },
    };
    static_assert(ArrayLength(programs) == ArrayLength(s_progAddr), "Register new VU1 programs here!");

    // Built into the command buffer like every other VIF1 transfer, which is why vu1::Init has to
    // run after cmdbuf::Init - see the ordering note in PS2_RefInit. Synchronous: the KickAndWait
    // below terminates, kicks and waits, so VU1 is ready once it returns.
    int nextProgramIdx  = 0;
    u32 nextProgramAddr = 0;

    for (const auto & program : programs)
    {
        PS2_AssertMsg(nextProgramAddr + program.instructionCount <= kMicroMemMaxInstructions,
                      "Microprograms overflow VU1 micro memory!");

        s_progAddr[nextProgramIdx++] = ProgramAddr(nextProgramAddr);
        rs::AddVUMicroProgram(ProgramAddr(nextProgramAddr), program.code);

        nextProgramAddr += (program.instructionCount + 1u) & ~1u;
    }

    // The warp program's sine constants. Every value in the block is a compile-time constant, so
    // unlike the light block it goes up once here and is never rewritten - which is why it can be
    // referenced where it lies rather than copied into the buffer first.
    rs::AddVUDataUpload(kWarpConstBlockAddr, &kWarpConstants, sizeof(kWarpConstants) / 16);

    rs::AddVUDoubleBufferSettings(kDoubleBufferBase, kDoubleBufferOffset);
    rs::KickAndWait();
}

} // namespace ps2::vu1
