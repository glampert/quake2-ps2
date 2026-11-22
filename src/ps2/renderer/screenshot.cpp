/* ================================================================================================
 * File: screenshot.cpp
 * Brief: The "screenshot" command. See screenshot.h.
 *
 *  ref_gl read the whole frame with glReadPixels into one buffer and wrote it bottom row first.
 *  Here the frame comes out of VRAM a strip at a time, top row first, and each strip goes to
 *  the file as soon as it is converted, so the TGA says its rows run top-down. That keeps the
 *  memory it takes to two strips rather than a whole frame (up to 960 KB at 640x512), which a
 *  heap with a level loaded may not have in one piece.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/common.h"
#include "ps2/renderer/screenshot.h"
#include "ps2/renderer/gs.h"
#include "ps2/renderer/render_system.h"
#include "ps2/system/heap.h"
#include "ps2/system/iop_boot.h" // SyncGameDataDevice, DebugOutputGamedir

#include <cstdio>
#include <cstring>
#include <gs_psm.h>

namespace ps2::screenshot {
namespace {

// ref_gl's numbering: quake00 to quake99.
constexpr int kMaxNumbered = 100;

// Rows read back and written at a time. A 32-bit frame is more than one DMA transfer can carry,
// so it comes out in strips through one staging block regardless.
constexpr int kStripRows = 32;

constexpr int kTgaHeaderBytes = 18;

// Longest name "screenshot <name>" takes, short of the ".tga".
constexpr int kMaxNameLen = 32;

bool FileExists(const char * path)
{
    std::FILE * file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return false;
    }
    std::fclose(file);
    return true;
}

// The file to write: scrnshot/<name>.tga for a name given, otherwise the first quakeNN.tga that
// doesn't exist. False, having said why, when there is none to use.
bool PickPath(char (&path)[MAX_OSPATH], const char * dir, const char * name)
{
    if (name != nullptr)
    {
        const int len = static_cast<int>(std::strlen(name));
        if (len == 0 || len > kMaxNameLen || std::strpbrk(name, "/\\:") != nullptr)
        {
            Com_Printf("screenshot: '%s' is not a file name (up to %d characters, no path).\n", name, kMaxNameLen);
            return false;
        }
        if (std::snprintf(path, sizeof(path), "%s/%s.tga", dir, name) >= static_cast<int>(sizeof(path)))
        {
            Com_Printf("screenshot: the path to %s is too long.\n", dir);
            return false;
        }
        return true;
    }

    for (int i = 0; i < kMaxNumbered; ++i)
    {
        if (std::snprintf(path, sizeof(path), "%s/quake%02d.tga", dir, i) >= static_cast<int>(sizeof(path)))
        {
            Com_Printf("screenshot: the path to %s is too long.\n", dir);
            return false;
        }
        if (!FileExists(path))
        {
            return true;
        }
    }
    Com_Printf("screenshot: quake00 to quake%02d.tga are all taken.\n", kMaxNumbered - 1);
    return false;
}

// One strip of framebuffer pixels as TGA's blue, green, red.
void ToBgr(const void * const pixels, const int count, const bool fb16, u8 * out)
{
    if (fb16)
    {
        // PSMCT16: five bits each of red, green and blue from the bottom up, the alpha bit on
        // top. Each channel widens to eight bits by repeating its top bits, so 31 becomes 255.
        const u16 * const src = static_cast<const u16 *>(pixels);
        for (int i = 0; i < count; ++i)
        {
            const u32 r = src[i] & 0x1Fu;
            const u32 g = (src[i] >> 5) & 0x1Fu;
            const u32 b = (src[i] >> 10) & 0x1Fu;
            *out++ = static_cast<u8>((b << 3) | (b >> 2));
            *out++ = static_cast<u8>((g << 3) | (g >> 2));
            *out++ = static_cast<u8>((r << 3) | (r >> 2));
        }
    }
    else
    {
        // PSMCT32: red, green, blue and alpha bytes.
        const u8 * const src = static_cast<const u8 *>(pixels);
        for (int i = 0; i < count; ++i)
        {
            *out++ = src[(i * 4) + 2];
            *out++ = src[(i * 4) + 1];
            *out++ = src[(i * 4) + 0];
        }
    }
}

// Reads the frame back and writes it to an open file, after the TGA header. False on a failed
// write.
bool WriteFrame(std::FILE * file, void * const strip, u8 * const bgr)
{
    const int  width  = gs::Width();
    const int  height = gs::Height();
    const bool fb16   = (gs::FramebufferPsm() == GS_PSM_16);

    u8 header[kTgaHeaderBytes] = {};
    header[2]  = 2; // uncompressed true colour
    header[12] = static_cast<u8>(width & 255);
    header[13] = static_cast<u8>(width >> 8);
    header[14] = static_cast<u8>(height & 255);
    header[15] = static_cast<u8>(height >> 8);
    header[16] = 24;   // bits per pixel
    header[17] = 0x20; // the rows run top-down, as they come out of VRAM
    if (std::fwrite(header, 1, sizeof(header), file) != sizeof(header))
    {
        return false;
    }

    // The frame the last EndFrame closed, which a deferred present may still have drawing.
    const gs::DrawContext ctx = rs::FinishFrameInFlight();

    for (int y = 0; y < height; y += kStripRows)
    {
        const int rows  = (height - y < kStripRows) ? (height - y) : kStripRows;
        const int count = width * rows;
        gs::DownloadFramebufferRows(ctx, y, rows, strip);
        ToBgr(strip, count, fb16, bgr);

        const size_t bytes = static_cast<size_t>(count) * 3u;
        if (std::fwrite(bgr, 1, bytes, file) != bytes)
        {
            return false;
        }
    }
    return true;
}

void ScreenshotCommand()
{
    // Console commands run between frames. Anything that called this inside one would have the
    // GS read back a frame half recorded, with its bus turned round under the chain.
    if (rs::FrameStarted())
    {
        Com_Printf("screenshot: not while a frame is being drawn.\n");
        return;
    }

    // A disc build writes it to the emulator's host: filesystem, and has nowhere on a console.
    const char * const gamedir = ps2::sys::DebugOutputGamedir();
    if (gamedir == nullptr)
    {
        Com_Printf("screenshot: a disc build has nowhere to write one.\n");
        return;
    }

    char dir[MAX_OSPATH];
    std::snprintf(dir, sizeof(dir), "%s/scrnshot", gamedir);
    Sys_Mkdir(dir);

    char path[MAX_OSPATH];
    if (!PickPath(path, dir, (Cmd_Argc() >= 2) ? Cmd_Argv(1) : nullptr))
    {
        return;
    }

    const int    width      = gs::Width();
    const bool   fb16       = (gs::FramebufferPsm() == GS_PSM_16);
    const size_t stripBytes = static_cast<size_t>(width * kStripRows * (fb16 ? 2 : 4));
    const size_t bgrBytes   = static_cast<size_t>(width * kStripRows * 3);

    void * const strip = heap::TryAllocAligned(heap::MemAlign(64), stripBytes, heap::MemTag::Renderer);
    u8 * const   bgr   = static_cast<u8 *>(heap::TryAllocAligned(heap::MemAlign(64), bgrBytes, heap::MemTag::Renderer));
    std::FILE *  file  = (strip != nullptr && bgr != nullptr) ? std::fopen(path, "wb") : nullptr;

    bool written = false;
    if (file != nullptr)
    {
        written = WriteFrame(file, strip, bgr);
        written = (std::fclose(file) == 0) && written;

        // PFS keeps the new file's metadata cached until told to flush it (see iop_boot.h).
        written = (ps2::sys::SyncGameDataDevice() == 0) && written;
    }

    if (bgr != nullptr)
    {
        heap::Free(bgr, bgrBytes, heap::MemTag::Renderer);
    }
    if (strip != nullptr)
    {
        heap::Free(strip, stripBytes, heap::MemTag::Renderer);
    }

    if (strip == nullptr || bgr == nullptr)
    {
        Com_Printf("screenshot: no memory for the %u KB it reads the frame through.\n",
                   static_cast<unsigned>((stripBytes + bgrBytes) / 1024u));
    }
    else if (file == nullptr)
    {
        Com_Printf("screenshot: couldn't create %s\n", path);
    }
    else if (!written)
    {
        Com_Printf("screenshot: couldn't write all of %s\n", path);
    }
    else
    {
        Com_Printf("Wrote %s\n", path);
    }
}

} // namespace

void Init()
{
    Cmd_AddCommand("screenshot", ScreenshotCommand);
}

} // namespace ps2::screenshot
