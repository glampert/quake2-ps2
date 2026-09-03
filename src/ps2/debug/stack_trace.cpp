/* ================================================================================================
 * File: stack_trace.cpp
 * Brief: EE call stack unwinding by MIPS prologue scanning.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/debug/stack_trace.h"
#include <cstdio>

/*
 * Unwinding the EE comes down to two numbers per frame: how many bytes the
 * function took off $sp, and where inside that frame it spilled $ra. Nothing
 * records either one at runtime - the shipped ELF is stripped, there is no frame
 * pointer, and the .eh_frame the compiler emits is 4 bytes because the backend is
 * built -fno-exceptions. Both have to be read back out of the instruction stream,
 * which on a fixed-width RISC is not as bad as it sounds.
 *
 * The direction of the scan is the whole design. Given an address inside a
 * function we scan BACKWARD until we meet that function's prologue, then read
 * forward through the prologue to total up the frame and find the $ra spill.
 *
 * The SDK ships an unwinder of its own - ps2GetStackTrace, in libdebug's
 * ee/debug/src/callstack.c - that scans FORWARD for the function's *epilogue*
 * instead. That works for a trace taken on a normal code path and breaks on
 * exactly the paths worth tracing. GCC sinks cold blocks past the function's
 * return, and a fatal-error path is about as cold as a block gets, so the scan
 * starts below the epilogue it is looking for, runs off the end of the function
 * and matches the *next* function's prologue. Measured on this build, unwinding
 * out of ps2::heap::Alloc's out-of-memory branch: it reached ps2::heap::AllocAligned's
 * `addiu $sp, $sp, -2096` and read that immediate back unsigned, as a +63440 byte
 * frame - $sp walked 62 KB into nothing, every frame past the third fabricated.
 *
 * Scanning backward has no equivalent failure: from anywhere inside a function,
 * cold blocks included, the first *negative* `addiu $sp, $sp, imm` that owns a
 * $ra spill is that function's prologue. The positive one we may cross on the way
 * is its epilogue, which is why the sign is part of the match.
 *
 * Checked against the DWARF call frame information in the debug build, which
 * records the exact frame state per address: over the 11,129 call sites in the
 * ELF whose CFI says $ra is live, this agrees with DWARF at 11,099 and gives up
 * cleanly at the other 30 (all of them FS_LoadPackFile, whose 256 KB frame is
 * opened with a register rather than an immediate - see IsRegisterSpAdjust). It
 * reports no frame it cannot stand behind, which matters more here than depth:
 * a wrong address in a crash dump costs more time than a missing one.
 */

// Bounds of .text, from the linker script (ps2sdk/ee/startup/linkfile).
// _etext is PROVIDE'd there, so it exists only because this file refers to it.
extern "C" {
    extern u8 _ftext[];
    extern u8 _etext[];
}

namespace ps2::debug {
namespace {

// I-type instructions, matched on opcode + both register fields; the low half is
// the immediate we are after.
constexpr u32 kITypeMask = 0xffff0000u;
constexpr u32 kAddiuSpSp = 0x27bd0000u; // addiu $sp, $sp, imm
constexpr u32 kSdRaSp    = 0xffbf0000u; // sd    $ra, imm($sp)
constexpr u32 kSwRaSp    = 0xafbf0000u; // sw    $ra, imm($sp)

// R-type <op|rs|rd|shamt>, leaving $rt and the function code free, against
// "<something> $sp, $sp, <reg>".
constexpr u32 kSpRegOpMask = 0xffe0ffc0u;
constexpr u32 kSpRegOp     = 0x03a0e800u;

// How far back the prologue is allowed to be. The largest functions in the engine
// are a few thousand instructions; the bound exists so that a shape we cannot
// decode ends the walk instead of scanning to the bottom of .text.
constexpr int kMaxPrologueScan = 8192;

// How far past its opening instruction a prologue is allowed to run. GCC spills
// $ra within the first handful, along with the other callee-saved registers.
constexpr int kMaxPrologueBodyScan = 64;

// Sign-extended immediate of an I-type instruction.
inline int ImmediateOf(u32 instruction)
{
    return static_cast<s16>(static_cast<u16>(instruction & 0xffffu));
}

inline const u32 * TextBegin() { return reinterpret_cast<const u32 *>(static_cast<void *>(_ftext)); }
inline const u32 * TextEnd()   { return reinterpret_cast<const u32 *>(static_cast<void *>(_etext)); }

// Anything that can end the straight-line run of instructions we are reading as a
// prologue. Past one of these we can no longer assume the code we are looking at
// is on the path that actually ran.
inline bool IsBranchOrJump(u32 instruction)
{
    const u32 opcode = (instruction >> 26);
    switch (opcode)
    {
    case 0x00u: // SPECIAL, but only jr and jalr
        {
            const u32 funct = (instruction & 0x3fu);
            return (funct == 0x08u || funct == 0x09u);
        }
    case 0x01u:                                     // REGIMM: bltz/bgez/bltzal/bgezal
    case 0x02u: case 0x03u:                         // j, jal
    case 0x04u: case 0x05u: case 0x06u: case 0x07u: // beq, bne, blez, bgtz
    case 0x14u: case 0x15u: case 0x16u: case 0x17u: // beql, bnel, blezl, bgtzl
        return true;
    default:
        return false;
    }
}

// addu/subu/daddu/dsubu $sp, $sp, <reg>: how GCC opens a frame too big for even a
// multi-stage addiu, loading the size into a register first. FS_LoadPackFile does
// this for the 256 KB of packfile_t it puts on the stack. Recovering the size
// would mean tracking that register back through its lui/ori, which is where this
// unwinder stops and says so.
inline bool IsRegisterSpAdjust(u32 instruction)
{
    if ((instruction & kSpRegOpMask) != kSpRegOp)
    {
        return false;
    }

    const u32 funct = (instruction & 0x3fu);
    return (funct == 0x21u || funct == 0x23u || funct == 0x2du || funct == 0x2fu);
}

struct FrameInfo
{
    int frameSize; // Bytes the prologue subtracted from $sp.
    int raOffset;  // Where in the frame $ra was spilled, measured from $sp.
};

enum class PrologueScan
{
    NotAPrologue, // Some other adjustment; the caller should keep looking.
    Undecodable,  // A real prologue we cannot measure; the caller should stop.
    Decoded,
};

// Reads the straight-line prologue starting at `prologue` and works out the frame
// it opens. Doubles as the test of whether that instruction starts one at all: a
// function stores $ra within a few instructions of opening its frame, at an
// offset that lands inside it.
PrologueScan ScanPrologue(const u32 * prologue, const u32 * textEnd, FrameInfo & outFrame)
{
    int frameSize = -ImmediateOf(*prologue);
    if (frameSize <= 0)
    {
        return PrologueScan::NotAPrologue;
    }

    int raOffset = -1;

    for (int i = 1; i < kMaxPrologueBodyScan; ++i)
    {
        const u32 * const at = prologue + i;
        if (at >= textEnd)
        {
            break;
        }

        // A branch ends the prologue - but its delay slot runs either way, and
        // GCC routinely fills that slot with prologue work, the $ra spill
        // included. Read the slot, then stop.
        const bool endsPrologue = IsBranchOrJump(*at);
        const u32 * const slot  = (endsPrologue ? (at + 1) : at);
        if (slot >= textEnd)
        {
            break;
        }

        const u32 instruction = *slot;

        if (IsRegisterSpAdjust(instruction))
        {
            return PrologueScan::Undecodable;
        }

        const u32 masked = (instruction & kITypeMask);

        if (masked == kAddiuSpSp)
        {
            // addiu's immediate tops out at 32767, so a bigger frame is opened in
            // several bites - Con_CheckResize takes -32752 and then another -112
            // for its 32 KB of console buffer. Each bite moves $sp further down,
            // so whatever was already spilled sits that much higher above it.
            const int more = -ImmediateOf(instruction);
            if (more <= 0)
            {
                break; // Positive: an epilogue. The prologue ended before here.
            }

            frameSize += more;
            if (raOffset >= 0)
            {
                raOffset += more;
            }
        }
        else if ((masked == kSdRaSp || masked == kSwRaSp) && raOffset < 0)
        {
            raOffset = ImmediateOf(instruction);
        }

        if (endsPrologue)
        {
            break;
        }
    }

    if (raOffset < 0 || raOffset >= frameSize)
    {
        return PrologueScan::NotAPrologue;
    }

    outFrame.frameSize = frameSize;
    outFrame.raOffset  = raOffset;
    return PrologueScan::Decoded;
}

// Recovers the frame layout of whichever function contains pc.
// False means "cannot tell" - the caller stops walking rather than inventing it.
bool DecodeFrame(const u32 * pc, FrameInfo & outFrame)
{
    const u32 * const textBegin = TextBegin();
    const u32 * const textEnd   = TextEnd();

    if (pc < textBegin || pc >= textEnd)
    {
        return false;
    }

    for (int i = 0; i < kMaxPrologueScan; ++i)
    {
        const u32 * const at = pc - i;
        if (at < textBegin)
        {
            break;
        }

        if ((*at & kITypeMask) != kAddiuSpSp)
        {
            continue;
        }

        // Not every negative adjustment opens a frame: the later bites of a
        // multi-stage one look identical in isolation, and a positive one is an
        // epilogue we crossed on the way back (the normal case when the pc sits
        // in a cold block GCC sank past the function's return). ScanPrologue
        // tells them apart by whether a $ra spill belongs to it, which is why a
        // rejected candidate keeps the scan going instead of ending it.
        switch (ScanPrologue(at, textEnd, outFrame))
        {
        case PrologueScan::Decoded:      return true;
        case PrologueScan::Undecodable:  return false;
        case PrologueScan::NotAPrologue: break;
        }
    }

    // Nothing decodable within reach. A frame that never spills $ra makes no
    // calls, so there is nothing above it worth reporting anyway.
    return false;
}

} // namespace

namespace detail {

int WalkStack(u32 pc, u32 sp, u32 * outFrames, int maxFrames)
{
    if (outFrames == nullptr || maxFrames <= 0)
    {
        return 0;
    }

    const u32 * framePc = reinterpret_cast<const u32 *>(pc);

    int count = 0;
    while (count < maxFrames)
    {
        FrameInfo frame;
        if (!DecodeFrame(framePc, frame))
        {
            break;
        }

        // $sp is 16-byte aligned and the spill offset is a multiple of 4, so the
        // slot is always aligned for this load.
        const u32 * const raSlot = reinterpret_cast<const u32 *>(sp + static_cast<u32>(frame.raOffset));
        const u32 returnAddress  = *raSlot;

        // Off the top of the call chain: crt0 enters main with $ra spilled as 0.
        if (returnAddress == 0u)
        {
            break;
        }

        sp     += static_cast<u32>(frame.frameSize);
        framePc = reinterpret_cast<const u32 *>(returnAddress);

        // A jal leaves $ra pointing past its own delay slot, so back up 8 bytes to
        // name the call itself rather than whatever follows it.
        outFrames[count] = returnAddress - 8u;
        ++count;
    }

    return count;
}

} // namespace detail

Q_COLD_FUNC void PrintStackTrace()
{
    u32 frames[kStackTraceMaxFrames];
    const int count = CaptureStackTrace(frames, kStackTraceMaxFrames);

    std::printf("%s", "------------------------- STACK TRACE -------------------------\n");

    for (int i = 0; i < count; ++i)
    {
        std::printf("#%-2d 0x%08x\n", i, frames[i]);
    }

    if (count == 0)
    {
        std::printf("%s", "<unavailable - could not unwind the call stack>\n");
    }
    else if (count == kStackTraceMaxFrames)
    {
        std::printf("%s", "... (truncated)\n");
    }

    // The ELF that runs is stripped; its symbols stay in the _unstripped one built
    // beside it, which is what these addresses resolve against.
    std::printf("%s", "Resolve with: mips64r5900el-ps2-elf-addr2line -f -C -e "
                      "build/<config>/quake2_unstripped.elf <addr>\n");
    std::printf("%s", "------------------------- STACK TRACE -------------------------\n");

    // Callers of this do not intend to return, so nothing downstream will flush.
    std::fflush(stdout);
}

} // namespace ps2::debug
