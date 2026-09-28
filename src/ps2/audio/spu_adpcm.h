#pragma once
/* ================================================================================================
 * File: spu_adpcm.h
 * Brief: SPU2 ADPCM - the PS1/PS2 sound chip's native 4-bit sample encoding, the payload of
 *        Sony's .VAG files - and the layout of the music files streamed from it
 *        (baseq2/music/trackNN.adp: written by src/tools/host/musenc.cpp, played by
 *        music_stream.cpp for the CD audio replacement in cd_audio.cpp).
 *
 *        Header-only and free of ps2sdk types on purpose: the EE runtime and the host
 *        encoder include this same file, so the encoder's closed-loop search and the SNR
 *        it reports are computed with the exact decoder the console runs.
 *
 *        A block is 16 bytes and decodes to 28 samples:
 *
 *          byte 0      filter << 4 | shift
 *          byte 1      flags - loop/end markers, only the SPU2 voice hardware reads them
 *          bytes 2-15  28 signed 4-bit residuals, low nibble first
 *
 *        Each sample is its residual scaled by 2^(12 - shift), plus a two-tap prediction
 *        from the two samples decoded before it. The history carries across blocks, so a
 *        channel has to be decoded in order from its first block.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <cstdint>

namespace ps2::audio {
namespace spu_adpcm {

constexpr int kBlockBytes   = 16;
constexpr int kBlockSamples = 28;
constexpr int kNumFilters   = 5;
constexpr int kMaxShift     = 12;

// Block flags (byte 1). The end flag marks a channel's last block; a voice playing the
// data straight off SPU2 RAM stops there.
constexpr std::uint8_t kFlagEnd = 0x01;

// Prediction coefficients per filter, in 1/64ths:
//   prediction = (s1 * K0 + s2 * K1 + 32) >> 6
constexpr int kFilterK0[kNumFilters] = { 0, 60, 115,  98, 122 };
constexpr int kFilterK1[kNumFilters] = { 0,  0, -52, -55, -60 };

// The two most recently decoded samples of one channel, s1 the newer.
struct History
{
    int s1 = 0;
    int s2 = 0;
};

constexpr int ClampS16(const int value)
{
    return (value < -32768) ? -32768 : ((value > 32767) ? 32767 : value);
}

// The two halves of a decoded sample: the residual scaled up from its 4-bit nibble
// (0..15, two's complement) and the filter's prediction from the history.
constexpr int Residual(const int nibble, const int shift)
{
    // Sign-extend the nibble into the top four bits of a 16-bit sample, then scale down.
    return (((nibble ^ 8) - 8) * 4096) >> shift;
}

constexpr int Prediction(const int k0, const int k1, const int s1, const int s2)
{
    return (s1 * k0 + s2 * k1 + 32) >> 6;
}

inline int Predict(const int filter, const History & history)
{
    return Prediction(kFilterK0[filter], kFilterK1[filter], history.s1, history.s2);
}

// Decodes one sample and pushes it into the history. The encoder runs its candidates
// through this, one sample at a time.
inline int DecodeSample(const int nibble, const int shift, const int filter, History & history)
{
    const int sample = ClampS16(Residual(nibble, shift) + Predict(filter, history));

    history.s2 = history.s1;
    history.s1 = sample;
    return sample;
}

// Decodes one block into 28 samples written `stride` apart: 1 for a plain mono buffer,
// 2 for one channel of interleaved stereo frames. Same arithmetic as DecodeSample.
inline void DecodeBlock(const std::uint8_t * block, std::int16_t * out, const int stride, History & history)
{
    int shift  = block[0] & 0x0F;
    int filter = block[0] >> 4;

    // Values the encoder never writes. The SPU treats shifts 13-15 as 9; an unknown
    // filter is read as no prediction.
    if (shift > kMaxShift)
    {
        shift = 9;
    }
    if (filter >= kNumFilters)
    {
        filter = 0;
    }

    // The history lives in locals for the loop: with strict aliasing off (the EE build),
    // every out[] store could alias the History fields as far as the compiler knows, and
    // it would load and store both of them around every sample.
    const int k0 = kFilterK0[filter];
    const int k1 = kFilterK1[filter];
    int       s1 = history.s1;
    int       s2 = history.s2;

    // One byte - two samples, low nibble first - per iteration, so there is no odd/even
    // select in the loop.
    for (int i = 0; i < kBlockSamples / 2; ++i)
    {
        const int packed = block[2 + i];

        const int first = ClampS16(Residual(packed & 0x0F, shift) + Prediction(k0, k1, s1, s2));
        s2 = s1;
        s1 = first;

        const int second = ClampS16(Residual(packed >> 4, shift) + Prediction(k0, k1, s1, s2));
        s2 = s1;
        s1 = second;

        out[(i * 2) * stride]       = static_cast<std::int16_t>(first);
        out[((i * 2) + 1) * stride] = static_cast<std::int16_t>(second);
    }

    history.s1 = s1;
    history.s2 = s2;
}

} // namespace spu_adpcm

// ------------------------------------------------------------------------------------------------
// Music file layout (music/trackNN.adp)
// ------------------------------------------------------------------------------------------------

// Little-endian. A 2048 byte header block, so the payload - and with it every read the
// stream makes - starts sector aligned, followed by the ADPCM payload interleaved in
// per-channel chunks:
//
//   [header, zero padded to 2048] [L chunk][R chunk][L chunk][R chunk] ...
//
// A chunk is chunkBytes of one channel's consecutive blocks (128 blocks at the standard
// 2048). Chunk interleave rather than block interleave is what SPU2 voice streaming needs
// - each voice reads its own channel's blocks contiguously - so the same files can move
// onto the hardware voices later without re-encoding. The payload tail is padded with
// silent blocks to whole chunks; frameCount says where the music really ends.
namespace music_file {

constexpr char          kMagic[4]    = { 'Q', '2', 'M', 'U' };
constexpr std::uint16_t kVersion     = 1;
constexpr int           kHeaderBytes = 2048;
constexpr int           kChunkBytes  = 2048; // What musenc writes; the header carries the real value.
constexpr int           kMaxChannels = 2;

struct Header
{
    char          magic[4];   // kMagic
    std::uint16_t version;    // kVersion
    std::uint8_t  channels;   // 1 or 2
    std::uint8_t  reserved;
    std::uint32_t sampleRate; // Hz
    std::uint32_t frameCount; // Samples per channel, excluding the tail padding.
    std::uint32_t chunkBytes; // Per-channel interleave unit, a multiple of kBlockBytes.
    std::uint32_t dataBytes;  // Payload size, a multiple of chunkBytes * channels.
};
static_assert(sizeof(Header) == 24, "Header layout must match the file");

// Null if the header describes a file we can play, otherwise the reason it can't.
inline const char * Validate(const Header & header)
{
    for (int i = 0; i < 4; ++i)
    {
        if (header.magic[i] != kMagic[i])
        {
            return "not a music file (bad magic)";
        }
    }
    if (header.version != kVersion)
    {
        return "unsupported version";
    }
    if (header.channels < 1 || header.channels > kMaxChannels)
    {
        return "bad channel count";
    }
    if (header.sampleRate < 4000 || header.sampleRate > 48000)
    {
        return "bad sample rate";
    }
    if (header.chunkBytes == 0 || (header.chunkBytes % spu_adpcm::kBlockBytes) != 0)
    {
        return "bad chunk size";
    }

    const std::uint32_t groupBytes = header.chunkBytes * header.channels;
    if (header.dataBytes == 0 || (header.dataBytes % groupBytes) != 0)
    {
        return "payload is not whole chunks";
    }

    const std::uint32_t blocksPerChannel = header.dataBytes / header.channels / spu_adpcm::kBlockBytes;
    if (header.frameCount == 0 || header.frameCount > blocksPerChannel * spu_adpcm::kBlockSamples)
    {
        return "frame count doesn't match the payload";
    }
    return nullptr;
}

} // namespace music_file
} // namespace ps2::audio
