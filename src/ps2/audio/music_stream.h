#pragma once
/* ================================================================================================
 * File: music_stream.h
 * Brief: Streams one music file (baseq2/music/trackNN.adp, layout in spu_adpcm.h) off disk
 *        and decodes it to 16-bit stereo PCM on demand, never holding more of it in memory
 *        than two 8 KB read buffers - ~650 ms of audio at 22050Hz. It knows nothing about
 *        Quake: cd_audio.cpp drives the single instance and feeds what it decodes to the
 *        engine's mixer.
 *
 *        The reads are what make this more than a decoder. Every file read is a SIF RPC to
 *        the IOP that puts its caller to sleep until the data lands, and on a USB stick one
 *        can take tens of milliseconds - a dropped frame if the main loop issued it. So they
 *        come from a reader thread instead, at the main thread's own priority: it never
 *        preempts frame work, it just gets the CPU whenever the main thread sleeps, which
 *        happens several times a frame (the audsrv RPCs in SNDDMA_Submit, any file I/O), and
 *        whenever MusicStream yields to it explicitly after queueing a read. Both SDK file
 *        clients (fio for host:, fileXio for mass:) serialize their RPCs with semaphores, so
 *        the reader can share them with the main thread's own file I/O safely.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <tamtypes.h>
#include "ps2/audio/spu_adpcm.h"

namespace ps2::audio {

class MusicStream final
{
public:
    // Open() loop count: wrap back to the start forever.
    static constexpr int kLoopForever = -1;

    // Opens `path` and primes the stream. Blocking: the open, the header read and the first
    // payload read all complete before it returns (Play normally lands inside a level load,
    // and waiting for the first buffer lets the music start on the very next frame).
    // `extraLoops` is how many times to wrap back to the start after the first pass - 0 plays
    // it once, kLoopForever never ends. A wrap is seamless: the reads run ahead across it.
    // Returns false, and logs why, if the file is missing or malformed.
    bool Open(const char * path, int extraLoops);

    // Closes the file, if open. Blocks for at most the one read in flight.
    void Close();

    bool IsOpen() const { return m_fd >= 0; }

    // Decodes up to maxFrames frames into outStereo as interleaved L/R pairs (a mono file
    // is copied to both sides). Returns fewer when the next read hasn't landed yet - just
    // call again next frame - or when the stream has ended, see Finished().
    int Decode(s16 * outStereo, int maxFrames);

    // True once the last frame of the last pass has been decoded, or a read failed.
    bool Finished() const { return m_finished; }

    // True if it finished because a read failed rather than because the music ran out.
    bool Failed() const { return m_failed; }

    int SampleRate() const { return m_sampleRate; }
    int FrameCount() const { return m_frameCount; }

    // Frames decoded so far in the current pass.
    int PositionFrames() const { return m_framesDecoded; }

    // Seamless wraps back to the start since the last call.
    int TakeWraps();

private:
    // One read: two chunk groups of a stereo file, ~325 ms at 22050Hz. A multiple of every
    // chunk group the stream accepts (Open() checks), so a buffer never splits a chunk.
    static constexpr int kReadBytes   = 8192;
    static constexpr int kNumBuffers  = 2;
    static constexpr int kStackBytes  = 8 * 1024;

    enum class BufferState : int
    {
        Empty,   // Free. Owned by the main thread.
        Pending, // Queued or being read. Owned by the reader thread.
        Ready,   // Holds data. Owned by the main thread.
    };

    struct Buffer
    {
        // The file read DMAs straight into this over SIF, so it must be cache-line aligned
        // and sized: the DMAC drops an unaligned destination's low address bits, and a line
        // shared with a neighbour could be written back over the transferred bytes.
        alignas(64) u8 data[kReadBytes];

        u32 payloadOffset = 0; // Where data[0] sits in the ADPCM payload.
        int requestBytes  = 0;

        // Written by the reader thread, read by the main thread.
        volatile int         resultBytes = 0;
        volatile BufferState state       = BufferState::Empty;
    };

    bool StartReaderThread();
    static void ReaderEntry(void * self);
    void ReaderLoop();

    void RequestNext(int bufferIndex);
    void WaitForReads();
    void YieldToReader() const;

    bool DecodeNextBlock();
    bool AcquireBuffer();
    void ReleaseBuffer();

    // --- File -----------------------------------------------------------------------------
    int m_fd             = -1;
    int m_channels       = 0;
    int m_sampleRate     = 0;
    int m_frameCount     = 0;
    int m_chunkBytes     = 0;
    int m_groupBytes     = 0; // One chunk of every channel.
    int m_blocksPerChunk = 0;
    u32 m_payloadBytes   = 0;

    // --- Request side (main thread) -------------------------------------------------------
    u32 m_nextOffset       = 0; // Next payload offset to read.
    int m_requestLoopsLeft = 0; // Wraps the reads may still take.
    int m_requestIndex     = 0; // Buffer the next request goes into.

    // --- Reader side (reader thread; reset by the main thread only while it is idle) ------
    int m_readerIndex   = 0;
    u32 m_readerFilePos = 0;

    // --- Decode side (main thread) --------------------------------------------------------
    int  m_decodeIndex     = 0;     // Buffer being decoded, once m_haveBuffer.
    bool m_haveBuffer      = false;
    int  m_blockInBuffer   = 0;     // Next block (per channel) within it.
    int  m_blocksInBuffer  = 0;
    int  m_framesDecoded   = 0;     // Into the current pass.
    int  m_decodeLoopsLeft = 0;
    int  m_wraps           = 0;
    bool m_finished        = false;
    bool m_failed          = false;

    spu_adpcm::History m_history[music_file::kMaxChannels];

    // The current block, decoded, as interleaved stereo frames.
    s16 m_block[spu_adpcm::kBlockSamples * 2] = {};
    int m_blockFrames = 0;
    int m_blockPos    = 0;

    // --- Reader thread --------------------------------------------------------------------
    int m_threadId    = -1;
    int m_priority    = 0;
    int m_requestSema = -1; // Counts queued requests.
    int m_doneSema    = -1; // Signaled after every completed read.

    alignas(16) u8 m_stack[kStackBytes] = {};

    Buffer m_buffers[kNumBuffers];
};

} // namespace ps2::audio
