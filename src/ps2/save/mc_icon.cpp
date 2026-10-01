/* ================================================================================================
 * File: mc_icon.cpp
 * Brief: The save directory's icon.sys and 3D icon. See mc_icon.h.
 *
 *  icon.sys is libmc's mcIcon: the title (Shift-JIS, two lines split at nlOffset), the four
 *  background corner colours and the lighting the browser shows the icon with, and the icon
 *  files to use for listing, copying and deleting - all the same one here.
 *
 *  The icon is the Quake II emblem from baseq2/quake-icon.pcx (128x128, 8-bit, palette index
 *  255 transparent), or the menu cursor's first frame (pics/m_cursor0.pcx) where the game data
 *  doesn't have it: a flat cut-out of the picture's opaque pixels, textured front and back, in
 *  its own proportions - cut out because the browser draws icon textures opaque, ignoring the
 *  texels' alpha. It is built from the game data whenever a save goes to a card, and rewritten
 *  if the card's copy differs - so a change of picture reaches cards that already have a save
 *  directory. The format isn't documented in the SDK; the layout below follows the community
 *  documentation of it (the one bmp2icon-style tools write):
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

// The picture the icon is cut from: the first of these the game data has. A picture wider or
// taller than the 128x128 texture gains nothing; 8-bit PCX with its own palette, index 255
// transparent, as Quake's own pictures are.
constexpr const char * kIconPictures[] = { "quake2-icon.pcx", "pics/m_cursor0.pcx" };

// Browser-space size of the longer side of what the icon shows - the picture's opaque part -
// the other side following its proportions. The texture stretches any picture to the square
// 128x128 every icon texture is; the geometry keeps the picture's own proportions.
constexpr float kSlabSize      = 3.2f;
constexpr float kSlabThickness = 0.06f; // Keeps the back face from z-fighting the front.

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
constexpr u32 kTextureBytes = kTextureSize * kTextureSize * 2u;

constexpr u32 kAnimationBytes = sizeof(IconAnimHeader) + sizeof(IconFrame) + sizeof(IconKey);

// Each opaque rectangle of the picture is a front and a back quad, two triangles each.
constexpr int kVerticesPerRect = 12;

// Past this many rectangles - a picture with ragged, noisy transparency - the icon falls back
// to one solid slab: 256 of them is already 72 KB of vertices on the card.
constexpr int kMaxRects = 256;

// The palette index Quake's pictures use for "no pixel".
constexpr int kTransparent = 255;

// A decoded 8-bit Quake PCX.
struct Picture
{
    int        width;
    int        height;
    int        stride;  // Bytes per row of indexes (the PCX's bytes per line, even).
    u8 *       indexes; // stride * height palette indexes, on the heap.
    const u8 * palette; // 256 RGB triplets, inside the file's data.
};

// A block of opaque pixels: [x0, x1) x [y0, y1), in picture coordinates.
struct Rect
{
    int x0, y0, x1, y1;
};

static Rect s_rects[kMaxRects];

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

inline u16 PackTexel(const int r, const int g, const int b, const bool opaque)
{
    return static_cast<u16>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (opaque ? 0x8000 : 0));
}

bool Opaque(const Picture & picture, const int x, const int y)
{
    return picture.indexes[y * picture.stride + x] != kTransparent;
}

// Decodes an 8-bit Quake PCX. False if the data isn't one, or out of memory.
bool DecodePcx(const u8 * data, const int sizeBytes, Picture & outPicture)
{
    constexpr int kHeaderBytes  = 128;
    constexpr int kPaletteBytes = 768;

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
    std::memset(indexes + decoded, kTransparent, numIndexes - decoded); // A short file: the rest is empty.

    outPicture.width   = width;
    outPicture.height  = height;
    outPicture.stride  = bytesPerLine;
    outPicture.indexes = indexes;
    outPicture.palette = data + sizeBytes - kPaletteBytes;
    return true;
}

void FreePicture(Picture & picture)
{
    const size_t numIndexes = static_cast<size_t>(picture.stride) * static_cast<size_t>(picture.height);
    ps2::heap::Free(picture.indexes, numIndexes, MemTag::SaveData);
    picture.indexes = nullptr;
}

// Stretches the picture over the texture, bilinear. Only opaque pixels lend their colour, so
// the edges of the shape don't darken towards the transparent ones (which are black), and the
// texels just outside it carry the edge colour for the GS's filtering to blend with. The alpha
// bit follows the nearest pixel, though the browser draws icon textures opaque regardless.
void ResampleTexture(const Picture & picture, u16 * outTexels)
{
    const int width  = picture.width;
    const int height = picture.height;

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

            const int   xs[4]      = { x0, x1, x0, x1 };
            const int   ys[4]      = { y0, y0, y1, y1 };
            const float weights[4] = { (1.0f - fx) * (1.0f - fy), fx * (1.0f - fy), (1.0f - fx) * fy, fx * fy };

            float rgb[3]      = {};
            float totalWeight = 0.0f;
            for (int c = 0; c < 4; ++c)
            {
                if (!Opaque(picture, xs[c], ys[c]))
                {
                    continue;
                }
                const int index = picture.indexes[ys[c] * picture.stride + xs[c]];
                for (int channel = 0; channel < 3; ++channel)
                {
                    rgb[channel] += weights[c] * static_cast<float>(picture.palette[index * 3 + channel]);
                }
                totalWeight += weights[c];
            }

            if (totalWeight > 0.0f)
            {
                for (float & channel : rgb)
                {
                    channel /= totalWeight;
                }
            }

            const bool opaque = Opaque(picture, (fx < 0.5f) ? x0 : x1, (fy < 0.5f) ? y0 : y1);
            outTexels[ty * kTextureSize + tx] = PackTexel(static_cast<int>(rgb[0]), static_cast<int>(rgb[1]),
                                                          static_cast<int>(rgb[2]), opaque);
        }
    }
}

// Covers the picture's opaque pixels with rectangles: each run of opaque pixels in a row,
// grown down over the rows below that have exactly the same run. Returns how many, 0 for a
// picture with no opaque pixel, or -1 if there would be more than maxRects.
int FindOpaqueRects(const Picture & picture, Rect * outRects, const int maxRects)
{
    const int width  = picture.width;
    const int height = picture.height;

    // Whether row y has an opaque run starting at x0 and ending right before x1.
    const auto sameRun = [&picture, width](const int y, const int x0, const int x1) {
        for (int x = x0; x < x1; ++x)
        {
            if (!Opaque(picture, x, y))
            {
                return false;
            }
        }
        return (x0 == 0 || !Opaque(picture, x0 - 1, y)) && (x1 == width || !Opaque(picture, x1, y));
    };

    const size_t numPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    u8 * const covered = static_cast<u8 *>(ps2::heap::TryAlloc(numPixels, MemTag::SaveData));
    if (covered == nullptr)
    {
        return -1;
    }
    std::memset(covered, 0, numPixels);

    int count = 0;
    for (int y = 0; y < height && count >= 0; ++y)
    {
        for (int x = 0; x < width;)
        {
            if (!Opaque(picture, x, y))
            {
                ++x;
                continue;
            }

            int runEnd = x;
            while (runEnd < width && Opaque(picture, runEnd, y))
            {
                ++runEnd;
            }

            // A run a rectangle from above already covers is covered whole: that rectangle
            // only grew into rows where the run was exactly its own.
            if (covered[y * width + x] == 0)
            {
                if (count == maxRects)
                {
                    count = -1;
                    break;
                }

                int rectEnd = y + 1;
                while (rectEnd < height && sameRun(rectEnd, x, runEnd))
                {
                    ++rectEnd;
                }
                for (int row = y; row < rectEnd; ++row)
                {
                    std::memset(covered + row * width + x, 1, static_cast<size_t>(runEnd - x));
                }
                outRects[count++] = { x, y, runEnd, rectEnd };
            }
            x = runEnd;
        }
    }

    ps2::heap::Free(covered, numPixels, MemTag::SaveData);
    return count;
}

// Where picture pixels land in browser space: `unit` per pixel, from the point (centreX,
// bottomY) of the picture, which lands at X = 0 on the floor (Y = 0; Y points down).
struct Placement
{
    float unit;
    float centreX;
    float bottomY;
};

// Fits the rectangles' bounding box - the opaque part of the picture, its empty margins left
// out - to kSlabSize on its longer side, standing on the floor, centred.
Placement PlaceRects(const Rect * rects, const int numRects)
{
    Rect bounds = rects[0];
    for (int i = 1; i < numRects; ++i)
    {
        bounds.x0 = (rects[i].x0 < bounds.x0) ? rects[i].x0 : bounds.x0;
        bounds.y0 = (rects[i].y0 < bounds.y0) ? rects[i].y0 : bounds.y0;
        bounds.x1 = (rects[i].x1 > bounds.x1) ? rects[i].x1 : bounds.x1;
        bounds.y1 = (rects[i].y1 > bounds.y1) ? rects[i].y1 : bounds.y1;
    }

    const int boundsWidth  = bounds.x1 - bounds.x0;
    const int boundsHeight = bounds.y1 - bounds.y0;
    const int longerSide   = (boundsWidth > boundsHeight) ? boundsWidth : boundsHeight;

    Placement placement;
    placement.unit    = kSlabSize / static_cast<float>(longerSide);
    placement.centreX = static_cast<float>(bounds.x0 + bounds.x1) * 0.5f;
    placement.bottomY = static_cast<float>(bounds.y1);
    return placement;
}

// A rectangle of the picture as a front and a back quad. Both faces carry the same texture
// coordinates, so from behind the picture reads mirrored - as the shape does.
void AddRect(IconVertex * out, const Rect & rect, const int width, const int height, const Placement & placement)
{
    const float u0 = static_cast<float>(rect.x0) / static_cast<float>(width);
    const float u1 = static_cast<float>(rect.x1) / static_cast<float>(width);
    const float v0 = static_cast<float>(rect.y0) / static_cast<float>(height);
    const float v1 = static_cast<float>(rect.y1) / static_cast<float>(height);

    const float left   = (static_cast<float>(rect.x0) - placement.centreX) * placement.unit;
    const float right  = (static_cast<float>(rect.x1) - placement.centreX) * placement.unit;
    const float top    = (static_cast<float>(rect.y0) - placement.bottomY) * placement.unit;
    const float bottom = (static_cast<float>(rect.y1) - placement.bottomY) * placement.unit;
    const float front  = -kSlabThickness * 0.5f;
    const float back   =  kSlabThickness * 0.5f;

    // Front, facing -Z.
    SetVertex(out[0],  left,  top,    front, -1.0f, u0, v0);
    SetVertex(out[1],  right, top,    front, -1.0f, u1, v0);
    SetVertex(out[2],  left,  bottom, front, -1.0f, u0, v1);
    SetVertex(out[3],  right, top,    front, -1.0f, u1, v0);
    SetVertex(out[4],  right, bottom, front, -1.0f, u1, v1);
    SetVertex(out[5],  left,  bottom, front, -1.0f, u0, v1);

    // Back, facing +Z, wound the other way.
    SetVertex(out[6],  left,  top,    back,   1.0f, u0, v0);
    SetVertex(out[7],  left,  bottom, back,   1.0f, u0, v1);
    SetVertex(out[8],  right, top,    back,   1.0f, u1, v0);
    SetVertex(out[9],  right, top,    back,   1.0f, u1, v0);
    SetVertex(out[10], left,  bottom, back,   1.0f, u0, v1);
    SetVertex(out[11], right, bottom, back,   1.0f, u1, v1);
}

// Builds the icon file on the heap and returns it, with its size; null if out of memory.
//
// The PS2 browser draws icon textures opaque - it ignores the texels' alpha bit - so the
// picture's transparent pixels would show as black. The slab is instead built only where
// the picture is opaque, one rectangle per block of opaque pixels.
u8 * BuildIconFile(u32 & outSizeBytes)
{
    void * pcx = nullptr;
    Picture picture = {};
    bool decoded = false;

    for (const char * name : kIconPictures)
    {
        const int pcxBytes = FS_LoadFile(name, &pcx);
        decoded = (pcxBytes > 0) && DecodePcx(static_cast<const u8 *>(pcx), pcxBytes, picture);
        if (decoded)
        {
            break;
        }
        if (pcx != nullptr)
        {
            FS_FreeFile(pcx);
            pcx = nullptr;
        }
    }
    if (!decoded)
    {
        Com_Printf("Save icon: none of its pictures could be loaded, the icon will be plain.\n");
    }

    // No picture, nothing opaque in it, or too ragged to cut out: one whole slab.
    const int width  = decoded ? picture.width  : 1;
    const int height = decoded ? picture.height : 1;
    int numRects = decoded ? FindOpaqueRects(picture, s_rects, kMaxRects) : 0;
    if (numRects <= 0)
    {
        numRects   = 1;
        s_rects[0] = { 0, 0, width, height };
    }

    const u32 numVertices   = static_cast<u32>(numRects * kVerticesPerRect);
    const u32 textureOffset = sizeof(IconHeader) + numVertices * sizeof(IconVertex) + kAnimationBytes;
    outSizeBytes = textureOffset + kTextureBytes;

    u8 * const out = static_cast<u8 *>(ps2::heap::TryAlloc(outSizeBytes, MemTag::SaveData));
    if (out != nullptr)
    {
        IconHeader header = {};
        header.magic       = kIconMagic;
        header.numShapes   = 1;
        header.textureType = kTextureRaw16;
        header.reserved    = kFloatOne;
        header.numVertices = numVertices;

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

        const Placement placement = PlaceRects(s_rects, numRects);

        put(&header, sizeof(header));
        for (int i = 0; i < numRects; ++i)
        {
            IconVertex vertices[kVerticesPerRect];
            AddRect(vertices, s_rects[i], width, height, placement);
            put(vertices, sizeof(vertices));
        }
        put(&anim, sizeof(anim));
        put(&frame, sizeof(frame));
        put(&key, sizeof(key));

        // The texture goes straight in place; `out` comes from the heap and every block before
        // it is a whole number of u16s, so it is aligned for them.
        PS2_Assert(cursor == out + textureOffset);
        u16 * const texels = static_cast<u16 *>(static_cast<void *>(cursor));
        if (decoded)
        {
            ResampleTexture(picture, texels);
        }
        else
        {
            for (int i = 0; i < kTextureSize * kTextureSize; ++i)
            {
                texels[i] = PackTexel(72, 56, 40, true); // A dark bronze.
            }
        }
    }

    if (decoded)
    {
        FreePicture(picture);
    }
    if (pcx != nullptr)
    {
        FS_FreeFile(pcx);
    }
    return out;
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

// Whether the device holds exactly these bytes under that name.
bool FileMatches(Device & device, const char * name, const void * expected, const u32 sizeBytes)
{
    u32 sizeOnDevice = 0;
    if (!device.FileSize(name, sizeOnDevice) || sizeOnDevice != sizeBytes)
    {
        return false;
    }

    const FileHandle handle = device.Open(name, OpenMode::Read);
    if (handle == FileHandle::Invalid)
    {
        return false;
    }

    static u8 s_chunk[2048];
    const u8 * const bytes = static_cast<const u8 *>(expected);
    bool same = true;

    for (u32 offset = 0; same && offset < sizeBytes;)
    {
        const u32 n = (sizeBytes - offset < sizeof(s_chunk)) ? sizeBytes - offset : static_cast<u32>(sizeof(s_chunk));
        same = device.Read(handle, s_chunk, n) && std::memcmp(s_chunk, bytes + offset, n) == 0;
        offset += n;
    }

    device.Close(handle);
    return same;
}

} // namespace

bool EnsureSaveIcons(Device & device)
{
    u32 iconBytes = 0;
    u8 * const iconFile = BuildIconFile(iconBytes);
    if (iconFile == nullptr)
    {
        SetError("Not enough memory to create the save icon.");
        return false;
    }

    mcIcon sys;
    BuildIconSys(sys);

    const bool upToDate = FileMatches(device, kIconModelFile, iconFile, iconBytes) &&
                          FileMatches(device, kIconSysFile, &sys, sizeof(sys));

    const bool ok = upToDate ||
                    (WriteWholeFile(device, kIconModelFile, iconFile, iconBytes) &&
                     WriteWholeFile(device, kIconSysFile, &sys, sizeof(sys)));

    ps2::heap::Free(iconFile, iconBytes, MemTag::SaveData);

    if (!ok)
    {
        SetError("Could not write the save icon to the memory card.");
    }
    else if (!upToDate)
    {
        Com_Printf("Save icon: written to %s.\n", device.Describe(kIconModelFile));
    }
    return ok;
}

} // namespace ps2::save
