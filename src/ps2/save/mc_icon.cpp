/* ================================================================================================
 * File: mc_icon.cpp
 * Brief: The save directory's icon.sys and 3D icon. See mc_icon.h.
 *
 *  icon.sys is libmc's mcIcon: the title (Shift-JIS, two lines split at nlOffset), the four
 *  background corner colours and the lighting the browser shows the icon with, and the icon
 *  files to use for listing, copying and deleting - all the same one here.
 *
 *  The icon is the main menu's "QUAKE 2" plaque (pics/m_main_plaque.pcx), built when the save
 *  directory is created: a flat slab, textured front and back, in its true proportions. The
 *  format isn't documented in the SDK; the layout below follows the community documentation
 *  of it (the one bmp2icon-style tools write):
 *
 *      header      magic 0x00010000, shape count, texture type, 1.0f, vertex count (x3)
 *      vertices    per vertex: a position per shape, a normal, UV, RGBA - s16 values in
 *                  4.12 fixed point (4096 = 1.0), colours with 0x80 = 1.0
 *      animation   header + frames + keys; a still icon has one frame of one key
 *      texture     128x128 texels, 16 bits each (GS PSMCT16: R5 G5 B5 A1), uncompressed
 *
 *  The browser's space has Y pointing down, with the icon standing on Y = 0.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/mc_icon.h"

#include <cstring>
#include <libmc.h>
#include <sjis.h>

namespace ps2::save {
namespace {

using ps2::heap::MemTag;

constexpr const char * kTitleLine1 = "Quake II";
constexpr const char * kTitleLine2 = "Saved Games";

constexpr const char * kIconPicture = "pics/m_main_plaque.pcx";

// Browser-space size of the plaque. The picture is 38x166; the texture stretches it to the
// square 128x128 every icon texture is, and the slab's proportions undo the stretch.
constexpr float kPlaqueHeight    = 2.8f;
constexpr float kPlaqueWidth     = kPlaqueHeight * (38.0f / 166.0f);
constexpr float kPlaqueThickness = 0.06f; // Keeps the back face from z-fighting the front.

constexpr int kTextureSize = 128;

struct IconHeader
{
    u32 magic;
    u32 numShapes;
    u32 textureType;
    u32 reserved;
    u32 numVertices;
};

struct IconVertex // One animation shape.
{
    s16 x, y, z, w;
    s16 nx, ny, nz, nw;
    s16 u, v;
    u8  r, g, b, a;
};

struct IconAnimHeader
{
    u32   tag;
    u32   frameLength;
    float speed;
    u32   playOffset;
    u32   numFrames;
};

struct IconFrame
{
    u32 shapeId;
    u32 numKeys;
    u32 unknown1;
    u32 unknown2;
};

struct IconKey
{
    float time;
    float value;
};

static_assert(sizeof(IconHeader)     == 20);
static_assert(sizeof(IconVertex)     == 24);
static_assert(sizeof(IconAnimHeader) == 20);
static_assert(sizeof(IconFrame)      == 16);
static_assert(sizeof(IconKey)        == 8);

constexpr u32 kIconMagic    = 0x00010000u;
constexpr u32 kTextureRaw16 = 0x07u; // Textured, uncompressed.
constexpr u32 kFloatOne     = 0x3F800000u;
constexpr int kNumVertices  = 12;    // Front and back face, two triangles each.
constexpr u32 kTextureBytes = kTextureSize * kTextureSize * 2u;

constexpr u32 kIconFileBytes = sizeof(IconHeader) + kNumVertices * sizeof(IconVertex) +
                               sizeof(IconAnimHeader) + sizeof(IconFrame) + sizeof(IconKey) + kTextureBytes;

inline s16 Fixed(const float value)
{
    return static_cast<s16>(value * 4096.0f);
}

void SetVertex(IconVertex & vertex,
               const float x, const float y, const float z,
               const float normalZ,
               const float u, const float v)
{
    vertex = IconVertex{};
    vertex.x  = Fixed(x);
    vertex.y  = Fixed(y);
    vertex.z  = Fixed(z);
    vertex.nz = Fixed(normalZ);
    vertex.u  = Fixed(u);
    vertex.v  = Fixed(v);
    vertex.r  = 0x80;
    vertex.g  = 0x80;
    vertex.b  = 0x80;
    vertex.a  = 0x80;
}

// The slab: a front face and a back face, the back's texture mirrored so it reads right too.
void BuildVertices(IconVertex (&vertices)[kNumVertices])
{
    constexpr float kLeft   = -kPlaqueWidth * 0.5f;
    constexpr float kRight  =  kPlaqueWidth * 0.5f;
    constexpr float kTop    = -kPlaqueHeight;
    constexpr float kBottom =  0.0f;
    constexpr float kFront  = -kPlaqueThickness * 0.5f;
    constexpr float kBack   =  kPlaqueThickness * 0.5f;

    // Front, facing -Z.
    SetVertex(vertices[0],  kLeft,  kTop,    kFront, -1.0f, 0.0f, 0.0f);
    SetVertex(vertices[1],  kRight, kTop,    kFront, -1.0f, 1.0f, 0.0f);
    SetVertex(vertices[2],  kLeft,  kBottom, kFront, -1.0f, 0.0f, 1.0f);
    SetVertex(vertices[3],  kRight, kTop,    kFront, -1.0f, 1.0f, 0.0f);
    SetVertex(vertices[4],  kRight, kBottom, kFront, -1.0f, 1.0f, 1.0f);
    SetVertex(vertices[5],  kLeft,  kBottom, kFront, -1.0f, 0.0f, 1.0f);

    // Back, facing +Z, wound the other way.
    SetVertex(vertices[6],  kLeft,  kTop,    kBack,   1.0f, 1.0f, 0.0f);
    SetVertex(vertices[7],  kLeft,  kBottom, kBack,   1.0f, 1.0f, 1.0f);
    SetVertex(vertices[8],  kRight, kTop,    kBack,   1.0f, 0.0f, 0.0f);
    SetVertex(vertices[9],  kRight, kTop,    kBack,   1.0f, 0.0f, 0.0f);
    SetVertex(vertices[10], kLeft,  kBottom, kBack,   1.0f, 1.0f, 1.0f);
    SetVertex(vertices[11], kRight, kBottom, kBack,   1.0f, 0.0f, 1.0f);
}

inline u16 PackTexel(const int r, const int g, const int b, const bool opaque)
{
    return static_cast<u16>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (opaque ? 0x8000 : 0));
}

// Decodes an 8-bit Quake PCX and resamples it (bilinear colour, nearest transparency)
// into the icon texture. False if the file isn't one.
bool TextureFromPcx(const u8 * data, const int sizeBytes, u16 * outTexels)
{
    constexpr int kHeaderBytes  = 128;
    constexpr int kPaletteBytes = 768;
    constexpr int kTransparent  = 255;

    if (sizeBytes < kHeaderBytes + kPaletteBytes || data[0] != 0x0A || data[2] != 1 || data[3] != 8)
    {
        return false;
    }

    const int width        = (data[8]  | (data[9]  << 8)) - (data[4] | (data[5] << 8)) + 1;
    const int height       = (data[10] | (data[11] << 8)) - (data[6] | (data[7] << 8)) + 1;
    const int bytesPerLine = data[66] | (data[67] << 8);
    if (width <= 0 || height <= 0 || bytesPerLine < width || width > 320 || height > 240)
    {
        return false;
    }

    const size_t numIndexes = static_cast<size_t>(bytesPerLine) * static_cast<size_t>(height);
    u8 * const indexes = static_cast<u8 *>(ps2::heap::TryAlloc(numIndexes, MemTag::SaveData));
    if (indexes == nullptr)
    {
        return false;
    }

    // Run-length decode: a byte with the top two bits set repeats the next one (count & 0x3F).
    const u8 * src = data + kHeaderBytes;
    const u8 * const srcEnd = data + sizeBytes - kPaletteBytes;
    size_t decoded = 0;
    while (decoded < numIndexes && src < srcEnd)
    {
        u8 value = *src++;
        size_t run = 1;
        if ((value & 0xC0) == 0xC0)
        {
            run = value & 0x3Fu;
            value = (src < srcEnd) ? *src++ : 0;
        }
        while (run-- != 0 && decoded < numIndexes)
        {
            indexes[decoded++] = value;
        }
    }

    const u8 * const palette = data + sizeBytes - kPaletteBytes;
    const auto indexAt = [&](const int x, const int y) -> int {
        return indexes[static_cast<size_t>(y * bytesPerLine + x)];
    };

    for (int ty = 0; ty < kTextureSize; ++ty)
    {
        const float sy = (static_cast<float>(ty) + 0.5f) * static_cast<float>(height) / kTextureSize - 0.5f;
        const int   y0 = (sy < 0.0f) ? 0 : static_cast<int>(sy);
        const int   y1 = (y0 + 1 < height) ? y0 + 1 : y0;
        const float fy = (sy < 0.0f) ? 0.0f : sy - static_cast<float>(y0);

        for (int tx = 0; tx < kTextureSize; ++tx)
        {
            const float sx = (static_cast<float>(tx) + 0.5f) * static_cast<float>(width) / kTextureSize - 0.5f;
            const int   x0 = (sx < 0.0f) ? 0 : static_cast<int>(sx);
            const int   x1 = (x0 + 1 < width) ? x0 + 1 : x0;
            const float fx = (sx < 0.0f) ? 0.0f : sx - static_cast<float>(x0);

            const int corners[4] = { indexAt(x0, y0), indexAt(x1, y0), indexAt(x0, y1), indexAt(x1, y1) };
            const float weights[4] = { (1.0f - fx) * (1.0f - fy), fx * (1.0f - fy), (1.0f - fx) * fy, fx * fy };

            float rgb[3] = {};
            for (int c = 0; c < 4; ++c)
            {
                if (corners[c] == kTransparent)
                {
                    continue; // Counts as black.
                }
                for (int channel = 0; channel < 3; ++channel)
                {
                    rgb[channel] += weights[c] * static_cast<float>(palette[corners[c] * 3 + channel]);
                }
            }

            const int nearest = indexAt((fx < 0.5f) ? x0 : x1, (fy < 0.5f) ? y0 : y1);
            outTexels[ty * kTextureSize + tx] = PackTexel(static_cast<int>(rgb[0]), static_cast<int>(rgb[1]),
                                                          static_cast<int>(rgb[2]), nearest != kTransparent);
        }
    }

    ps2::heap::Free(indexes, numIndexes, MemTag::SaveData);
    return true;
}

void BuildTexture(u16 * outTexels)
{
    void * pcx = nullptr;
    const int pcxBytes = FS_LoadFile(kIconPicture, &pcx);
    const bool loaded = (pcxBytes > 0) && TextureFromPcx(static_cast<const u8 *>(pcx), pcxBytes, outTexels);

    if (pcx != nullptr)
    {
        FS_FreeFile(pcx);
    }

    if (!loaded)
    {
        // No picture: a plain slab in the plaque's dark bronze.
        Com_Printf("Save icon: couldn't use %s, the icon will be plain.\n", kIconPicture);
        for (int i = 0; i < kTextureSize * kTextureSize; ++i)
        {
            outTexels[i] = PackTexel(72, 56, 40, true);
        }
    }
}

void BuildIconFile(u8 * out)
{
    IconHeader header = {};
    header.magic       = kIconMagic;
    header.numShapes   = 1;
    header.textureType = kTextureRaw16;
    header.reserved    = kFloatOne;
    header.numVertices = kNumVertices;

    IconVertex vertices[kNumVertices];
    BuildVertices(vertices);

    IconAnimHeader anim = {};
    anim.tag         = 1;
    anim.frameLength = 1;
    anim.speed       = 1.0f;
    anim.playOffset  = 0;
    anim.numFrames   = 1;

    IconFrame frame = {};
    frame.shapeId  = 0;
    frame.numKeys  = 1;
    frame.unknown1 = 1;
    frame.unknown2 = 0;

    const IconKey key = { 1.0f, 1.0f };

    u8 * cursor = out;
    const auto put = [&cursor](const void * data, const size_t sizeBytes) {
        std::memcpy(cursor, data, sizeBytes);
        cursor += sizeBytes;
    };

    put(&header, sizeof(header));
    put(vertices, sizeof(vertices));
    put(&anim, sizeof(anim));
    put(&frame, sizeof(frame));
    put(&key, sizeof(key));

    // The texture goes straight in place; `out` comes from the heap, so it is aligned for u16.
    BuildTexture(static_cast<u16 *>(static_cast<void *>(cursor)));
}

void BuildIconSys(mcIcon & sys)
{
    static const iconIVECTOR kBackground[4] = {
        { 96, 36, 20, 0 }, // top left
        { 96, 36, 20, 0 }, // top right
        { 20,  8,  4, 0 }, // bottom left
        { 20,  8,  4, 0 }, // bottom right
    };
    static const iconFVECTOR kLightDir[3] = {
        {  0.5f,  0.5f, 0.5f,  0.0f },
        {  0.0f, -0.4f, -0.1f, 0.0f },
        { -0.5f, -0.5f, 0.5f,  0.0f },
    };
    static const iconFVECTOR kLightColour[3] = {
        { 0.3f, 0.3f, 0.3f, 0.0f },
        { 0.4f, 0.4f, 0.4f, 0.0f },
        { 0.5f, 0.5f, 0.5f, 0.0f },
    };
    static const iconFVECTOR kAmbient = { 0.5f, 0.5f, 0.5f, 0.0f };

    std::memset(&sys, 0, sizeof(sys));
    std::memcpy(sys.head, "PS2D", 4);
    sys.type  = MCICON_TYPE_SAVED_DATA;
    sys.trans = 0x60;

    // Both lines as one string; nlOffset is where the second starts, in bytes of Shift-JIS
    // (every character converts to two). strcpy_sjis would turn a '\n' into junk.
    char title[40];
    std::snprintf(title, sizeof(title), "%s%s", kTitleLine1, kTitleLine2);
    strcpy_sjis(reinterpret_cast<short *>(sys.title), title);
    sys.nlOffset = static_cast<unsigned short>(std::strlen(kTitleLine1) * 2u);

    std::memcpy(sys.bgCol, kBackground, sizeof(kBackground));
    std::memcpy(sys.lightDir, kLightDir, sizeof(kLightDir));
    std::memcpy(sys.lightCol, kLightColour, sizeof(kLightColour));
    std::memcpy(sys.lightAmbient, kAmbient, sizeof(kAmbient));

    std::snprintf(reinterpret_cast<char *>(sys.view), sizeof(sys.view), "%s", kIconModelFile);
    std::snprintf(reinterpret_cast<char *>(sys.copy), sizeof(sys.copy), "%s", kIconModelFile);
    std::snprintf(reinterpret_cast<char *>(sys.del),  sizeof(sys.del),  "%s", kIconModelFile);
}

bool WriteWholeFile(Device & device, const char * name, const void * data, const u32 sizeBytes)
{
    const FileHandle handle = device.Open(name, OpenMode::Write);
    if (handle == FileHandle::Invalid)
    {
        return false;
    }

    const bool written = device.Write(handle, data, sizeBytes);
    return device.Close(handle) && written;
}

} // namespace

bool WriteSaveIcons(Device & device)
{
    u8 * const iconFile = static_cast<u8 *>(ps2::heap::TryAlloc(kIconFileBytes, MemTag::SaveData));
    if (iconFile == nullptr)
    {
        SetError("Not enough memory to create the save icon.");
        return false;
    }

    BuildIconFile(iconFile);

    mcIcon sys;
    BuildIconSys(sys);

    const bool ok = WriteWholeFile(device, kIconModelFile, iconFile, kIconFileBytes) &&
                    WriteWholeFile(device, kIconSysFile, &sys, sizeof(sys));

    ps2::heap::Free(iconFile, kIconFileBytes, MemTag::SaveData);

    if (!ok)
    {
        SetError("Could not write the save icon to the memory card.");
    }
    return ok;
}

} // namespace ps2::save
