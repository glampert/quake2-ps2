#pragma once
/* ================================================================================================
 * File: cd_audio.h
 * Brief: What the rest of the backend asks of the soundtrack (cd_audio.cpp) beyond the engine's
 *        CDAudio_* seam: holding the music through a stall
 *        that would otherwise cut it off mid-note.
 *
 *        The main thread decodes the music into the mixer's raw-sample ring about 350 ms ahead of
 *        what is playing, and the mixer only paints from it while frames run. Through a stall -
 *        a save going to the memory card, a second or more on a console - the output is held
 *        silent (sound_hold.h), so the music stopped dead and came back just as abruptly.
 *        HoldMusic fades it out first, and ReleaseMusic fades it back in.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

namespace ps2::audio {

// Paints the mix ahead and hands it to audsrv, as the end of a frame does (S_Update_), ahead of a
// stretch that holds the main thread up for a while: what is painted ahead is all that plays
// meanwhile. Does nothing without sound, or with the output held.
void MixAhead();

// Fades the music out and holds it. What is already queued in the ring fades out where it is,
// none of it dropped, so the music picks up where the fade ended; beyond it the music is decoded
// on, ramped down, to the fade's full length. Returns once all of the fade has gone to audsrv,
// painting the mix meanwhile as frames would: what was painted ahead and about half a second, or
// at once when no music is playing or the output is held already. The hold that follows plays it
// out (sound_hold.h). Nothing more is decoded until ReleaseMusic. Nothing happens if the music is
// held already.
void HoldMusic();

// Lets the held music go on, fading back in from where it stopped. A track opened in the
// meantime starts at full volume, as tracks always do.
void ReleaseMusic();

} // namespace ps2::audio
