/* ================================================================================================
 * File: snd.cpp
 * Brief: Sound output backend, replacing null/snddma_null.c. Implements the five SNDDMA_*
 *        entry points the stock Quake mixer calls out to, on top of an AudsrvDevice (the
 *        audsrv IOP driver, see audsrv_device.h) and a MixRing (the paint buffer and its
 *        submit cursor, see mix_ring.h).
 *
 *        The mixing itself is entirely portable and already in the build (client/snd_dma.c,
 *        snd_mem.c, snd_mix.c): every frame S_Update_ paints s_mixahead seconds of stereo
 *        into dma.buffer and then calls SNDDMA_Submit. All this file does is give the mixer
 *        somewhere to paint, tell it where playback has got to, and push what it painted
 *        towards the SPU2.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/audio/audsrv_device.h"
#include "ps2/audio/mix_ring.h"
#include "ps2/audio/music_stream.h"
#include "ps2/audio/sound_hold.h"
#include "ps2/common.h"
#include "ps2/fs/async_io.h"
#include "ps2/renderer/profile.h"
#include "ps2/system/iop_boot.h"

// The sound backend is client code (the engine's own win32/snd_win.c is the same): it
// fills in the shared dma_t and reads the mixer's paintedtime. The legacy headers
// redeclare a few q_common.h functions, hence the pragma diagnostic ignored here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
extern "C" {
    #include "client/client.h"
    #include "client/snd_loc.h"
}
#pragma GCC diagnostic pop

namespace {

static ps2::audio::AudsrvDevice s_device;
static ps2::audio::MixRing s_mixRing;

static const cvar_t * s_disableSound = nullptr;
static const cvar_t * s_soundDefer   = nullptr; // ps2_sound_defer: 0 off, 1 on the HDD, 2 always

// Holding the submit back behind a background read (audio.md). On the HDD, an audsrv RPC made
// while the loader or the music reader has a file call out waits on the IOP until fileXio's
// thread lets go: 8-44 ms on the console. The ring holds ~85 ms at a submit, so the submit can
// wait out the read instead, while the last one is under kMaxSubmitDelayMs old, and then goes
// anyway. The test runs at a submit, so the gap can reach the limit plus a frame: 30 ms holds
// back one 50 Hz frame at most, and none after a frame longer than that.
constexpr int kMaxSubmitDelayMs = 30;
static bool s_gameDataOnHdd  = false;
static int  s_lastSubmitMs   = 0;
static bool s_submitDeferred = false; // the last SNDDMA_Submit held back (sndFree -2)

// What holds the output silent (sound_hold.h): PS2_SoundHoldBegin/End's count, and the
// loading plaque. Either one is enough.
static int  s_soundHolds  = 0;
static bool s_loadingHold = false;

// Brings the device in line with the holds, after one of them changed.
void ApplySoundHold()
{
    if (ps2::audio::OutputHeld())
    {
        s_device.Hold();
    }
    else
    {
        s_device.Release();
    }
}

bool DeferSubmit()
{
    const int mode = static_cast<int>(s_soundDefer->value);
    if (mode <= 0 || (mode == 1 && !s_gameDataOnHdd))
    {
        return false;
    }

    // The loader and the reader only run while the main thread sleeps, so a read can't start
    // between this test and the RPCs; one that starts while they wait queues behind them.
    if (!ps2::aio::FileCallInFlight() && !ps2::audio::MusicReadPending())
    {
        return false;
    }

    return (Sys_Milliseconds() - s_lastSubmitMs) < kMaxSubmitDelayMs;
}

// Output rate in Hz for a given s_khz. Only rates audsrv can take at 16-bit stereo
// are offered - its upsampler table is a fixed list, and a rate outside it would fail
// set_format. 22050 is the default: it matches the majority of the game's WAVs and the
// 22050Hz audio in the .cin videos, which spares us a snd_restart whenever one plays.
constexpr int kDefaultKhz          = 22;
constexpr int kDefaultSampleRateHz = 22050;

int PickSampleRate()
{
    const int khz = static_cast<int>(s_khz->value);

    // Note s_khz is a rounded-down kHz count, not the rate: 22 means 22050.
    switch (khz)
    {
    case 11 : return 11025;
    case 22 : return kDefaultSampleRateHz;
    case 44 : return 44100;
    default : break;
    } // switch (khz)

    Com_Printf("Unsupported s_khz value %d, using %d.\n", khz, kDefaultKhz);
    Cvar_SetValue("s_khz", static_cast<float>(kDefaultKhz));
    return kDefaultSampleRateHz;
}

} // namespace

bool ps2::audio::OutputHeld()
{
    return (s_soundHolds > 0) || s_loadingHold;
}

int ps2::audio::SubmitFreeBytes()
{
    if (ps2::audio::OutputHeld())
    {
        return -1;
    }
    return s_submitDeferred ? -2 : s_mixRing.LastFreeBytes();
}

int ps2::audio::DeviceRingBytes()
{
    return s_device.RingBytes();
}

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

extern "C" {

qboolean SNDDMA_Init()
{
    s_disableSound  = Cvar_Get("ps2_disable_sound", "0", CVAR_ARCHIVE);
    s_soundDefer    = Cvar_Get("ps2_sound_defer", "1", 0);
    s_gameDataOnHdd = ps2::sys::GameDataOnHdd();
    if (s_disableSound->value != 0.0f)
    {
        Com_Printf("Sound system disabled by ps2_disable_sound - running silent.\n");
        return false; // Sound system disabled at boot time.
    }

    const int sampleRateHz = PickSampleRate();

    if (!s_device.Init(sampleRateHz))
    {
        Com_Printf("Sound backend unavailable - running silent.\n");
        return false;
    }

    s_mixRing.Reset();

    // NOTE: `dma` is a global defined by client/snd_dma.c
    dma.channels         = ps2::audio::AudsrvDevice::kChannels;
    dma.samples          = ps2::audio::MixRing::kSamples;
    dma.submission_chunk = 1; // audsrv takes any number of whole stereo frames
    dma.samplepos        = 0;
    dma.samplebits       = ps2::audio::AudsrvDevice::kSampleBits;
    dma.speed            = sampleRateHz;
    dma.buffer           = s_mixRing.Buffer();

    // A restart under a hold (snd_restart while the plaque is up) opens a playing stream.
    if (ps2::audio::OutputHeld())
    {
        s_device.Hold();
    }

    return true;
}

void SNDDMA_Shutdown()
{
    s_device.Shutdown();
    dma.buffer = nullptr;
}

int SNDDMA_GetDMAPos()
{
    return s_mixRing.PositionInSamples();
}

void SNDDMA_BeginPainting()
{
    // Nothing to lock or remap: the mixer paints straight into our own buffer, which
    // stays valid for as long as the program runs. Note that S_Update_ bails out
    // *without* calling SNDDMA_Submit when dma.buffer is null, so it must never be
    // cleared anywhere but SNDDMA_Shutdown.
}

void SNDDMA_Submit()
{
    // Profiled because this is the one part of the sound path that is ours and is
    // not free: Drain() calls audsrv_available() and audsrv_play_audio(), both
    // blocking SIF RPCs, and the second copies the painted samples across to the
    // IOP. The mixing above it is engine code (client/snd_mix.c) and is not
    // covered here - what this measures is the hand-off, not the mix.
    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Sound);

    // Held, nothing goes over: the submit cursor, which is the play position the mixer
    // reads back, stands still, so the mixer stops a mixahead ahead of it and the music
    // stops filling its ring. Both carry on from there after the release.
    if (ps2::audio::OutputHeld())
    {
        return;
    }

    // A background read is out on the IOP: send nothing yet, like a short hold. The mixer stops a
    // mixahead past the cursor and the next submit tops the ring up as usual.
    s_submitDeferred = DeferSubmit();
    if (s_submitDeferred)
    {
        return;
    }
    s_lastSubmitMs = Sys_Milliseconds();

    // NOTE: `paintedtime` is a global defined by client/snd_dma.c
    s_mixRing.Drain(s_device, paintedtime);
}

void PS2_SoundHoldBegin()
{
    ++s_soundHolds;
    ApplySoundHold();
}

void PS2_SoundHoldEnd()
{
    PS2_AssertMsg(s_soundHolds > 0, "PS2_SoundHoldEnd without a PS2_SoundHoldBegin!");
    if (s_soundHolds > 0)
    {
        --s_soundHolds;
    }
    ApplySoundHold();
}

void PS2_SoundSilence()
{
    if (!ps2::audio::OutputHeld())
    {
        s_device.Silence();
    }
}

void PS2_SoundSetLoadingHold(const int held)
{
    s_loadingHold = (held != 0);
    ApplySoundHold();
}

} // extern "C"
