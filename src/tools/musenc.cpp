/* ================================================================================================
 * File: musenc.cpp
 * Brief:
 *  Encodes the soundtrack for the PS2 CD audio module (src/ps2/audio/cd_audio.cpp): a
 *  16-bit PCM .wav in, a music/trackNN.adp out - SPU2 ADPCM, the PS2's native sample
 *  format, chunk-interleaved as src/ps2/audio/spu_adpcm.h lays out. 3.5x smaller than
 *  16-bit PCM, and the EE decodes it for next to nothing.
 *
 *  Usage:
 *    musenc [-r rate] <in.wav> <out.adp>   encode; rate defaults to 22050, the game's mix rate
 *    musenc -d <in.adp> <out.wav>          decode back to 16-bit PCM, to listen to or diff
 *
 *  `make music` runs the first form over every baseq2/music/trackNN.wav.
 *
 *  Input at the target rate is encoded as is. Input at exactly twice the target - a 44.1kHz
 *  CD rip for the default 22050 - is first halved through a windowed-sinc low-pass. Any
 *  other rate has to be converted beforehand, e.g. with macOS's afconvert:
 *    afconvert -f WAVE -d LEI16@22050 -c 2 --src-complexity bats -r 127 in.flac out.wav
 *
 *  The encoder is closed loop: for every 28-sample block it runs all 5 predictors x 13 shifts
 *  through the game's own decoder (spu_adpcm.h), carrying the decoded history forward, and
 *  keeps the lowest squared error. The SNR it reports is therefore of exactly what the
 *  console will play.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../ps2/audio/spu_adpcm.h"

namespace adpcm = ps2::audio::spu_adpcm;
namespace music = ps2::audio::music_file;

// ------------------------------------------------------------------------------------------------
// File and WAV helpers
// ------------------------------------------------------------------------------------------------

struct PcmAudio
{
    int sampleRate = 0;
    std::vector<std::vector<int16_t>> channels; // One sample vector per channel.

    int FrameCount() const { return channels.empty() ? 0 : static_cast<int>(channels[0].size()); }
};

static bool ReadWholeFile(const char * path, std::vector<uint8_t> & out)
{
    FILE * file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return false;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    out.resize(size > 0 ? static_cast<size_t>(size) : 0);
    const size_t got = out.empty() ? 0 : std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    return got == out.size();
}

static bool WriteWholeFile(const char * path, const std::vector<uint8_t> & data)
{
    FILE * file = std::fopen(path, "wb");
    if (file == nullptr)
    {
        return false;
    }
    const size_t put = std::fwrite(data.data(), 1, data.size(), file);
    return (std::fclose(file) == 0) && (put == data.size());
}

static uint32_t Le16(const uint8_t * p) { return p[0] | (p[1] << 8); }
static uint32_t Le32(const uint8_t * p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

static void PutLe16(std::vector<uint8_t> & out, size_t at, uint32_t v)
{
    out[at + 0] = static_cast<uint8_t>(v);
    out[at + 1] = static_cast<uint8_t>(v >> 8);
}

static void PutLe32(std::vector<uint8_t> & out, size_t at, uint32_t v)
{
    PutLe16(out, at, v & 0xFFFF);
    PutLe16(out, at + 2, v >> 16);
}

// 16-bit PCM only, mono or stereo. Walks the chunk list, so LIST/fact/etc. are skipped.
static bool ParseWav(const std::vector<uint8_t> & file, PcmAudio & out, std::string & error)
{
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) != 0 || std::memcmp(file.data() + 8, "WAVE", 4) != 0)
    {
        error = "not a RIFF/WAVE file";
        return false;
    }

    uint32_t format = 0, numChannels = 0, sampleRate = 0, bits = 0;
    size_t dataPos = 0, dataLen = 0;
    bool haveFmt = false, haveData = false;

    size_t pos = 12;
    while (pos + 8 <= file.size())
    {
        const uint8_t * id   = file.data() + pos;
        const size_t    body = pos + 8;
        size_t          len  = Le32(id + 4);
        if (body + len > file.size())
        {
            len = file.size() - body; // Tolerate a data chunk that claims more than is there.
        }

        if (std::memcmp(id, "fmt ", 4) == 0 && len >= 16)
        {
            const uint8_t * fmt = file.data() + body;
            format      = Le16(fmt);
            numChannels = Le16(fmt + 2);
            sampleRate  = Le32(fmt + 4);
            bits        = Le16(fmt + 14);
            if (format == 0xFFFE && len >= 26) // WAVE_FORMAT_EXTENSIBLE: the real tag is the subformat's.
            {
                format = Le16(fmt + 24);
            }
            haveFmt = true;
        }
        else if (std::memcmp(id, "data", 4) == 0)
        {
            dataPos  = body;
            dataLen  = len;
            haveData = true;
        }
        pos = body + len + (len & 1); // Chunks are word aligned.
    }

    if (!haveFmt || !haveData)
    {
        error = "missing fmt or data chunk";
        return false;
    }
    if (format != 1 || bits != 16)
    {
        error = "only 16-bit PCM is supported (format " + std::to_string(format) + ", " + std::to_string(bits) + " bits)";
        return false;
    }
    if (numChannels < 1 || numChannels > static_cast<uint32_t>(music::kMaxChannels))
    {
        error = "only mono or stereo is supported";
        return false;
    }

    const size_t frames = dataLen / (2 * numChannels);
    out.sampleRate = static_cast<int>(sampleRate);
    out.channels.assign(numChannels, std::vector<int16_t>(frames));

    const uint8_t * src = file.data() + dataPos;
    for (size_t f = 0; f < frames; ++f)
    {
        for (uint32_t c = 0; c < numChannels; ++c, src += 2)
        {
            out.channels[c][f] = static_cast<int16_t>(Le16(src));
        }
    }
    return true;
}

static std::vector<uint8_t> BuildWav(const PcmAudio & audio)
{
    const uint32_t numChannels = static_cast<uint32_t>(audio.channels.size());
    const uint32_t frames      = static_cast<uint32_t>(audio.FrameCount());
    const uint32_t dataBytes   = frames * numChannels * 2;

    std::vector<uint8_t> out(44 + dataBytes);
    std::memcpy(&out[0], "RIFF", 4);
    PutLe32(out, 4, 36 + dataBytes);
    std::memcpy(&out[8], "WAVEfmt ", 8);
    PutLe32(out, 16, 16);
    PutLe16(out, 20, 1);
    PutLe16(out, 22, numChannels);
    PutLe32(out, 24, static_cast<uint32_t>(audio.sampleRate));
    PutLe32(out, 28, static_cast<uint32_t>(audio.sampleRate) * numChannels * 2);
    PutLe16(out, 32, numChannels * 2);
    PutLe16(out, 34, 16);
    std::memcpy(&out[36], "data", 4);
    PutLe32(out, 40, dataBytes);

    size_t at = 44;
    for (uint32_t f = 0; f < frames; ++f)
    {
        for (uint32_t c = 0; c < numChannels; ++c, at += 2)
        {
            PutLe16(out, at, static_cast<uint16_t>(audio.channels[c][f]));
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// 2:1 decimation
// ------------------------------------------------------------------------------------------------

static double BesselI0(const double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k)
    {
        const double t = x / (2.0 * k);
        term *= t * t;
        sum  += term;
        if (term < sum * 1e-14)
        {
            break;
        }
    }
    return sum;
}

// Halves the sample rate: a Kaiser-windowed sinc low-pass (255 taps, beta 9: ~90 dB
// stopband, ~1 kHz transition band at 44.1kHz) centred just under the new Nyquist, then
// every other sample. The stopband starts at the new Nyquist, so nothing audible folds back.
static std::vector<int16_t> Decimate2(const std::vector<int16_t> & in, const int inRate)
{
    constexpr int    kTaps = 255;
    constexpr int    kHalf = kTaps / 2;
    constexpr double kBeta = 9.0;

    // Cutoff (-6 dB) half a transition band below the output Nyquist (inRate / 4).
    const double cutoff = (inRate / 4.0 - 525.0 * inRate / 44100.0) / inRate;

    std::vector<double> taps(kTaps);
    double sum = 0.0;
    for (int i = 0; i < kTaps; ++i)
    {
        const int    n    = i - kHalf;
        const double sinc = (n == 0) ? 2.0 * cutoff : std::sin(2.0 * M_PI * cutoff * n) / (M_PI * n);
        const double r    = static_cast<double>(n) / kHalf;
        taps[i] = sinc * BesselI0(kBeta * std::sqrt(1.0 - r * r)) / BesselI0(kBeta);
        sum    += taps[i];
    }
    for (double & tap : taps)
    {
        tap /= sum; // Unity gain at DC.
    }

    const int inCount  = static_cast<int>(in.size());
    const int outCount = (inCount + 1) / 2;
    std::vector<int16_t> out(static_cast<size_t>(outCount));

    for (int m = 0; m < outCount; ++m)
    {
        const int centre = 2 * m;
        double acc = 0.0;
        for (int i = 0; i < kTaps; ++i)
        {
            const int k = centre + i - kHalf;
            if (k >= 0 && k < inCount)
            {
                acc += taps[i] * in[k];
            }
        }
        out[m] = static_cast<int16_t>(adpcm::ClampS16(static_cast<int>(std::lround(acc))));
    }
    return out;
}

// ------------------------------------------------------------------------------------------------
// Encoding
// ------------------------------------------------------------------------------------------------

struct EncodedChannel
{
    std::vector<uint8_t> blocks; // Padded to whole chunks.
    double signal = 0.0;         // Sum of squares of the input, for the SNR.
    double noise  = 0.0;         // Sum of squared decode errors.
};

static void EncodeChannel(const std::vector<int16_t> & in, const int paddedBlocks, EncodedChannel & out)
{
    const int frames     = static_cast<int>(in.size());
    const int realBlocks = (frames + adpcm::kBlockSamples - 1) / adpcm::kBlockSamples;

    // Zero blocks decode to silence with filter 0, which is what the tail padding is.
    out.blocks.assign(static_cast<size_t>(paddedBlocks) * adpcm::kBlockBytes, 0);

    adpcm::History history;
    for (int b = 0; b < realBlocks; ++b)
    {
        int x[adpcm::kBlockSamples];
        for (int i = 0; i < adpcm::kBlockSamples; ++i)
        {
            const int index = b * adpcm::kBlockSamples + i;
            x[i] = (index < frames) ? in[index] : 0;
        }

        double         bestError = HUGE_VAL;
        int            bestFilter = 0, bestShift = 0;
        uint8_t        bestNibbles[adpcm::kBlockSamples] = {};
        adpcm::History bestHistory;

        for (int filter = 0; filter < adpcm::kNumFilters; ++filter)
        {
            for (int shift = 0; shift <= adpcm::kMaxShift; ++shift)
            {
                adpcm::History h = history;
                uint8_t        nibbles[adpcm::kBlockSamples];
                const double   step  = static_cast<double>(1 << (adpcm::kMaxShift - shift));
                double         error = 0.0;

                int i = 0;
                for (; i < adpcm::kBlockSamples; ++i)
                {
                    long q = std::lround((x[i] - adpcm::Predict(filter, h)) / step);
                    q = (q < -8) ? -8 : ((q > 7) ? 7 : q);
                    nibbles[i] = static_cast<uint8_t>(q & 0x0F);

                    const double e = x[i] - adpcm::DecodeSample(nibbles[i], shift, filter, h);
                    error += e * e;
                    if (error >= bestError)
                    {
                        break; // Already worse than the best candidate.
                    }
                }

                if (i == adpcm::kBlockSamples && error < bestError)
                {
                    bestError   = error;
                    bestFilter  = filter;
                    bestShift   = shift;
                    bestHistory = h;
                    std::memcpy(bestNibbles, nibbles, sizeof(nibbles));
                }
            }
        }

        uint8_t * block = &out.blocks[static_cast<size_t>(b) * adpcm::kBlockBytes];
        block[0] = static_cast<uint8_t>((bestFilter << 4) | bestShift);
        block[1] = (b == realBlocks - 1) ? adpcm::kFlagEnd : 0;
        for (int i = 0; i < adpcm::kBlockSamples; i += 2)
        {
            block[2 + i / 2] = static_cast<uint8_t>(bestNibbles[i] | (bestNibbles[i + 1] << 4));
        }

        history = bestHistory;
        for (int i = 0; i < adpcm::kBlockSamples && (b * adpcm::kBlockSamples + i) < frames; ++i)
        {
            out.signal += static_cast<double>(x[i]) * x[i];
        }
        out.noise += bestError; // Past-the-end samples are zeros encoded as zeros: ~no error.
    }
}

static double SnrDb(const double signal, const double noise)
{
    return (noise > 0.0) ? 10.0 * std::log10(signal / noise) : 999.0;
}

static int Encode(const char * inPath, const char * outPath, const int targetRate)
{
    std::vector<uint8_t> file;
    if (!ReadWholeFile(inPath, file))
    {
        std::fprintf(stderr, "musenc: can't read %s\n", inPath);
        return EXIT_FAILURE;
    }

    PcmAudio    audio;
    std::string error;
    if (!ParseWav(file, audio, error))
    {
        std::fprintf(stderr, "musenc: %s: %s\n", inPath, error.c_str());
        return EXIT_FAILURE;
    }
    file.clear();

    const int inRate = audio.sampleRate;
    if (inRate == targetRate * 2)
    {
        for (std::vector<int16_t> & channel : audio.channels)
        {
            channel = Decimate2(channel, inRate);
        }
        audio.sampleRate = targetRate;
    }
    else if (inRate != targetRate)
    {
        std::fprintf(stderr, "musenc: %s is %d Hz; only %d Hz or %d Hz input is supported. Convert it first, e.g.\n"
                             "  afconvert -f WAVE -d LEI16@%d -c 2 --src-complexity bats -r 127 in.wav out.wav\n",
                     inPath, inRate, targetRate, targetRate * 2, targetRate);
        return EXIT_FAILURE;
    }

    const int numChannels = static_cast<int>(audio.channels.size());
    const int frames      = audio.FrameCount();
    if (frames == 0)
    {
        std::fprintf(stderr, "musenc: %s has no samples\n", inPath);
        return EXIT_FAILURE;
    }

    const int blocksPerChunk = music::kChunkBytes / adpcm::kBlockBytes;
    const int realBlocks     = (frames + adpcm::kBlockSamples - 1) / adpcm::kBlockSamples;
    const int numChunks      = (realBlocks + blocksPerChunk - 1) / blocksPerChunk;
    const int paddedBlocks   = numChunks * blocksPerChunk;

    std::vector<EncodedChannel> encoded(static_cast<size_t>(numChannels));
    for (int c = 0; c < numChannels; ++c)
    {
        EncodeChannel(audio.channels[c], paddedBlocks, encoded[c]);
    }

    const uint32_t dataBytes = static_cast<uint32_t>(numChunks) * music::kChunkBytes * static_cast<uint32_t>(numChannels);
    std::vector<uint8_t> out(music::kHeaderBytes, 0);
    out.reserve(out.size() + dataBytes);

    std::memcpy(&out[0], music::kMagic, 4);
    PutLe16(out, 4, music::kVersion);
    out[6] = static_cast<uint8_t>(numChannels);
    out[7] = 0;
    PutLe32(out, 8, static_cast<uint32_t>(audio.sampleRate));
    PutLe32(out, 12, static_cast<uint32_t>(frames));
    PutLe32(out, 16, music::kChunkBytes);
    PutLe32(out, 20, dataBytes);

    for (int chunk = 0; chunk < numChunks; ++chunk)
    {
        for (int c = 0; c < numChannels; ++c)
        {
            const uint8_t * src = &encoded[c].blocks[static_cast<size_t>(chunk) * music::kChunkBytes];
            out.insert(out.end(), src, src + music::kChunkBytes);
        }
    }

    if (!WriteWholeFile(outPath, out))
    {
        std::fprintf(stderr, "musenc: can't write %s\n", outPath);
        return EXIT_FAILURE;
    }

    double signal = 0.0, noise = 0.0;
    for (const EncodedChannel & channel : encoded)
    {
        signal += channel.signal;
        noise  += channel.noise;
    }

    const double seconds = static_cast<double>(frames) / audio.sampleRate;
    std::printf("%s: %d Hz %s -> %d Hz, %.2f s, %zu bytes (%.0f B/s), SNR %.1f dB",
                outPath, inRate, (numChannels == 2) ? "stereo" : "mono", audio.sampleRate, seconds,
                out.size(), out.size() / seconds, SnrDb(signal, noise));
    if (numChannels == 2)
    {
        std::printf(" (L %.1f, R %.1f)", SnrDb(encoded[0].signal, encoded[0].noise), SnrDb(encoded[1].signal, encoded[1].noise));
    }
    std::printf("\n");
    return EXIT_SUCCESS;
}

// ------------------------------------------------------------------------------------------------
// Decoding
// ------------------------------------------------------------------------------------------------

static int Decode(const char * inPath, const char * outPath)
{
    std::vector<uint8_t> file;
    if (!ReadWholeFile(inPath, file) || file.size() < static_cast<size_t>(music::kHeaderBytes))
    {
        std::fprintf(stderr, "musenc: can't read %s\n", inPath);
        return EXIT_FAILURE;
    }

    music::Header header;
    std::memcpy(&header, file.data(), sizeof(header));
    if (const char * error = music::Validate(header))
    {
        std::fprintf(stderr, "musenc: %s: %s\n", inPath, error);
        return EXIT_FAILURE;
    }
    if (file.size() < music::kHeaderBytes + static_cast<size_t>(header.dataBytes))
    {
        std::fprintf(stderr, "musenc: %s: truncated payload\n", inPath);
        return EXIT_FAILURE;
    }

    // The same chunk walk as the game's MusicStream::DecodeNextBlock.
    const int numChannels      = header.channels;
    const int chunkBytes       = static_cast<int>(header.chunkBytes);
    const int groupBytes       = chunkBytes * numChannels;
    const int blocksPerChunk   = chunkBytes / adpcm::kBlockBytes;
    const int frames           = static_cast<int>(header.frameCount);
    const int blocksPerChannel = (frames + adpcm::kBlockSamples - 1) / adpcm::kBlockSamples;

    PcmAudio audio;
    audio.sampleRate = static_cast<int>(header.sampleRate);
    audio.channels.assign(static_cast<size_t>(numChannels), std::vector<int16_t>(static_cast<size_t>(blocksPerChannel) * adpcm::kBlockSamples));

    for (int c = 0; c < numChannels; ++c)
    {
        adpcm::History history;
        for (int k = 0; k < blocksPerChannel; ++k)
        {
            const size_t offset = music::kHeaderBytes + static_cast<size_t>(k / blocksPerChunk) * groupBytes +
                                  static_cast<size_t>(c) * chunkBytes + static_cast<size_t>(k % blocksPerChunk) * adpcm::kBlockBytes;
            adpcm::DecodeBlock(&file[offset], &audio.channels[c][static_cast<size_t>(k) * adpcm::kBlockSamples], 1, history);
        }
        audio.channels[c].resize(static_cast<size_t>(frames));
    }

    if (!WriteWholeFile(outPath, BuildWav(audio)))
    {
        std::fprintf(stderr, "musenc: can't write %s\n", outPath);
        return EXIT_FAILURE;
    }

    std::printf("%s: %d Hz %s, %.2f s\n", outPath, audio.sampleRate, (numChannels == 2) ? "stereo" : "mono",
                static_cast<double>(frames) / audio.sampleRate);
    return EXIT_SUCCESS;
}

// ------------------------------------------------------------------------------------------------

static void PrintUsage(const char * self)
{
    std::printf("Usage:\n"
                "  %s [-r rate] <in.wav> <out.adp>   encode 16-bit PCM to SPU2 ADPCM (rate default 22050)\n"
                "  %s -d <in.adp> <out.wav>          decode back to 16-bit PCM\n",
                self, self);
}

int main(int argc, char * argv[])
{
    int  targetRate = 22050;
    bool decode     = false;

    int arg = 1;
    for (; arg < argc && argv[arg][0] == '-'; ++arg)
    {
        if (std::strcmp(argv[arg], "-d") == 0)
        {
            decode = true;
        }
        else if (std::strcmp(argv[arg], "-r") == 0 && arg + 1 < argc)
        {
            targetRate = std::atoi(argv[++arg]);
        }
        else
        {
            PrintUsage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (argc - arg != 2 || targetRate < 4000 || targetRate > 48000)
    {
        PrintUsage(argv[0]);
        return EXIT_FAILURE;
    }

    return decode ? Decode(argv[arg], argv[arg + 1]) : Encode(argv[arg], argv[arg + 1], targetRate);
}
