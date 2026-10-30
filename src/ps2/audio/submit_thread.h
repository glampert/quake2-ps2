#pragma once
/* ================================================================================================
 * File: submit_thread.h
 * Brief: The audio submit off the main thread. SNDDMA_Submit's two audsrv RPCs (FreeBytes and
 *        Enqueue) are synchronous, and on the HDD they wait on the IOP while fileXio's thread
 *        holds its CPU for a read: 4-25 ms on a console, behind the background loader's chunks
 *        and the music's 32 KB requests. SubmitThread runs MixRing::Send for the main thread
 *        instead, so the main thread posts a frame's audio and carries on; if the send before
 *        is still out on the IOP, it posts nothing that frame, and the next one sends both.
 *
 *        The thread runs one priority above the main thread, so the IOP's reply readies it at
 *        once: the play RPC follows the free-space reply as soon as it lands, as it did when the
 *        main thread made both calls, and the ring is fed on the same schedule. It holds the CPU
 *        only between two RPCs (a copy of under 10 KB into the SDK's send buffer).
 *
 *        Why a thread and not a SIF_RPC_M_NOWAIT call (plan §12.4's first idea): audsrv's IOP
 *        server has one receive buffer, and a request's data is DMAed into it when the request
 *        is sent, not when the server gets to it. Two requests out at once, the free-space query
 *        and the play, would let the second overwrite the first; the SDK's own client keeps
 *        them one at a time with a semaphore, and the thread keeps using that client.
 *
 *        While a send is out (Busy), the thread owns the device: the main thread may not call
 *        AudsrvDevice (Hold, Release, Shutdown, ...) until Wait. The paint buffer is shared:
 *        the thread reads the range it was given, which the mixer has finished painting.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/audio/mix_ring.h"

namespace ps2::audio {

class SubmitThread final
{
public:
    // Starts the thread on the first call; true once it runs. False if it couldn't start (one
    // console line, once): the caller then submits on its own thread, as before.
    bool Start(AudsrvDevice & device);

    // A send posted and not finished yet: the thread is in the device, or waiting to be.
    bool Busy() const { return m_state == State::Posted; }

    // Hands the thread one MixRing::Send over [startFrames, startFrames + pendingFrames) of
    // buffer. Only when started and not Busy.
    void Post(const u8 * buffer, int startFrames, int pendingFrames);

    // The finished send's result, once: true and result filled when a send finished since the
    // last call, false otherwise.
    bool Collect(MixRing::SendResult & result);

    // Blocks until no send is out, before the main thread calls the device itself. Returns at
    // once when idle; the longest wait is an IOP read's.
    void Wait();

private:
    enum class State : u8 { Idle, Posted, Done };

    static void ThreadMain(void * arg);

    static constexpr int kStackBytes = 8 * 1024;

    AudsrvDevice *      m_device      = nullptr;
    s32                 m_threadId    = -1;
    s32                 m_workSema    = -1; // one signal per Post
    s32                 m_doneSema    = -1; // one signal per finished send; Wait re-checks
    bool                m_startFailed = false;
    volatile State      m_state       = State::Idle; // written last by each side

    // The posted send, written by the main thread before m_state goes Posted.
    const u8 *          m_buffer      = nullptr;
    int                 m_startFrames = 0;
    int                 m_frames      = 0;

    // Its result, written by the thread before m_state goes Done.
    MixRing::SendResult m_result;

    alignas(16) u8      m_stack[kStackBytes] = {};
};

} // namespace ps2::audio
