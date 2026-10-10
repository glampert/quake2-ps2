/* ================================================================================================
 * File: scr_print.cpp
 * Brief: Very crude debug printing to screen (PS2). Only used for fatal error reporting and dev.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/debug/scr_print.h"
#include "ps2/system/video_mode.h"

// PS2DEV libraries:
#include <stdio.h>
#include <stdarg.h>
#include <kernel.h>     // SetGsCrt, GsPutIMR, UNCACHED_SEG
#include <ee_regs.h>    // R_EE_* pointers to the GS privileged and DMAC registers
#include <gif_tags.h>   // GIF_SET_TAG, GIF_REG_AD, GIF_FLG_*, GIF_PRIM_*
#include <gs_gp.h>      // GS_REG_*/GS_SET_* general purpose register addresses and packers
#include <gs_psm.h>     // GS_PSM_*
#include <draw_tests.h> // ZTEST_METHOD_*

/*
 * This module is a rewrite of the on-screen debug printing found in the PS2DEV
 * SDK (ee/debug/src/scr_printf.c). It deliberately bypasses the engine renderer:
 * ScrInit takes the GS over, points it at a private framebuffer at the start of
 * VRAM, and from then on every glyph is blitted there with a HOST->LOCAL image
 * transfer pushed by hand down DMA channel 2 (the GIF channel). No libdraw, no
 * libgraph, no VU code. That is precisely what we want out of a panic screen:
 * it still works once the renderer itself has fallen over.
 *
 * The SDK original was a handful of inline assembly blobs poking hardware
 * register addresses, plus two structs full of undocumented magic numbers. Both
 * are replaced here by the SDK's register pointer macros (ee_regs.h) and its
 * GIF/GS bitfield packers (gif_tags.h, gs_gp.h), which produce exactly the same
 * bits while spelling out what each one of them means.
 */

namespace ps2::debug {

// Defined at the end of this file.
extern const u8 s_scrFontBitmap[];

namespace {

// ------------------------------------------------------------------------------------------------
// Video mode and framebuffer that the debug screen sets up for itself
// ------------------------------------------------------------------------------------------------

// The private framebuffer installed by ScrInit. Everything below is sized from
// these two numbers: the scissor box, the screen clearing sprite, the FRAME and
// BITBLTBUF buffer widths, and the char grid exported by scr_print.h.
constexpr int kScrFrameBufferWidth  = 640;
constexpr int kScrFrameBufferHeight = 224;

// FRAME/DISPFB/BITBLTBUF all express the buffer width in units of 64 pixels.
constexpr int kScrFrameBufferWidthIn64s = kScrFrameBufferWidth / 64;

// Chars are laid out on a fixed grid of ScrCharSize+2 pixel cells (see the notes
// in scr_print.h), which has to tile the framebuffer declared just above.
static_assert(kScrMaxX == kScrFrameBufferWidth  / (kScrCharSize + 2), "Char grid doesn't match framebuffer width!");
static_assert(kScrMaxY == kScrFrameBufferHeight / (kScrCharSize + 2), "Char grid doesn't match framebuffer height!");

// VRAM layout in GS pages (one page is 8192 bytes). The framebuffer needs
// 640*224*4 bytes == 70 pages starting at page 0. The Z-buffer is parked far
// past that so the two can never overlap; we never depth test anything, but the
// GS still demands a valid ZBUF_1 setting.
constexpr int kScrFrameBufferBasePage = 0;
constexpr int kScrZBufferBasePage     = 140;

// XYOFFSET_1 places the top-left corner of our drawing window somewhere inside
// the GS's 4096x4096 drawing space. The exact spot is arbitrary and inherited
// from the SDK debug screen, but every vertex below has to be biased by it.
// Both the offset and the vertex coordinates are 12.4 fixed point.
constexpr int kScrDrawOriginX = 1728;
constexpr int kScrDrawOriginY = 1936;

constexpr int ScrToFixed12_4(const int pixels)
{
    return pixels * 16;
}

// The GS DISPLAY register, packed by hand: DX, DY, MAGH and MAGV (minus one), then DW and DH
// (minus one) in the high word.
constexpr u64 MakeDisplay(const u32 dx, const u32 dy, const u32 magh, const u32 magv, const u32 dw, const u32 dh)
{
    return static_cast<u64>(dx & 0xFFFu)
         | (static_cast<u64>(dy & 0x7FFu) << 12)
         | (static_cast<u64>(magh & 0xFu) << 23)
         | (static_cast<u64>(magv & 0x3u) << 27)
         | (static_cast<u64>(dw & 0xFFFu) << 32)
         | (static_cast<u64>(dh & 0x7FFu) << 44);
}

static_assert(MakeDisplay(636, 50, 3, 1, 2559, 447) == 0x001BF9FF0983227Cull,
              "MakeDisplay must pack the value the SDK debug screen wrote by hand");

// ------------------------------------------------------------------------------------------------
// GIF packet layouts
//
// A GIFtag is one quadword: the low 64 bits carry NLOOP/EOP/FLG/NREG, the high
// 64 bits carry the register descriptor list. In PACKED mode with a single A+D
// descriptor, each of the NLOOP quadwords that follow is one GS register write.
// ------------------------------------------------------------------------------------------------

// A single entry of a PACKED mode A+D register list: 64 bits of payload followed
// by the address of the GS register it should be written to.
struct GifRegWrite
{
    u64 data;
    u64 regAddr;
};

constexpr int kScrSetupRegCount = 14;
constexpr int kScrGlyphRegCount = 4;

// Sent once by ScrInit. Programs the whole drawing environment (framebuffer,
// Z-buffer, scissor, color clamping, ...) and then paints a single black sprite
// over the entire framebuffer to clear whatever was on screen before us.
struct ScrSetupPacket
{
    u64 gifTag;
    u64 gifTagRegs;
    GifRegWrite regs[kScrSetupRegCount];
};

// Number of quadwords of pixel data trailing the glyph packet: one 8x8 glyph
// expanded to 32bpp is 8*8*4 == 256 bytes == 16 quadwords.
constexpr int kScrGlyphPixelQwords = static_cast<int>((kScrCharSize * kScrCharSize * sizeof(u32)) / 16);

// Sent ahead of every glyph. Sets up a HOST->LOCAL transfer of one 8x8 RGBA block
// into the framebuffer at (destX,destY), then opens an IMAGE mode GIFtag that the
// GIF fills with the pixel quadwords DMA'd right behind this packet.
struct ScrGlyphPacket
{
    u64 gifTag;
    u64 gifTagRegs;
    GifRegWrite bitBltBuf; // Transfer destination buffer, i.e. our framebuffer
    GifRegWrite trxPos;    // Destination X/Y - patched for each glyph by ScrPrintChar
    GifRegWrite trxReg;    // Transfer size, one ScrCharSize squared block
    GifRegWrite trxDir;    // Transfer direction; writing this register also starts it
    u64 pixelsGifTag;      // IMAGE mode tag for the pixel quadwords that follow
    u64 pixelsGifTagRegs;  // Descriptor half of the tag above, unused in IMAGE mode
};

static_assert((sizeof(ScrSetupPacket) % 16) == 0, "GIF packets must be a whole number of quadwords!");
static_assert((sizeof(ScrGlyphPacket) % 16) == 0, "GIF packets must be a whole number of quadwords!");

constexpr int kScrSetupPacketQwords = static_cast<int>(sizeof(ScrSetupPacket) / 16);
constexpr int kScrGlyphPacketQwords = static_cast<int>(sizeof(ScrGlyphPacket) / 16);

// ------------------------------------------------------------------------------------------------
// Screen printing local data
// ------------------------------------------------------------------------------------------------

static bool s_scrIsInit   = false;
static int s_scrCurrX     = 0;
static int s_scrCurrY     = 0;
static u32 s_scrTextColor = 0xFFFFFFFF; // text: white
static u32 s_scrBgColor   = 0x00000000; // background: black

// Never patched by the CPU after startup, so it can live in read-only memory.
alignas(16) static const ScrSetupPacket s_scrSetupPacket = {
    GIF_SET_TAG(kScrSetupRegCount, /* EOP = */ 1, 0, 0, GIF_FLG_PACKED, /* NREG = */ 1),
    GIF_REG_AD,
    {
        // Draw into our 32bpp framebuffer at the very start of VRAM, no write mask.
        { GS_SET_FRAME(kScrFrameBufferBasePage, kScrFrameBufferWidthIn64s, GS_PSM_32, 0), GS_REG_FRAME_1 },
        { GS_SET_ZBUF(kScrZBufferBasePage, GS_PSMZ_32, /* ZMSK = */ 0), GS_REG_ZBUF_1 },

        // Position our window inside the GS drawing space and clip everything to it.
        { GS_SET_XYOFFSET(ScrToFixed12_4(kScrDrawOriginX), ScrToFixed12_4(kScrDrawOriginY)), GS_REG_XYOFFSET_1 },
        { GS_SET_SCISSOR(0, kScrFrameBufferWidth - 1, 0, kScrFrameBufferHeight - 1), GS_REG_SCISSOR_1 },

        { GS_SET_PRMODECONT(1), GS_REG_PRMODECONT }, // Take the primitive state from PRIM, not PRMODE
        { GS_SET_COLCLAMP(1), GS_REG_COLCLAMP },     // Clamp RGB to 0..255 rather than wrapping around
        { GS_SET_DTHE(0), GS_REG_DTHE },             // No dithering

        // ZTE has to stay 1 (the GS has no way to switch depth testing off), so
        // the test method is what we play with: GREATER is the resting state,
        // then ALLPASS lets the clearing sprite below through no matter what is
        // left over in the Z-buffer we never initialized.
        { GS_SET_TEST(0, 0, 0, 0, 0, 0, /* ZTE = */ 1, ZTEST_METHOD_GREATER), GS_REG_TEST_1 },
        { GS_SET_TEST(0, 0, 0, 0, 0, 0, /* ZTE = */ 1, ZTEST_METHOD_ALLPASS), GS_REG_TEST_1 },

        // One opaque black sprite covering the whole framebuffer, given by its
        // top-left and bottom-right corners, both biased by the draw origin.
        { GS_SET_PRIM(GIF_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0), GS_REG_PRIM },
        { GS_SET_RGBAQ(0, 0, 0, 0, /* Q = 1.0f */ 0x3F800000), GS_REG_RGBAQ },
        { GS_SET_XYZ(ScrToFixed12_4(kScrDrawOriginX), ScrToFixed12_4(kScrDrawOriginY), 0), GS_REG_XYZ2 },
        { GS_SET_XYZ(ScrToFixed12_4(kScrDrawOriginX + kScrFrameBufferWidth),
                     ScrToFixed12_4(kScrDrawOriginY + kScrFrameBufferHeight), 0), GS_REG_XYZ2 },

        // Put the depth test back the way it was before the clear.
        { GS_SET_TEST(0, 0, 0, 0, 0, 0, /* ZTE = */ 1, ZTEST_METHOD_GREATER), GS_REG_TEST_1 },
    }
};

// Patched in place (through the uncached segment) by every ScrPrintChar call.
alignas(16) static ScrGlyphPacket s_scrGlyphPacket = {
    GIF_SET_TAG(kScrGlyphRegCount, /* EOP = */ 0, 0, 0, GIF_FLG_PACKED, /* NREG = */ 1),
    GIF_REG_AD,
    { GS_SET_BITBLTBUF(0, 0, 0, kScrFrameBufferBasePage, kScrFrameBufferWidthIn64s, GS_PSM_32), GS_REG_BITBLTBUF },
    { GS_SET_TRXPOS(0, 0, /* DSAX = */ 0, /* DSAY = */ 0, 0), GS_REG_TRXPOS },
    { GS_SET_TRXREG(kScrCharSize, kScrCharSize), GS_REG_TRXREG },
    { GS_SET_TRXDIR(/* HOST -> LOCAL */ 0), GS_REG_TRXDIR },
    GIF_SET_TAG(kScrGlyphPixelQwords, /* EOP = */ 0, 0, 0, GIF_FLG_IMAGE, /* NREG = */ 0),
    0
};

// Scratch buffer holding one glyph expanded from the 1bpp font to 32bpp RGBA.
// DMA'd to the GS immediately behind s_scrGlyphPacket.
alignas(16) static u32 s_scrGlyphPixels[kScrCharSize * kScrCharSize];

// ------------------------------------------------------------------------------------------------
// GS and DMAC plumbing
// ------------------------------------------------------------------------------------------------

// Dn_CHCR bits (see the DMAC chapter of the EE User's Manual).
constexpr u32 kDmaChcrDirFromMemory = 1 << 0; // DIR: 0 == to memory, 1 == from memory
constexpr u32 kDmaChcrStart         = 1 << 8; // STR: set to start, cleared by the DMAC when done

// D_CTRL bits.
constexpr u32 kDmaCtrlEnable = 1 << 0; // DMAE: global DMA transfer enable

// D_ENABLER/D_ENABLEW bits.
constexpr u32 kDmaEnableSuspend = 1 << 16; // CPND: suspend every channel

// Reset bits of VIF1_FBRST and GIF_CTRL, and VU1's in VU0 control register 28 (FBRST).
constexpr u32 kVif1FbrstReset  = 1 << 0; // RST
constexpr u32 kGifCtrlReset    = 1 << 0; // RST
constexpr u32 kVuFbrstResetVu1 = 1 << 9; // RS1

// COP0 Status bit that makes the COP2 (VU0 macro mode) instructions usable.
constexpr u32 kCop0StatusCu2 = 1u << 30;

// D_STAT interrupt status bits for every DMA channel except SIF0/SIF1/SIF2
// (bits 5-7), which belong to the IOP<->EE RPC layer and must be left alone,
// plus the stall/MFIFO-empty/bus-error status bits.
constexpr u32 kDmaStatNonSifBits = 0xFF1F;

// D2_SADR isn't documented as existing for channel 2, hence no R_EE_ macro for
// it, but Sony's own DMAC init writes it and so does the SDK debug screen.
constexpr u32 kScrD2SadrAddr = 0x1000A080;

// Soft reset the GS and reprogram the CRT controller for the given video mode.
void ResetGs(s16 interlace, s16 videoMode, s16 fieldMode)
{
    *R_EE_GS_CSR = 0x200; // CSR.RESET
    GsPutIMR(0xFF00);     // Mask off every GS interrupt source; we poll instead
    SetGsCrt(interlace, videoMode, fieldMode);
}

// Point read circuit 2 at our framebuffer and stretch it over the visible raster. The
// register values are the SDK debug screen's, apart from where the picture starts, which
// follows the standard.
void SetVideoMode(const ps2::video::Standard standard)
{
    // EN1 0 / EN2 1 (only read circuit 2 is enabled), MMOD/AMOD take the output
    // alpha from the ALP field instead of the framebuffer, ALP 0xFF (opaque).
    *R_EE_GS_PMODE = 0xFF62;

    // INT 1 (interlaced), FFMD 0 (FIELD mode - reads every other scanline).
    // Note this overrides the field mode handed to SetGsCrt() in ScrInit.
    *R_EE_GS_SMODE2 = 1;

    // FBP 0, FBW 10 (640 pixels), PSM PSMCT32, no X/Y offset into the buffer.
    *R_EE_GS_DISPFB2 = 0x1400;

    // MAGH 4x, MAGV 2x, DW 2560, DH 448: the 4x/2x magnification is what blows our comparatively
    // tiny 640x224 buffer up to fill the screen - all of NTSC's 448 lines, PAL's top 448 of 512.
    // DX/DY are libgraph's origin for the standard, so the text sits where the game's own picture
    // does. On NTSC that is 4 pixels right of the SDK debug screen's DX of 636.
    const u32 dx = static_cast<u32>(ps2::video::DisplayX(standard));
    const u32 dy = static_cast<u32>(ps2::video::DisplayY(standard));
    *R_EE_GS_DISPLAY2 = MakeDisplay(dx, dy, 3, 1, 2559, 447);
}

// Frees every path into the GS before the debug screen takes PATH3. A fatal error
// can land while the renderer's chain is in flight, or because it wedged: a VU1
// program that never ends, a GIF packet that never reaches EOP. Then the GIF
// never grants PATH3, the first DmaWaitGif() spins forever, and the screen
// ResetGs() has already blanked is all that is left. That is the likely story of
// the solid green screen a console showed after a pipeline hang, with the error
// only in the log.
//
// The order is libgs's GsResetPath (ee/libgs/src/ResetPath.s): VIF1, then VU1,
// then the GIF, each one after whatever feeds it, so nothing restarts what was
// just reset. The DMA channels go first, for the same reason.
void ResetGraphicsPaths()
{
    // Stop the VIF1 chain and any GIF transfer in flight. Channel 2 in particular:
    // the rest of an interrupted texture upload would reach the freshly reset GIF
    // as tags. A running channel may only be stopped with the DMAC suspended.
    const u32 dmaEnable = *R_EE_D_ENABLER;
    *R_EE_D_ENABLEW = dmaEnable | kDmaEnableSuspend;
    *R_EE_D1_CHCR = 0;
    *R_EE_D2_CHCR = 0;
    *R_EE_D_ENABLEW = dmaEnable;

    // VIF1 first, so it starts no further microprogram or DIRECT transfer.
    *R_EE_VIF1_FBRST = kVif1FbrstReset;

    // VU1, or a program still running would XGKICK into the GIF again, and one
    // stalled in an XGKICK would resume with half a packet. Its reset lives in
    // VU0's control register 28, so it takes COP2, which a thread may not have
    // enabled; without it, this would fault and turn the error into a CPU
    // exception report. Read-modify-write, as libgs does, to keep the D/T-bit
    // enables in the same register.
    u32 cop0Status;
    asm volatile("mfc0 %0, $12" : "=r"(cop0Status));
    if ((cop0Status & kCop0StatusCu2) != 0)
    {
        u32 vuFbrst;
        asm volatile("cfc2 %0, $28" : "=r"(vuFbrst));
        vuFbrst |= kVuFbrstResetVu1;
        asm volatile("ctc2 %0, $28 \n\t"
                     "sync.p       \n\t"
                     : : "r"(vuFbrst) : "memory");
    }

    // Last, the GIF: drops the open path and anything queued in its FIFO.
    *R_EE_GIF_CTRL = kGifCtrlReset;
}

// Reset DMA channel 2 (GIF) plus the global DMAC registers, so that we can drive
// the GIF by hand no matter what state the engine's renderer left the DMAC in.
// Modelled on Sony's bulk DMAC init, which is why parts of it look redundant.
void DmaReset()
{
    *reinterpret_cast<vu32 *>(kScrD2SadrAddr) = 0;

    // NOTE: the SDK routine this was transcribed from clears channel 3's CHCR
    // here (0x1000B000) and never touches D2_CHCR. Current ps2sdk reads that as
    // a typo and clears D2_CHCR instead. Kept as-is: ResetGraphicsPaths() has
    // already stopped channel 2.
    *R_EE_D3_CHCR = 0;

    *R_EE_D2_TADR = 0;
    *R_EE_D2_MADR = 0;
    *R_EE_D2_ASR1 = 0;
    *R_EE_D2_ASR0 = 0;

    // Writing a 1 to a D_STAT status bit clears it, while a 1 in the mask half
    // (bits 16-31) *toggles* that mask. Both writes below stay inside the low
    // half on purpose, so the interrupt masks - and the SIF channels - come out
    // exactly as we found them.
    *R_EE_D_STAT  = kDmaStatNonSifBits;
    *R_EE_D_STAT &= kDmaStatNonSifBits;

    *R_EE_D_CTRL = 0;
    *R_EE_D_PCR  = 0;
    *R_EE_D_SQWC = 0;
    *R_EE_D_RBOR = 0;
    *R_EE_D_RBSR = 0;

    *R_EE_D_CTRL |= kDmaCtrlEnable;
}

// Kick a normal mode DMA transfer from EE memory into the GIF. 'data' must be 16
// byte aligned and 'qwordCount' is its size in 16 byte quadwords. Returns as soon
// as the channel starts; call DmaWaitGif() to find out when it has drained.
void DmaSendToGif(const void * data, int qwordCount)
{
    *R_EE_D2_QWC  = static_cast<u32>(qwordCount);
    *R_EE_D2_MADR = reinterpret_cast<u32>(data);
    *R_EE_D2_CHCR = kDmaChcrStart | kDmaChcrDirFromMemory;
}

// Block until DMA channel 2 (the GIF channel) has finished its current transfer.
void DmaWaitGif()
{
    while ((*R_EE_D2_CHCR & kDmaChcrStart) != 0)
    {
        // Busy wait. This module only runs from fatal error paths and dev code.
    }
}

// Advance the ScrPrintf cursor to the start of the next line, clearing it.
void NextLine()
{
    s_scrCurrX = 0;
    ++s_scrCurrY;
    if (s_scrCurrY == kScrMaxY)
    {
        s_scrCurrY = 0;
    }
    ScrClearLine(s_scrCurrY);
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Screen debug printing functions
// ------------------------------------------------------------------------------------------------

void ScrInit()
{
    ResetGraphicsPaths();
    DmaReset();

    // In the standard the game chose (ps2_video_mode), or the console's own if it hasn't yet - an
    // error screen in a standard the TV can't take would show nothing at all.
    const ps2::video::Standard standard = ps2::video::Selected();

    // Interlaced; FRAME field mode, which SetVideoMode() below flips back to FIELD.
    ResetGs(true, ps2::video::GsCrtMode(standard), 1);
    SetVideoMode(standard);

    DmaWaitGif();
    DmaSendToGif(&s_scrSetupPacket, kScrSetupPacketQwords);
    DmaWaitGif();

    s_scrIsInit = true;
}

void ScrPrintChar(int x, int y, u32 color, int ch)
{
    if (x < 0 || x >= kScrMaxX || y < 0 || y >= kScrMaxY)
    {
        return; // Invalid screen index.
    }

    if (!s_scrIsInit)
    {
        ScrInit();
    }

    // The GIF reads both of these straight out of RAM, so every CPU store below
    // has to go through the uncached segment to bypass the data cache.
    ScrGlyphPacket * const glyphPacket = static_cast<ScrGlyphPacket *>(UNCACHED_SEG(&s_scrGlyphPacket));
    u32 * const glyphPixels = static_cast<u32 *>(UNCACHED_SEG(s_scrGlyphPixels));

    // Offset a little in the sides and between each char, so they don't bunch-up.
    const int destX = x * (kScrCharSize + 2) + 2;
    const int destY = y * (kScrCharSize + 2) + 2;
    glyphPacket->trxPos.data = GS_SET_TRXPOS(0, 0, destX, destY, 0);

    // Let the transfer setup go out while we expand the glyph below.
    DmaSendToGif(&s_scrGlyphPacket, kScrGlyphPacketQwords);

    // Expand the 1bpp font glyph into 32bpp pixels. Bit 7 of each font byte is
    // the leftmost pixel of that row; unset bits take the background color, so
    // the blit also erases whichever char occupied this cell before.
    const u8 * fontRow = &s_scrFontBitmap[(ch & 0xFF) * kScrCharSize];
    for (int row = 0; row < kScrCharSize; ++row, ++fontRow)
    {
        for (int col = 0; col < kScrCharSize; ++col)
        {
            const bool isLit = (*fontRow & (0x80 >> col)) != 0;
            glyphPixels[(row * kScrCharSize) + col] = (isLit ? color : s_scrBgColor);
        }
    }

    DmaWaitGif();
    DmaSendToGif(s_scrGlyphPixels, kScrGlyphPixelQwords);
    DmaWaitGif();
}

void ScrPrintf(const char * format, ...)
{
    if (!s_scrIsInit)
    {
        ScrInit();
    }

    // 'static' should avoid stressing the stack with this large buffer.
    // This function should be robust enough to be used for most error
    // reporting situations.
    static char s_tempbuff[2048];

    va_list argptr;
    va_start(argptr, format);
    int bufsz = vsnprintf(s_tempbuff, sizeof(s_tempbuff), format, argptr);
    va_end(argptr);

    // vsnprintf returns the untruncated length; clamp to what was actually written.
    if (bufsz >= static_cast<int>(sizeof(s_tempbuff)))
    {
        bufsz = sizeof(s_tempbuff) - 1;
    }

    // Echo to stdout and flush.
    printf("[Q2] %s", s_tempbuff);
    fflush(stdout);

    for (int i = 0; i < bufsz; ++i)
    {
        const int c = s_tempbuff[i];
        switch (c)
        {
        case '\n':
            NextLine();
            break;

        case '\t':
            // 4 spaces per TAB, stopping at the screen edge so the cursor never overshoots ScrMaxX.
            for (int j = 0; j < 4 && s_scrCurrX < kScrMaxX; ++j)
            {
                ScrPrintChar(s_scrCurrX, s_scrCurrY, s_scrTextColor, ' ');
                ++s_scrCurrX;
            }
            break;

        default:
            // Wrap lazily so a full line followed by '\n' doesn't leave a blank row.
            if (s_scrCurrX == kScrMaxX)
            {
                NextLine();
            }
            ScrPrintChar(s_scrCurrX, s_scrCurrY, s_scrTextColor, c);
            ++s_scrCurrX;
            break;
        } // switch (c)
    }
}

int ScrGetPrintPosX()
{
    return s_scrCurrX;
}

int ScrGetPrintPosY()
{
    return s_scrCurrY;
}

void ScrSetPrintPos(int x, int y)
{
    if (x >= 0 && x < kScrMaxX)
    {
        s_scrCurrX = x;
    }
    if (y >= 0 && y < kScrMaxY)
    {
        s_scrCurrY = y;
    }
}

void ScrSetBgColor(u32 color)
{
    s_scrBgColor = color;
}

u32 ScrGetBgColor()
{
    return s_scrBgColor;
}

void ScrSetTextColor(u32 color)
{
    s_scrTextColor = color;
}

u32 ScrGetTextColor()
{
    return s_scrTextColor;
}

void ScrClear()
{
    for (int y = 0; y < kScrMaxY; ++y)
    {
        ScrClearLine(y);
    }

    s_scrCurrX = 0;
    s_scrCurrY = 0;
}

void ScrClearLine(int y)
{
    for (int x = 0; x < kScrMaxX; ++x)
    {
        ScrPrintChar(x, y, s_scrBgColor, ' ');
    }
}

// ------------------------------------------------------------------------------------------------
// The debug printing bitmap font
//
// 256 chars of 8 bytes each: one byte per glyph row, one bit per pixel, MSB first.
// ------------------------------------------------------------------------------------------------

alignas(16) const u8 s_scrFontBitmap[] =
"\x00\x00\x00\x00\x00\x00\x00\x00\x3c\x42\xa5\x81\xa5\x99\x42\x3c"
"\x3c\x7e\xdb\xff\xff\xdb\x66\x3c\x6c\xfe\xfe\xfe\x7c\x38\x10\x00"
"\x10\x38\x7c\xfe\x7c\x38\x10\x00\x10\x38\x54\xfe\x54\x10\x38\x00"
"\x10\x38\x7c\xfe\xfe\x10\x38\x00\x00\x00\x00\x30\x30\x00\x00\x00"
"\xff\xff\xff\xe7\xe7\xff\xff\xff\x38\x44\x82\x82\x82\x44\x38\x00"
"\xc7\xbb\x7d\x7d\x7d\xbb\xc7\xff\x0f\x03\x05\x79\x88\x88\x88\x70"
"\x38\x44\x44\x44\x38\x10\x7c\x10\x30\x28\x24\x24\x28\x20\xe0\xc0"
"\x3c\x24\x3c\x24\x24\xe4\xdc\x18\x10\x54\x38\xee\x38\x54\x10\x00"
"\x10\x10\x10\x7c\x10\x10\x10\x10\x10\x10\x10\xff\x00\x00\x00\x00"
"\x00\x00\x00\xff\x10\x10\x10\x10\x10\x10\x10\xf0\x10\x10\x10\x10"
"\x10\x10\x10\x1f\x10\x10\x10\x10\x10\x10\x10\xff\x10\x10\x10\x10"
"\x10\x10\x10\x10\x10\x10\x10\x10\x00\x00\x00\xff\x00\x00\x00\x00"
"\x00\x00\x00\x1f\x10\x10\x10\x10\x00\x00\x00\xf0\x10\x10\x10\x10"
"\x10\x10\x10\x1f\x00\x00\x00\x00\x10\x10\x10\xf0\x00\x00\x00\x00"
"\x81\x42\x24\x18\x18\x24\x42\x81\x01\x02\x04\x08\x10\x20\x40\x80"
"\x80\x40\x20\x10\x08\x04\x02\x01\x00\x10\x10\xff\x10\x10\x00\x00"
"\x00\x00\x00\x00\x00\x00\x00\x00\x20\x20\x20\x20\x00\x00\x20\x00"
"\x50\x50\x50\x00\x00\x00\x00\x00\x50\x50\xf8\x50\xf8\x50\x50\x00"
"\x20\x78\xa0\x70\x28\xf0\x20\x00\xc0\xc8\x10\x20\x40\x98\x18\x00"
"\x40\xa0\x40\xa8\x90\x98\x60\x00\x10\x20\x40\x00\x00\x00\x00\x00"
"\x10\x20\x40\x40\x40\x20\x10\x00\x40\x20\x10\x10\x10\x20\x40\x00"
"\x20\xa8\x70\x20\x70\xa8\x20\x00\x00\x20\x20\xf8\x20\x20\x00\x00"
"\x00\x00\x00\x00\x00\x20\x20\x40\x00\x00\x00\x78\x00\x00\x00\x00"
"\x00\x00\x00\x00\x00\x60\x60\x00\x00\x00\x08\x10\x20\x40\x80\x00"
"\x70\x88\x98\xa8\xc8\x88\x70\x00\x20\x60\xa0\x20\x20\x20\xf8\x00"
"\x70\x88\x08\x10\x60\x80\xf8\x00\x70\x88\x08\x30\x08\x88\x70\x00"
"\x10\x30\x50\x90\xf8\x10\x10\x00\xf8\x80\xe0\x10\x08\x10\xe0\x00"
"\x30\x40\x80\xf0\x88\x88\x70\x00\xf8\x88\x10\x20\x20\x20\x20\x00"
"\x70\x88\x88\x70\x88\x88\x70\x00\x70\x88\x88\x78\x08\x10\x60\x00"
"\x00\x00\x20\x00\x00\x20\x00\x00\x00\x00\x20\x00\x00\x20\x20\x40"
"\x18\x30\x60\xc0\x60\x30\x18\x00\x00\x00\xf8\x00\xf8\x00\x00\x00"
"\xc0\x60\x30\x18\x30\x60\xc0\x00\x70\x88\x08\x10\x20\x00\x20\x00"
"\x70\x88\x08\x68\xa8\xa8\x70\x00\x20\x50\x88\x88\xf8\x88\x88\x00"
"\xf0\x48\x48\x70\x48\x48\xf0\x00\x30\x48\x80\x80\x80\x48\x30\x00"
"\xe0\x50\x48\x48\x48\x50\xe0\x00\xf8\x80\x80\xf0\x80\x80\xf8\x00"
"\xf8\x80\x80\xf0\x80\x80\x80\x00\x70\x88\x80\xb8\x88\x88\x70\x00"
"\x88\x88\x88\xf8\x88\x88\x88\x00\x70\x20\x20\x20\x20\x20\x70\x00"
"\x38\x10\x10\x10\x90\x90\x60\x00\x88\x90\xa0\xc0\xa0\x90\x88\x00"
"\x80\x80\x80\x80\x80\x80\xf8\x00\x88\xd8\xa8\xa8\x88\x88\x88\x00"
"\x88\xc8\xc8\xa8\x98\x98\x88\x00\x70\x88\x88\x88\x88\x88\x70\x00"
"\xf0\x88\x88\xf0\x80\x80\x80\x00\x70\x88\x88\x88\xa8\x90\x68\x00"
"\xf0\x88\x88\xf0\xa0\x90\x88\x00\x70\x88\x80\x70\x08\x88\x70\x00"
"\xf8\x20\x20\x20\x20\x20\x20\x00\x88\x88\x88\x88\x88\x88\x70\x00"
"\x88\x88\x88\x88\x50\x50\x20\x00\x88\x88\x88\xa8\xa8\xd8\x88\x00"
"\x88\x88\x50\x20\x50\x88\x88\x00\x88\x88\x88\x70\x20\x20\x20\x00"
"\xf8\x08\x10\x20\x40\x80\xf8\x00\x70\x40\x40\x40\x40\x40\x70\x00"
"\x00\x00\x80\x40\x20\x10\x08\x00\x70\x10\x10\x10\x10\x10\x70\x00"
"\x20\x50\x88\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xf8\x00"
"\x40\x20\x10\x00\x00\x00\x00\x00\x00\x00\x70\x08\x78\x88\x78\x00"
"\x80\x80\xb0\xc8\x88\xc8\xb0\x00\x00\x00\x70\x88\x80\x88\x70\x00"
"\x08\x08\x68\x98\x88\x98\x68\x00\x00\x00\x70\x88\xf8\x80\x70\x00"
"\x10\x28\x20\xf8\x20\x20\x20\x00\x00\x00\x68\x98\x98\x68\x08\x70"
"\x80\x80\xf0\x88\x88\x88\x88\x00\x20\x00\x60\x20\x20\x20\x70\x00"
"\x10\x00\x30\x10\x10\x10\x90\x60\x40\x40\x48\x50\x60\x50\x48\x00"
"\x60\x20\x20\x20\x20\x20\x70\x00\x00\x00\xd0\xa8\xa8\xa8\xa8\x00"
"\x00\x00\xb0\xc8\x88\x88\x88\x00\x00\x00\x70\x88\x88\x88\x70\x00"
"\x00\x00\xb0\xc8\xc8\xb0\x80\x80\x00\x00\x68\x98\x98\x68\x08\x08"
"\x00\x00\xb0\xc8\x80\x80\x80\x00\x00\x00\x78\x80\xf0\x08\xf0\x00"
"\x40\x40\xf0\x40\x40\x48\x30\x00\x00\x00\x90\x90\x90\x90\x68\x00"
"\x00\x00\x88\x88\x88\x50\x20\x00\x00\x00\x88\xa8\xa8\xa8\x50\x00"
"\x00\x00\x88\x50\x20\x50\x88\x00\x00\x00\x88\x88\x98\x68\x08\x70"
"\x00\x00\xf8\x10\x20\x40\xf8\x00\x18\x20\x20\x40\x20\x20\x18\x00"
"\x20\x20\x20\x00\x20\x20\x20\x00\xc0\x20\x20\x10\x20\x20\xc0\x00"
"\x40\xa8\x10\x00\x00\x00\x00\x00\x00\x00\x20\x50\xf8\x00\x00\x00"
"\x70\x88\x80\x80\x88\x70\x20\x60\x90\x00\x00\x90\x90\x90\x68\x00"
"\x10\x20\x70\x88\xf8\x80\x70\x00\x20\x50\x70\x08\x78\x88\x78\x00"
"\x48\x00\x70\x08\x78\x88\x78\x00\x20\x10\x70\x08\x78\x88\x78\x00"
"\x20\x00\x70\x08\x78\x88\x78\x00\x00\x70\x80\x80\x80\x70\x10\x60"
"\x20\x50\x70\x88\xf8\x80\x70\x00\x50\x00\x70\x88\xf8\x80\x70\x00"
"\x20\x10\x70\x88\xf8\x80\x70\x00\x50\x00\x00\x60\x20\x20\x70\x00"
"\x20\x50\x00\x60\x20\x20\x70\x00\x40\x20\x00\x60\x20\x20\x70\x00"
"\x50\x00\x20\x50\x88\xf8\x88\x00\x20\x00\x20\x50\x88\xf8\x88\x00"
"\x10\x20\xf8\x80\xf0\x80\xf8\x00\x00\x00\x6c\x12\x7e\x90\x6e\x00"
"\x3e\x50\x90\x9c\xf0\x90\x9e\x00\x60\x90\x00\x60\x90\x90\x60\x00"
"\x90\x00\x00\x60\x90\x90\x60\x00\x40\x20\x00\x60\x90\x90\x60\x00"
"\x40\xa0\x00\xa0\xa0\xa0\x50\x00\x40\x20\x00\xa0\xa0\xa0\x50\x00"
"\x90\x00\x90\x90\xb0\x50\x10\xe0\x50\x00\x70\x88\x88\x88\x70\x00"
"\x50\x00\x88\x88\x88\x88\x70\x00\x20\x20\x78\x80\x80\x78\x20\x20"
"\x18\x24\x20\xf8\x20\xe2\x5c\x00\x88\x50\x20\xf8\x20\xf8\x20\x00"
"\xc0\xa0\xa0\xc8\x9c\x88\x88\x8c\x18\x20\x20\xf8\x20\x20\x20\x40"
"\x10\x20\x70\x08\x78\x88\x78\x00\x10\x20\x00\x60\x20\x20\x70\x00"
"\x20\x40\x00\x60\x90\x90\x60\x00\x20\x40\x00\x90\x90\x90\x68\x00"
"\x50\xa0\x00\xa0\xd0\x90\x90\x00\x28\x50\x00\xc8\xa8\x98\x88\x00"
"\x00\x70\x08\x78\x88\x78\x00\xf8\x00\x60\x90\x90\x90\x60\x00\xf0"
"\x20\x00\x20\x40\x80\x88\x70\x00\x00\x00\x00\xf8\x80\x80\x00\x00"
"\x00\x00\x00\xf8\x08\x08\x00\x00\x84\x88\x90\xa8\x54\x84\x08\x1c"
"\x84\x88\x90\xa8\x58\xa8\x3c\x08\x20\x00\x00\x20\x20\x20\x20\x00"
"\x00\x00\x24\x48\x90\x48\x24\x00\x00\x00\x90\x48\x24\x48\x90\x00"
"\x28\x50\x20\x50\x88\xf8\x88\x00\x28\x50\x70\x08\x78\x88\x78\x00"
"\x28\x50\x00\x70\x20\x20\x70\x00\x28\x50\x00\x20\x20\x20\x70\x00"
"\x28\x50\x00\x70\x88\x88\x70\x00\x50\xa0\x00\x60\x90\x90\x60\x00"
"\x28\x50\x00\x88\x88\x88\x70\x00\x50\xa0\x00\xa0\xa0\xa0\x50\x00"
"\xfc\x48\x48\x48\xe8\x08\x50\x20\x00\x50\x00\x50\x50\x50\x10\x20"
"\xc0\x44\xc8\x54\xec\x54\x9e\x04\x10\xa8\x40\x00\x00\x00\x00\x00"
"\x00\x20\x50\x88\x50\x20\x00\x00\x88\x10\x20\x40\x80\x28\x00\x00"
"\x7c\xa8\xa8\x68\x28\x28\x28\x00\x38\x40\x30\x48\x48\x30\x08\x70"
"\x00\x00\x00\x00\x00\x00\xff\xff\xf0\xf0\xf0\xf0\x0f\x0f\x0f\x0f"
"\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00"
"\x00\x00\x00\x3c\x3c\x00\x00\x00\xff\xff\xff\xff\xff\xff\x00\x00"
"\xc0\xc0\xc0\xc0\xc0\xc0\xc0\xc0\x0f\x0f\x0f\x0f\xf0\xf0\xf0\xf0"
"\xfc\xfc\xfc\xfc\xfc\xfc\xfc\xfc\x03\x03\x03\x03\x03\x03\x03\x03"
"\x3f\x3f\x3f\x3f\x3f\x3f\x3f\x3f\x11\x22\x44\x88\x11\x22\x44\x88"
"\x88\x44\x22\x11\x88\x44\x22\x11\xfe\x7c\x38\x10\x00\x00\x00\x00"
"\x00\x00\x00\x00\x10\x38\x7c\xfe\x80\xc0\xe0\xf0\xe0\xc0\x80\x00"
"\x01\x03\x07\x0f\x07\x03\x01\x00\xff\x7e\x3c\x18\x18\x3c\x7e\xff"
"\x81\xc3\xe7\xff\xff\xe7\xc3\x81\xf0\xf0\xf0\xf0\x00\x00\x00\x00"
"\x00\x00\x00\x00\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x00\x00\x00\x00"
"\x00\x00\x00\x00\xf0\xf0\xf0\xf0\x33\x33\xcc\xcc\x33\x33\xcc\xcc"
"\x00\x20\x20\x50\x50\x88\xf8\x00\x20\x20\x70\x20\x70\x20\x20\x00"
"\x00\x00\x00\x50\x88\xa8\x50\x00\xff\xff\xff\xff\xff\xff\xff\xff"
"\x00\x00\x00\x00\xff\xff\xff\xff\xf0\xf0\xf0\xf0\xf0\xf0\xf0\xf0"
"\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x0f\xff\xff\xff\xff\x00\x00\x00\x00"
"\x00\x00\x68\x90\x90\x90\x68\x00\x30\x48\x48\x70\x48\x48\x70\xc0"
"\xf8\x88\x80\x80\x80\x80\x80\x00\xf8\x50\x50\x50\x50\x50\x98\x00"
"\xf8\x88\x40\x20\x40\x88\xf8\x00\x00\x00\x78\x90\x90\x90\x60\x00"
"\x00\x50\x50\x50\x50\x68\x80\x80\x00\x50\xa0\x20\x20\x20\x20\x00"
"\xf8\x20\x70\xa8\xa8\x70\x20\xf8\x20\x50\x88\xf8\x88\x50\x20\x00"
"\x70\x88\x88\x88\x50\x50\xd8\x00\x30\x40\x40\x20\x50\x50\x50\x20"
"\x00\x00\x00\x50\xa8\xa8\x50\x00\x08\x70\xa8\xa8\xa8\x70\x80\x00"
"\x38\x40\x80\xf8\x80\x40\x38\x00\x70\x88\x88\x88\x88\x88\x88\x00"
"\x00\xf8\x00\xf8\x00\xf8\x00\x00\x20\x20\xf8\x20\x20\x00\xf8\x00"
"\xc0\x30\x08\x30\xc0\x00\xf8\x00\x18\x60\x80\x60\x18\x00\xf8\x00"
"\x10\x28\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\xa0\x40"
"\x00\x20\x00\xf8\x00\x20\x00\x00\x00\x50\xa0\x00\x50\xa0\x00\x00"
"\x00\x18\x24\x24\x18\x00\x00\x00\x00\x30\x78\x78\x30\x00\x00\x00"
"\x00\x00\x00\x00\x30\x00\x00\x00\x3e\x20\x20\x20\xa0\x60\x20\x00"
"\xa0\x50\x50\x50\x00\x00\x00\x00\x40\xa0\x20\x40\xe0\x00\x00\x00"
"\x00\x38\x38\x38\x38\x38\x38\x00\x00\x00\x00\x00\x00\x00\x00";

} // namespace ps2::debug
