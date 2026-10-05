#pragma once
/* ================================================================================================
 * File: half_band.h
 * Brief: Streaming 2:1 decimator for stereo 16-bit audio: a half-band low-pass, then every
 *        other frame. cd_audio.cpp runs music through it when the track's sample rate is
 *        exactly twice the mixer's - a 44.1kHz CD rip played as the .wav fallback at the
 *        default s_khz 22, or any 22050Hz track at s_khz 11. Other ratios get its linear
 *        resampler, which is fine upward but would let everything above the new Nyquist
 *        fold straight back down into the music.
 *
 *        The filter: 23 taps, a Kaiser-windowed (beta 6.8) half-band sinc. Half-band is what
 *        makes it cheap - the taps alternate with zeros around a centre of exactly 0.5 - so
 *        an output frame costs six mirrored pairs per channel. Response at 44.1kHz in:
 *        flat to -0.14 dB at 8 kHz, -0.81 dB at 9 kHz, -6 dB at the new 11.025 kHz Nyquist
 *        (fixed for any half-band), and everything above 15 kHz - what would fold back below
 *        7 kHz - at least 67 dB down. Q14, so the products and the centre always sum within
 *        31 bits (the taps' absolute sum is 1.41).
 *
 *        Header-only and free of anything Quake, so a host harness can check its streaming
 *        against a direct convolution.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include <cstring>
#include <tamtypes.h>

namespace ps2::audio {

class HalfBandDecimator final
{
public:
    // Most output frames one Produce() call computes. Sizes the input buffer.
    static constexpr int kMaxOutputs = 256;

    void Reset() { m_held = 0; }

    // Input frames it can take right now, and where they go (interleaved L/R).
    int Room() const { return kCapacity - m_held; }
    s16 * Tail() { return m_frames + (m_held * 2); }

    // Takes the `frames` the caller just wrote at Tail().
    void Commit(const int frames) { m_held += frames; }

    // Input frames still missing before `outputs` output frames can be produced: the filter
    // spans kSpan input frames for the first, and each one after it needs two more.
    int InputNeeded(const int outputs) const
    {
        const int needed = kSpan + (2 * (outputs - 1)) - m_held;
        return (needed > 0) ? needed : 0;
    }

    // Writes up to maxOutputs (at most kMaxOutputs) frames at half the input rate into
    // outStereo, as many as the held input allows, and returns how many. The input that
    // the next output's window still needs is kept.
    int Produce(s16 * const outStereo, const int maxOutputs)
    {
        int outputs = (m_held >= kSpan) ? (((m_held - kSpan) / 2) + 1) : 0;
        if (outputs > maxOutputs)
        {
            outputs = maxOutputs;
        }

        // One channel at a time: a channel's 13 taps fit in registers, both channels' don't,
        // and gcc keeps consecutive windows' shared samples live across iterations, so
        // interleaving them spilled to the stack every frame.
        for (int channel = 0; channel < 2; ++channel)
        {
            // The centre frame of output 0's window, which starts at input frame 0; each
            // output after it is two frames on.
            const s16 * __restrict centre = m_frames + (kCentre * 2) + channel;
            s16 * __restrict       out    = outStereo + channel;

            for (int i = 0; i < outputs; ++i)
            {
                // Spelled out: -O2 (the debug build) leaves a loop over the pairs rolled,
                // reloading each tap. The offsets are in s16s, frames 1, 3, ... 11 away.
                int acc = (centre[0] * kCentreTap) + kRound;
                acc += kTaps[0] * (centre[-2]  + centre[2]);
                acc += kTaps[1] * (centre[-6]  + centre[6]);
                acc += kTaps[2] * (centre[-10] + centre[10]);
                acc += kTaps[3] * (centre[-14] + centre[14]);
                acc += kTaps[4] * (centre[-18] + centre[18]);
                acc += kTaps[5] * (centre[-22] + centre[22]);
                *out = Clamp16(acc >> kTapBits);

                centre += 4;
                out    += 2;
            }
        }

        if (outputs > 0)
        {
            const int consumed = 2 * outputs;
            m_held -= consumed;
            std::memmove(m_frames, m_frames + (consumed * 2), static_cast<size_t>(m_held) * 2u * sizeof(s16));
        }
        return outputs;
    }

private:
    static constexpr int kPairs  = 6;                  // Nonzero taps either side of the centre.
    static constexpr int kSpan   = (4 * kPairs) - 1;   // 23 input frames under the filter.
    static constexpr int kCentre = kSpan / 2;          // The window's middle frame.

    static constexpr int kTapBits   = 14;
    static constexpr int kCentreTap = 1 << (kTapBits - 1); // Exactly 0.5.
    static constexpr int kRound     = 1 << (kTapBits - 1);

    // Taps at 1, 3, 5, 7, 9 and 11 frames from the centre (the rest are zero), rounded so
    // the DC gain is exactly 1.
    static constexpr int kTaps[kPairs] = { 5103, -1425, 591, -232, 72, -13 };
    static_assert(kCentreTap + (2 * (kTaps[0] + kTaps[1] + kTaps[2] + kTaps[3] + kTaps[4] + kTaps[5])) == (1 << kTapBits),
                  "half-band taps must keep a DC gain of 1");

    // Enough for kMaxOutputs outputs from an empty buffer.
    static constexpr int kCapacity = (kSpan - 2) + (2 * kMaxOutputs);

    static s16 Clamp16(const int value)
    {
        return static_cast<s16>((value < -32768) ? -32768 : ((value > 32767) ? 32767 : value));
    }

    int m_held = 0; // Input frames at the front of m_frames.
    s16 m_frames[kCapacity * 2] = {};
};

} // namespace ps2::audio
