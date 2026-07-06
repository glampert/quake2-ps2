
/* ================================================================================================
 * -*- C -*-
 * File: debug_print.c
 * Author: Guilherme R. Lampert
 * Created on: 14/10/15
 * Brief: Very crude debug printing to screen (PS2). Only used for fatal error reporting and dev.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include "ps2/debug_print.h"
#include "ps2/defs_ps2.h"

// PS2DEV libraries:
#include <stdio.h>
#include <stdarg.h>
#include <tamtypes.h>
#include <sifcmd.h>
#include <kernel.h>
#include <osd_config.h>

//=============================================================================
//
// Screen printing local data/constants:
//
//=============================================================================

static int scr_is_init    = 0;
static int scr_curr_x     = 0;
static int scr_curr_y     = 0;
static u32 scr_text_color = 0xFFFFFFFF; // text: white
static u32 scr_bg_color   = 0x00000000; // background: black

// Defined at the end of this file.
extern const unsigned char scr_font_bitmap[];

typedef struct
{
    u64 dd0[6];
    u32 dw0[2];
    u64 dd1[1];
    u16 dh0[4];
    u64 dd2[21];
} scr_setup_data_t;

typedef struct
{
    u64 dd0[4];
    u32 dw0[1];
    u16 x, y;
    u64 dd1[1];
    u32 dw1[2];
    u64 dd2[5];
} scr_char_data_t;

/*
 * Following magic numbers are unknown to me.
 * These are the values used in the original debug
 * printing routines of PS2DEV SDK, they didn't
 * provide any comments on the meaning of these values, and
 * the variable names in the structs above also don't help...
 */

static scr_setup_data_t scr_setup_buffer PS2_ALIGN(16) = {
    { 0x100000000000800E, 0xE, 0xA0000, 0x4C, 0x8C, 0x4E },
    { 27648, 30976 },
    { 0x18 },
    { 0, 639, 0, 223 },
    { 0x40, 1, 0x1A, 1, 0x46, 0, 0x45, 0x70000,
      0x47, 0x30000, 0x47, 6, 0, 0x3F80000000000000,
      1, 0x79006C00, 5, 0x87009400, 5, 0x70000, 0x47 }
};

static scr_char_data_t scr_char_buffer PS2_ALIGN(16) = {
    { 0x1000000000000004, 0xE, 0xA000000000000, 0x50 },
    { 0 },
    0, 0,
    { 0x51 },
    { DBG_SCR_CHAR_SIZE, DBG_SCR_CHAR_SIZE },
    { 0x52, 0, 0x53, 0x800000000000010, 0  }
};

static u32 scr_charmap[DBG_SCR_CHAR_SIZE * DBG_SCR_CHAR_SIZE] PS2_ALIGN(16);

//=============================================================================
//
// Local helpers:
// Following code was based on debug printing functions from the PS2DEV SDK.
//
//=============================================================================

/*
================
DetectSignal
================
*/
static int DetectSignal(void)
{
    char romname[15] = {0};
    GetRomName(romname);
    return ((romname[4] == 'E') ? 1 : 0);
}

/*
================
InitGS
================
*/
static void InitGS(int a, int b, int c)
{
    *(vu64 *)0x12001000 = 0x200;
    GsPutIMR(0xFF00);
    SetGsCrt(a, b, c);
}

/*
================
SetVideoMode
================
*/
static void SetVideoMode(void)
{
    unsigned dma_addr;
    unsigned val1;
    unsigned val2;
    unsigned val3;
    unsigned val4;
    unsigned val4_lo;

    asm volatile("        .set push               \n"
                 "        .set noreorder          \n"
                 "        lui     %4, 0x001b      \n"
                 "        lui     %5, 0x0983      \n"
                 "        lui     %0, 0x1200      \n"
                 "        li      %2, 1           \n"
                 "        ori     %4, %4, 0xf9ff  \n"
                 "        ori     %5, %5, 0x227c  \n"
                 "        li      %1, 0xff62      \n"
                 "        dsll32  %4, %4, 0       \n"
                 "        li      %3, 0x1400      \n"
                 "        sd      %1, 0(%0)       \n"
                 "        or      %4, %4, %5      \n"
                 "        sd      %2, 0x20(%0)    \n"
                 "        sd      %3, 0x90(%0)    \n"
                 "        sd      %4, 0xa0(%0)    \n"
                 "        .set pop                \n"
                 : "=&r" (dma_addr), "=&r" (val1), "=&r" (val2),
                   "=&r" (val3),     "=&r" (val4), "=&r" (val4_lo));
}

/*
================
DMAWait
================
*/
static void DMAWait(void)
{
    unsigned dma_addr;
    unsigned status;

    asm volatile("        .set push               \n"
                 "        .set noreorder          \n"
                 "        lui   %0, 0x1001        \n"
                 "        lw    %1, -0x6000(%0)   \n"
                 "1:      andi  %1, %1, 0x100     \n"
                 "        nop                     \n"
                 "        nop                     \n"
                 "        nop                     \n"
                 "        nop                     \n"
                 "        bnel  %1, $0, 1b        \n"
                 "        lw    %1, -0x6000(%0)   \n"
                 "        .set pop                \n"
                 : "=&r" (dma_addr), "=&r" (status));
}

/*
================
DMAReset
================
*/
static void DMAReset(void)
{
    unsigned dma_addr;
    unsigned temp;

    asm volatile("        .set push               \n"
                 "        .set noreorder          \n"
                 "        lui   %0, 0x1001        \n"
                 "        sw    $0, -0x5f80(%0)   \n"
                 "        sw    $0, -0x5000(%0)   \n"
                 "        sw    $0, -0x5fd0(%0)   \n"
                 "        sw    $0, -0x5ff0(%0)   \n"
                 "        sw    $0, -0x5fb0(%0)   \n"
                 "        sw    $0, -0x5fc0(%0)   \n"
                 "        lui   %1, 0             \n"
                 "        ori   %1, %1, 0xff1f    \n"
                 "        sw    %1, -0x1ff0(%0)   \n"
                 "        lw    %1, -0x1ff0(%0)   \n"
                 "        andi  %1, %1, 0xff1f    \n"
                 "        sw    %1, -0x1ff0(%0)   \n"
                 "        sw    $0, -0x2000(%0)   \n"
                 "        sw    $0, -0x1fe0(%0)   \n"
                 "        sw    $0, -0x1fd0(%0)   \n"
                 "        sw    $0, -0x1fb0(%0)   \n"
                 "        sw    $0, -0x1fc0(%0)   \n"
                 "        lw    %1, -0x2000(%0)   \n"
                 "        ori   %1, %1, 1         \n"
                 "        sw    %1, -0x2000(%0)   \n"
                 "        .set pop                \n"
                 : "=&r" (dma_addr), "=&r" (temp));
}

/*
================
DMATransfer
================
*/
static void DMATransfer(void * addr, int size)
{
    /*
     * 'addr' is the address of the data to be transfered.
     * Address must be 16 bytes aligned.
     *
     * 'size' is the size (in 16 byte quadwords)
     * of the data to be transfered.
     */

    unsigned dma_addr;
    unsigned temp;

    asm volatile("        .set push               \n"
                 "        .set noreorder          \n"
                 "        lui   %0, 0x1001        \n"
                 "        sw    %3, -0x5fe0(%0)   \n"
                 "        sw    %2, -0x5ff0(%0)   \n"
                 "        li    %1, 0x101         \n"
                 "        sw    %1, -0x6000(%0)   \n"
                 "        .set pop                \n"
                 : "=&r" (dma_addr), "=&r" (temp)
                 : "r" (addr), "r" (size));
}

//=============================================================================
//
// Screen debug printing functions:
//
//=============================================================================

/*
================
Dbg_ScrInit
================
*/
void Dbg_ScrInit(void)
{
    DMAReset();
    InitGS(1, (DetectSignal() == 1 ? 3 : 2), 1);

    SetVideoMode();

    DMAWait();
    DMATransfer(&scr_setup_buffer, sizeof(scr_setup_buffer) / 16);

    scr_is_init = 1;
}

/*
================
Dbg_ScrPrintChar
================
*/
void Dbg_ScrPrintChar(int x, int y, unsigned int color, int ch)
{
    if (x < 0 || x >= DBG_SCR_MAX_X || y < 0 || y >= DBG_SCR_MAX_Y)
    {
        return; // Invalid screen index.
    }

    if (!scr_is_init)
    {
        Dbg_ScrInit();
    }

    int i, j, l;
    unsigned int pixel;
    const unsigned char * font = &scr_font_bitmap[ch * DBG_SCR_CHAR_SIZE];

    // Offset a little in the sides and between each char, so they don't bunch-up.
    ((scr_char_data_t *)UNCACHED_SEG(&scr_char_buffer))->x = x * (DBG_SCR_CHAR_SIZE + 2) + 2;
    ((scr_char_data_t *)UNCACHED_SEG(&scr_char_buffer))->y = y * (DBG_SCR_CHAR_SIZE + 2) + 2;

    DMATransfer(&scr_char_buffer, sizeof(scr_char_buffer) / 16);
    for (i = l = 0; i < DBG_SCR_CHAR_SIZE; ++i, l += DBG_SCR_CHAR_SIZE, ++font)
    {
        for (j = 0; j < DBG_SCR_CHAR_SIZE; ++j)
        {
            if ((*font & (128 >> j)))
            {
                pixel = color;
            }
            else
            {
                pixel = scr_bg_color;
            }
            *(u32 *)UNCACHED_SEG(&scr_charmap[l + j]) = pixel;
        }
    }
    DMAWait();

    DMATransfer(scr_charmap, sizeof(scr_charmap) / 16);
    DMAWait();
}

/*
================
Dbg_ScrPrintf
================
*/
void Dbg_ScrPrintf(const char * format, ...)
{
    if (!scr_is_init)
    {
        Dbg_ScrInit();
    }

    // 'static' should avoid stressing the stack with this large buffer.
    // This function should be robust enough to be used for most error
    // reporting situations.
    static char tempbuff[2048];

    va_list argptr;
    int i, j, c, bufsz;

    va_start(argptr, format);
    bufsz = vsnprintf(tempbuff, sizeof(tempbuff), format, argptr);
    va_end(argptr);

    for (i = 0; i < bufsz; ++i)
    {
        c = tempbuff[i];
        switch (c)
        {
        case '\n':
            scr_curr_x = 0;
            ++scr_curr_y;
            if (scr_curr_y == DBG_SCR_MAX_Y)
            {
                scr_curr_y = 0;
            }
            Dbg_ScrClearLine(scr_curr_y);
            break;

        case '\t':
            for (j = 0; j < 4; ++j) // 4 spaces per TAB
            {
                Dbg_ScrPrintChar(scr_curr_x, scr_curr_y, scr_text_color, ' ');
                ++scr_curr_x;
            }
            break;

        default:
            Dbg_ScrPrintChar(scr_curr_x, scr_curr_y, scr_text_color, c);
            ++scr_curr_x;
            if (scr_curr_x == DBG_SCR_MAX_X)
            {
                scr_curr_x = 0;
                ++scr_curr_y;
                if (scr_curr_y == DBG_SCR_MAX_Y)
                {
                    scr_curr_y = 0;
                }
                Dbg_ScrClearLine(scr_curr_y);
            }
            break;
        } // switch (c)
    }
}

/*
================
Dbg_ScrGetPrintPosX
================
*/
int Dbg_ScrGetPrintPosX(void)
{
    return scr_curr_x;
}

/*
================
Dbg_ScrGetPrintPosY
================
*/
int Dbg_ScrGetPrintPosY(void)
{
    return scr_curr_y;
}

/*
================
Dbg_ScrSetPrintPos
================
*/
void Dbg_ScrSetPrintPos(int x, int y)
{
    if (x >= 0 && x < DBG_SCR_MAX_X)
    {
        scr_curr_x = x;
    }
    if (y >= 0 && y < DBG_SCR_MAX_Y)
    {
        scr_curr_y = y;
    }
}

/*
================
Dbg_ScrSetBgColor
================
*/
void Dbg_ScrSetBgColor(unsigned int color)
{
    scr_bg_color = color;
}

/*
================
Dbg_ScrGetBgColor
================
*/
unsigned int Dbg_ScrGetBgColor(void)
{
    return scr_bg_color;
}

/*
================
Dbg_ScrSetTextColor
================
*/
void Dbg_ScrSetTextColor(unsigned int color)
{
    scr_text_color = color;
}

/*
================
Dbg_ScrGetTextColor
================
*/
unsigned int Dbg_ScrGetTextColor(void)
{
    return scr_text_color;
}

/*
================
Dbg_ScrClear
================
*/
void Dbg_ScrClear(void)
{
    int y;
    for (y = 0; y < DBG_SCR_MAX_Y; ++y)
    {
        Dbg_ScrClearLine(y);
    }

    scr_curr_x = 0;
    scr_curr_y = 0;
}

/*
================
Dbg_ScrClearLine
================
*/
void Dbg_ScrClearLine(int y)
{
    int x;
    for (x = 0; x < DBG_SCR_MAX_X; ++x)
    {
        Dbg_ScrPrintChar(x, y, scr_bg_color, ' ');
    }
}

//=============================================================================
//
// The debug printing bitmap font:
//
//=============================================================================

const unsigned char scr_font_bitmap[] PS2_ALIGN(16) =
"\x00\x00\x00\x00\x00\x00\x00\x00\x3c\x42\xa5\x81\xa5\x99\x42\x3c"
"\x3c\x7e\xdb\xff\xff\xdb\x66\x3c\x6c\xfe\xfe\xfe\x7c\x38\x10\x00"
"\x10\x38\x7c\xfe\x7c\x38\x10\x00\x10\x38\x54\xfe\x54\x10\x38\x00"
"\x10\x38\x7c\xfe\xfe\x10\x38\x00\x00\x00\x00\x30\x30\x00\x00\x00"
"\xff\xff\xff\xe7\xe7\xff\xff\xff\x38\x44\x82\x82\x82\x44\x38\x00"
"\xc7\xbb\x7d\x7d\x7d\xbb\xc7\xff\x0f\x03\x05\x79\x88\x88\x88\x70"
"\x38\x44\x44\x44\x38\x10\x7c\x10\x30\x28\x24\x24\x28\x20\xe0\xc0"
"\x3c\x24\x3c\x24\x24\xe4\xdc\x18\x10\x54\x38\xee\x38\x54\x10\x00"
"\x10\x10\x10\x7c\x10\x10\x10\x10\x10\x10\x10\xff\x00\x00\x00\x00"
"\x00\x00\x00\xff\x10\x10\x10\x10\x10\x10\x10\xf0\x10\x10\x10\x10"
"\x10\x10\x10\x1f\x10\x10\x10\x10\x10\x10\x10\xff\x10\x10\x10\x10"
"\x10\x10\x10\x10\x10\x10\x10\x10\x00\x00\x00\xff\x00\x00\x00\x00"
"\x00\x00\x00\x1f\x10\x10\x10\x10\x00\x00\x00\xf0\x10\x10\x10\x10"
"\x10\x10\x10\x1f\x00\x00\x00\x00\x10\x10\x10\xf0\x00\x00\x00\x00"
"\x81\x42\x24\x18\x18\x24\x42\x81\x01\x02\x04\x08\x10\x20\x40\x80"
"\x80\x40\x20\x10\x08\x04\x02\x01\x00\x10\x10\xff\x10\x10\x00\x00"
"\x00\x00\x00\x00\x00\x00\x00\x00\x20\x20\x20\x20\x00\x00\x20\x00"
"\x50\x50\x50\x00\x00\x00\x00\x00\x50\x50\xf8\x50\xf8\x50\x50\x00"
"\x20\x78\xa0\x70\x28\xf0\x20\x00\xc0\xc8\x10\x20\x40\x98\x18\x00"
"\x40\xa0\x40\xa8\x90\x98\x60\x00\x10\x20\x40\x00\x00\x00\x00\x00"
"\x10\x20\x40\x40\x40\x20\x10\x00\x40\x20\x10\x10\x10\x20\x40\x00"
"\x20\xa8\x70\x20\x70\xa8\x20\x00\x00\x20\x20\xf8\x20\x20\x00\x00"
"\x00\x00\x00\x00\x00\x20\x20\x40\x00\x00\x00\x78\x00\x00\x00\x00"
"\x00\x00\x00\x00\x00\x60\x60\x00\x00\x00\x08\x10\x20\x40\x80\x00"
"\x70\x88\x98\xa8\xc8\x88\x70\x00\x20\x60\xa0\x20\x20\x20\xf8\x00"
"\x70\x88\x08\x10\x60\x80\xf8\x00\x70\x88\x08\x30\x08\x88\x70\x00"
"\x10\x30\x50\x90\xf8\x10\x10\x00\xf8\x80\xe0\x10\x08\x10\xe0\x00"
"\x30\x40\x80\xf0\x88\x88\x70\x00\xf8\x88\x10\x20\x20\x20\x20\x00"
"\x70\x88\x88\x70\x88\x88\x70\x00\x70\x88\x88\x78\x08\x10\x60\x00"
"\x00\x00\x20\x00\x00\x20\x00\x00\x00\x00\x20\x00\x00\x20\x20\x40"
"\x18\x30\x60\xc0\x60\x30\x18\x00\x00\x00\xf8\x00\xf8\x00\x00\x00"
"\xc0\x60\x30\x18\x30\x60\xc0\x00\x70\x88\x08\x10\x20\x00\x20\x00"
"\x70\x88\x08\x68\xa8\xa8\x70\x00\x20\x50\x88\x88\xf8\x88\x88\x00"
"\xf0\x48\x48\x70\x48\x48\xf0\x00\x30\x48\x80\x80\x80\x48\x30\x00"
"\xe0\x50\x48\x48\x48\x50\xe0\x00\xf8\x80\x80\xf0\x80\x80\xf8\x00"
"\xf8\x80\x80\xf0\x80\x80\x80\x00\x70\x88\x80\xb8\x88\x88\x70\x00"
"\x88\x88\x88\xf8\x88\x88\x88\x00\x70\x20\x20\x20\x20\x20\x70\x00"
"\x38\x10\x10\x10\x90\x90\x60\x00\x88\x90\xa0\xc0\xa0\x90\x88\x00"
"\x80\x80\x80\x80\x80\x80\xf8\x00\x88\xd8\xa8\xa8\x88\x88\x88\x00"
"\x88\xc8\xc8\xa8\x98\x98\x88\x00\x70\x88\x88\x88\x88\x88\x70\x00"
"\xf0\x88\x88\xf0\x80\x80\x80\x00\x70\x88\x88\x88\xa8\x90\x68\x00"
"\xf0\x88\x88\xf0\xa0\x90\x88\x00\x70\x88\x80\x70\x08\x88\x70\x00"
"\xf8\x20\x20\x20\x20\x20\x20\x00\x88\x88\x88\x88\x88\x88\x70\x00"
"\x88\x88\x88\x88\x50\x50\x20\x00\x88\x88\x88\xa8\xa8\xd8\x88\x00"
"\x88\x88\x50\x20\x50\x88\x88\x00\x88\x88\x88\x70\x20\x20\x20\x00"
"\xf8\x08\x10\x20\x40\x80\xf8\x00\x70\x40\x40\x40\x40\x40\x70\x00"
"\x00\x00\x80\x40\x20\x10\x08\x00\x70\x10\x10\x10\x10\x10\x70\x00"
"\x20\x50\x88\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xf8\x00"
"\x40\x20\x10\x00\x00\x00\x00\x00\x00\x00\x70\x08\x78\x88\x78\x00"
"\x80\x80\xb0\xc8\x88\xc8\xb0\x00\x00\x00\x70\x88\x80\x88\x70\x00"
"\x08\x08\x68\x98\x88\x98\x68\x00\x00\x00\x70\x88\xf8\x80\x70\x00"
"\x10\x28\x20\xf8\x20\x20\x20\x00\x00\x00\x68\x98\x98\x68\x08\x70"
"\x80\x80\xf0\x88\x88\x88\x88\x00\x20\x00\x60\x20\x20\x20\x70\x00"
"\x10\x00\x30\x10\x10\x10\x90\x60\x40\x40\x48\x50\x60\x50\x48\x00"
"\x60\x20\x20\x20\x20\x20\x70\x00\x00\x00\xd0\xa8\xa8\xa8\xa8\x00"
"\x00\x00\xb0\xc8\x88\x88\x88\x00\x00\x00\x70\x88\x88\x88\x70\x00"
"\x00\x00\xb0\xc8\xc8\xb0\x80\x80\x00\x00\x68\x98\x98\x68\x08\x08"
"\x00\x00\xb0\xc8\x80\x80\x80\x00\x00\x00\x78\x80\xf0\x08\xf0\x00"
"\x40\x40\xf0\x40\x40\x48\x30\x00\x00\x00\x90\x90\x90\x90\x68\x00"
"\x00\x00\x88\x88\x88\x50\x20\x00\x00\x00\x88\xa8\xa8\xa8\x50\x00"
"\x00\x00\x88\x50\x20\x50\x88\x00\x00\x00\x88\x88\x98\x68\x08\x70"
"\x00\x00\xf8\x10\x20\x40\xf8\x00\x18\x20\x20\x40\x20\x20\x18\x00"
"\x20\x20\x20\x00\x20\x20\x20\x00\xc0\x20\x20\x10\x20\x20\xc0\x00"
"\x40\xa8\x10\x00\x00\x00\x00\x00\x00\x00\x20\x50\xf8\x00\x00\x00"
"\x70\x88\x80\x80\x88\x70\x20\x60\x90\x00\x00\x90\x90\x90\x68\x00"
"\x10\x20\x70\x88\xf8\x80\x70\x00\x20\x50\x70\x08\x78\x88\x78\x00"
"\x48\x00\x70\x08\x78\x88\x78\x00\x20\x10\x70\x08\x78\x88\x78\x00"
"\x20\x00\x70\x08\x78\x88\x78\x00\x00\x70\x80\x80\x80\x70\x10\x60"
"\x20\x50\x70\x88\xf8\x80\x70\x00\x50\x00\x70\x88\xf8\x80\x70\x00"
"\x20\x10\x70\x88\xf8\x80\x70\x00\x50\x00\x00\x60\x20\x20\x70\x00"
"\x20\x50\x00\x60\x20\x20\x70\x00\x40\x20\x00\x60\x20\x20\x70\x00"
"\x50\x00\x20\x50\x88\xf8\x88\x00\x20\x00\x20\x50\x88\xf8\x88\x00"
"\x10\x20\xf8\x80\xf0\x80\xf8\x00\x00\x00\x6c\x12\x7e\x90\x6e\x00"
"\x3e\x50\x90\x9c\xf0\x90\x9e\x00\x60\x90\x00\x60\x90\x90\x60\x00"
"\x90\x00\x00\x60\x90\x90\x60\x00\x40\x20\x00\x60\x90\x90\x60\x00"
"\x40\xa0\x00\xa0\xa0\xa0\x50\x00\x40\x20\x00\xa0\xa0\xa0\x50\x00"
"\x90\x00\x90\x90\xb0\x50\x10\xe0\x50\x00\x70\x88\x88\x88\x70\x00"
"\x50\x00\x88\x88\x88\x88\x70\x00\x20\x20\x78\x80\x80\x78\x20\x20"
"\x18\x24\x20\xf8\x20\xe2\x5c\x00\x88\x50\x20\xf8\x20\xf8\x20\x00"
"\xc0\xa0\xa0\xc8\x9c\x88\x88\x8c\x18\x20\x20\xf8\x20\x20\x20\x40"
"\x10\x20\x70\x08\x78\x88\x78\x00\x10\x20\x00\x60\x20\x20\x70\x00"
"\x20\x40\x00\x60\x90\x90\x60\x00\x20\x40\x00\x90\x90\x90\x68\x00"
"\x50\xa0\x00\xa0\xd0\x90\x90\x00\x28\x50\x00\xc8\xa8\x98\x88\x00"
"\x00\x70\x08\x78\x88\x78\x00\xf8\x00\x60\x90\x90\x90\x60\x00\xf0"
"\x20\x00\x20\x40\x80\x88\x70\x00\x00\x00\x00\xf8\x80\x80\x00\x00"
"\x00\x00\x00\xf8\x08\x08\x00\x00\x84\x88\x90\xa8\x54\x84\x08\x1c"
"\x84\x88\x90\xa8\x58\xa8\x3c\x08\x20\x00\x00\x20\x20\x20\x20\x00"
"\x00\x00\x24\x48\x90\x48\x24\x00\x00\x00\x90\x48\x24\x48\x90\x00"
"\x28\x50\x20\x50\x88\xf8\x88\x00\x28\x50\x70\x08\x78\x88\x78\x00"
"\x28\x50\x00\x70\x20\x20\x70\x00\x28\x50\x00\x20\x20\x20\x70\x00"
"\x28\x50\x00\x70\x88\x88\x70\x00\x50\xa0\x00\x60\x90\x90\x60\x00"
"\x28\x50\x00\x88\x88\x88\x70\x00\x50\xa0\x00\xa0\xa0\xa0\x50\x00"
"\xfc\x48\x48\x48\xe8\x08\x50\x20\x00\x50\x00\x50\x50\x50\x10\x20"
"\xc0\x44\xc8\x54\xec\x54\x9e\x04\x10\xa8\x40\x00\x00\x00\x00\x00"
"\x00\x20\x50\x88\x50\x20\x00\x00\x88\x10\x20\x40\x80\x28\x00\x00"
"\x7c\xa8\xa8\x68\x28\x28\x28\x00\x38\x40\x30\x48\x48\x30\x08\x70"
"\x00\x00\x00\x00\x00\x00\xff\xff\xf0\xf0\xf0\xf0\x0f\x0f\x0f\x0f"
"\x00\x00\xff\xff\xff\xff\xff\xff\xff\xff\x00\x00\x00\x00\x00\x00"
"\x00\x00\x00\x3c\x3c\x00\x00\x00\xff\xff\xff\xff\xff\xff\x00\x00"
"\xc0\xc0\xc0\xc0\xc0\xc0\xc0\xc0\x0f\x0f\x0f\x0f\xf0\xf0\xf0\xf0"
"\xfc\xfc\xfc\xfc\xfc\xfc\xfc\xfc\x03\x03\x03\x03\x03\x03\x03\x03"
"\x3f\x3f\x3f\x3f\x3f\x3f\x3f\x3f\x11\x22\x44\x88\x11\x22\x44\x88"
"\x88\x44\x22\x11\x88\x44\x22\x11\xfe\x7c\x38\x10\x00\x00\x00\x00"
"\x00\x00\x00\x00\x10\x38\x7c\xfe\x80\xc0\xe0\xf0\xe0\xc0\x80\x00"
"\x01\x03\x07\x0f\x07\x03\x01\x00\xff\x7e\x3c\x18\x18\x3c\x7e\xff"
"\x81\xc3\xe7\xff\xff\xe7\xc3\x81\xf0\xf0\xf0\xf0\x00\x00\x00\x00"
"\x00\x00\x00\x00\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x00\x00\x00\x00"
"\x00\x00\x00\x00\xf0\xf0\xf0\xf0\x33\x33\xcc\xcc\x33\x33\xcc\xcc"
"\x00\x20\x20\x50\x50\x88\xf8\x00\x20\x20\x70\x20\x70\x20\x20\x00"
"\x00\x00\x00\x50\x88\xa8\x50\x00\xff\xff\xff\xff\xff\xff\xff\xff"
"\x00\x00\x00\x00\xff\xff\xff\xff\xf0\xf0\xf0\xf0\xf0\xf0\xf0\xf0"
"\x0f\x0f\x0f\x0f\x0f\x0f\x0f\x0f\xff\xff\xff\xff\x00\x00\x00\x00"
"\x00\x00\x68\x90\x90\x90\x68\x00\x30\x48\x48\x70\x48\x48\x70\xc0"
"\xf8\x88\x80\x80\x80\x80\x80\x00\xf8\x50\x50\x50\x50\x50\x98\x00"
"\xf8\x88\x40\x20\x40\x88\xf8\x00\x00\x00\x78\x90\x90\x90\x60\x00"
"\x00\x50\x50\x50\x50\x68\x80\x80\x00\x50\xa0\x20\x20\x20\x20\x00"
"\xf8\x20\x70\xa8\xa8\x70\x20\xf8\x20\x50\x88\xf8\x88\x50\x20\x00"
"\x70\x88\x88\x88\x50\x50\xd8\x00\x30\x40\x40\x20\x50\x50\x50\x20"
"\x00\x00\x00\x50\xa8\xa8\x50\x00\x08\x70\xa8\xa8\xa8\x70\x80\x00"
"\x38\x40\x80\xf8\x80\x40\x38\x00\x70\x88\x88\x88\x88\x88\x88\x00"
"\x00\xf8\x00\xf8\x00\xf8\x00\x00\x20\x20\xf8\x20\x20\x00\xf8\x00"
"\xc0\x30\x08\x30\xc0\x00\xf8\x00\x18\x60\x80\x60\x18\x00\xf8\x00"
"\x10\x28\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\x20\xa0\x40"
"\x00\x20\x00\xf8\x00\x20\x00\x00\x00\x50\xa0\x00\x50\xa0\x00\x00"
"\x00\x18\x24\x24\x18\x00\x00\x00\x00\x30\x78\x78\x30\x00\x00\x00"
"\x00\x00\x00\x00\x30\x00\x00\x00\x3e\x20\x20\x20\xa0\x60\x20\x00"
"\xa0\x50\x50\x50\x00\x00\x00\x00\x40\xa0\x20\x40\xe0\x00\x00\x00"
"\x00\x38\x38\x38\x38\x38\x38\x00\x00\x00\x00\x00\x00\x00\x00";

