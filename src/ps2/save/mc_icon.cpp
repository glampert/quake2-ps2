/* ================================================================================================
 * File: mc_icon.cpp
 * Brief: The save directory's icon.sys and 3D icon. See mc_icon.h.
 *
 *  icon.sys is libmc's mcIcon: the title (Shift-JIS, two lines split at nlOffset), the four
 *  background corner colours and the lighting the browser shows the icon with, and the icon
 *  files to use for listing, copying and deleting - all the same one here.
 *
 *  The icon is the quad damage pickup (models/items/quaddama/tris.md2), the Quake II emblem in
 *  3D: its first keyframe with its skin, read with the renderer's own MD2 parsing
 *  (renderer/model_load.h) and turned into icon space. Should the game data not have it, the
 *  icon is a plain square instead, so a save directory always has one.
 *
 *  It is built from the game data whenever a save goes to a card, and rewritten if the card's
 *  copy differs - so a change of icon reaches cards that already have a save directory. The
 *  format isn't documented in the SDK; the layout below follows the community documentation
 *  of it (the one bmp2icon-style tools write):
 *
 *      header      magic 0x00010000, shape count, texture type, 1.0f, vertex count (x3)
 *      vertices    per vertex: a position per shape, a normal, UV, RGBA - s16 values in
 *                  4.12 fixed point (4096 = 1.0), colours with 0x80 = 1.0
 *      animation   header + frames + keys; a still icon has one frame of one key
 *      texture     128x128 texels, 16 bits each (GS PSMCT16: R5 G5 B5 A1), uncompressed
 *
 *  The browser's space has Y pointing down, with the icon standing on Y = 0. It turns the
 *  icon about the Y axis by itself, and draws its texture opaque, ignoring the alpha bit.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/save/mc_icon.h"
#include "ps2/renderer/model_load.h"

#include <cstring>
#include <libmc.h>
#include <sjis.h>

namespace ps2::save {
namespace {

using ps2::heap::MemTag;
using ps2::math::Vec3;

constexpr const char * kTitleLine1 = "Quake II";
constexpr const char * kTitleLine2 = "Saved Games";
constexpr const char * kIconModel  = "models/items/quaddama/tris.md2";

// Browser-space size of the longest side of the model's bounding box.
constexpr float kIconSize = 3.2f;

// Each model triangle goes in twice, once per winding, with the same normals. Whether the
// browser culls back faces, and which winding it takes for the front, isn't known: this way
// the outside of the model shows either way. Without culling, the two copies of a triangle
// draw identically, so there is nothing for them to fight over.
constexpr bool kDoubleSidedModel = true;

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

// The palette index Quake's images use for "no pixel". A skin has a stray few at most; they
// take their neighbours' colour.
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

// An icon file being built: the whole file on the heap, and where its vertices and texture go.
struct IconFile
{
    u8 *  data;
    u32   sizeBytes;
    u8 *  vertices;
    u16 * texels;
};

inline u16 PackTexel(const int r, const int g, const int b)
{
    return static_cast<u16>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000);
}

inline s16 Fixed(const float value)
{
    return static_cast<s16>(value * 4096.0f);
}

void SetVertex(IconVertex & vertex,
               const float x,  const float y,  const float z,
               const float nx, const float ny, const float nz,
               const float u,  const float v)
{
    vertex = IconVertex{};
    vertex.x  = Fixed(x);
    vertex.y  = Fixed(y);
    vertex.z  = Fixed(z);
    vertex.nx = Fixed(nx);
    vertex.ny = Fixed(ny);
    vertex.nz = Fixed(nz);
    vertex.u  = Fixed(u);
    vertex.v  = Fixed(v);
    vertex.r  = 0x80;
    vertex.g  = 0x80;
    vertex.b  = 0x80;
    vertex.a  = 0x80;
}

// Allocates an icon file for this many vertices and fills in everything but the vertices and
// the texture: the header, and a still animation. False if out of memory.
bool NewIconFile(const u32 numVertices, IconFile & outFile)
{
    const u32 verticesOffset = sizeof(IconHeader);
    const u32 textureOffset  = verticesOffset + numVertices * sizeof(IconVertex) + kAnimationBytes;
    const u32 sizeBytes      = textureOffset + kTextureBytes;

    u8 * const data = static_cast<u8 *>(ps2::heap::TryAlloc(sizeBytes, MemTag::SaveData));
    if (data == nullptr)
    {
        return false;
    }

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

    u8 * cursor = data + textureOffset - kAnimationBytes;
    std::memcpy(data, &header, sizeof(header));
    std::memcpy(cursor, &anim, sizeof(anim));
    cursor += sizeof(anim);
    std::memcpy(cursor, &frame, sizeof(frame));
    cursor += sizeof(frame);
    std::memcpy(cursor, &key, sizeof(key));

    outFile.data      = data;
    outFile.sizeBytes = sizeBytes;
    outFile.vertices  = data + verticesOffset;

    // `data` comes from the heap and every block before the texture is a whole number of
    // u16s, so the texture is aligned for them.
    outFile.texels = static_cast<u16 *>(static_cast<void *>(data + textureOffset));
    return true;
}

inline void PutVertex(IconFile & file, u32 & index, const IconVertex & vertex)
{
    std::memcpy(file.vertices + index * sizeof(IconVertex), &vertex, sizeof(vertex));
    ++index;
}

void FillPlainTexture(u16 * outTexels)
{
    for (int i = 0; i < kTextureSize * kTextureSize; ++i)
    {
        outTexels[i] = PackTexel(72, 56, 40); // A dark bronze.
    }
}

// ------------------------------------------------------------------------------------------------
// Skin texture
// ------------------------------------------------------------------------------------------------

inline bool HasColour(const Picture & picture, const int x, const int y)
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

// Stretches the picture over the texture, bilinear. "No pixel" pixels lend no colour - they
// would otherwise pull their surroundings towards black.
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
                if (!HasColour(picture, xs[c], ys[c]))
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

            outTexels[ty * kTextureSize + tx] = PackTexel(static_cast<int>(rgb[0]), static_cast<int>(rgb[1]),
                                                          static_cast<int>(rgb[2]));
        }
    }
}

// Loads a PCX from the game data and stretches it over the texture. False if it couldn't be
// loaded or decoded, leaving the texture untouched.
bool TextureFromPcxFile(const char * name, u16 * outTexels)
{
    void * pcx = nullptr;
    const int pcxBytes = FS_LoadFile(name, &pcx);

    Picture picture = {};
    const bool decoded = (pcxBytes > 0) && DecodePcx(static_cast<const u8 *>(pcx), pcxBytes, picture);
    if (decoded)
    {
        ResampleTexture(picture, outTexels);
        FreePicture(picture);
    }

    if (pcx != nullptr)
    {
        FS_FreeFile(pcx);
    }
    return decoded;
}

// ------------------------------------------------------------------------------------------------
// The model
// ------------------------------------------------------------------------------------------------

// One keyframe vertex, decoded to model space - Quake's: X forward, Y left, Z up.
Vec3 KeyframePosition(const daliasframe_t & frame, const int index)
{
    const dtrivertx_t & vertex = frame.verts[index];
    return Vec3{ static_cast<float>(vertex.v[0]) * frame.scale[0] + frame.translate[0],
                 static_cast<float>(vertex.v[1]) * frame.scale[1] + frame.translate[1],
                 static_cast<float>(vertex.v[2]) * frame.scale[2] + frame.translate[2] };
}

// Turns the model's triangles into the icon's vertices: the bounding box's longest side
// kIconSize, centred, standing on the floor. Model space maps to icon space as X <- Y,
// Y <- -Z, Z <- -X: up is the icon's -Y, and the model's front faces -Z.
void AddModelTriangles(IconFile & file, const dmdl_t & header, const daliasframe_t & frame,
                       const mod::AliasVertex * corners, const int numTris)
{
    Vec3 mins = KeyframePosition(frame, 0);
    Vec3 maxs = mins;
    for (int i = 1; i < header.num_xyz; ++i)
    {
        const Vec3 p = KeyframePosition(frame, i);
        mins.x = (p.x < mins.x) ? p.x : mins.x;
        mins.y = (p.y < mins.y) ? p.y : mins.y;
        mins.z = (p.z < mins.z) ? p.z : mins.z;
        maxs.x = (p.x > maxs.x) ? p.x : maxs.x;
        maxs.y = (p.y > maxs.y) ? p.y : maxs.y;
        maxs.z = (p.z > maxs.z) ? p.z : maxs.z;
    }

    const float sizeX   = maxs.x - mins.x;
    const float sizeY   = maxs.y - mins.y;
    const float sizeZ   = maxs.z - mins.z;
    const float longest = (sizeX > sizeY) ? ((sizeX > sizeZ) ? sizeX : sizeZ) : ((sizeY > sizeZ) ? sizeY : sizeZ);
    const float unit    = (longest > 0.0f) ? (kIconSize / longest) : 1.0f;
    const float centreX = (mins.x + maxs.x) * 0.5f;
    const float centreY = (mins.y + maxs.y) * 0.5f;

    const auto makeVertex = [&](const mod::AliasVertex & corner) -> IconVertex {
        const int index = static_cast<int>(corner.index);
        const Vec3 p = KeyframePosition(frame, index);

        int normalIndex = frame.verts[index].lightnormalindex;
        normalIndex = (normalIndex < NUMVERTEXNORMALS) ? normalIndex : 0;
        const float * const n = bytedirs[normalIndex];

        IconVertex vertex;
        SetVertex(vertex,
                  (p.y - centreY) * unit, -(p.z - mins.z) * unit, -(p.x - centreX) * unit,
                  n[1], -n[2], -n[0],
                  corner.s, corner.t);
        return vertex;
    };

    u32 vertexIndex = 0;
    for (int tri = 0; tri < numTris; ++tri)
    {
        const IconVertex a = makeVertex(corners[tri * 3 + 0]);
        const IconVertex b = makeVertex(corners[tri * 3 + 1]);
        const IconVertex c = makeVertex(corners[tri * 3 + 2]);

        PutVertex(file, vertexIndex, a);
        PutVertex(file, vertexIndex, b);
        PutVertex(file, vertexIndex, c);

        if (kDoubleSidedModel)
        {
            PutVertex(file, vertexIndex, a);
            PutVertex(file, vertexIndex, c);
            PutVertex(file, vertexIndex, b);
        }
    }
}

// The model icon. False if the model or memory isn't there, with nothing allocated.
bool BuildModelIcon(IconFile & outFile)
{
    void * file = nullptr;
    const int fileBytes = FS_LoadFile(kIconModel, &file);
    if (fileBytes <= 0)
    {
        Com_Printf("Save icon: no %s in the game data, the icon will be plain.\n", kIconModel);
        return false;
    }

    // FS_LoadFile's buffer comes from the heap, aligned for the header. The blocks inside it
    // are read in place too, so their offsets must keep that alignment - true of every MD2
    // in pak0, whose records are all whole words.
    const u8 * const bytes = static_cast<const u8 *>(file);
    const dmdl_t & header = *static_cast<const dmdl_t *>(file);
    const bool aligned = (fileBytes >= static_cast<int>(sizeof(dmdl_t))) &&
                         ((header.ofs_glcmds | header.ofs_frames | header.ofs_skins) & 3) == 0;

    bool built = false;
    if (aligned && mod::ValidateMD2Header(header, fileBytes, kIconModel))
    {
        const size_t cornersBytes = static_cast<size_t>(header.num_tris) * 3u * sizeof(mod::AliasVertex);
        auto * const corners = static_cast<mod::AliasVertex *>(
            ps2::heap::TryAllocAligned(ps2::heap::MemAlign(alignof(mod::AliasVertex)), cornersBytes, MemTag::SaveData));

        const s32 * const glcmds = static_cast<const s32 *>(static_cast<const void *>(bytes + header.ofs_glcmds));
        const int numTris = (corners != nullptr)
                          ? mod::ExpandGLCmdsToTriangles(glcmds, header.num_glcmds, header.num_xyz,
                                                         header.num_tris, corners, kIconModel)
                          : -1;

        const u32 verticesPerTri = kDoubleSidedModel ? 6u : 3u;
        if (numTris > 0 && NewIconFile(static_cast<u32>(numTris) * verticesPerTri, outFile))
        {
            const auto & frame = *static_cast<const daliasframe_t *>(static_cast<const void *>(bytes + header.ofs_frames));
            AddModelTriangles(outFile, header, frame, corners, numTris);

            // The model's first skin, stretched over the texture as it is - its coordinates are
            // already normalised, so the stretch is undone where they're looked up.
            char skinName[MAX_SKINNAME] = {};
            if (header.num_skins > 0)
            {
                std::memcpy(skinName, bytes + header.ofs_skins, sizeof(skinName) - 1);
            }
            if (skinName[0] == '\0' || !TextureFromPcxFile(skinName, outFile.texels))
            {
                Com_Printf("Save icon: couldn't load the skin of %s, the model will be plain.\n", kIconModel);
                FillPlainTexture(outFile.texels);
            }
            built = true;
        }

        ps2::heap::Free(corners, cornersBytes, MemTag::SaveData);
    }

    FS_FreeFile(file);
    return built;
}

// What the icon is without the model: a plain square, kIconSize across, so the save directory
// still has an icon. Both windings, like the model. False if out of memory.
bool BuildPlainIcon(IconFile & outFile)
{
    constexpr int kNumCorners = 6; // Two triangles.
    if (!NewIconFile(kDoubleSidedModel ? kNumCorners * 2 : kNumCorners, outFile))
    {
        return false;
    }

    constexpr float kHalf = kIconSize * 0.5f;
    IconVertex corners[kNumCorners];
    SetVertex(corners[0], -kHalf, -kIconSize, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f);
    SetVertex(corners[1],  kHalf, -kIconSize, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f);
    SetVertex(corners[2], -kHalf,  0.0f,      0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f);
    SetVertex(corners[3],  kHalf, -kIconSize, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f);
    SetVertex(corners[4],  kHalf,  0.0f,      0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 1.0f);
    SetVertex(corners[5], -kHalf,  0.0f,      0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f);

    u32 vertexIndex = 0;
    for (int tri = 0; tri < kNumCorners / 3; ++tri)
    {
        const IconVertex & a = corners[tri * 3 + 0];
        const IconVertex & b = corners[tri * 3 + 1];
        const IconVertex & c = corners[tri * 3 + 2];

        PutVertex(outFile, vertexIndex, a);
        PutVertex(outFile, vertexIndex, b);
        PutVertex(outFile, vertexIndex, c);

        if (kDoubleSidedModel)
        {
            PutVertex(outFile, vertexIndex, a);
            PutVertex(outFile, vertexIndex, c);
            PutVertex(outFile, vertexIndex, b);
        }
    }

    FillPlainTexture(outFile.texels);
    return true;
}

void BuildIconSys(mcIcon & sys)
{
    const iconIVECTOR kBackground[4] = {
        { 96, 36, 20, 0 }, // top left
        { 96, 36, 20, 0 }, // top right
        { 20,  8,  4, 0 }, // bottom left
        { 20,  8,  4, 0 }, // bottom right
    };
    const iconFVECTOR kLightDir[3] = {
        {  0.5f,  0.5f, 0.5f,  0.0f },
        {  0.0f, -0.4f, -0.1f, 0.0f },
        { -0.5f, -0.5f, 0.5f,  0.0f },
    };
    const iconFVECTOR kLightColour[3] = {
        { 0.3f, 0.3f, 0.3f, 0.0f },
        { 0.4f, 0.4f, 0.4f, 0.0f },
        { 0.5f, 0.5f, 0.5f, 0.0f },
    };
    const iconFVECTOR kAmbient = { 0.5f, 0.5f, 0.5f, 0.0f };

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

} // namespace

bool EnsureSaveIcons(Device & device)
{
    IconFile icon = {};
    if (!BuildModelIcon(icon) && !BuildPlainIcon(icon))
    {
        SetError("Not enough memory to create the save icon.");
        return false;
    }

    mcIcon sys;
    BuildIconSys(sys);

    const bool upToDate = FileMatches(device, kIconModelFile, icon.data, icon.sizeBytes) &&
                          FileMatches(device, kIconSysFile, &sys, sizeof(sys));

    const bool ok = upToDate ||
                    (WriteWholeFile(device, kIconModelFile, icon.data, icon.sizeBytes) &&
                     WriteWholeFile(device, kIconSysFile, &sys, sizeof(sys)));

    ps2::heap::Free(icon.data, icon.sizeBytes, MemTag::SaveData);

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
