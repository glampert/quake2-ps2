/* ================================================================================================
 * File: music_stream.cpp
 * Brief: Streamed SPU2 ADPCM music. See music_stream.h.
 *
 *  The read buffers cycle Empty -> Pending -> Ready -> Empty, always in index order, which
 *  is what lets the request queue get away with no storage of its own: the main thread
 *  fills in a buffer's request, marks it Pending and signals m_requestSema; the reader
 *  thread serves buffers strictly in turn, marks each one Ready and signals m_doneSema.
 *  The state is written last on both sides, so whichever thread reads it sees the rest of
 *  the buffer already in place (one core, and the kernel calls around it keep the order).
 *
 *  Decoding walks the chunk interleave directly: block k of a buffer lives in chunk group
 *  k / blocksPerChunk, the right channel's copy of it one chunk further on.
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

constexpr int kHeaderBytes = music_file::kHeaderBytes;
constexpr int kBlockBytes  = spu_adpcm::kBlockBytes;

// Reader file position when it isn't known (after a failed read): forces a seek.
constexpr u32 kUnknownFilePos = ~0u;

} // namespace

// ------------------------------------------------------------------------------------------------
// Reader thread
// ------------------------------------------------------------------------------------------------

bool MusicStream::StartReaderThread()
{
    if (m_threadId >= 0)
    {
        return true;
    }

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
    thread.stack            = m_stack;
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

        // Sequential reads skip the seek; only a loop wrap (or a failure) needs one.
        const u32 filePos = static_cast<u32>(kHeaderBytes) + buffer.payloadOffset;
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
    buffer.requestBytes  = (remaining < static_cast<u32>(kReadBytes)) ? static_cast<int>(remaining) : kReadBytes;
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
// Open / Close
// ------------------------------------------------------------------------------------------------

bool MusicStream::Open(const char * const path, const int extraLoops)
{
    Close();

    if (!StartReaderThread())
    {
        return false;
    }

    const int fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        return false; // Not there. The caller tries every search path, so this is quiet.
    }

    // The header is read through the first buffer, which the payload reads reuse after.
    u8 * const scratch = m_buffers[0].data;
    const int  got     = static_cast<int>(read(fd, scratch, kHeaderBytes));

    music_file::Header header;
    std::memcpy(&header, scratch, sizeof(header));

    const char * error = (got == kHeaderBytes) ? music_file::Validate(header) : "truncated header";
    if (error == nullptr && (kReadBytes % static_cast<int>(header.chunkBytes * header.channels)) != 0)
    {
        error = "chunk size doesn't divide the read size";
    }
    if (error != nullptr)
    {
        Com_Printf("WARNING: %s: %s\n", path, error);
        close(fd);
        return false;
    }

    m_fd             = fd;
    m_channels       = header.channels;
    m_sampleRate     = static_cast<int>(header.sampleRate);
    m_frameCount     = static_cast<int>(header.frameCount);
    m_chunkBytes     = static_cast<int>(header.chunkBytes);
    m_groupBytes     = m_chunkBytes * m_channels;
    m_blocksPerChunk = m_chunkBytes / kBlockBytes;
    m_payloadBytes   = static_cast<u32>(header.dataBytes);

    // The reader is idle (Close() waited it out), so its side can be reset from here.
    m_readerIndex   = 0;
    m_readerFilePos = static_cast<u32>(kHeaderBytes);

    m_nextOffset       = 0;
    m_requestLoopsLeft = extraLoops;
    m_requestIndex     = 0;

    m_decodeIndex     = 0;
    m_haveBuffer      = false;
    m_blockInBuffer   = 0;
    m_blocksInBuffer  = 0;
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
    return true;
}

void MusicStream::Close()
{
    if (m_fd < 0)
    {
        return;
    }

    // The reader may be inside read() on this fd right now.
    WaitForReads();

    close(m_fd);
    m_fd = -1;

    for (Buffer & buffer : m_buffers)
    {
        buffer.state = BufferState::Empty;
    }
    m_haveBuffer = false;
    m_finished   = false;
    m_failed     = false;
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

    m_haveBuffer     = true;
    m_blockInBuffer  = 0;
    m_blocksInBuffer = (buffer.requestBytes / m_groupBytes) * m_blocksPerChunk;
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

bool MusicStream::DecodeNextBlock()
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

        // Wrap. The rest of the current buffer is tail padding, and the reads wrapped
        // ahead of us, so the next buffer starts the payload over. The history restarts
        // too, exactly as the encoder started it.
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

    if (!m_haveBuffer && !AcquireBuffer())
    {
        return false;
    }

    const int group   = m_blockInBuffer / m_blocksPerChunk;
    const int inChunk = m_blockInBuffer % m_blocksPerChunk;
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

    if (++m_blockInBuffer == m_blocksInBuffer)
    {
        ReleaseBuffer();
    }
    return true;
}

int MusicStream::Decode(s16 * const outStereo, const int maxFrames)
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
