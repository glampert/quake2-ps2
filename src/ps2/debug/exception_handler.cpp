/* ================================================================================================
 * File: exception_handler.cpp
 * Brief: EE CPU exception post-mortem. See exception_handler.h.
 *
 *  Built on ps2sdk's libeedebug, which owns the hard part: it replaces the EE's level 1
 *  exception vectors with an assembly stub that spills the full register set into an
 *  EE_RegFrame and dispatches to a C handler per cause.
 *
 *  That handler runs at exception level (EXL set, on libeedebug's own stack), where nothing that
 *  talks to the IOP can work: printf, the log file and every other SIF RPC need interrupts, and
 *  newlib faults before it gets that far. So the handler only copies what the report needs out
 *  of the frame, points the frame's EPC and $sp at ReportCrash, and returns. libeedebug reloads
 *  the registers from the frame and erets, so the faulting thread resumes in ReportCrash, on a
 *  stack of its own, as ordinary thread code. The report is written from there: to the log file
 *  (log_file.h), then to the screen the way Sys_Error shows an error, which echoes it to stdout.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#if PS2_QUAKE_DEBUG
#include "ps2/debug/exception_handler.h"
#include "ps2/debug/stack_trace.h"
#include "ps2/debug/scr_print.h"
#include "ps2/debug/log_file.h"

#include <cstdarg>
#include <cstdio>
#include <tamtypes.h>
#include <kernel.h>
#include <ee_debug.h>

// Bounds of our own .text, from the ps2sdk linkfile. Used to tell a fault inside
// the program from one the kernel took on our behalf - the two want unwinding
// from different places.
extern "C" {
    extern u8 _ftext[];
    extern u8 _etext[];
}

namespace ps2::debug {
namespace {

// MIPS ExcCode values, indexed by the cause field libeedebug dispatches on.
// Only the ones it routes to a level 1 handler are ever seen here; the gaps are
// syscall and breakpoint, which the kernel keeps for itself.
const char * CauseName(const int cause)
{
    switch (cause)
    {
    case 1  : return "TLB modified";
    case 2  : return "TLB refill (load/fetch)";
    case 3  : return "TLB refill (store)";
    case 4  : return "Address error (load/fetch)";
    case 5  : return "Address error (store)";
    case 6  : return "Bus error (instruction fetch)";
    case 7  : return "Bus error (data)";
    case 10 : return "Reserved instruction";
    case 11 : return "Coprocessor unusable";
    case 12 : return "Arithmetic overflow";
    case 13 : return "Trap";
    default : return "Unknown";
    }
}

// What the report needs, copied out of the exception frame. Copying it is all the handler does
// at exception level.
struct Fault
{
    int cause;
    u32 epc;
    u32 badVAddr;
    u32 status;
    u32 ra;
    u32 sp;
    u32 fp;
    u32 gp;
    u32 a[4];
    u32 v[2];
};

// How far the report has got. A fault inside ReportCrash re-enters the handler, which goes by
// this to decide what to try next.
enum class Stage
{
    Idle,      // no fault yet
    Reporting, // ReportCrash's first run
    Retrying,  // that faulted - most likely walking a stack the fault left unreadable - so it
               // runs once more without the unwind
    Stopped,   // the retry faulted too
};

static Stage s_stage = Stage::Idle;
static Fault s_fault = {};

// Recorded at install, from main(): a fault on any other thread has to stop this one, or its
// next frame would draw over the report.
static int s_mainThreadId = -1;

// ReportCrash runs on this rather than the faulting thread's stack, which may be what broke.
constexpr u32 kReportStackBytes = 16u * 1024u;
alignas(64) static u8 s_reportStack[kReportStackBytes];

// The report, built whole and then written out in one go. It is laid out for the debug screen:
// 64 columns, and few enough lines to fit 22 rows with a full 32-frame stack.
static char s_report[2048];
static int  s_reportLength = 0;

// The low 32 bits of a saved 128-bit EE register, and a write of a 32-bit value to one.
inline u32 Reg(const u32 (&r)[4]) { return r[0]; }

inline void SetReg(u32 (&r)[4], const u32 value)
{
    r[0] = value;
    r[1] = 0;
    r[2] = 0;
    r[3] = 0;
}

inline bool InOurText(const u32 addr)
{
    return addr >= reinterpret_cast<u32>(_ftext) && addr < reinterpret_cast<u32>(_etext);
}

__attribute__((format(printf, 1, 2))) void Append(const char * const fmt, ...)
{
    const int room = static_cast<int>(sizeof(s_report)) - s_reportLength;
    if (room <= 1)
    {
        return;
    }

    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(&s_report[s_reportLength], static_cast<size_t>(room), fmt, args);
    va_end(args);

    if (written > 0)
    {
        s_reportLength += (written < room) ? written : (room - 1);
    }
}

// One unwind, five frames to a line. 'pc' must be an address the scanner can walk back from -
// see the note at the call site about which one to hand it.
void AppendUnwind(const char * const from, const u32 pc, const u32 sp)
{
    u32 frames[kStackTraceMaxFrames];
    const int count = detail::WalkStack(pc, sp, frames, kStackTraceMaxFrames);

    Append("Stack, innermost first (unwound from %s):\n", from);
    for (int i = 0; i < count; ++i)
    {
        const bool lineEnds = ((i % 5) == 4) || (i == count - 1);
        Append("0x%08x%s", frames[i], lineEnds ? "\n" : " ");
    }

    if (count == 0)
    {
        Append("%s", "<could not unwind>\n");
    }
    else if (count == kStackTraceMaxFrames)
    {
        Append("%s", "... (truncated)\n");
    }
}

[[noreturn]] void ReportCrash()
{
    // The fault may have struck with interrupts masked, and all the I/O below needs them.
    EIntr();

    const Fault & f = s_fault;
    const int threadId = GetThreadId();

    s_reportLength = 0;
    Append("EE CPU EXCEPTION: %s (cause %d), thread %d%s\n",
           CauseName(f.cause), f.cause, threadId, (threadId == s_mainThreadId) ? " (main)" : "");
    Append("EPC 0x%08x  BadVAddr 0x%08x  Status 0x%08x\n", f.epc, f.badVAddr, f.status);
    Append("ra  0x%08x  sp 0x%08x  fp 0x%08x  gp 0x%08x\n", f.ra, f.sp, f.fp, f.gp);

    // The argument registers are worth having: a null pointer handed to a callee is the common
    // shape of this fault, and $a0-$a3 usually still hold it.
    Append("a0-a3 0x%08x 0x%08x 0x%08x 0x%08x\n", f.a[0], f.a[1], f.a[2], f.a[3]);
    Append("v0-v1 0x%08x 0x%08x\n", f.v[0], f.v[1]);

    // Where to unwind from depends on where the fault landed.
    //
    // Inside our own text, EPC names the faulting instruction and the scanner can walk back from
    // it to the function prologue. Outside it - a kernel routine handed a bad pointer, which is
    // what "pc=0x82000 addr=0x0" was - EPC is in code the scanner cannot read prologues for, and
    // unwinding from it produces fiction. There $ra is the useful number: it points back into
    // whichever of our functions made the call.
    if (s_stage == Stage::Retrying)
    {
        Append("%s", "Walking the stack faulted as well, so it's left out.\n");
    }
    else if (InOurText(f.epc))
    {
        AppendUnwind("EPC", f.epc, f.sp);
    }
    else if (InOurText(f.ra))
    {
        Append("%s", "EPC is outside our text: a kernel or library call faulted.\n");
        AppendUnwind("$ra", f.ra, f.sp);
    }
    else
    {
        Append("%s", "EPC and $ra are both outside our text: a smashed stack,\n"
                     "or a jump through a corrupt pointer.\n");
    }
    Append("%s", "Resolve the addresses against quake2_unstripped.elf.\n");

    // The log first: on a console it is the record that outlives this.
    LogFileWriteFatal(s_report);

    // A fault on another thread leaves the main one running, and its next frame would draw over
    // the report. Stopped only now, after the log write, since it may have held a lock that the
    // write needed.
    if (s_mainThreadId >= 0 && threadId != s_mainThreadId)
    {
        SuspendThread(s_mainThreadId);
    }

    // On screen as Sys_Error shows an error. ScrPrintf echoes every line to stdout.
    ScrInit();
    ScrSetTextColor(0xFF0000FF); // red text
    ScrPrintf("***************************************************************\n");
    ScrPrintf("%s", s_report);
    ScrPrintf("***************************************************************\n");

    for (;;)
    {
        SleepThread();
    }
}

int OnException(EE_RegFrame * const frame)
{
    switch (s_stage)
    {
    case Stage::Idle:
        s_fault.cause    = static_cast<int>((frame->cause >> 2) & 0x1F);
        s_fault.epc      = frame->epc;
        s_fault.badVAddr = frame->badvaddr;
        s_fault.status   = frame->status;
        s_fault.ra       = Reg(frame->ra);
        s_fault.sp       = Reg(frame->sp);
        s_fault.fp       = Reg(frame->fp);
        s_fault.gp       = Reg(frame->gp);
        s_fault.a[0]     = Reg(frame->a0);
        s_fault.a[1]     = Reg(frame->a1);
        s_fault.a[2]     = Reg(frame->a2);
        s_fault.a[3]     = Reg(frame->a3);
        s_fault.v[0]     = Reg(frame->v0);
        s_fault.v[1]     = Reg(frame->v1);
        s_stage = Stage::Reporting;
        break;

    case Stage::Reporting:
        s_stage = Stage::Retrying; // s_fault still holds the first fault
        break;

    case Stage::Retrying:
    case Stage::Stopped:
        // The retry faulted as well. Nothing is safe to try from here, and returning would
        // resume the fault: stop.
        s_stage = Stage::Stopped;
        for (;;) {}
    }

    // Leave exception level: libeedebug reloads every register from this frame and erets to its
    // EPC, so the faulting thread resumes in ReportCrash, on the report's own stack.
    frame->epc = reinterpret_cast<u32>(&ReportCrash);
    SetReg(frame->sp, reinterpret_cast<u32>(s_reportStack + kReportStackBytes));
    SetReg(frame->ra, 0);
    return 0;
}

} // namespace

void InstallExceptionHandlers()
{
    s_mainThreadId = GetThreadId();

    // Level 1 only. Level 2 is the debug/counter vector, used by hardware
    // breakpoints; nothing here sets any, and installing it would replace the
    // vector a real debugger wants.
    if (ee_dbg_install(1) < 0)
    {
        std::printf("WARNING: could not install EE exception handlers.\n");
        return;
    }

    // The causes libeedebug routes to level 1 handlers: TLB refill (1-3),
    // address and bus errors (4-7), and the instruction-level faults (10-13).
    // Syscall (8) and breakpoint (9) are deliberately absent - the kernel and a
    // debugger own those.
    for (int cause = 1; cause <= 13; ++cause)
    {
        if (cause == 8 || cause == 9) { continue; }
        ee_dbg_set_level1_handler(cause, OnException);
    }

    std::printf("EE exception handlers installed.\n");
}

} // namespace ps2::debug
#endif // PS2_QUAKE_DEBUG
