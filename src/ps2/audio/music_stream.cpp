/* ================================================================================================
 * File: music_stream.cpp
 * Brief: Streamed music, SPU2 ADPCM or 16-bit PCM WAV. See music_stream.h.
 *
 *  The read buffers cycle Empty -> Pending -> Ready -> Empty, always in index order, which
 *  is what lets the request queue get away with no storage of its own: the main thread
 *  fills in a buffer's request, marks it Pending and signals m_requestSema; the reader
 *  thread serves buffers strictly in turn, marks each one Ready and signals m_doneSema.
 *  The state is written last on both sides, so whichever thread reads it sees the rest of
 *  the buffer already in place (one core, and the kernel calls around it keep the order).
 *
 *  Both formats stream the same way - a payload read in whole units, chunk groups or
 *  frames - and differ only in how a buffer turns into frames. ADPCM decoding walks the
 *  chunk interleave directly: block k of a buffer lives in chunk group k / blocksPerChunk,
 *  the right channel's copy of it one chunk further on. PCM is already frames.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/audio/music_stream.h"
#include "ps2/common.h"

#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <kernel.h>

namespace ps2::audio {
namespace {

using ps2::heap::MemTag;

constexpr int kBlockBytes = spu_adpcm::kBlockBytes;

// ADPCM reads: two chunk groups of a stereo file (8 KB), ~325 ms at 22050Hz.
constexpr int kAdpcmReadBytes = 8 * 1024;

// PCM reads aim at ~3/8 s each, like the ADPCM pair's ~650 ms in flight, within these.
// The cap is a 44.1kHz stereo CD rip: two 64 KB buffers, ~740 ms.
constexpr int kPcmMinReadBytes = 8 * 1024;
constexpr int kPcmMaxReadBytes = 64 * 1024;

// RIFF chunks walked looking for "data" before calling the file malformed.
constexpr int kMaxWavChunks = 32;

// Reader file position when it isn't known (after a failed read, or the header reads):
// forces a seek.
constexpr u32 kUnknownFilePos = ~0u;

u32 LoadLe16(const u8 * const p)
{
    return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8);
}

u32 LoadLe32(const u8 * const p)
{
    return LoadLe16(p) | (LoadLe16(p + 2) << 16);
}

bool HasId(const u8 * const p, const char * const id)
{
    return std::memcmp(p, id, 4) == 0;
}

int RoundUp(const int value, const int multiple)
{
    return ((value + multiple - 1) / multiple) * multiple;
}

} // namespace

alignas(16) u8 MusicStream::s_readerStack[MusicStream::kStackBytes];

// ------------------------------------------------------------------------------------------------
// Reader thread
// ------------------------------------------------------------------------------------------------

bool MusicStream::StartReaderThread()
{
    if (m_threadId >= 0)
    {
        return true;
    }

    // s_readerStack can only back one reader thread.
    static bool s_stackTaken = false;
    PS2_AssertMsg(!s_stackTaken, "MusicStream: only one instance can have a reader thread");
    s_stackTaken = true;

    // The reader runs at the main thread's priority: equal priority never preempts, so it
    // only ever takes the CPU the main thread gives up (see the header note).
    ee_thread_status_t mainStatus = {};
    if (ReferThreadStatus(GetThreadId(), &mainStatus) < 0)
    {
        Com_Printf("WARNING: MusicStream: can't read the main thread's priority.\n");
        return false;
    }
    m_priority = mainStatus.current_priority;

    ee_sema_t sema  = {};
    sema.init_count = 0;
    sema.max_count  = kNumBuffers;
    m_requestSema   = CreateSema(&sema);
    m_doneSema      = CreateSema(&sema);

    ee_thread_t thread      = {};
    thread.func             = reinterpret_cast<void *>(&MusicStream::ReaderEntry);
    thread.stack            = s_readerStack;
    thread.stack_size       = kStackBytes;
    thread.gp_reg           = &_gp;
    thread.initial_priority = m_priority;

    if (m_requestSema >= 0 && m_doneSema >= 0)
    {
        m_threadId = CreateThread(&thread);
    }

    if (m_threadId < 0 || StartThread(m_threadId, this) < 0)
    {
        Com_Printf("WARNING: MusicStream: can't start the reader thread.\n");
        if (m_threadId >= 0)
        {
            DeleteThread(m_threadId);
            m_threadId = -1;
        }
        if (m_requestSema >= 0)
        {
            DeleteSema(m_requestSema);
            m_requestSema = -1;
        }
        if (m_doneSema >= 0)
        {
            DeleteSema(m_doneSema);
            m_doneSema = -1;
        }
        return false;
    }
    return true;
}

void MusicStream::ReaderEntry(void * const self)
{
    static_cast<MusicStream *>(self)->ReaderLoop();
}

void MusicStream::ReaderLoop()
{
    // Never returns: the thread lives as long as the program, idle in WaitSema between
    // tracks, so opening one never pays for thread creation.
    for (;;)
    {
        WaitSema(m_requestSema);

        Buffer & buffer = m_buffers[m_readerIndex];
        m_readerIndex = (m_readerIndex + 1) % kNumBuffers;

        // Sequential reads skip the seek; only the first read, a loop wrap or a failure
        // needs one.
        const u32 filePos = m_payloadStart + buffer.payloadOffset;
        int result = -1;

        if (filePos == m_readerFilePos ||
            lseek(m_fd, static_cast<off_t>(filePos), SEEK_SET) == static_cast<off_t>(filePos))
        {
            result = static_cast<int>(read(m_fd, buffer.data, static_cast<size_t>(buffer.requestBytes)));
        }
        m_readerFilePos = (result > 0) ? (filePos + static_cast<u32>(result)) : kUnknownFilePos;

        buffer.resultBytes = result;
        buffer.state       = BufferState::Ready;
        SignalSema(m_doneSema);
    }
}

// ------------------------------------------------------------------------------------------------
// Requests (main thread)
// ------------------------------------------------------------------------------------------------

void MusicStream::RequestNext(const int bufferIndex)
{
    if (m_nextOffset >= m_payloadBytes)
    {
        // End of the payload: wrap for another pass, or leave the buffer Empty - the decode
        // side reaches the end of the music before it would ever look at it.
        if (m_requestLoopsLeft == 0)
        {
            return;
        }
        if (m_requestLoopsLeft > 0)
        {
            --m_requestLoopsLeft;
        }
        m_nextOffset = 0;
    }

    Buffer & buffer = m_buffers[bufferIndex];
    PS2_Assert(bufferIndex  == m_requestIndex);
    PS2_Assert(buffer.state == BufferState::Empty);

    const u32 remaining  = m_payloadBytes - m_nextOffset;
    buffer.payloadOffset = m_nextOffset;
    buffer.requestBytes  = (remaining < static_cast<u32>(m_readBytes)) ? static_cast<int>(remaining) : m_readBytes;
    buffer.resultBytes   = 0;

    m_nextOffset  += static_cast<u32>(buffer.requestBytes);
    m_requestIndex = (m_requestIndex + 1) % kNumBuffers;

    buffer.state = BufferState::Pending;
    SignalSema(m_requestSema);
}

void MusicStream::WaitForReads()
{
    // m_doneSema can carry a stale count from a read whose Ready state was already seen,
    // so each wake re-checks rather than trusting the count.
    for (Buffer & buffer : m_buffers)
    {
        while (buffer.state == BufferState::Pending)
        {
            WaitSema(m_doneSema);
        }
    }
    while (PollSema(m_doneSema) >= 0)
    {
    }
}

void MusicStream::YieldToReader() const
{
    // Hands the CPU to the reader if it is ready to run - with a request to start or a
    // completed read to hand back - and comes straight back once it sleeps again.
    RotateThreadReadyQueue(m_priority);
}

// ------------------------------------------------------------------------------------------------
// Headers (main thread, reader idle)
// ------------------------------------------------------------------------------------------------

int MusicStream::ReadScratch(const int fd, const u32 filePos, const int sizeBytes)
{
    PS2_Assert(sizeBytes <= kScratchBytes);

    if (lseek(fd, static_cast<off_t>(filePos), SEEK_SET) != static_cast<off_t>(filePos))
    {
        return -1;
    }
    return static_cast<int>(read(fd, m_scratch, static_cast<size_t>(sizeBytes)));
}

const char * MusicStream::ParseAdpcmHeader()
{
    music_file::Header header;
    std::memcpy(&header, m_scratch, sizeof(header));

    if (const char * const error = music_file::Validate(header))
    {
        return error;
    }
    if (header.chunkBytes > static_cast<u32>(kPcmMaxReadBytes) / header.channels)
    {
        return "chunk size too large";
    }

    m_format         = Format::Adpcm;
    m_channels       = header.channels;
    m_sampleRate     = static_cast<int>(header.sampleRate);
    m_frameCount     = static_cast<int>(header.frameCount);
    m_payloadStart   = static_cast<u32>(music_file::kHeaderBytes);
    m_payloadBytes   = static_cast<u32>(header.dataBytes);
    m_chunkBytes     = static_cast<int>(header.chunkBytes);
    m_groupBytes     = m_chunkBytes * m_channels;
    m_blocksPerChunk = m_chunkBytes / kBlockBytes;
    m_frameBytes     = 0;

    // Whole chunk groups, so a buffer never splits one: as many as fit in the 8 KB target.
    const int groups = (kAdpcmReadBytes / m_groupBytes > 0) ? (kAdpcmReadBytes / m_groupBytes) : 1;
    m_readBytes = groups * m_groupBytes;
    return nullptr;
}

const char * MusicStream::ParseWavHeader(const int fd)
{
    // Walk the RIFF chunks for "fmt " and "data", skipping whatever else a ripper wrote
    // (LIST, fact, cue, ...). One read per chunk, header plus the start of its body.
    u32  formatTag = 0, channels = 0, sampleRate = 0, frameBytes = 0, bits = 0;
    u32  dataStart = 0, dataBytes = 0;
    bool haveFormat = false, haveData = false;

    u32 pos = 12; // Past "RIFF", the file size and "WAVE".
    for (int i = 0; i < kMaxWavChunks && !haveData; ++i)
    {
        const int got = ReadScratch(fd, pos, kScratchBytes);
        if (got < 8)
        {
            break; // Out of chunks.
        }

        const u32 chunkBytes = LoadLe32(m_scratch + 4);
        if (HasId(m_scratch, "fmt "))
        {
            if (chunkBytes < 16 || got < 8 + 16)
            {
                return "bad fmt chunk";
            }
            const u8 * const fmt = m_scratch + 8;
            formatTag  = LoadLe16(fmt);
            channels   = LoadLe16(fmt + 2);
            sampleRate = LoadLe32(fmt + 4);
            frameBytes = LoadLe16(fmt + 12);
            bits       = LoadLe16(fmt + 14);

            // WAVE_FORMAT_EXTENSIBLE: the real format tag opens the subformat GUID.
            if (formatTag == 0xFFFEu && chunkBytes >= 26 && got >= 8 + 26)
            {
                formatTag = LoadLe16(fmt + 24);
            }
            haveFormat = true;
        }
        else if (HasId(m_scratch, "data"))
        {
            dataStart = pos + 8;
            dataBytes = chunkBytes;
            haveData  = true;
            break;
        }

        // Next chunk, word aligned. A size that runs past the end of the file just makes
        // the next read come up short.
        const u32 next = pos + 8 + chunkBytes + (chunkBytes & 1u);
        if (next <= pos)
        {
            break;
        }
        pos = next;
    }

    if (!haveData)
    {
        return "no data chunk";
    }
    if (!haveFormat)
    {
        return "no fmt chunk before the data";
    }
    if (formatTag != 1 || bits != 16)
    {
        return "not 16-bit PCM";
    }
    if (channels < 1 || channels > static_cast<u32>(music_file::kMaxChannels))
    {
        return "not mono or stereo";
    }
    if (frameBytes != channels * 2)
    {
        return "bad block alignment";
    }
    if (sampleRate < 4000 || sampleRate > 48000)
    {
        return "unsupported sample rate";
    }

    // Trust the file's size over the data chunk's, which a ripper that was cut short (or
    // that streamed it) can leave wrong.
    const off_t fileEnd = lseek(fd, 0, SEEK_END);
    if (fileEnd < static_cast<off_t>(dataStart))
    {
        return "truncated";
    }
    const u32 available = static_cast<u32>(fileEnd) - dataStart;
    if (dataBytes > available)
    {
        dataBytes = available;
    }
    dataBytes -= dataBytes % frameBytes;
    if (dataBytes == 0)
    {
        return "no samples";
    }

    m_format         = Format::Pcm16;
    m_channels       = static_cast<int>(channels);
    m_sampleRate     = static_cast<int>(sampleRate);
    m_frameBytes     = static_cast<int>(frameBytes);
    m_frameCount     = static_cast<int>(dataBytes / frameBytes);
    m_payloadStart   = dataStart;
    m_payloadBytes   = dataBytes;
    m_chunkBytes     = 0;
    m_groupBytes     = 0;
    m_blocksPerChunk = 0;

    // ~3/8 s per buffer, in whole 4 KB pages - which keeps every read a whole number of
    // frames and of cache lines.
    const int perBuffer = RoundUp((m_sampleRate * m_frameBytes * 3) / 8, 4096);
    m_readBytes = (perBuffer < kPcmMinReadBytes) ? kPcmMinReadBytes : ((perBuffer > kPcmMaxReadBytes) ? kPcmMaxReadBytes : perBuffer);
    return nullptr;
}

// ------------------------------------------------------------------------------------------------
// Buffers
// ------------------------------------------------------------------------------------------------

bool MusicStream::AllocateBuffers(const char * const path)
{
    // One block for both, each buffer a whole number of cache lines, so the second starts
    // line aligned too. TryAlloc: a track that doesn't fit is a silent track, not a halt.
    const int    stride = RoundUp(m_readBytes, 64);
    const size_t bytes  = static_cast<size_t>(stride) * kNumBuffers;

    void * const memory = ps2::heap::TryAllocAligned(ps2::heap::MemAlign(64), bytes, MemTag::Music);
    if (memory == nullptr)
    {
        Com_Printf("WARNING: %s: no memory for its %d KB of stream buffers, the track stays silent.\n",
                   path, static_cast<int>(bytes / 1024u));
        return false;
    }

    m_bufferMemory      = static_cast<u8 *>(memory);
    m_bufferMemoryBytes = bytes;
    for (int i = 0; i < kNumBuffers; ++i)
    {
        m_buffers[i].data = m_bufferMemory + (i * stride);
    }
    return true;
}

void MusicStream::FreeBuffers()
{
    ps2::heap::Free(m_bufferMemory, m_bufferMemoryBytes, MemTag::Music);
    m_bufferMemory      = nullptr;
    m_bufferMemoryBytes = 0;

    for (Buffer & buffer : m_buffers)
    {
        buffer.data = nullptr;
    }
}

// ------------------------------------------------------------------------------------------------
// Open / Close
// ------------------------------------------------------------------------------------------------

MusicStream::OpenResult MusicStream::Open(const char * const path, const int extraLoops)
{
    Close();

    if (!StartReaderThread())
    {
        return OpenResult::Unusable;
    }

    const int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        return OpenResult::Missing; // The caller tries every candidate, so this is quiet.
    }

    // The format comes from the header, whatever the extension says.
    const int    got   = ReadScratch(fd, 0, kScratchBytes);
    const char * error = nullptr;

    if (got >= 4 && std::memcmp(m_scratch, music_file::kMagic, 4) == 0)
    {
        error = (got >= static_cast<int>(sizeof(music_file::Header))) ? ParseAdpcmHeader() : "truncated header";
    }
    else if (got >= 12 && HasId(m_scratch, "RIFF") && HasId(m_scratch + 8, "WAVE"))
    {
        error = ParseWavHeader(fd);
    }
    else
    {
        error = "not a music file (neither Q2MU ADPCM nor RIFF WAVE)";
    }

    if (error != nullptr)
    {
        Com_Printf("WARNING: %s: %s\n", path, error);
        close(fd);
        return OpenResult::Unusable;
    }

    if (!AllocateBuffers(path))
    {
        close(fd);
        return OpenResult::Unusable;
    }

    m_fd = fd;

    // The reader is idle (Close() waited it out), so its side can be reset from here. The
    // header reads left the file position wherever they did, so its first read seeks.
    m_readerIndex   = 0;
    m_readerFilePos = kUnknownFilePos;

    m_nextOffset       = 0;
    m_requestLoopsLeft = extraLoops;
    m_requestIndex     = 0;

    m_decodeIndex     = 0;
    m_haveBuffer      = false;
    m_unitPos         = 0;
    m_unitCount       = 0;
    m_framesDecoded   = 0;
    m_decodeLoopsLeft = extraLoops;
    m_wraps           = 0;
    m_finished        = false;
    m_failed          = false;
    m_blockFrames     = 0;
    m_blockPos        = 0;

    for (spu_adpcm::History & history : m_history)
    {
        history = {};
    }

    for (int i = 0; i < kNumBuffers; ++i)
    {
        RequestNext(i);
    }

    // Sleeping here is what lets the reader run: it issues the first read, sleeps on it,
    // wakes when it lands and signals us, then carries straight on with the second.
    while (m_buffers[0].state == BufferState::Pending)
    {
        WaitSema(m_doneSema);
    }
    return OpenResult::Opened;
}

void MusicStream::Close()
{
    if (m_fd < 0)
    {
        return;
    }

    // The reader may be inside read() on this fd - and into these buffers - right now.
    WaitForReads();

    close(m_fd);
    m_fd = -1;

    for (Buffer & buffer : m_buffers)
    {
        buffer.state = BufferState::Empty;
    }
    FreeBuffers();

    m_haveBuffer = false;
    m_finished   = false;
    m_failed     = false;
}

const char * MusicStream::FormatName() const
{
    return (m_format == Format::Adpcm) ? "ADPCM" : "PCM";
}

// ------------------------------------------------------------------------------------------------
// Decoding (main thread)
// ------------------------------------------------------------------------------------------------

bool MusicStream::AcquireBuffer()
{
    const Buffer & buffer = m_buffers[m_decodeIndex];

    if (buffer.state == BufferState::Pending)
    {
        return false; // Not landed yet; try again next frame.
    }

    if (buffer.state != BufferState::Ready || buffer.resultBytes != buffer.requestBytes)
    {
        Com_Printf("WARNING: music stream read failed (%d of %d bytes).\n",
                   static_cast<int>(buffer.resultBytes), buffer.requestBytes);
        m_finished = true;
        m_failed   = true;
        return false;
    }

    m_haveBuffer = true;
    m_unitPos    = 0;
    m_unitCount  = (m_format == Format::Adpcm) ? ((buffer.requestBytes / m_groupBytes) * m_blocksPerChunk)
                                               : (buffer.requestBytes / m_frameBytes);
    return true;
}

void MusicStream::ReleaseBuffer()
{
    const int released = m_decodeIndex;

    m_buffers[released].state = BufferState::Empty;
    m_haveBuffer  = false;
    m_decodeIndex = (m_decodeIndex + 1) % kNumBuffers;

    // Refill it straight away, and get the read going now rather than at the main thread's
    // next sleep.
    RequestNext(released);
    YieldToReader();
}

bool MusicStream::BufferReady()
{
    if (m_fd < 0 || m_finished)
    {
        return false;
    }

    if (m_framesDecoded >= m_frameCount)
    {
        if (m_decodeLoopsLeft == 0)
        {
            m_finished = true;
            return false;
        }
        if (m_decodeLoopsLeft > 0)
        {
            --m_decodeLoopsLeft;
        }

        // Wrap. The reads wrapped ahead of us, so the next buffer starts the payload over;
        // whatever is left of the current one is ADPCM tail padding (a PCM pass always ends
        // with its buffer). The ADPCM history restarts too, exactly as the encoder started it.
        if (m_haveBuffer)
        {
            ReleaseBuffer();
        }
        for (spu_adpcm::History & history : m_history)
        {
            history = {};
        }
        m_framesDecoded = 0;
        ++m_wraps;
    }

    return m_haveBuffer || AcquireBuffer();
}

bool MusicStream::DecodeNextBlock()
{
    if (!BufferReady())
    {
        return false;
    }

    const int group   = m_unitPos / m_blocksPerChunk;
    const int inChunk = m_unitPos % m_blocksPerChunk;
    const u8 * const block = m_buffers[m_decodeIndex].data + (group * m_groupBytes) + (inChunk * kBlockBytes);

    spu_adpcm::DecodeBlock(block, m_block, 2, m_history[0]);
    if (m_channels > 1)
    {
        spu_adpcm::DecodeBlock(block + m_chunkBytes, m_block + 1, 2, m_history[1]);
    }
    else
    {
        for (int i = 0; i < spu_adpcm::kBlockSamples; ++i)
        {
            m_block[(i * 2) + 1] = m_block[i * 2];
        }
    }

    // The last block of a pass is only partly music.
    const int framesLeft = m_frameCount - m_framesDecoded;
    m_blockFrames    = (framesLeft < spu_adpcm::kBlockSamples) ? framesLeft : spu_adpcm::kBlockSamples;
    m_blockPos       = 0;
    m_framesDecoded += m_blockFrames;

    if (++m_unitPos == m_unitCount)
    {
        ReleaseBuffer();
    }
    return true;
}

int MusicStream::DecodeAdpcm(s16 * const outStereo, const int maxFrames)
{
    int produced = 0;

    while (produced < maxFrames)
    {
        if (m_blockPos == m_blockFrames && !DecodeNextBlock())
        {
            break;
        }

        int count = m_blockFrames - m_blockPos;
        if (count > maxFrames - produced)
        {
            count = maxFrames - produced;
        }

        std::memcpy(outStereo + (produced * 2), m_block + (m_blockPos * 2),
                    static_cast<size_t>(count) * 2 * sizeof(s16));
        m_blockPos += count;
        produced   += count;
    }
    return produced;
}

int MusicStream::DecodePcm(s16 * const outStereo, const int maxFrames)
{
    int produced = 0;

    while (produced < maxFrames && BufferReady())
    {
        int count = m_unitCount - m_unitPos;
        if (count > maxFrames - produced)
        {
            count = maxFrames - produced;
        }
        if (count > m_frameCount - m_framesDecoded)
        {
            count = m_frameCount - m_framesDecoded;
        }

        // Little-endian on disk and on the EE: the frames copy straight out.
        const u8 * const src = m_buffers[m_decodeIndex].data + (m_unitPos * m_frameBytes);
        s16 * const      dst = outStereo + (produced * 2);
        if (m_channels > 1)
        {
            std::memcpy(dst, src, static_cast<size_t>(count) * 2 * sizeof(s16));
        }
        else
        {
            // The buffer is cache-line aligned and a mono frame two bytes, so this is aligned.
            const s16 * const mono = static_cast<const s16 *>(static_cast<const void *>(src));
            for (int i = 0; i < count; ++i)
            {
                dst[i * 2]       = mono[i];
                dst[(i * 2) + 1] = mono[i];
            }
        }

        m_unitPos       += count;
        m_framesDecoded += count;
        produced        += count;

        if (m_unitPos == m_unitCount)
        {
            ReleaseBuffer();
        }
    }
    return produced;
}

int MusicStream::Decode(s16 * const outStereo, const int maxFrames)
{
    const int produced = (m_format == Format::Adpcm) ? DecodeAdpcm(outStereo, maxFrames)
                                                     : DecodePcm(outStereo, maxFrames);

    // A read may have landed since the reader last ran: let it hand the buffer back now,
    // so the next frame finds it Ready.
    for (const Buffer & buffer : m_buffers)
    {
        if (buffer.state == BufferState::Pending)
        {
            YieldToReader();
            break;
        }
    }
    return produced;
}

int MusicStream::TakeWraps()
{
    const int wraps = m_wraps;
    m_wraps = 0;
    return wraps;
}

} // namespace ps2::audio
