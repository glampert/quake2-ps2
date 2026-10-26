/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// snd_mem.c: sound caching

#include "client.h"
#include "snd_loc.h"
#include "ps2/system/heap.h" // [PS2_QUAKE]: PS2_AudioMalloc/PS2_AudioFree
#include "ps2/audio/sound_load.h" // [PS2_QUAKE]: mid-level loads through the background loader

int cache_full_cycle;

/*
==============
[PS2_QUAKE]: S_FreeSoundCache

The allocation was 'length * width' data bytes plus the header, and ResampleSfx
leaves both fields describing exactly what it wrote, so the size is recoverable
here without storing it separately.
==============
*/
void S_FreeSoundCache(sfxcache_t * sc)
{
    if (!sc)
        return;

    PS2_AudioFree(sc, (size_t)(sc->length * sc->width) + sizeof(sfxcache_t));
}

/*
================
ResampleSfx
================
*/
void ResampleSfx(sfx_t * sfx, int inrate, int inwidth, byte * data)
{
    int outcount;
    int srcsample;
    float stepscale;
    int i;
    int sample, samplefrac, fracstep;
    sfxcache_t * sc;

    sc = sfx->cache;
    if (!sc)
        return;

    stepscale = (float)inrate / dma.speed; // this is usually 0.5, 1, or 2

    outcount = sc->length / stepscale;
    sc->length = outcount;
    if (sc->loopstart != -1)
        sc->loopstart = sc->loopstart / stepscale;

    sc->speed = dma.speed;
    if (s_loadas8bit->value)
        sc->width = 1;
    else
        sc->width = inwidth;
    sc->stereo = 0;

    // resample / decimate to the current source rate

    if (stepscale == 1 && inwidth == 1 && sc->width == 1)
    {
        // fast special case
        for (i = 0; i < outcount; i++)
            ((signed char *)sc->data)[i] = (int)((unsigned char)(data[i]) - 128);
    }
    else
    {
        // general case
        samplefrac = 0;
        fracstep = stepscale * 256;
        for (i = 0; i < outcount; i++)
        {
            srcsample = samplefrac >> 8;
            samplefrac += fracstep;
            if (inwidth == 2)
                sample = LittleShort(((short *)data)[srcsample]);
            else
                sample = (int)((unsigned char)(data[srcsample]) - 128) << 8;
            if (sc->width == 2)
                ((short *)sc->data)[i] = sample;
            else
                ((signed char *)sc->data)[i] = sample >> 8;
        }
    }
}

//=============================================================================

/*
==============
S_LoadSound
==============
*/
sfxcache_t * S_LoadSound(sfx_t * s)
{
    char namebuffer[MAX_QPATH];
    byte * data;
    sfxcache_t * sc;
    int size;
    char * name;

    if (s->name[0] == '*')
        return NULL;

    // see if still in memory
    sc = s->cache;
    if (sc)
        return sc;

    // [PS2_QUAKE]: its read is in the background loader's hands; the cache arrives through the
    // pump (ps2/audio/sound_load.h). NULL until then, as for a sound that failed to load.
    if (PS2_SoundLoadPending(s))
        return NULL;

    //Com_Printf ("S_LoadSound: %x\n", (int)stackbuf);
    // load it in
    if (s->truename)
        name = s->truename;
    else
        name = s->name;

    if (name[0] == '#')
        strcpy(namebuffer, &name[1]);
    else
        Com_sprintf(namebuffer, sizeof(namebuffer), "sound/%s", name);

    //	Com_Printf ("loading %s\n",namebuffer);

    // [PS2_QUAKE]: a sound first asked for while a level runs loads in the background: the read
    // alone was 58 ms from the HDD and 147 ms from a USB stick in the frame that asked. Level
    // loads (s_registering, and S_EndRegistration's pass) stay synchronous under the plaque. When
    // the loader can't take it - off, missing file, no memory - the synchronous path below runs.
    if (!s_registering && PS2_SoundLoadAsync(s, namebuffer))
        return NULL;

    size = FS_LoadFile(namebuffer, (void **)&data);

    if (!data)
    {
        Com_DPrintf("Couldn't load %s\n", namebuffer);
        return NULL;
    }

    sc = S_FinishLoadSound(s, data, size); // [PS2_QUAKE]
    FS_FreeFile(data);
    return sc;
}

/*
==============
S_FinishLoadSound

[PS2_QUAKE]: the second half of S_LoadSound, on a WAV image already in memory: shared by the
synchronous path above and by the background loader's pump, which calls it through
PS2_SoundFinishLoad once the read has landed. 'data' stays the caller's.
==============
*/
sfxcache_t * S_FinishLoadSound(sfx_t * s, byte * data, int size)
{
    wavinfo_t info;
    int len;
    int outwidth;
    float stepscale;
    sfxcache_t * sc;

    info = GetWavinfo(s->name, data, size);
    if (info.channels != 1)
    {
        Com_Printf("%s is a stereo sample\n", s->name);
        return NULL;
    }

    stepscale = (float)info.rate / dma.speed;
    len = info.samples / stepscale;

    // [PS2_QUAKE] 2026-08-27
    // Size this with the width ResampleSfx will actually WRITE, not the width the
    // source file happens to have. With s_loadas8bit set (the default here) it
    // stores one byte per sample regardless of a 16-bit source, so using
    // info.width allocated exactly twice what was ever touched - and since every
    // shipped Quake II sound is 16-bit, that wasted half of ~8 MB of sound cache
    // on a 32 MB console, which was enough on its own to fail a level load.
    // S_EndRegistration already measures the cache the same way this does now
    // (cache->length * cache->width), so the two agree.
    outwidth = s_loadas8bit->value ? 1 : info.width;
    len = len * outwidth * info.channels;

    // Not Z_Malloc: the decoded sound cache is one of the biggest pools in the
    // game, and inside the zone it was invisible behind the generic "Quake"
    // memtag. Tagging it ps2::heap::MemTag::Audio puts it on its own row in the memory
    // overlay. Freed by S_FreeSoundCache, never Z_Free.
    sc = s->cache = PS2_AudioMalloc(len + sizeof(sfxcache_t));
    if (!sc)
    {
        return NULL;
    }

    sc->length = info.samples;
    sc->loopstart = info.loopstart;
    sc->speed = info.rate;
    sc->width = info.width;
    sc->stereo = info.channels;

    ResampleSfx(s, sc->speed, sc->width, data + info.dataofs);

    return sc;
}

// [PS2_QUAKE]: the pump's entry (ps2/audio/sound_load.h), with the types it can name.
int PS2_SoundFinishLoad(struct sfx_s * sfx, unsigned char * data, int size)
{
    return S_FinishLoadSound(sfx, data, size) != NULL;
}

/*
===============================================================================

WAV loading

===============================================================================
*/

byte * data_p;
byte * iff_end;
byte * last_chunk;
byte * iff_data;
int iff_chunk_len;

short GetLittleShort(void)
{
    short val = 0;
    val = *data_p;
    val = val + (*(data_p + 1) << 8);
    data_p += 2;
    return val;
}

int GetLittleLong(void)
{
    int val = 0;
    val = *data_p;
    val = val + (*(data_p + 1) << 8);
    val = val + (*(data_p + 2) << 16);
    val = val + (*(data_p + 3) << 24);
    data_p += 4;
    return val;
}

void FindNextChunk(char * name)
{
    while (1)
    {
        data_p = last_chunk;

        if (data_p >= iff_end)
        { // didn't find the chunk
            data_p = NULL;
            return;
        }

        data_p += 4;
        iff_chunk_len = GetLittleLong();
        if (iff_chunk_len < 0)
        {
            data_p = NULL;
            return;
        }
        //		if (iff_chunk_len > 1024*1024)
        //			Sys_Error ("FindNextChunk: %i length is past the 1 meg sanity limit", iff_chunk_len);
        data_p -= 8;
        last_chunk = data_p + 8 + ((iff_chunk_len + 1) & ~1);
        if (!strncmp(data_p, name, 4))
            return;
    }
}

void FindChunk(char * name)
{
    last_chunk = iff_data;
    FindNextChunk(name);
}

void DumpChunks(void)
{
    char str[5];

    str[4] = 0;
    data_p = iff_data;
    do
    {
        memcpy(str, data_p, 4);
        data_p += 4;
        iff_chunk_len = GetLittleLong();
        Com_Printf("0x%x : %s (%d)\n", (int)(data_p - 4), str, iff_chunk_len);
        data_p += (iff_chunk_len + 1) & ~1;
    } while (data_p < iff_end);
}

/*
============
GetWavinfo
============
*/
wavinfo_t GetWavinfo(char * name, byte * wav, int wavlength)
{
    wavinfo_t info;
    int i;
    int format;
    int samples;

    memset(&info, 0, sizeof(info));

    if (!wav)
        return info;

    iff_data = wav;
    iff_end = wav + wavlength;

    // find "RIFF" chunk
    FindChunk("RIFF");
    if (!(data_p && !strncmp(data_p + 8, "WAVE", 4)))
    {
        Com_Printf("Missing RIFF/WAVE chunks\n");
        return info;
    }

    // get "fmt " chunk
    iff_data = data_p + 12;
    // DumpChunks ();

    FindChunk("fmt ");
    if (!data_p)
    {
        Com_Printf("Missing fmt chunk\n");
        return info;
    }
    data_p += 8;
    format = GetLittleShort();
    if (format != 1)
    {
        Com_Printf("Microsoft PCM format only\n");
        return info;
    }

    info.channels = GetLittleShort();
    info.rate = GetLittleLong();
    data_p += 4 + 2;
    info.width = GetLittleShort() / 8;

    // get cue chunk
    FindChunk("cue ");
    if (data_p)
    {
        data_p += 32;
        info.loopstart = GetLittleLong();
        //		Com_Printf("loopstart=%d\n", sfx->loopstart);

        // if the next chunk is a LIST chunk, look for a cue length marker
        FindNextChunk("LIST");
        if (data_p)
        {
            if (!strncmp(data_p + 28, "mark", 4))
            { // this is not a proper parse, but it works with cooledit...
                data_p += 24;
                i = GetLittleLong(); // samples in loop
                info.samples = info.loopstart + i;
                //				Com_Printf("looped length: %i\n", i);
            }
        }
    }
    else
        info.loopstart = -1;

    // find data chunk
    FindChunk("data");
    if (!data_p)
    {
        Com_Printf("Missing data chunk\n");
        return info;
    }

    data_p += 4;
    samples = GetLittleLong() / info.width;

    if (info.samples)
    {
        if (samples < info.samples)
            Com_Error(ERR_DROP, "Sound %s has a bad loop length", name);
    }
    else
        info.samples = samples;

    info.dataofs = data_p - wav;

    return info;
}
