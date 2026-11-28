/* ================================================================================================
 * File: hi_res.cpp
 * Brief: Strip ("hi-res") output for 720p and 1080i. See hi_res.h.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/hi_res.h"
#include "ps2/debug/profile.h" // ReadCycles
#include "ps2/system/heap.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dma.h>
#include <dma_tags.h>
#include <kernel.h>
#include <gif_tags.h>
#include <gs_gp.h>
#include <gs_privileged.h>
#include <gs_psm.h>
#include <ee_regs.h>
#include <timer.h> // T1_COUNT, T1_MODE, T1_COMP
#include <vif_codes.h>

namespace ps2::hires {
namespace {

// ------------------------------------------------------------------------------------------------
// Layout
// ------------------------------------------------------------------------------------------------

// A PSMCT16 page is 64x64 pixels, 8 KB: a strip starts on a page, so its height is whole pages.
constexpr int kPageRows  = 64;
constexpr int kPageWords = 2048;

static bool   s_active = false;
static Layout s_layout = {};

int StripTop(const int strip)
{
    return strip * s_layout.stripRows;
}

// Rows the strip really shows: the last one stops at the picture's bottom.
int StripShownRows(const int strip)
{
    const int top    = StripTop(strip);
    const int bottom = (top + s_layout.stripRows < s_layout.height) ? (top + s_layout.stripRows) : s_layout.height;
    return bottom - top;
}

int ZStripWords(const int pass)
{
    // 3 passes: the next strip, which the CRTC reads only after the pass after this one has drawn
    // it again. 2: the strip of its own.
    return (s_layout.passes == 3) ? StripWords((pass + 1) % 3) : s_layout.zWords;
}

// ------------------------------------------------------------------------------------------------
// Engine state
// ------------------------------------------------------------------------------------------------

// A frame the engine can replay: its chain, its length and the half it lives in, or kCopyHalf for
// the heap copy LendHalves makes.
struct Frame
{
    const void * chain;
    int          qwords;
    int          half;
};

constexpr Frame kNoFrame  = { nullptr, 0, -1 };
constexpr int   kCopyHalf = 2;

// The ZBUF fixups of each half's chain (AddZbufFixup), written while it is built and read by the
// passes that replay it, and the copy's. A frame's chain holds one per triangle or particle chunk
// and one per 2D section: a few hundred, against the 8192 here. From the heap, in strip mode only:
// 96 KB the other modes don't pay for.
constexpr int kMaxFixups = 8192;
using FixupList = u64 * [kMaxFixups];
static FixupList * s_fixups = nullptr; // [3]
static int s_numFixups[3] = {};
static int   s_buildHalf    = 0;

// LendHalves's copy, and whether the passes have moved on from it (ReleaseCopy frees it then).
static void *        s_copy        = nullptr;
static u32           s_copyBytes   = 0;
static volatile bool s_copyRetired = false;

// What the passes draw, and the frame finished since, which the next field's first pass adopts.
// Both are only touched under the GS lock.
static Frame s_replay  = kNoFrame;
static Frame s_pending = kNoFrame;

// Hold(): passes are skipped, and the display is off, until Resume - or, for a hold until the next
// frame, until a pass adopts one.
static volatile bool s_held          = false;
static bool          s_resumeOnAdopt = false;

static u8   s_clearRgb[3] = {};
static bool s_dither      = false;
static u64  s_pixelTests  = 0;

// The pass the GS is drawing, from its kick until the FINISH its terminator raises (the
// handler's to clear), and when it went out.
static volatile bool      s_passInFlight = false;
static volatile CpuCycles s_passStart    = 0;

#if PS2_QUAKE_DEBUG
// The watchdog: VSINTs a pass has been in flight through, and the handler's word that one never
// ended (WaitPassDone reports it).
static volatile u32  s_inFlightVsyncs = 0;
static volatile bool s_passStuck      = false;
constexpr u32 kStuckPassVsyncs = 60; // about a second
#endif // PS2_QUAKE_DEBUG

// The interrupt side: HSINTs since the last VSINT, the line count each pass starts at, and the
// pass the handler last asked for.
static volatile u32 s_lines       = 0;
static u32          s_passLine[kMaxPasses] = {};

// The schedule as gsKit works it out (s_passLine is it, scaled when the field is not as long), and
// the lines a field of the mode has: 750 a 720p frame, 562 or 563 a 1080i field. PCSX2 counts 525
// HSINTs a frame in 720p (it gives the mode NTSC's line timing; its log shows the rate as ??.???),
// where pass 0's line 540 never comes: VSINT scales the schedule to the field it measured, so the
// passes still all run. A console's count is the mode's, which leaves gsKit's lines as they are.
static u32 s_baseLine[kMaxPasses] = {};
static u32 s_nominalLines = 750;
static u32 s_scaledFor    = 0;

// ps2_hires_timer 1 (the default): the lines are counted by EE timer T1, clocked by H-blank, not by
// an HSINT per line. An HSINT is a kernel INTC dispatch, and at 525 to 750 a frame those cost the
// EE about a fifth of its time in PCSX2 (every stage of the 720p perf capture ran ~20% long for the
// same work). With the timer, VSINT reads T1 and arms its compare on the earliest pass's line, and
// each compare interrupt wakes its pass and arms the next: four interrupts a frame. ps2sdk takes
// T2 and T3; T0 and T1 are free.
static bool s_useTimer = true;
static int  s_order[kMaxPasses] = {}; // pass numbers by line, earliest first
static volatile int s_nextOrder   = 0;
static volatile u32 s_vsyncCount  = 0; // T1 at the last VSINT

constexpr u32 kTimerClockHblank = 3u;       // CLKS: count H-blanks
constexpr u32 kTimerCountEnable = 1u << 7;  // CUE
constexpr u32 kTimerCompareInt  = 1u << 8;  // CMPE
constexpr u32 kTimerEqualFlag   = 1u << 10; // EQUF, write 1 to clear
constexpr u32 kTimerMode        = kTimerClockHblank | kTimerCountEnable | kTimerCompareInt;

static volatile int s_signalled   = 0;
static volatile int s_renderField = 1; // 1080i: 1 draws the odd lines (no offset), 0 the even

static s32 s_passSema  = -1; // the handler wakes the pass thread
static s32 s_lockSema  = -1; // the GS lock, a mutex
static s32 s_adoptSema = -1; // a pass adopted a frame (WaitForHalf)
static s32 s_finishSema = -1; // FINISH ended the pass in flight (WaitPassDone)
static int s_handlerId = -1;
static s32 s_threadId  = -1;

constexpr int kThreadStackBytes = 8 * 1024;
alignas(16) static u8 s_threadStack[kThreadStackBytes];

static Stats s_stats = {};

// ------------------------------------------------------------------------------------------------
// The pass prologue
// ------------------------------------------------------------------------------------------------
//
// One DMA chain per pass: a CNT tag whose DIRECT sends the prologue's GIF packet - FRAME, ZBUF,
// XYOFFSET and SCISSOR for both contexts, then the strip's clear, then TEST back to the 3D pixel
// tests - and a NEXT tag on into the frame's own chain, which runs to its END. Built per pass
// into one of two buffers, alternating, so a late pass never rewrites the one the DMAC is reading.

constexpr int kClearColumnPixels = 32; // the clear goes down in page-friendly columns, as libdraw's
constexpr int kMaxClearColumns   = 1920 / kClearColumnPixels;
constexpr int kPrologueRegs      = 15 + (2 * kMaxClearColumns);
constexpr int kPrologueQwords    = 1 + 1 + kPrologueRegs + 1; // CNT tag, GIF tag, A+D, NEXT tag

alignas(64) static u64 s_prologue[2][2 * kPrologueQwords];
static int s_prologueSlot = 0;

// Fills one prologue for 'pass' and returns its qword count.
int BuildPrologue(u64 * const q, const int pass)
{
    const Layout & l = s_layout;

    const u32 fbp   = static_cast<u32>(StripWords(pass) / kPageWords);
    const u32 zbp   = static_cast<u32>(ZStripWords(pass) / kPageWords);
    const u32 fbw   = static_cast<u32>(l.width / 64);
    const u32 rows  = static_cast<u32>(StripShownRows(pass));
    const u32 right = static_cast<u32>(l.width - 1);

    // The picture centred on X = 2048, so the guard band fits 12.4 at any width (see
    // gs::ScreenOriginX); Y as ever, from the strip's top. 1080i's even field draws half a row
    // lower, gsKit's QueueIF: its 540 rows land between the odd field's.
    const u32 originX = static_cast<u32>(2048 - (l.width / 2)) << 4;
    const u32 halfRow = (l.output == video::Output::Dtv1080i && s_renderField == 0) ? 8u : 0u;
    const u32 originY = (static_cast<u32>(2048 + StripTop(pass)) << 4) + halfRow;

    const u64 frame    = GS_SET_FRAME(fbp, fbw, GS_PSM_16, 0u);
    const u64 zbuf     = GS_SET_ZBUF(zbp, GS_ZBUF_16, 0u);
    const u64 offset   = GS_SET_XYOFFSET(originX, originY);
    const u64 scissor  = GS_SET_SCISSOR(0u, right, 0u, rows - 1u);
    const u64 clearTst = GS_SET_TEST(0, 0, 0, 0, 0, 0, 1, 1); // z-test on, ALWAYS
    const u64 sprite   = GS_SET_PRIM(GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0);
    const u64 color    = GS_SET_RGBAQ(s_clearRgb[0], s_clearRgb[1], s_clearRgb[2], 0x80u, 0x3F800000u);

    const int columns = l.width / kClearColumnPixels;
    const int regs    = 15 + (2 * columns);
    PS2_Assert(regs <= kPrologueRegs);

    int n = 0;
    // CNT: the GIF packet follows; NOP, then DIRECT for it.
    q[n++] = DMATAG(static_cast<u32>(1 + regs), 0, DMA_TAG_CNT, 0, 0, 0);
    q[n++] = (static_cast<u64>(VIF_CODE(static_cast<u32>(1 + regs), 0, VIF_CMD_DIRECT, 0)) << 32)
           | static_cast<u64>(VIF_CODE(0, 0, VIF_CMD_NOP, 0));

    q[n++] = GIF_SET_TAG(static_cast<u32>(regs), 1, 0, 0, GIF_FLG_PACKED, 1);
    q[n++] = GIF_REG_AD;

    auto ad = [&](const u64 value, const u64 reg) { q[n++] = value; q[n++] = reg; };
    ad(frame,   GS_REG_FRAME_1);
    ad(frame,   GS_REG_FRAME_2);
    ad(zbuf,    GS_REG_ZBUF_1);
    ad(zbuf,    GS_REG_ZBUF_2);
    ad(offset,  GS_REG_XYOFFSET_1);
    ad(offset,  GS_REG_XYOFFSET_2);
    ad(scissor, GS_REG_SCISSOR_1);
    ad(scissor, GS_REG_SCISSOR_2);
    ad(GS_SET_DTHE(s_dither ? 1 : 0), GS_REG_DTHE);
    ad(clearTst, GS_REG_TEST_1);
    ad(s_pixelTests, GS_REG_TEST_2);
    ad(sprite, GS_REG_PRIM);
    ad(color,  GS_REG_RGBAQ);
    for (int c = 0; c < columns; ++c)
    {
        const u32 x0 = originX + (static_cast<u32>(c * kClearColumnPixels) << 4);
        const u32 x1 = x0 + (static_cast<u32>(kClearColumnPixels) << 4);
        const u32 y1 = originY + (rows << 4);
        ad(GS_SET_XYZ(x0, originY, 0u), GS_REG_XYZ2);
        ad(GS_SET_XYZ(x1, y1, 0u), GS_REG_XYZ2);
    }
    ad(s_pixelTests, GS_REG_TEST_1);
    ad(s_pixelTests, GS_REG_TEST_2);

    PS2_Assert(n == 2 * (2 + regs));

    // NEXT, into the frame's chain.
    const u32 chain = static_cast<u32>(reinterpret_cast<std::uintptr_t>(s_replay.chain) & 0x0FFFFFFFu);
    q[n++] = DMATAG(0u, 0, DMA_TAG_NEXT, 0, chain, 0);
    q[n++] = 0; // two VIF NOPs

    return n / 2;
}

// Points every ZBUF the replayed chain writes at the pass's z strip. Through the uncached segment:
// the chain was written back and the cache invalidated when it was closed (cmdbuf's FlushCache),
// so these go straight to the RAM the DMAC reads, and nothing cached can land on them later.
void PatchFixups(const int pass)
{
    const u64 zbp = static_cast<u64>(ZStripWords(pass) / kPageWords);
    const int half  = s_replay.half;
    const int count = s_numFixups[half];
    for (int i = 0; i < count; ++i)
    {
        volatile u64 * const value = reinterpret_cast<volatile u64 *>(UNCACHED_SEG(s_fixups[half][i]));
        *value = (*value & ~static_cast<u64>(0x1FF)) | zbp;
    }
}

// ------------------------------------------------------------------------------------------------
// Passes
// ------------------------------------------------------------------------------------------------

void SetDisplayOn(const bool on)
{
    // Read circuit 2 alone, as the digital outputs run (graph_enable_output with no filter).
    *GS_REG_PMODE = on ? GS_SET_PMODE(0, 1, 0, 1, 0, 0x80) : GS_SET_PMODE(0, 0, 0, 0, 0, 0);
}

void Adopt()
{
    if (s_pending.chain != nullptr)
    {
        if (s_replay.half == kCopyHalf)
        {
            s_copyRetired = true;
        }
        s_replay  = s_pending;
        s_pending = kNoFrame;
        ++s_stats.adoptions;

        if (s_resumeOnAdopt)
        {
            s_resumeOnAdopt = false;
            s_held = false;
            SetDisplayOn(true);
        }
        SignalSema(s_adoptSema);
    }
}

#if PS2_QUAKE_DEBUG
// A pass that never ends would leave the pass thread, and then everything that takes the GS lock,
// waiting for good with nothing printed: after a second the VSINT handler wakes the waiter, which
// says what the engine was waiting on, and gives up on the pass.
Q_COLD_FUNC void ReportStuckPass()
{
    std::printf("hi-res: a pass never finished: inFlight %d, D1_CHCR %08x, CSR %08x, IMR %08x, T1 %u/%u mode %04x, "
                "vsyncs %u, passes %u, next %d, held %d\n",
                s_passInFlight ? 1 : 0, static_cast<unsigned>(*R_EE_D1_CHCR), static_cast<unsigned>(*GS_REG_CSR),
                static_cast<unsigned>(GsGetIMR()), static_cast<unsigned>(*T1_COUNT), static_cast<unsigned>(*T1_COMP),
                static_cast<unsigned>(*T1_MODE), s_stats.vsyncs, s_stats.passes, s_nextOrder, s_held ? 1 : 0);
}
#endif // PS2_QUAKE_DEBUG

// Sleeps, never spins: the pass thread is above every other, and once the GS can't draw a field's
// passes inside the field (a heavy view, every pass replaying the whole frame), each pass finds the
// one before still drawing and waits most of a pass for it, back to back. A spin there kept the main
// and sound threads off the EE for good: on a console 720p's 3 passes froze at base2's first view,
// and its 2 at waste2's fight, sound looping, no log (2026-11-28). A FINISH left over from a pass
// nobody waited for only costs a turn of the loop.
void WaitPassDone()
{
    while (s_passInFlight)
    {
        WaitSema(s_finishSema);
#if PS2_QUAKE_DEBUG
        if (s_passStuck)
        {
            ReportStuckPass();
            s_passStuck    = false;
            s_passInFlight = false;
        }
#endif // PS2_QUAKE_DEBUG
    }
    while ((*R_EE_D1_CHCR & (1u << 8)) != 0) {}
}

// Sends one pass of the replayed frame. Under the GS lock, with no pass in flight.
void SendPass(const int pass)
{
    s_prologueSlot ^= 1;
    u64 * const prologue = s_prologue[s_prologueSlot];
    const int qwords = BuildPrologue(prologue, pass);
    PatchFixups(pass);

    *GS_REG_CSR = 2; // clear FINISH, so the handler's is this pass's
    s_passStart    = ReadCycles();
#if PS2_QUAKE_DEBUG
    s_inFlightVsyncs = 0;
#endif // PS2_QUAKE_DEBUG
    s_passInFlight = true;
    dma_channel_send_chain(DMA_CHANNEL_VIF1, prologue, qwords, DMA_FLAG_TRANSFERTAG, 0);
    ++s_stats.passes;
}

void RunPass(const int pass)
{
    WaitSema(s_lockSema);

    if (pass == 0)
    {
        ++s_stats.fields;
        Adopt();
    }

    if (s_held || s_replay.chain == nullptr)
    {
        ++s_stats.skipped;
        SignalSema(s_lockSema);
        return;
    }

    // The pass before should be long done: the GS has had a strip's scan for it. If it isn't,
    // this strip tears; wait it out, since the channel takes one chain at a time.
    if (s_passInFlight)
    {
        ++s_stats.late;
        WaitPassDone();
    }

    SendPass(pass);
    SignalSema(s_lockSema);
}

void PassThread(void *)
{
    for (;;)
    {
        WaitSema(s_passSema);
        RunPass(s_signalled);
    }
}

// Aims T1's compare at the next pass this frame, or where it can't be reached before the next
// VSINT re-arms it: a line behind the count, which it would take the 16-bit counter's whole lap
// (about 1.4 s) to come round to.
void ArmTimer()
{
    const int index = s_nextOrder;
    if (index < s_layout.passes)
    {
        *T1_COMP = (s_vsyncCount + s_passLine[s_order[index]]) & 0xFFFFu;
    }
    else
    {
        *T1_COMP = (*T1_COUNT - 1u) & 0xFFFFu;
    }
}

// The GS events the engine takes, as CSR bits: each is cleared by writing its bit alone (the event
// bits are write-one-to-clear), so the others survive.
constexpr u32 kCsrFinish   = 2u;
constexpr u32 kCsrHsint    = 4u;
constexpr u32 kCsrVsint    = 8u;
constexpr u32 kTakenEvents = kCsrFinish | kCsrHsint | kCsrVsint;

// What CSR has raised that the IMR lets through: a masked event is someone else's to poll (the
// readback masks FINISH and waits for it itself), so it is never taken or cleared here.
u32 PendingGsEvents(const u64 csr)
{
    const u32 unmasked = ~static_cast<u32>(iGsGetIMR() >> 8);
    return static_cast<u32>(csr) & unmasked & kTakenEvents;
}

// HSINT counts the lines, VSINT starts a field, FINISH ends a pass. Interrupt context only (the
// GS handler, or the timer's rescue below).
void ServiceGsEvents(const u32 events, const u64 csr)
{
    if ((events & kCsrHsint) != 0)
    {
        *GS_REG_CSR = kCsrHsint;
        const u32 line = s_lines + 1u;
        s_lines = line;

        for (int p = 0; p < s_layout.passes; ++p)
        {
            if (line == s_passLine[p])
            {
                // Pass 0 starts as the CRTC shows the previous field's last strip, so the field
                // it draws is the one after the CRTC's.
                if (p == 0)
                {
                    s_renderField = ((csr >> 13) & 1u) != 0 ? 0 : 1;
                }
                s_signalled = p;
                iSignalSema(s_passSema);
                break;
            }
        }
    }

    if ((events & kCsrVsint) != 0)
    {
        *GS_REG_CSR = kCsrVsint;
        u32 lines = s_lines;
        if (s_useTimer)
        {
            const u32 now = *T1_COUNT;
            lines = (now - s_vsyncCount) & 0xFFFFu;
            s_vsyncCount = now;
        }
        ++s_stats.vsyncs;
        s_stats.fieldLines = lines;

#if PS2_QUAKE_DEBUG
        if (s_passInFlight && !s_passStuck)
        {
            const u32 vsyncs = s_inFlightVsyncs + 1u;
            s_inFlightVsyncs = vsyncs;
            if (vsyncs > kStuckPassVsyncs)
            {
                s_passStuck = true;
                iSignalSema(s_finishSema);
            }
        }
#endif // PS2_QUAKE_DEBUG
        s_lines = 0;

        const bool offNominal = (lines + 8u < s_nominalLines) || (lines > s_nominalLines + 8u);
        if (lines != 0 && offNominal && lines != s_scaledFor)
        {
            for (int p = 0; p < s_layout.passes; ++p)
            {
                s_passLine[p] = (s_baseLine[p] * lines) / s_nominalLines;
            }
            s_scaledFor = lines;
        }

        if (s_useTimer)
        {
            s_nextOrder = 0;
            ArmTimer();
        }
    }

    // FINISH is cleared whether or not a pass is in flight: one left set would hold the GS's
    // interrupt line up (GsHandler).
    if ((events & kCsrFinish) != 0)
    {
        *GS_REG_CSR = kCsrFinish;
        if (s_passInFlight) // the pass's terminator
        {
            const CpuCycles took = ReadCycles() - s_passStart;
            s_stats.busyCycles += took;
            if (took > s_stats.worstCycles)
            {
                s_stats.worstCycles = took;
            }
            s_passInFlight = false;
            iSignalSema(s_finishSema);
        }
    }
}

s32 TimerHandler(const s32 cause)
{
    (void)cause;
    *T1_MODE = kTimerMode | kTimerEqualFlag; // clear EQUF

    // The rescue: a GS event still pending with no GS interrupt on its way (INTC_STAT's GS bit
    // clear) is one whose interrupt was lost (GsHandler). This handler runs at every pass, and a
    // lap of the counter (~1.4 s) after the last one when nothing else re-arms it, so a lost FINISH
    // or VSINT costs a strip or a field instead of the pass engine for good.
    const u64 csr    = *GS_REG_CSR;
    const u32 missed = PendingGsEvents(csr) & (kCsrFinish | kCsrVsint);
    if (missed != 0 && (*R_EE_I_STAT & EE_I_STAT_GS) == 0)
    {
        ++s_stats.rescues;
        ServiceGsEvents(missed, csr);
        if ((missed & kCsrVsint) != 0)
        {
            // The field's schedule starts again from here: its first compare is armed.
            ExitHandler();
            return 0;
        }
    }

    const int index = s_nextOrder;
    if (index < s_layout.passes)
    {
        const int pass = s_order[index];
        if (pass == 0)
        {
            s_renderField = ((csr >> 13) & 1u) != 0 ? 0 : 1;
        }
        s_signalled = pass;
        s_nextOrder = index + 1;
        ArmTimer();
        iSignalSema(s_passSema);
    }

    ExitHandler();
    return 0;
}

// INTC_GS. The INTC latches the GS's interrupt line on its rising edge, and the GS holds the line up
// while any unmasked event is pending, so an event raised after CSR was read, or one left set, keeps
// it up with no edge to come: no GS interrupt ever again, the pass in flight never seen to finish,
// and everything behind the GS lock stopped. The likely cause of the console's 3-pass runs freezing
// minutes in, the sound looping (2026-11-28; gsKit's handler takes no FINISH, and has an HSINT every
// line). So this takes events until none is left, then masks and unmasks them all, which gives the
// INTC a new edge for one that came in between; the timer's rescue is the net under both.
s32 GsHandler(const s32 cause)
{
    (void)cause;
    for (int round = 0; round < 4; ++round)
    {
        const u64 csr    = *GS_REG_CSR;
        const u32 events = PendingGsEvents(csr);
        if (events == 0)
        {
            break;
        }
        ServiceGsEvents(events, csr);
    }

    const u64 imr = iGsGetIMR();
    iGsPutIMR(imr | 0x7F00u);
    iGsPutIMR(imr);

    ExitHandler();
    return 0;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Layout
// ------------------------------------------------------------------------------------------------

bool Configure(const video::Output output, int passes)
{
    if (output != video::Output::Dtv720p && output != video::Output::Dtv1080i)
    {
        return false;
    }

    // 1080i in 2 passes would need 2 x 320 rows of 1920 plus a 320-row z strip: 3.7 MB of the 4.
    if (output == video::Output::Dtv1080i || passes != 2)
    {
        passes = 3;
    }

    Layout & l = s_layout;
    l.output = output;
    l.width  = (output == video::Output::Dtv1080i) ? 1920 : 1280;
    l.height = (output == video::Output::Dtv1080i) ? 540 : 720;
    l.passes = passes;

    const int rows = (l.height + passes - 1) / passes;
    l.stripRows = ((rows + kPageRows - 1) / kPageRows) * kPageRows;

    s_fixups = static_cast<FixupList *>(heap::Alloc(3u * sizeof(FixupList), heap::MemTag::HiRes));

    const int stripWords = (l.stripRows * l.width * 2) / 4;
    l.baseWords = 0;
    l.zWords    = (passes == 2) ? (passes * stripWords) : -1;
    l.endWords  = (passes * stripWords) + ((passes == 2) ? stripWords : 0);
    PS2_Assert((stripWords % kPageWords) == 0);

    s_active = true;
    return true;
}

bool Active()
{
    return s_active;
}

bool Running()
{
    return s_threadId >= 0;
}

const Layout & Get()
{
    return s_layout;
}

int StripWords(const int strip)
{
    return s_layout.baseWords + (strip * ((s_layout.stripRows * s_layout.width * 2) / 4));
}

// ------------------------------------------------------------------------------------------------
// The engine
// ------------------------------------------------------------------------------------------------

void Start(const u64 pixelTests)
{
    PS2_AssertMsg(s_active, "hires::Start without a strip layout!");
    s_pixelTests = pixelTests;

    // Where each pass starts, in HSINTs after VSINT: as the CRTC begins the strip before it, from
    // the first line DISPLAY shows (its DY, plus gsKit's 4; halved in an interlaced field).
    // Buffer rows are scan lines in both modes: 720p is MAGV 0, and 1080i's FRAME mode scans its
    // whole 540-row buffer in each field's 540 lines.
    //
    // DISPLAY is write-only, so its DY comes from the value gs::Init wrote, worked out again.
    const u64 display = video::DtvDisplay(s_layout.output, s_layout.width, s_layout.height);
    u32 yStart = static_cast<u32>((display >> 12) & 0x7FFu) + 4u;
    if (s_layout.output == video::Output::Dtv1080i)
    {
        yStart /= 2u;
    }
    for (int p = 0; p < s_layout.passes; ++p)
    {
        const int crtcStrip = (p == 0) ? (s_layout.passes - 1) : (p - 1);
        s_baseLine[p] = yStart + static_cast<u32>(StripTop(crtcStrip));
        s_passLine[p] = s_baseLine[p];
    }
    s_nominalLines = (s_layout.output == video::Output::Dtv1080i) ? 562u : 750u;

    // The passes in the order their lines come: 1, 2, then 0, which starts as the CRTC reaches the
    // last strip.
    for (int i = 0; i < s_layout.passes; ++i)
    {
        s_order[i] = (i + 1) % s_layout.passes;
    }

    // Not archived: an A/B switch for the line counting (set it in the config before a run).
    s_useTimer = (Cvar_Get("ps2_hires_timer", "1", 0)->value != 0.0f);

    ee_sema_t sema  = {};
    sema.init_count = 0;
    sema.max_count  = 1;
    s_passSema  = CreateSema(&sema);
    s_adoptSema = CreateSema(&sema);
    s_finishSema = CreateSema(&sema);
    sema.init_count = 1;
    s_lockSema  = CreateSema(&sema);
    if (s_passSema < 0 || s_adoptSema < 0 || s_finishSema < 0 || s_lockSema < 0)
    {
        Sys_Error("hi-res: no semaphores for the pass engine (%d %d %d %d).", static_cast<int>(s_passSema),
                  static_cast<int>(s_adoptSema), static_cast<int>(s_finishSema), static_cast<int>(s_lockSema));
        return; // unreachable; Sys_Error halts (it is not marked noreturn)
    }

    // Above every other thread: the main thread runs at 1, the sound submit thread at 0. Equal to
    // that one, which only ever runs for a copy between RPCs.
    ee_thread_t thread      = {};
    thread.func             = reinterpret_cast<void *>(&PassThread);
    thread.stack            = s_threadStack;
    thread.stack_size       = kThreadStackBytes;
    thread.gp_reg           = &_gp;
    thread.initial_priority = 0;
    s_threadId = CreateThread(&thread);
    if (s_threadId < 0 || StartThread(s_threadId, nullptr) < 0)
    {
        Sys_Error("hi-res: the pass thread would not start (%d).", static_cast<int>(s_threadId));
        return; // unreachable; Sys_Error halts (it is not marked noreturn)
    }

    // T1 first, so the first VSINT finds it counting.
    if (s_useTimer)
    {
        *T1_MODE  = 0u;
        *T1_COUNT = 0u;
        *T1_COMP  = 0xFFFFu;
        if (AddIntcHandler(INTC_TIM1, &TimerHandler, 0) < 0)
        {
            Sys_Error("hi-res: no INTC_TIM1 handler.");
            return; // unreachable; Sys_Error halts (it is not marked noreturn)
        }
        *T1_MODE = kTimerMode | kTimerEqualFlag;
        EnableIntc(INTC_TIM1);
    }

    // FINISH and VSINT unmasked (graph_set_mode left VSINT so), and HSINT when it counts the lines;
    // the rest stay masked.
    *GS_REG_CSR = 2u | 4u | 8u;
    s_handlerId = AddIntcHandler(INTC_GS, &GsHandler, 0);
    if (s_handlerId < 0)
    {
        Sys_Error("hi-res: no INTC_GS handler (%d).", s_handlerId);
        return; // unreachable; Sys_Error halts (it is not marked noreturn)
    }
    EnableIntc(INTC_GS);
    GsPutIMR(s_useTimer ? 0x7500 : 0x7100);

    Com_Printf("hi-res: %s, %dx%d in %d strips of %d rows, passes at lines %u/%u/%u (%s), %d KB of VRAM.\n",
               video::OutputName(video::Selected(), s_layout.output), s_layout.width, s_layout.height,
               s_layout.passes, s_layout.stripRows, s_passLine[0], s_passLine[1],
               (s_layout.passes > 2) ? s_passLine[2] : 0u, s_useTimer ? "T1 H-blank count" : "HSINT count",
               (s_layout.endWords * 4) / 1024);
}

void SetClear(const u8 rgb[3], const bool dither)
{
    s_clearRgb[0] = rgb[0];
    s_clearRgb[1] = rgb[1];
    s_clearRgb[2] = rgb[2];
    s_dither      = dither;
}

void BeginBuild(const int half)
{
    s_buildHalf = half;
    s_numFixups[half] = 0;
}

void AddZbufFixup(u64 * const zbufValue)
{
    int & count = s_numFixups[s_buildHalf];
    if (count >= kMaxFixups) [[unlikely]]
    {
        Sys_Error("hi-res: more than %d ZBUF writes in one frame's chain.", kMaxFixups);
        return; // unreachable; Sys_Error halts (it is not marked noreturn)
    }
    s_fixups[s_buildHalf][count++] = zbufValue;
}

void Publish(const void * const chain, const int qwords, const int half)
{
    WaitSema(s_lockSema);
    s_pending = Frame{ chain, qwords, half };
    SignalSema(s_lockSema);
}

void LendHalves(const void * const halfBase0, const void * const halfBase1, const u32 halfBytes)
{
    LockGs();

    // The newest frame there is: the one waiting for a field, or else the one being replayed.
    const Frame frame = (s_pending.chain != nullptr) ? s_pending : s_replay;
    s_pending = kNoFrame;

    if (frame.chain == nullptr || frame.half == kCopyHalf)
    {
        // Nothing to keep (no frame yet), or the copy already is what is replayed: a second load
        // with no frame drawn in between.
        if (frame.chain == nullptr)
        {
            s_held = true;
            s_resumeOnAdopt = true;
            SetDisplayOn(false);
        }
        s_replay = frame;
        UnlockGs();
        return;
    }

    const u8 * const base    = static_cast<const u8 *>(frame.chain);
    const u32        bytes   = static_cast<u32>(frame.qwords) * 16u;
    PS2_Assert(bytes <= halfBytes);
    (void)halfBytes; // the assert's alone
    const u8 * const half    = static_cast<const u8 *>((frame.half == 0) ? halfBase0 : halfBase1);
    const u32        inHalf0 = static_cast<u32>(reinterpret_cast<std::uintptr_t>(half) & 0x0FFFFFFFu);

    // The copy is freed by the main thread once the passes are past it (ReleaseCopy); one still
    // held from a load before has to go first.
    if (s_copy != nullptr && s_copyRetired)
    {
        heap::Free(s_copy, s_copyBytes, heap::MemTag::HiRes);
        s_copy = nullptr;
    }

    void * const copy = (s_copy == nullptr) ? heap::TryAllocAligned(heap::MemAlign(64), bytes, heap::MemTag::HiRes) : nullptr;
    if (copy == nullptr)
    {
        Com_Printf("hi-res: no room to keep the loading screen through the load; the picture goes blank.\n");
        s_held = true;
        s_resumeOnAdopt = true;
        SetDisplayOn(false);
        s_replay = kNoFrame;
        UnlockGs();
        return;
    }
    std::memcpy(copy, base, bytes);

    // Walk the chain as the DMAC would and move every address that points into the half - the NEXT
    // tags that skip a committed allocation, the REFs of data the frame built beside its tags - to
    // the copy. Nothing else in a frame's chain holds an address. A tag the walk does not know
    // (CALL, RET) gives up on the copy.
    u64 * const q        = static_cast<u64 *>(copy);
    const u32   copyAddr = static_cast<u32>(reinterpret_cast<std::uintptr_t>(copy) & 0x0FFFFFFFu);
    const u32   chainOff = static_cast<u32>(base - half);
    const u32   halfLo   = inHalf0 + chainOff;
    const u32   halfHi   = halfLo + bytes;
    auto relocate = [&](u64 & tag)
    {
        const u32 addr = static_cast<u32>((tag >> 32) & 0x7FFFFFFFu);
        if (addr >= halfLo && addr < halfHi)
        {
            tag = (tag & 0x80000000FFFFFFFFull) | (static_cast<u64>(copyAddr + (addr - halfLo)) << 32);
        }
    };

    bool ok = false;
    u32 at = 0; // qword index of the tag being read
    for (int guard = 0; guard < frame.qwords && at < static_cast<u32>(frame.qwords); ++guard)
    {
        u64 & tag = q[at * 2];
        const u32 qwc = static_cast<u32>(tag & 0xFFFFu);
        const u32 id  = static_cast<u32>((tag >> 28) & 7u);
        if (id == DMA_TAG_CNT)
        {
            at += 1u + qwc;
        }
        else if (id == DMA_TAG_NEXT)
        {
            const u32 addr = static_cast<u32>((tag >> 32) & 0x7FFFFFFFu);
            relocate(tag);
            at = (addr - halfLo) / 16u;
        }
        else if (id == DMA_TAG_REF || id == DMA_TAG_REFS)
        {
            relocate(tag);
            at += 1u;
        }
        else if (id == DMA_TAG_END)
        {
            ok = true;
            break;
        }
        else
        {
            break;
        }
    }

    if (!ok)
    {
        Com_Printf("hi-res: the loading screen's chain has a tag the copy can't follow; the picture goes blank.\n");
        heap::Free(copy, bytes, heap::MemTag::HiRes);
        s_held = true;
        s_resumeOnAdopt = true;
        SetDisplayOn(false);
        s_replay = kNoFrame;
        UnlockGs();
        return;
    }

    // Its fixups, moved the same way.
    const int count = s_numFixups[frame.half];
    for (int i = 0; i < count; ++i)
    {
        const u8 * const p = reinterpret_cast<const u8 *>(s_fixups[frame.half][i]);
        s_fixups[kCopyHalf][i] = static_cast<u64 *>(static_cast<void *>(static_cast<u8 *>(copy) + (p - base)));
    }
    s_numFixups[kCopyHalf] = count;

    // Out of the cache, as the halves are: the DMAC reads RAM, and the passes patch it uncached.
    FlushCache(0);

    s_copy        = copy;
    s_copyBytes   = bytes;
    s_copyRetired = false;
    s_replay      = Frame{ copy, frame.qwords, kCopyHalf };
    UnlockGs();
}

void ReleaseCopy()
{
    if (s_copy != nullptr && s_copyRetired)
    {
        heap::Free(s_copy, s_copyBytes, heap::MemTag::HiRes);
        s_copy = nullptr;
        s_copyRetired = false;
    }
}

void WaitForHalf(const int half)
{
    for (;;)
    {
        while (PollSema(s_adoptSema) >= 0) {}

        WaitSema(s_lockSema);
        const bool busy = (s_replay.half == half) && (s_pending.chain != nullptr);
        SignalSema(s_lockSema);

        if (!busy)
        {
            return;
        }

        WaitSema(s_adoptSema);
    }
}

void Hold()
{
    LockGs();
    s_held = true;
    SetDisplayOn(false);
    UnlockGs();
}

void HoldUntilNextFrame()
{
    LockGs();
    s_held = true;
    s_resumeOnAdopt = true;
    SetDisplayOn(false);

    // A frame finished but not yet adopted is dropped too: it samples the textures the caller is
    // about to evict or move, and adopting it would end the hold on a picture of them.
    s_pending = kNoFrame;
    UnlockGs();
}

void Resume()
{
    WaitSema(s_lockSema);
    s_held = false;
    s_resumeOnAdopt = false;
    SetDisplayOn(true);
    SignalSema(s_lockSema);
}

bool Held()
{
    return s_held;
}

void LockGs()
{
    WaitSema(s_lockSema);
    WaitPassDone();
}

void UnlockGs()
{
    SignalSema(s_lockSema);
}

bool DrawPassNow(const int pass)
{
    LockGs();
    Adopt(); // the newest frame, as the next field would show
    const bool drawn = (s_replay.chain != nullptr);
    if (drawn)
    {
        SendPass(pass);
        WaitPassDone();
    }
    UnlockGs();
    return drawn;
}

Stats TakeStats()
{
    if (!Running())
    {
        return {};
    }
    // The INTC handler adds to these too (the passes' FINISH times, the VSINTs), so the copy and the
    // reset run with interrupts off, or a pass ending between them would be lost.
    WaitSema(s_lockSema);
    DIntr();
    const Stats taken = s_stats;
    s_stats = {};
    EIntr();
    SignalSema(s_lockSema);
    return taken;
}

u32 StripBudgetCycles()
{
    // A 720p frame is 750 lines, a 1080i field 562.5, both 59.94 a second; the shortest strip is
    // the budget that bites.
    const u32 cyclesPerSecond = ps2::debug::ProfileCyclesPerMillisec() * 1000u;
    const u32 linesPerSecond  = (s_layout.output == video::Output::Dtv1080i) ? 33716u : 44955u;
    const int lastStrip       = s_layout.passes - 1;
    return static_cast<u32>((static_cast<u64>(cyclesPerSecond) * static_cast<u64>(StripShownRows(lastStrip))) / linesPerSecond);
}

} // namespace ps2::hires
