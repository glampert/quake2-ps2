#pragma once
/* ================================================================================================
 * File: hi_res.h
 * Brief: Strip ("hi-res") output for 720p and 1080i: one front buffer cut into strips, each
 *        drawn again every field just behind the beam, after gsKit's gsHires.c.
 *
 *        A 1280x720 or 1920x540 picture has no room for two framebuffers and a z-buffer in the
 *        GS's 4 MB, so it is drawn the way gsKit draws its hi-res modes. There is one front
 *        buffer, cut into strips of a whole number of 64-row pages. While the CRTC scans strip
 *        i-1 out, the GS draws strip i, using strip i+1 as its z-buffer (the pass after clears it
 *        before the CRTC reaches it), so every strip is drawn just in time, every field. The
 *        frame being shown is therefore drawn again, whole, once per strip per field: the chain
 *        the EE built for it is kept and replayed, each pass behind a short prologue that points
 *        FRAME, ZBUF, XYOFFSET and SCISSOR at its strip and clears it. With 2 passes the z-buffer
 *        is a strip of its own instead, outside the front buffer.
 *
 *        What that asks of the rest of the renderer:
 *          - The chain must be replayable: no mid-frame kick, nothing in it that refers to one
 *            strip. ZBUF is the one register the chain writes with an address in it (the batch
 *            state blocks and the 2D sections' ZBUF re-arm); rs records where each lands
 *            (AddZbufFixup) and every pass patches its z strip's ZBP into them.
 *          - The picture is centred on X = 2048 (XYOFFSET.x is 2048 - W/2), so the guard band
 *            fits the GS's 12.4 window at 1920 wide; the 2D emitters shift by -W/2 to match
 *            (gs::ScreenOriginX).
 *          - The frame replayed is the last one finished, until the next one is: the EE builds
 *            into the other half, and BeginFrame waits only when the half it wants is still the
 *            one being replayed (WaitForHalf).
 *          - GS traffic outside the chain (texture and CLUT uploads, readbacks) runs between
 *            passes, under the GS lock, and the texture heap keeps the replayed frame's textures
 *            pinned as well as the frame being built's.
 *
 *        The pass engine is a thread above every other (priority 0), woken from an INTC_GS
 *        handler that counts HSINTs from each VSINT, as gsKit's does. The GS's FINISH, raised by
 *        each pass's terminator, is taken by the same handler and timestamps the pass's end,
 *        which is what the frame log's strip columns are made of.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/system/video_mode.h"

#include <tamtypes.h>
#include <packet2.h>

namespace ps2::hires {

// ------------------------------------------------------------------------------------------------
// Layout
// ------------------------------------------------------------------------------------------------

constexpr int kMaxPasses = 3;

struct Layout
{
    video::Output output;
    int           width;      // pixels, a multiple of 64
    int           height;     // framebuffer rows: 720, or 540 for 1080i (each field the whole)
    int           passes;     // 2 or 3
    int           stripRows;  // rows per strip, a multiple of 64 (one PSMCT16 page row)
    int           baseWords;  // VRAM word address of strip 0; the strips are contiguous
    int           zWords;     // 2 passes: the z strip's word address; 3: unused (-1)
    int           endWords;   // first word past the layout, page-aligned: where the CLUTs begin
};

// Works out the layout for a strip output (720p, 1080i) at the start of VRAM, with 'passes' 2
// or 3 (ps2_hires_passes; 1080i takes 3 whatever is asked: 2 would leave no texture heap).
// Records it for Get(). Returns false for an output that is not drawn in strips.
bool Configure(video::Output output, int passes);

// True once Configure has set a strip output up: everything below is for strip mode only.
bool Active();

// True once Start has the pass engine running (Active, and past gs::Init's own sends).
bool Running();

const Layout & Get();

// The VRAM word address of strip 'strip'.
int StripWords(int strip);

// ------------------------------------------------------------------------------------------------
// The pass engine
// ------------------------------------------------------------------------------------------------

// Brings the engine up: the pass thread, the INTC_GS handler, HSINT unmasked. Call once, from
// gs::Init, once the CRT mode, DISPLAY and DISPFB are programmed. 'pixelTests' is the TEST value
// the chain's 3D relies on between draws (gs::MakePixelTests), which each pass's clear restores.
void Start(u64 pixelTests);

// The colour each pass clears its strip to, and whether 16-bit dithering is on: rs::BeginFrame's.
void SetClear(const u8 rgb[3], bool dither);

// Starts the fixup list of the half a frame is about to be built in (rs::BeginFrame, once
// WaitForHalf has let it).
void BeginBuild(int half);

// Hands a finished frame's chain to the engine: 'chain' is the start of its DMA chain (terminated
// and written back), 'qwords' its length, 'half' the command buffer half it is in. It is replayed
// from the next field's first pass on, until another frame replaces it. Its ZBUF fixups are the
// ones recorded since BeginBuild.
void Publish(const void * chain, int qwords, int half);

// cmdbuf::DrainBeforeWorldLoad's: both halves are about to become the .bsp loader's scratch, so the
// frame the passes would replay (the loading screen) is copied to a heap buffer of its own, its
// chain's addresses and fixups moved with it, and replayed from there. Freed once a frame drawn
// after the load takes over (ReleaseCopy). With no frame, or no memory for the copy, the passes
// hold, blank, for the load instead.
void LendHalves(const void * halfBase0, const void * halfBase1, u32 halfBytes);

// Frees the copy LendHalves made, once the passes no longer replay it. From the main thread
// (rs::BeginFrame), since the heap is the main thread's.
void ReleaseCopy();

// Records a qword of the chain being built whose low word is a ZBUF register value: every pass
// rewrites its ZBP to the pass's z strip. 'zbufValue' points at the 64-bit value itself.
void AddZbufFixup(u64 * zbufValue);

// Blocks until the engine no longer replays the given command buffer half, so the EE may build
// into it. Immediately when it is not the one being replayed; otherwise at the next field's first
// pass, when the frame published after it takes over (or the hold below releases it).
void WaitForHalf(int half);

// Stops drawing: the picture holds what the last passes left in VRAM, and nothing the replayed
// frame samples is read again until Resume. What the texture heap's recovery needs (rs::FenceGs),
// and what a readback needs. Waits for a pass in flight to finish.
void Hold();
void Resume();
bool Held();

// Hold, released by the next frame a pass adopts rather than by Resume: what the texture heap's
// recovery wants, since the frame being built is the one that may then sample what the replayed
// one held.
void HoldUntilNextFrame();

// For a readback (screenshot.cpp): draws the replayed frame's pass 'pass' now and returns once the
// GS has finished it, leaving its strip's colour in VRAM until the next pass draws over the strip
// after it. Hold first, so no field's passes run in between. False with no frame to draw.
bool DrawPassNow(int pass);

// The GS lock: anything that sends to the GS outside the chain (a texture or CLUT upload, a
// readback) holds it, so it never meets a pass on the GIF. Passes wait for it.
void LockGs();
void UnlockGs();

// ------------------------------------------------------------------------------------------------
// Statistics (the frame log's strip columns)
// ------------------------------------------------------------------------------------------------

struct Stats
{
    u32 fields;        // fields drawn
    u32 passes;        // passes sent
    u32 late;          // passes that found the one before still drawing: that strip tore
    u32 skipped;       // passes not sent: held, or nothing published yet
    u32 busyCycles;    // EE cycles from each pass's kick to its FINISH, summed
    u32 worstCycles;   // the longest single pass
    u32 adoptions;     // frames that took over the replay
    u32 vsyncs;        // VSINTs: one a frame at 720p, a field at 1080i
    u32 fieldLines;    // HSINTs counted between the last two of them: the mode's lines a field
};

// Everything since the last call, and starts again from zero.
Stats TakeStats();

// What one strip's scan takes, in EE cycles: the budget a pass has before it tears.
u32 StripBudgetCycles();

} // namespace ps2::hires
