/* ================================================================================================
 * File: submit_thread.cpp
 * Brief: The audio submit off the main thread. See submit_thread.h.
 *
 *  The handoff is the background loader's (fs/async_io.cpp): each side writes the fields it owns
 *  and then m_state last, behind a compiler barrier, and reads m_state first. The two threads
 *  share one in-order CPU and the kernel switches between them, never runs them at once, so
 *  nothing here locks. The main thread owns the job while m_state is Idle or Done, the thread
 *  while it is Posted.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/audio/submit_thread.h"
#include "ps2/common.h"

#include <kernel.h>

namespace ps2::audio {
namespace {

Q_ALWAYS_INLINE void CompilerBarrier()
{
    asm volatile("" ::: "memory");
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Submit thread
// ------------------------------------------------------------------------------------------------

void SubmitThread::ThreadMain(void * const arg)
{
    SubmitThread * const self = static_cast<SubmitThread *>(arg);
    for (;;)
    {
        WaitSema(self->m_workSema);
        if (self->m_state != State::Posted)
        {
            continue;
        }
        CompilerBarrier(); // the job was written before the state

        self->m_result = MixRing::Send(*self->m_device, self->m_buffer, self->m_startFrames, self->m_frames);

        CompilerBarrier();
        self->m_state = State::Done;
        SignalSema(self->m_doneSema);
    }
}

// ------------------------------------------------------------------------------------------------
// Main-thread side
// ------------------------------------------------------------------------------------------------

bool SubmitThread::Start(AudsrvDevice & device)
{
    if (m_threadId >= 0)
    {
        return true;
    }
    if (m_startFailed)
    {
        return false;
    }
    m_startFailed = true; // until everything below succeeded
    m_device      = &device;

    // One priority above the main thread (a lower number), so an RPC's reply readies it at once
    // and the play RPC follows the free-space one as it did on the main thread. The main thread
    // runs at 1 (ps2sdk's InitThread); 0 is the highest there is, which the SDK's own kernel
    // helper thread shares, both of them running only in short bursts.
    ee_thread_status_t mainStatus = {};
    if (ReferThreadStatus(GetThreadId(), &mainStatus) < 0) [[unlikely]]
    {
        Com_Printf("WARNING: sound: can't read the main thread's priority; submitting on it.\n");
        return false;
    }
    const int priority = (mainStatus.current_priority > 0) ? (mainStatus.current_priority - 1) : 0;

    ee_sema_t sema  = {};
    sema.init_count = 0;
    sema.max_count  = 255;
    m_workSema      = CreateSema(&sema);
    m_doneSema      = CreateSema(&sema);

    ee_thread_t thread      = {};
    thread.func             = reinterpret_cast<void *>(&ThreadMain);
    thread.stack            = m_stack;
    thread.stack_size       = kStackBytes;
    thread.gp_reg           = &_gp;
    thread.initial_priority = priority;

    if (m_workSema >= 0 && m_doneSema >= 0)
    {
        m_threadId = CreateThread(&thread);
    }

    if (m_threadId < 0 || StartThread(m_threadId, this) < 0) [[unlikely]]
    {
        Com_Printf("WARNING: sound: can't start the submit thread; submitting on the main thread.\n");
        if (m_threadId >= 0)
        {
            DeleteThread(m_threadId);
            m_threadId = -1;
        }
        if (m_workSema >= 0)
        {
            DeleteSema(m_workSema);
            m_workSema = -1;
        }
        if (m_doneSema >= 0)
        {
            DeleteSema(m_doneSema);
            m_doneSema = -1;
        }
        return false;
    }

    m_startFailed = false;
    Com_DPrintf("sound: submit thread started at priority %d.\n", priority);
    return true;
}

void SubmitThread::Post(const u8 * const buffer, const int startFrames, const int pendingFrames)
{
    PS2_AssertMsg(m_threadId >= 0, "SubmitThread::Post before Start!");
    PS2_AssertMsg(m_state == State::Idle, "SubmitThread::Post with a send out or uncollected!");

    m_buffer      = buffer;
    m_startFrames = startFrames;
    m_frames      = pendingFrames;

    CompilerBarrier();
    m_state = State::Posted;
    SignalSema(m_workSema); // the thread is higher priority: it runs from here until its first RPC waits
}

bool SubmitThread::Collect(MixRing::SendResult & result)
{
    if (m_state != State::Done)
    {
        return false;
    }
    CompilerBarrier(); // the result was written before the state

    result  = m_result;
    m_state = State::Idle;
    return true;
}

void SubmitThread::Wait()
{
    if (m_threadId < 0)
    {
        return;
    }

    // Signals from sends nobody waited for are stale: drop them, then sleep until this one's.
    while (PollSema(m_doneSema) >= 0)
    {
    }
    while (Busy())
    {
        WaitSema(m_doneSema);
    }
}

} // namespace ps2::audio
