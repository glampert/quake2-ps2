/* ================================================================================================
 * File: cd_audio.cpp
 * Brief: Quake II's CD audio module (client/cdaudio.h), replacing null/cd_null.c. The
 *        soundtrack comes from baseq2/music/trackNN.adp instead of the disc's audio tracks -
 *        NN being the CD track a map asks for through CS_CDTRACK, 02..11 for the stock game -
 *        or, when `make music` hasn't encoded that track, from its trackNN.wav. A MusicStream
 *        reads and decodes the file (music_stream.h), and the result goes into the engine's
 *        raw-sample channel - s_rawsamples in client/snd_dma.c, the one the cinematics stream
 *        through - which the mixer lays under the sound effects. It then reaches the SPU2
 *        inside the audsrv stream that carries the mix anyway, so the music costs no SIF
 *        bandwidth of its own. A track at another sample rate than the mixer's is converted
 *        on the way in: a 2:1 half-band filter for exactly double (a 44.1kHz CD rip),
 *        linear interpolation otherwise.
 *
 *        Behaviour follows id's win32/cd_win.c: a map's track loops cd_loopcount times, then
 *        hands over to the ambient cd_looptrack, which loops for good; cd_nocd (the menu's
 *        "CD music" toggle) silences it, and the "cd" command drives it by hand. Additions:
 *        cd_volume, the level the menu's "music volume" slider sets - a CD played through
 *        the drive's own analog output, so it never answered to s_volume - and turning the
 *        music back on restarts the map's track instead of waiting for the next map.
 *
 *        Everything here runs on the main thread, once a frame, right after S_Update: the
 *        mixer and this top-up never race over s_rawsamples. Only MusicStream's file reads
 *        happen elsewhere.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/audio/half_band.h"
#include "ps2/audio/music_stream.h"
#include "ps2/common.h"
#include "ps2/renderer/profile.h"

#include <cstdlib>
#include <cstring>

// The CD module is client code: it reads cl.cinematictime and writes into the mixer's
// raw-sample ring. The legacy headers redeclare a few q_common.h functions, hence the
// pragma diagnostic ignored here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
extern "C" {
    #include "client/client.h"
    #include "client/snd_loc.h"

    extern int sound_started; // client/snd_dma.c, not in any header.
}
#pragma GCC diagnostic pop

namespace {

using ps2::audio::HalfBandDecimator;
using ps2::audio::MusicStream;

// Highest track number a map (or the "cd" command) may ask for, as on an audio CD.
constexpr int kMaxTrack = 99;

// Where a track's music can come from, tried in this order in each search directory: the
// .adp `make music` encodes, then the plain .wav it encodes from, for whoever hasn't run it.
// A ripper's capitalised Track02.wav is tried too, as `make music` accepts it: FAT and macOS
// hosts ignore case anyway, but PCSX2 on a case-sensitive host does not.
constexpr const char * kTrackFiles[] = {
    "%s/music/track%02d.adp",
    "%s/music/track%02d.wav",
    "%s/music/Track%02d.wav",
};

// How far ahead of the mixer's paint position the raw-sample ring is kept filled. This is
// the hitch tolerance: a frame that takes longer than this to come round again (348 ms at
// 22050Hz) leaves a gap in the music, not a desync. The margin keeps the fill clear of the
// ring's wrap onto samples the mixer hasn't consumed yet.
constexpr int kRawLeadFrames = MAX_RAW_SAMPLES - 512;

// Frames decoded per MusicStream::Decode call, into a stack buffer.
constexpr int kPumpFrames = 256;

// cd_volume maps to a 0..kUnityGain multiplier. At unity a sample scaled by it lands in the
// ring exactly as S_RawSamples would store it (<< 8), the scale the mixer expects.
constexpr int kUnityGain = 256;

static MusicStream s_stream;

static cvar_t * s_cdNoCd      = nullptr;
static cvar_t * s_cdVolume    = nullptr;
static cvar_t * s_cdLoopCount = nullptr;
static cvar_t * s_cdLoopTrack = nullptr;

static int  s_requestedTrack   = 0;     // What the game last asked for, replayed when cd_nocd clears.
static bool s_requestedLooping = false;
static int  s_playingTrack     = 0;     // The track open in s_stream; 0 when stopped.
static bool s_playLooping      = false;
static int  s_loopCounter      = 0;     // cd_win.c's loopcounter: passes played since CDAudio_Play.
static int  s_trackPass        = 0;     // Pass of the current track, from 1 - for display only.
static bool s_paused           = false; // "cd pause".
static bool s_disabled         = false; // cd_nocd, as CDAudio_Update last saw it.
static bool s_ringPrimed       = false; // s_rawsamples holds music the mixer hasn't painted yet.

static bool s_warnedMissing[kMaxTrack + 1] = {};
static char s_trackPath[MAX_OSPATH]        = {}; // The file s_stream has open, for the console.

// For a track at exactly twice the mixer's rate: a 44.1kHz WAV at the default s_khz 22, or
// any 22050Hz track at s_khz 11. See half_band.h.
static HalfBandDecimator s_halfBand;

// Linear resampler, for any other rate mismatch: a 22050Hz track at s_khz 44, or a WAV at an
// odd rate. 15-bit phase keeps the products inside 32 bits; the R5900 has no 64-bit multiply.
constexpr int kPhaseBits = 15;
constexpr int kPhaseOne  = 1 << kPhaseBits;

static int s_resamplePhase   = kPhaseOne; // Position between s_resamplePrev and s_resampleNext.
static int s_resamplePrev[2] = {};
static int s_resampleNext[2] = {};
static s16 s_sourceFrames[kPumpFrames * 2];
static int s_sourceCount     = 0;
static int s_sourcePos       = 0;

bool SoundRunning()
{
    return (sound_started != 0) && (dma.buffer != nullptr);
}

int CvarInt(const cvar_t * const var)
{
    return static_cast<int>(var->value);
}

void ResetRateConversion()
{
    s_halfBand.Reset();
    s_resamplePhase = kPhaseOne;
    s_resamplePrev[0] = s_resamplePrev[1] = 0;
    s_resampleNext[0] = s_resampleNext[1] = 0;
    s_sourceCount = s_sourcePos = 0;
}

// Throws away the music already queued in the ring but not painted yet, so a stop, pause or
// track change is heard at once rather than a ring's worth (~350 ms) later.
void DropQueuedMusic()
{
    if (s_ringPrimed && SoundRunning() && s_rawend > paintedtime)
    {
        s_rawend = paintedtime;
    }
    s_ringPrimed = false;
}

void CloseTrack(const bool dropQueued)
{
    s_stream.Close();
    s_playingTrack = 0;
    if (dropQueued)
    {
        DropQueuedMusic();
    }
}

// Tries each of kTrackFiles under every search path directory, the mod's first, like any
// other game file. Only loose files: the stream reads through its own fd, not the pak code.
bool OpenTrackFile(const int track, const int extraLoops)
{
    char path[MAX_OSPATH];
    const char * previous = nullptr;

    for (char * dir = FS_NextPath(nullptr); dir != nullptr; dir = FS_NextPath(dir))
    {
        // FS_NextPath lists the game directory twice (fs_gamedir, then its search path
        // entry), and with no mod loaded that is baseq2 both times.
        if (previous != nullptr && std::strcmp(previous, dir) == 0)
        {
            continue;
        }
        previous = dir;

        for (const char * const pattern : kTrackFiles)
        {
            Com_sprintf(path, sizeof(path), pattern, dir, track);
            switch (s_stream.Open(path, extraLoops))
            {
            case MusicStream::OpenResult::Opened:
                Com_sprintf(s_trackPath, sizeof(s_trackPath), "%s", path);
                return true;

            case MusicStream::OpenResult::Unusable:
                // Already reported. The fallback is for a file that isn't there, not one
                // that is broken or didn't fit in memory - the next one would most likely
                // not fit either.
                return false;

            case MusicStream::OpenResult::Missing:
                break;
            } // switch (s_stream.Open(path, extraLoops))
        }
    }

    if (!s_warnedMissing[track])
    {
        s_warnedMissing[track] = true;
        Com_DPrintf("CDAudio: no music/track%02d.adp or .wav, track %d stays silent.\n", track, track);
    }
    return false;
}

// cd_win.c's CDAudio_Play2. The loop logic lives in how many seamless passes the stream is
// told to make: the map's track as many as cd_loopcount still allows, the ambient track
// forever. CDAudio_Update picks what follows when the stream runs out.
void StartTrack(const int track, const bool looping, const bool dropQueued)
{
    if (track == s_playingTrack && looping == s_playLooping && s_stream.IsOpen() && !s_stream.Finished())
    {
        return; // Already playing it, as cd_win.c.
    }

    CloseTrack(dropQueued);

    if (track < 1 || track > kMaxTrack)
    {
        return; // Track 0 is how a map asks for silence.
    }

    int extraLoops = 0;
    if (looping)
    {
        if (track == CvarInt(s_cdLoopTrack))
        {
            extraLoops = MusicStream::kLoopForever;
        }
        else
        {
            const int passesLeft = CvarInt(s_cdLoopCount) - s_loopCounter;
            extraLoops = (passesLeft > 1) ? (passesLeft - 1) : 0;
        }
    }

    if (!OpenTrackFile(track, extraLoops))
    {
        return;
    }

    s_playingTrack = track;
    s_playLooping  = looping;
    s_trackPass    = 1;
    s_paused       = false;
    ResetRateConversion();

    char passes[32];
    if (extraLoops == MusicStream::kLoopForever)
    {
        Com_sprintf(passes, sizeof(passes), "looping");
    }
    else
    {
        Com_sprintf(passes, sizeof(passes), "%d pass(es)", extraLoops + 1);
    }
    Com_DPrintf("CDAudio: track %02d from %s (%s, %d Hz %s), %s.\n", track, s_trackPath,
                s_stream.FormatName(), s_stream.SampleRate(), (s_stream.Channels() > 1) ? "stereo" : "mono", passes);
}

// Called with the stream finished: all of its music is in the ring, still playing out.
void TrackFinished()
{
    const int  track  = s_playingTrack;
    const bool failed = s_stream.Failed();

    CloseTrack(false); // Keep the tail that is still queued.

    if (failed || !s_playLooping)
    {
        return;
    }

    // As cd_win.c: once the track has played cd_loopcount times, go to the ambient track.
    ++s_loopCounter;
    const int next = (s_loopCounter >= CvarInt(s_cdLoopCount)) ? CvarInt(s_cdLoopTrack) : track;
    StartTrack(next, true, false);
}

// Stores `count` decoded frames at the ring's fill position. The fill position is kept in
// a local for the loop: with strict aliasing off, every ring store could alias the global
// s_rawend, and it would be reloaded and stored back once a frame.
void StoreFrames(const s16 * const frames, const int count, const int gain)
{
    const s16 * __restrict             in   = frames;
    portable_samplepair_t * __restrict ring = s_rawsamples;
    int fill = s_rawend;

    for (int i = 0; i < count; ++i)
    {
        portable_samplepair_t & out = ring[fill & (MAX_RAW_SAMPLES - 1)];
        out.left  = in[i * 2] * gain;
        out.right = in[(i * 2) + 1] * gain;
        ++fill;
    }
    s_rawend = fill;
}

// Fills up to `wanted` ring frames from a track at twice the mixer's rate, through the
// half-band decimator. Returns how many it managed before the stream ran dry.
int StoreDecimated(const int wanted, const int gain)
{
    s16 frames[HalfBandDecimator::kMaxOutputs * 2];
    int done = 0;

    while (done < wanted)
    {
        const int batch = ((wanted - done) < HalfBandDecimator::kMaxOutputs) ? (wanted - done) : HalfBandDecimator::kMaxOutputs;

        int needed = s_halfBand.InputNeeded(batch);
        if (needed > s_halfBand.Room())
        {
            needed = s_halfBand.Room();
        }
        if (needed > 0)
        {
            s_halfBand.Commit(s_stream.Decode(s_halfBand.Tail(), needed));
        }

        const int count = s_halfBand.Produce(frames, batch);
        if (count <= 0)
        {
            break;
        }
        StoreFrames(frames, count, gain);
        done += count;
    }
    return done;
}

bool PullSourceFrame(int frame[2])
{
    if (s_sourcePos == s_sourceCount)
    {
        s_sourceCount = s_stream.Decode(s_sourceFrames, kPumpFrames);
        s_sourcePos   = 0;
        if (s_sourceCount <= 0)
        {
            s_sourceCount = 0;
            return false;
        }
    }
    frame[0] = s_sourceFrames[s_sourcePos * 2];
    frame[1] = s_sourceFrames[(s_sourcePos * 2) + 1];
    ++s_sourcePos;
    return true;
}

// Fills `wanted` ring frames by linear interpolation. Returns how many it managed before
// the stream ran dry.
int StoreResampled(const int wanted, const int gain)
{
    const int step = static_cast<int>((static_cast<u32>(s_stream.SampleRate()) << kPhaseBits) / static_cast<u32>(dma.speed));

    for (int done = 0; done < wanted; ++done)
    {
        while (s_resamplePhase >= kPhaseOne)
        {
            int frame[2];
            if (!PullSourceFrame(frame))
            {
                return done;
            }
            s_resamplePrev[0] = s_resampleNext[0];
            s_resamplePrev[1] = s_resampleNext[1];
            s_resampleNext[0] = frame[0];
            s_resampleNext[1] = frame[1];
            s_resamplePhase  -= kPhaseOne;
        }

        const int left  = s_resamplePrev[0] + (((s_resampleNext[0] - s_resamplePrev[0]) * s_resamplePhase) >> kPhaseBits);
        const int right = s_resamplePrev[1] + (((s_resampleNext[1] - s_resamplePrev[1]) * s_resamplePhase) >> kPhaseBits);

        portable_samplepair_t & out = s_rawsamples[s_rawend & (MAX_RAW_SAMPLES - 1)];
        out.left  = left * gain;
        out.right = right * gain;
        ++s_rawend;

        s_resamplePhase += step;
    }
    return wanted;
}

// Tops the raw-sample ring up to kRawLeadFrames ahead of the mixer.
void PumpMusic()
{
    if (!SoundRunning())
    {
        s_ringPrimed = false;
        return;
    }

    // A cinematic streams its own audio through the same ring. It stops the music as it
    // starts (SCR_PlayCinematic -> CDAudio_Stop), so this only guards a track requested
    // while one is still running.
    if (cl.cinematictime > 0)
    {
        return;
    }

    if (s_rawend < paintedtime)
    {
        // The mixer painted past everything we had queued: a frame took longer than the
        // lead, or the reads fell behind. As in S_RawSamples, carry on from here.
        if (s_ringPrimed) [[unlikely]]
        {
            Com_DPrintf("CDAudio: music underrun (%d frames).\n", paintedtime - s_rawend);
        }
        s_rawend = paintedtime;
    }

    int wanted = (paintedtime + kRawLeadFrames) - s_rawend;
    if (wanted <= 0)
    {
        return;
    }

    const int gain = static_cast<int>((s_cdVolume->value * static_cast<float>(kUnityGain)) + 0.5f);
    const int rate = s_stream.SampleRate();

    if (rate == dma.speed)
    {
        s16 frames[kPumpFrames * 2];
        while (wanted > 0)
        {
            const int count = s_stream.Decode(frames, (wanted < kPumpFrames) ? wanted : kPumpFrames);
            if (count <= 0)
            {
                break;
            }
            StoreFrames(frames, count, gain);
            wanted -= count;
            s_ringPrimed = true;
        }
    }
    else if (((rate == 2 * dma.speed) ? StoreDecimated(wanted, gain) : StoreResampled(wanted, gain)) > 0)
    {
        s_ringPrimed = true;
    }
}

void PrintInfo()
{
    Com_Printf("CD audio (music/trackNN.adp or .wav): %s\n", s_disabled ? "off (cd_nocd 1)" : "on");

    if (s_stream.IsOpen())
    {
        const int rate     = s_stream.SampleRate();
        const int position = s_stream.PositionFrames() / rate;
        const int length   = s_stream.FrameCount() / rate;
        Com_Printf("  track %02d %s, decoded to %d:%02d of %d:%02d, pass %d%s\n",
                   s_playingTrack, s_playLooping ? "looping" : "once",
                   position / 60, position % 60, length / 60, length % 60,
                   s_trackPass, s_paused ? ", paused" : "");
        Com_Printf("  %s: %s, %d Hz %s\n", s_trackPath, s_stream.FormatName(), rate,
                   (s_stream.Channels() > 1) ? "stereo" : "mono");
    }
    else
    {
        Com_Printf("  stopped (last requested track: %d)\n", s_requestedTrack);
    }

    Com_Printf("  cd_volume %.2f, cd_loopcount %d, cd_looptrack %d\n",
               static_cast<double>(s_cdVolume->value), CvarInt(s_cdLoopCount), CvarInt(s_cdLoopTrack));
}

// A subset of cd_win.c's "cd" command, the parts that mean something without a disc.
void CDComand()
{
    const char * const command = (Cmd_Argc() >= 2) ? Cmd_Argv(1) : "";

    if (Q_strcasecmp(command, "on") == 0)
    {
        Cvar_Set("cd_nocd", "0");
    }
    else if (Q_strcasecmp(command, "off") == 0)
    {
        Cvar_Set("cd_nocd", "1");
    }
    else if ((Q_strcasecmp(command, "play") == 0 || Q_strcasecmp(command, "loop") == 0) && Cmd_Argc() >= 3)
    {
        CDAudio_Play(std::atoi(Cmd_Argv(2)), Q_strcasecmp(command, "loop") == 0);
    }
    else if (Q_strcasecmp(command, "stop") == 0)
    {
        CDAudio_Stop();
    }
    else if (Q_strcasecmp(command, "pause") == 0)
    {
        s_paused = true;
        DropQueuedMusic();
    }
    else if (Q_strcasecmp(command, "resume") == 0)
    {
        s_paused = false;
    }
    else if (Q_strcasecmp(command, "info") == 0)
    {
        PrintInfo();
    }
    else
    {
        Com_Printf("usage: cd <on|off|play N|loop N|stop|pause|resume|info>\n");
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------

extern "C" {

int CDAudio_Init()
{
    s_cdNoCd      = Cvar_Get("cd_nocd", "0", CVAR_ARCHIVE);
    s_cdVolume    = Cvar_Get("cd_volume", "0.7", CVAR_ARCHIVE);
    s_cdLoopCount = Cvar_Get("cd_loopcount", "4", 0);
    s_cdLoopTrack = Cvar_Get("cd_looptrack", "11", 0);
    s_disabled    = (s_cdNoCd->value != 0.0f);

    Cmd_AddCommand("cd", CDComand);
    return 0;
}

void CDAudio_Shutdown()
{
    if (s_cdNoCd == nullptr)
    {
        return;
    }
    CloseTrack(true);
    Cmd_RemoveCommand("cd");
}

void CDAudio_Play(const int track, const qboolean looping)
{
    if (s_cdNoCd == nullptr)
    {
        return;
    }

    s_requestedTrack   = track;
    s_requestedLooping = (looping != 0);
    s_loopCounter      = 0;

    if (s_cdNoCd->value != 0.0f)
    {
        return; // Remembered for when the music is turned back on.
    }
    StartTrack(track, s_requestedLooping, true);
}

void CDAudio_Stop()
{
    if (s_cdNoCd == nullptr)
    {
        return;
    }
    s_requestedTrack = 0;
    CloseTrack(true);
}

void CDAudio_Update()
{
    if (s_cdNoCd == nullptr)
    {
        return;
    }

    PS2_PROFILE_SCOPED_EVENT(ps2::prof_evt::Music);

    const bool disabled = (s_cdNoCd->value != 0.0f);
    if (disabled != s_disabled)
    {
        s_disabled = disabled;
        if (disabled)
        {
            CloseTrack(true);
        }
        else if (s_requestedTrack > 0)
        {
            s_loopCounter = 0;
            StartTrack(s_requestedTrack, s_requestedLooping, true);
        }
    }

    if (s_cdVolume->value < 0.0f || s_cdVolume->value > 1.0f)
    {
        Cvar_SetValue("cd_volume", (s_cdVolume->value < 0.0f) ? 0.0f : 1.0f);
    }

    if (!s_stream.IsOpen() || s_paused)
    {
        return;
    }

    PumpMusic();

    const int wraps = s_stream.TakeWraps();
    if (wraps > 0)
    {
        s_loopCounter += wraps;
        s_trackPass   += wraps;
        Com_DPrintf("CDAudio: track %02d looped, pass %d.\n", s_playingTrack, s_trackPass);
    }

    if (s_stream.Finished())
    {
        TrackFinished();
    }
}

} // extern "C"
