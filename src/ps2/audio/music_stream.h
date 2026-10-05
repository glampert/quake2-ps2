#pragma once
/* ================================================================================================
 * File: music_stream.h
 * Brief: Streams one music file off disk and decodes it to 16-bit stereo PCM on demand,
 *        never holding more of it in memory than two read buffers. Two formats:
 *
 *         - baseq2/music/trackNN.adp, SPU2 ADPCM (layout in spu_adpcm.h), what `make music`
 *           produces: two 8 KB buffers hold ~650 ms at 22050Hz.
 *         - baseq2/music/trackNN.wav, 16-bit PCM, mono or stereo, the fallback for when
 *           the .adp hasn't been made. 3.5x the bytes per second (7x for a 44.1kHz CD rip),
 *           so its buffers grow with the byte rate - two of up to 64 KB, ~740 ms of a CD rip.
 *
 *        The buffers come from the heap when a track opens and go back when it closes, so
 *        music never holds memory across a level load (the loading plaque stops it). A
 *        track that can't get them stays silent rather than halting the game.
 *
 *        It knows nothing about Quake: cd_audio.cpp drives the single instance, picks the
 *        file, and feeds what it decodes - at the file's own sample rate - to the mixer.
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

#include <cstddef>
#include <tamtypes.h>
#include "ps2/audio/spu_adpcm.h"

namespace ps2::audio {

class MusicStream final
{
public:
    // Open() loop count: wrap back to the start forever.
    static constexpr int kLoopForever = -1;

    enum class OpenResult : int
    {
        Opened,   // Playing from the next Decode().
        Missing,  // No such file. Quiet, so the caller can try the next candidate.
        Unusable, // The file is there but can't be played - malformed, or no memory for its
                  // buffers. Already reported on the console.
    };

    // Opens `path` and primes the stream. The format comes from the file's own header
    // ("Q2MU" or RIFF/WAVE), not its extension. Blocking: the open, the header reads and the
    // first payload read all complete before it returns (Play normally lands inside a level
    // load, and waiting for the first buffer lets the music start on the very next frame).
    // `extraLoops` is how many times to wrap back to the start after the first pass - 0 plays
    // it once, kLoopForever never ends. A wrap is seamless: the reads run ahead across it.
    OpenResult Open(const char * path, int extraLoops);

    // Closes the file, if open, and frees its buffers. Blocks for at most the one read in
    // flight.
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
    int Channels()   const { return m_channels;   }
    int FrameCount() const { return m_frameCount; }

    // "ADPCM" or "PCM", for the console.
    const char * FormatName() const;

    // Frames decoded so far in the current pass.
    int PositionFrames() const { return m_framesDecoded; }

    // Seamless wraps back to the start since the last call.
    int TakeWraps();

private:
    static constexpr int kNumBuffers = 2;
    static constexpr int kStackBytes = 8 * 1024;

    // Header reads go through this: the RIFF chunk walk and the "Q2MU" header both fit.
    static constexpr int kScratchBytes = 64;

    enum class Format : int
    {
        Adpcm,
        Pcm16,
    };

    enum class BufferState : int
    {
        Empty,   // Free. Owned by the main thread.
        Pending, // Queued or being read. Owned by the reader thread.
        Ready,   // Holds data. Owned by the main thread.
    };

    struct Buffer
    {
        // Points into m_bufferMemory. The file read DMAs straight into it over SIF, so it is
        // cache-line aligned, and the buffers sit a whole number of lines apart: the DMAC
        // drops an unaligned destination's low address bits, and a line shared with a
        // neighbour could be written back over the transferred bytes.
        u8 * data = nullptr;

        u32 payloadOffset = 0; // Where data[0] sits in the payload.
        int requestBytes  = 0;

        // Written by the reader thread, read by the main thread.
        volatile int         resultBytes = 0;
        volatile BufferState state       = BufferState::Empty;
    };

    bool StartReaderThread();
    static void ReaderEntry(void * self);
    void ReaderLoop();

    int ReadScratch(int fd, u32 filePos, int sizeBytes);
    const char * ParseAdpcmHeader();
    const char * ParseWavHeader(int fd);
    bool AllocateBuffers(const char * path);
    void FreeBuffers();

    void RequestNext(int bufferIndex);
    void WaitForReads();
    void YieldToReader() const;

    bool BufferReady();
    bool AcquireBuffer();
    void ReleaseBuffer();

    int DecodeAdpcm(s16 * outStereo, int maxFrames);
    int DecodePcm(s16 * outStereo, int maxFrames);
    bool DecodeNextBlock();

    // --- File -----------------------------------------------------------------------------
    int    m_fd           = -1;
    Format m_format       = Format::Adpcm;
    int    m_channels     = 0;
    int    m_sampleRate   = 0;
    int    m_frameCount   = 0;
    u32    m_payloadStart = 0; // File offset of the first payload byte.
    u32    m_payloadBytes = 0; // Whole chunk groups (ADPCM) or whole frames (PCM).

    // ADPCM layout.
    int m_chunkBytes     = 0;
    int m_groupBytes     = 0; // One chunk of every channel.
    int m_blocksPerChunk = 0;

    // PCM layout.
    int m_frameBytes = 0; // One sample of every channel.

    // --- Buffers --------------------------------------------------------------------------
    u8 *   m_bufferMemory      = nullptr; // One heap block, kNumBuffers strides long.
    size_t m_bufferMemoryBytes = 0;
    int    m_readBytes         = 0;       // Full read size: whole chunk groups or frames.

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
    int  m_unitPos         = 0;     // Next ADPCM block (per channel) or PCM frame in it.
    int  m_unitCount       = 0;     // Blocks or frames it holds.
    int  m_framesDecoded   = 0;     // Into the current pass.
    int  m_decodeLoopsLeft = 0;
    int  m_wraps           = 0;
    bool m_finished        = false;
    bool m_failed          = false;

    spu_adpcm::History m_history[music_file::kMaxChannels];

    // The current ADPCM block, decoded, as interleaved stereo frames.
    s16 m_block[spu_adpcm::kBlockSamples * 2] = {};
    int m_blockFrames = 0;
    int m_blockPos    = 0;

    // --- Reader thread --------------------------------------------------------------------
    int m_threadId    = -1;
    int m_priority    = 0;
    int m_requestSema = -1; // Counts queued requests.
    int m_doneSema    = -1; // Signaled after every completed read.

    alignas(64) u8 m_scratch[kScratchBytes] = {};

    // The reader thread's stack. Static, so it lives in .bss rather than as 8 KB of zeros in
    // the ELF's .data with the rest of the (constant-initialized) object - which works because
    // there is one MusicStream in the program, cd_audio.cpp's (StartReaderThread checks).
    alignas(16) static u8 s_readerStack[kStackBytes];

    Buffer m_buffers[kNumBuffers];
};

} // namespace ps2::audio
