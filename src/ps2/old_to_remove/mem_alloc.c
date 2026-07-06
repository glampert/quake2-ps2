
/* ================================================================================================
 * -*- C -*-
 * File: mem_alloc.c
 * Author: Guilherme R. Lampert
 * Created on: 18/10/15
 * Brief: Wrappers for malloc/free/memalign.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include "ps2/mem_alloc.h"
#include "game/q_shared.h" // For mem_hunk_t

#include <stdlib.h>
#include <string.h>

// For debug printing
const char * ps2_mem_tag_names[MEMTAG_COUNT] =
{
    "MISC",
    "QUAKE",
    "RENDERER",
    "TEX IMAGE",
    "MDL ALIAS",
    "MDL SPRITE",
    "MDL WORLD"
};

// Tags updated on every alloc/free.
ps2_mem_counters_t ps2_mem_tag_counts[MEMTAG_COUNT] = {0};

//=============================================================================
//
// malloc/free hooks:
//
//=============================================================================

/*
================
PS2_OutOfMemoryError

Prints the out-of-memory report and calls Sys_Error().
Never returns to the caller.
================
*/
static void PS2_OutOfMemoryError(int alloc_attempt_size, ps2_mem_tag_t tag)
{
    char tags_dump_str[2048] = {0};
    char * ptr = tags_dump_str;
    unsigned int i, mem_total = 0;

    ptr += sprintf(ptr, "Tag Name   Bytes      Allocs  Frees   Small   Large\n");

    for (i = 0; i < MEMTAG_COUNT; ++i)
    {
        mem_total += ps2_mem_tag_counts[i].total_bytes;
        const char * total_str = PS2_FormatMemoryUnit(ps2_mem_tag_counts[i].total_bytes, true);

        ptr += sprintf(ptr, "%-10s %-10s %-7d %-7d %-7d %-7d\n",
                ps2_mem_tag_names[i], total_str,
                ps2_mem_tag_counts[i].total_allocs,
                ps2_mem_tag_counts[i].total_frees,
                ps2_mem_tag_counts[i].smallest_alloc,
                ps2_mem_tag_counts[i].largest_alloc);
    }

    ptr += sprintf(ptr, "\nTOTAL MEM: %s", PS2_FormatMemoryUnit(mem_total, true));

    Sys_Error("\nOut-of-memory for %s! Failed to alloc %s\n\n"
              "\t\t***** Current memory tags *****\n"
              "%s",
              ps2_mem_tag_names[tag],
              PS2_FormatMemoryUnit(alloc_attempt_size, true),
              tags_dump_str);
}

/*
================
PS2_MemAlloc
================
*/
void * PS2_MemAlloc(int size_bytes, ps2_mem_tag_t tag)
{
    if (size_bytes <= 0)
    {
        Sys_Error("Trying to allocate zero or negative size (%d)!", size_bytes);
    }

    void * ptr = malloc(size_bytes);
    if (ptr == NULL)
    {
        PS2_OutOfMemoryError(size_bytes, tag);
    }

    ps2_mem_tag_counts[tag].total_bytes += size_bytes;
    ps2_mem_tag_counts[tag].total_allocs++;

    if ((size_bytes < ps2_mem_tag_counts[tag].smallest_alloc) ||
        (ps2_mem_tag_counts[tag].smallest_alloc == 0))
    {
        ps2_mem_tag_counts[tag].smallest_alloc = size_bytes;
    }

    if (size_bytes > ps2_mem_tag_counts[tag].largest_alloc)
    {
        ps2_mem_tag_counts[tag].largest_alloc = size_bytes;
    }

    return ptr;
}

/*
================
PS2_MemAllocAligned
================
*/
void * PS2_MemAllocAligned(int alignment, int size_bytes, ps2_mem_tag_t tag)
{
    if (size_bytes <= 0)
    {
        Sys_Error("Trying to allocate zero or negative size (%d)!", size_bytes);
    }
    if (alignment <= 0)
    {
        Sys_Error("PS2_MemAllocAligned: Bad alignment: %d!", alignment);
    }

    void * ptr = memalign(alignment, size_bytes);
    if (ptr == NULL)
    {
        PS2_OutOfMemoryError(size_bytes, tag);
    }

    ps2_mem_tag_counts[tag].total_bytes += size_bytes;
    ps2_mem_tag_counts[tag].total_allocs++;

    if ((size_bytes < ps2_mem_tag_counts[tag].smallest_alloc) ||
        (ps2_mem_tag_counts[tag].smallest_alloc == 0))
    {
        ps2_mem_tag_counts[tag].smallest_alloc = size_bytes;
    }

    if (size_bytes > ps2_mem_tag_counts[tag].largest_alloc)
    {
        ps2_mem_tag_counts[tag].largest_alloc = size_bytes;
    }

    return ptr;
}

/*
================
PS2_MemFree
================
*/
void PS2_MemFree(void * ptr, int size_bytes, ps2_mem_tag_t tag)
{
    if (ptr == NULL)
    {
        return;
    }

    ps2_mem_tag_counts[tag].total_bytes -= size_bytes;
    ps2_mem_tag_counts[tag].total_frees++;

    // Good for both malloc() and memalign().
    free(ptr);
}

/*
================
PS2_TagsAddMem
================
*/
void PS2_TagsAddMem(ps2_mem_tag_t tag, unsigned int size_bytes)
{
    ps2_mem_tag_counts[tag].total_bytes += size_bytes;
    ps2_mem_tag_counts[tag].largest_alloc  = ps2_mem_tag_counts[tag].total_bytes;
    ps2_mem_tag_counts[tag].smallest_alloc = ps2_mem_tag_counts[tag].total_bytes;
    ps2_mem_tag_counts[tag].total_allocs++;
}

/*
================
PS2_FormatMemoryUnit
================
*/
const char * PS2_FormatMemoryUnit(unsigned int size_bytes, int abbreviated)
{
    enum
    {
        KILOBYTE = 1024,
        MEGABYTE = 1024 * KILOBYTE,
        GIGABYTE = 1024 * MEGABYTE
    };

    const char * mem_unit_str;
    float adjusted_size;

    if (size_bytes < KILOBYTE)
    {
        mem_unit_str  = abbreviated ? "B" : "Bytes";
        adjusted_size = (float)size_bytes;
    }
    else if (size_bytes < MEGABYTE)
    {
        mem_unit_str  = abbreviated ? "KB" : "Kilobytes";
        adjusted_size = size_bytes / 1024.0f;
    }
    else if (size_bytes < GIGABYTE)
    {
        mem_unit_str  = abbreviated ? "MB" : "Megabytes";
        adjusted_size = size_bytes / 1024.0f / 1024.0f;
    }
    else
    {
        mem_unit_str  = abbreviated ? "GB" : "Gigabytes";
        adjusted_size = size_bytes / 1024.0f / 1024.0f / 1024.0f;
    }

    char * fmtbuf;
    int chars_written;
    char num_str_buf[100];

    // Max chars reserved for the output string, max `num_str_buf + 28` chars for the unit str:
    enum
    {
        MEM_UNIT_STR_MAXLEN = sizeof(num_str_buf) + 28
    };
    static int bufnum = 0;
    static char local_str_buf[8][MEM_UNIT_STR_MAXLEN];

    fmtbuf = local_str_buf[bufnum];
    bufnum = (bufnum + 1) & 7;

    // We only care about the first 2 decimal digs
    chars_written = snprintf(num_str_buf, 100, "%.2f", adjusted_size);
    if (chars_written <= 0)
    {
        fmtbuf[0] = '?';
        fmtbuf[1] = '?';
        fmtbuf[2] = '?';
        fmtbuf[3] = '\0';
        return fmtbuf;
    }

    // Remove trailing zeros if no significant decimal digits:
    char * p = num_str_buf;
    for (; *p; ++p)
    {
        if (*p == '.')
        {
            // Find the end of the string:
            while (*++p)
            {
            }
            // Remove trailing zeros:
            while (*--p == '0')
            {
                *p = '\0';
            }
            // If the dot was left alone at the end, remove it too.
            if (*p == '.')
            {
                *p = '\0';
            }
            break;
        }
    }

    // Consolidate the strings:
    chars_written = snprintf(fmtbuf, MEM_UNIT_STR_MAXLEN, "%s %s", num_str_buf, mem_unit_str);
    if (chars_written <= 0)
    {
        fmtbuf[0] = '?';
        fmtbuf[1] = '?';
        fmtbuf[2] = '?';
        fmtbuf[3] = '\0';
        return fmtbuf;
    }

    return fmtbuf;
}

//=============================================================================
//
// Hunk memory allocator (similar to a stack), used by the renderer:
//
//=============================================================================

/*
================
Hunk_New
================
*/
void Hunk_New(mem_hunk_t * hunk, int max_size, int mem_tag)
{
    hunk->curr_size = 0;
    hunk->max_size  = max_size;
    hunk->mem_tag   = mem_tag;
    hunk->base_ptr  = PS2_MemAlloc(max_size, (ps2_mem_tag_t)mem_tag);
    memset(hunk->base_ptr, 0, max_size);
}

/*
================
Hunk_Free
================
*/
void Hunk_Free(mem_hunk_t * hunk)
{
    if (hunk->base_ptr != NULL)
    {
        PS2_MemFree(hunk->base_ptr, hunk->max_size, (ps2_mem_tag_t)hunk->mem_tag);
        hunk->base_ptr  = NULL;
        hunk->curr_size = 0;
        hunk->max_size  = 0;
        hunk->mem_tag   = 0;
    }
}

/*
================
Hunk_BlockAlloc
================
*/
byte * Hunk_BlockAlloc(mem_hunk_t * hunk, int block_size)
{
    // LAMPERT: This is the way Win32 Quake does it, so I ain't changing it...
    block_size = (block_size + 31) & ~31; // round to cacheline

    // The hunk stack doesn't resize.
    hunk->curr_size += block_size;
    if (hunk->curr_size > hunk->max_size)
    {
        Sys_Error("Hunk_BlockAlloc: Overflowed with %s request!", PS2_FormatMemoryUnit(block_size, true));
    }

    return hunk->base_ptr + hunk->curr_size - block_size;
}

/*
================
Hunk_GetTail
================
*/
int Hunk_GetTail(mem_hunk_t * hunk)
{
    return hunk->curr_size;
}
