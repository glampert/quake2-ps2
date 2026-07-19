/* ================================================================================================
 * File: image_load.cpp
 * Brief: PCX/WAL/TGA image loading. See image_load.h.
 *
 *  Decoders adapted from ref_gl/gl_image.c, with bounds checking on the
 *  compressed streams so a truncated or malformed file fails the load instead
 *  of reading past the buffer FS_LoadFile handed us.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/renderer/image_load.h"
#include "ps2/common.h"

#include <cstring>

extern "C" {
    #include "common/q_files.h" // pcx_t / miptex_t
}

namespace ps2::img {
namespace {

// The GS TEX0 W/H fields are log2-encoded with a maximum of 10 (1024 pixels);
// anything larger cannot be sampled, so reject it at load time.
constexpr int kMaxImageDim = 1024;

u8 * AllocPixels(int sizeBytes)
{
    // 16-byte aligned: the upload DMA chain references the buffer in place,
    // and REF transfer tags require qword-aligned source addresses.
    return static_cast<u8 *>(PS2_MemAllocAligned(16, static_cast<size_t>(sizeBytes), MEMTAG_TEXIMAGE));
}

void FreePixels(u8 * pic, int sizeBytes)
{
    PS2_MemFree(pic, static_cast<size_t>(sizeBytes), MEMTAG_TEXIMAGE);
}

// TGA rows are stored bottom-up; the decoders below write the file's pixel
// stream top-down into the buffer, then this puts the rows in sampling order.
void FlipRowsInPlace(u8 * pic, int width, int height)
{
    u32 * texels = static_cast<u32 *>(static_cast<void *>(pic));
    for (int top = 0, bottom = height - 1; top < bottom; ++top, --bottom)
    {
        u32 * rowA = texels + (top    * width);
        u32 * rowB = texels + (bottom * width);
        for (int x = 0; x < width; ++x)
        {
            const u32 tmp = rowA[x];
            rowA[x] = rowB[x];
            rowB[x] = tmp;
        }
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------
// PCX
// ------------------------------------------------------------------------------------------------

bool LoadPcx(const char * filename, u8 ** outPic, int * outWidth, int * outHeight)
{
    *outPic = nullptr;
    *outWidth = *outHeight = 0;

    u8 * fileData = nullptr;
    const int fileLen = FS_LoadFile(filename, reinterpret_cast<void **>(&fileData));
    if (fileData == nullptr || fileLen <= static_cast<int>(sizeof(pcx_t)))
    {
        Com_DPrintf("WARNING: Can't load PCX file '%s'\n", filename);
        return false;
    }

    // Header fields read as-is: the EE is little-endian, same as the on-disk
    // format, so no byte-order fixups are needed. Cast through void*:
    // FS_LoadFile buffers come from the heap, aligned well past the struct's
    // needs (-Wcast-align).
    const pcx_t * pcx = static_cast<const pcx_t *>(static_cast<const void *>(fileData));
    const int xmax = pcx->xmax;
    const int ymax = pcx->ymax;

    if (pcx->manufacturer != 0x0A || pcx->version != 5 || pcx->encoding != 1 ||
        pcx->bits_per_pixel != 8 || xmax >= 640 || ymax >= 480)
    {
        Com_DPrintf("WARNING: Bad PCX file '%s'. Invalid header value(s)!\n", filename);
        FS_FreeFile(fileData);
        return false;
    }

    const int width  = xmax + 1;
    const int height = ymax + 1;
    u8 * pic = AllocPixels(width * height);

    // Decode the RLE packets. Runs may carry the row's padding bytes past the
    // visible width; those are consumed but not stored.
    const u8 * in  = &pcx->data;
    const u8 * end = fileData + fileLen;
    bool malformed = false;

    for (int y = 0; y < height && !malformed; ++y)
    {
        u8 * row = pic + (y * width);
        for (int x = 0; x < width; )
        {
            if (in >= end)
            {
                malformed = true;
                break;
            }

            int dataByte  = *in++;
            int runLength = 1;
            if ((dataByte & 0xC0) == 0xC0)
            {
                runLength = dataByte & 0x3F;
                if (in >= end)
                {
                    malformed = true;
                    break;
                }
                dataByte = *in++;
            }

            while (runLength-- > 0 && x < width)
            {
                row[x++] = static_cast<u8>(dataByte);
            }
        }
    }

    FS_FreeFile(fileData);

    if (malformed)
    {
        Com_DPrintf("WARNING: Malformed PCX file '%s'\n", filename);
        FreePixels(pic, width * height);
        return false;
    }

    *outPic    = pic;
    *outWidth  = width;
    *outHeight = height;
    return true;
}

// ------------------------------------------------------------------------------------------------
// WAL
// ------------------------------------------------------------------------------------------------

bool LoadWal(const char * filename, u8 ** outPic, int * outWidth, int * outHeight)
{
    *outPic = nullptr;
    *outWidth = *outHeight = 0;

    u8 * fileData = nullptr;
    const int fileLen = FS_LoadFile(filename, reinterpret_cast<void **>(&fileData));
    if (fileData == nullptr || fileLen <= static_cast<int>(sizeof(miptex_t)))
    {
        Com_DPrintf("WARNING: Can't load WAL file '%s'\n", filename);
        return false;
    }

    // Header fields read as-is: the EE is little-endian, same as the on-disk
    // format, so no byte-order fixups are needed. Cast through void*:
    // FS_LoadFile buffers come from the heap, aligned well past the struct's
    // needs (-Wcast-align).
    const miptex_t * wal = static_cast<const miptex_t *>(static_cast<const void *>(fileData));
    const int width  = static_cast<int>(wal->width);
    const int height = static_cast<int>(wal->height);
    const int offset = static_cast<int>(wal->offsets[0]);

    if (width <= 0 || height <= 0 || width > kMaxImageDim || height > kMaxImageDim ||
        offset <= 0 || (offset + width * height) > fileLen)
    {
        Com_DPrintf("WARNING: Bad WAL file '%s'. Invalid header value(s)!\n", filename);
        FS_FreeFile(fileData);
        return false;
    }

    u8 * pic = AllocPixels(width * height);
    std::memcpy(pic, fileData + offset, static_cast<size_t>(width * height));

    FS_FreeFile(fileData);

    *outPic    = pic;
    *outWidth  = width;
    *outHeight = height;
    return true;
}

// ------------------------------------------------------------------------------------------------
// TGA
// ------------------------------------------------------------------------------------------------

bool LoadTga(const char * filename, u8 ** outPic, int * outWidth, int * outHeight, bool * outHasAlpha)
{
    constexpr int kTgaHeaderBytes = 18;

    *outPic = nullptr;
    *outWidth = *outHeight = 0;
    *outHasAlpha = false;

    u8 * fileData = nullptr;
    const int fileLen = FS_LoadFile(filename, reinterpret_cast<void **>(&fileData));
    if (fileData == nullptr || fileLen <= kTgaHeaderBytes)
    {
        Com_DPrintf("WARNING: Can't load TGA file '%s'\n", filename);
        return false;
    }

    // Fields read byte-wise: the header is packed and unaligned.
    const int idLength     = fileData[0];
    const int colormapType = fileData[1];
    const int imageType    = fileData[2];
    const int width        = fileData[12] | (fileData[13] << 8);
    const int height       = fileData[14] | (fileData[15] << 8);
    const int pixelSize    = fileData[16];

    const u8 * in  = fileData + kTgaHeaderBytes + idLength; // skip the image comment
    const u8 * end = fileData + fileLen;

    if (imageType != 2 && imageType != 10)
    {
        Com_DPrintf("WARNING: TGA file '%s': only types 2 and 10 supported!\n", filename);
        FS_FreeFile(fileData);
        return false;
    }
    if (colormapType != 0 || (pixelSize != 32 && pixelSize != 24))
    {
        Com_DPrintf("WARNING: TGA file '%s': only 32 or 24 bit images supported (no colormaps)!\n", filename);
        FS_FreeFile(fileData);
        return false;
    }
    if (width <= 0 || height <= 0 || width > kMaxImageDim || height > kMaxImageDim || in >= end)
    {
        Com_DPrintf("WARNING: Bad TGA file '%s'. Invalid header value(s)!\n", filename);
        FS_FreeFile(fileData);
        return false;
    }

    const int bytesPerPixel = pixelSize / 8;
    const int pixelCount    = width * height;

    u8 * pic = AllocPixels(pixelCount * 4);
    u8 * out = pic;

    // Both variants decode BGR(A) into a flat top-down RGBA stream (RLE runs
    // simply continue across row boundaries); rows are flipped afterwards.
    bool malformed = false;
    if (imageType == 2) // uncompressed
    {
        if (in + (pixelCount * bytesPerPixel) > end)
        {
            malformed = true;
        }
        else
        {
            for (int i = 0; i < pixelCount; ++i)
            {
                out[0] = in[2];
                out[1] = in[1];
                out[2] = in[0];
                out[3] = (bytesPerPixel == 4) ? in[3] : 255;
                out += 4;
                in  += bytesPerPixel;
            }
        }
    }
    else // type 10, run-length encoded
    {
        int decoded = 0;
        while (decoded < pixelCount && !malformed)
        {
            if (in >= end)
            {
                malformed = true;
                break;
            }

            const int packetHeader = *in++;
            int packetSize = 1 + (packetHeader & 0x7F);
            if (packetSize > (pixelCount - decoded))
            {
                packetSize = pixelCount - decoded; // never write past the image
            }

            if (packetHeader & 0x80) // run-length packet: one color repeated
            {
                if (in + bytesPerPixel > end)
                {
                    malformed = true;
                    break;
                }
                const u8 r = in[2], g = in[1], b = in[0];
                const u8 a = (bytesPerPixel == 4) ? in[3] : 255;
                in += bytesPerPixel;

                for (int i = 0; i < packetSize; ++i)
                {
                    out[0] = r;
                    out[1] = g;
                    out[2] = b;
                    out[3] = a;
                    out += 4;
                }
            }
            else // raw packet
            {
                if (in + (packetSize * bytesPerPixel) > end)
                {
                    malformed = true;
                    break;
                }
                for (int i = 0; i < packetSize; ++i)
                {
                    out[0] = in[2];
                    out[1] = in[1];
                    out[2] = in[0];
                    out[3] = (bytesPerPixel == 4) ? in[3] : 255;
                    out += 4;
                    in  += bytesPerPixel;
                }
            }

            decoded += packetSize;
        }
    }

    FS_FreeFile(fileData);

    if (malformed)
    {
        Com_DPrintf("WARNING: Malformed TGA file '%s'\n", filename);
        FreePixels(pic, pixelCount * 4);
        return false;
    }

    FlipRowsInPlace(pic, width, height);

    *outPic      = pic;
    *outWidth    = width;
    *outHeight   = height;
    *outHasAlpha = (pixelSize == 32);
    return true;
}

} // namespace ps2::img
