
/* ================================================================================================
 * -*- C -*-
 * File: sys_ps2.c
 * Author: Guilherme R. Lampert
 * Created on: 13/10/15
 * Brief: Implementation of Quake2 Sys_ functions (system module) for the PS2.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include "common/q_common.h"
#include "ps2/debug_print.h"
#include "ps2/mem_alloc.h"
#include "ps2/defs_ps2.h"
#include "game/game.h" // For GetGameAPI()

// PS2DEV SDK:
#include <kernel.h>
#include <smod.h>
#include <sifrpc.h>
#include <loadfile.h>

// ref_ps2.c
extern void PS2_RendererShutdown(void);

// The program code and static data will use a fair slice of
// the main memory at all times. This is a rough estimate of that.
enum
{
    PROG_MEGABYTES = 2
};

//=============================================================================
//
// System init/shutdown and misc helpers:
//
//=============================================================================

/*
================
Sys_LoadIOPModules
================
*/
void Sys_LoadIOPModules(void)
{
    //
    // Notice that the IRX modules are declared as 'extern void'.
    // This seems like the best declaration, since they are not
    // quite the same as arrays of bytes. Check out this discussion:
    //   http://stackoverflow.com/q/10038964/1198654
    // Emphasis to this answer:
    //   http://stackoverflow.com/a/19054875/1198654
    //

    int result;

    // usbd.irx:
    extern void usbd_irx;
    extern int size_usbd_irx;
    result = SifExecModuleBuffer(&usbd_irx, size_usbd_irx, 0, NULL, NULL);
    if (result <= 0)
    {
        Sys_Error("Failed to load IOP module usbd! %d", result);
    }

    // Give the IOP a moment to be sure the modules are ready.
    nopdelay();
}

/*
================
Sys_Init
================
*/
void Sys_Init(void)
{
    // We can fire off the SIF and IO services
    // here to ensure a known initialization point,
    // but this is not strictly necessary. These
    // are lazily called by the PS2DEV SDK otherwise.
    SifInitRpc(0);
    fioInit();

    // Load the built-in IOP modules we need for the game.
    Sys_LoadIOPModules();

    // Add our estimate of the amount of memory used to allocate
    // the program executable and all the prog data:
    PS2_TagsAddMem(MEMTAG_MISC, PROG_MEGABYTES * 1024 * 1024);
}

/*
================
Sys_Error
================
*/
void Sys_Error(const char * error, ...)
{
    va_list argptr;
    char tempbuff[2048];

    va_start(argptr, error);
    vsnprintf(tempbuff, sizeof(tempbuff), error, argptr);
    tempbuff[sizeof(tempbuff) - 1] = '\0';
    va_end(argptr);

    // Make sure no other rendering ops are in-flight,
    // since we are bringing up the crash screen.
    PS2_RendererShutdown();

    Dbg_ScrInit();
    Dbg_ScrSetTextColor(0xFF0000FF); // red text
    Dbg_ScrPrintf("-------------------------------\n");
    Dbg_ScrPrintf("Sys_Error: %s\n", tempbuff);
    Dbg_ScrPrintf("-------------------------------\n");

    for (;;) // (HCF - Halt and Catch Fire)
    {
        SleepThread();
    }
}

/*
================
Sys_Quit
================
*/
void Sys_Quit(void)
{
    // Shutdown the default rendering path
    // to bring up the crash screen.
    PS2_RendererShutdown();

    Dbg_ScrInit();
    Dbg_ScrSetTextColor(0xFF0000FF); // red text
    Dbg_ScrPrintf("\n*** Sys_Quit called! ***\n");

    for (;;) // (HCF - Halt and Catch Fire)
    {
        SleepThread();
    }
}

/*
================
Sys_PrintLoadedIOPModules
================
*/
void Sys_PrintLoadedIOPModules(int max_modules, void (*printer)(const char *, ...))
{
    static char module_name_str[128] PS2_ALIGN(64);

    smod_mod_info_t module_info;
    SifRpcReceiveData_t rpc_data;
    int evens_odds = 0, listed_count = 0;

    if (!smod_get_next_mod(NULL, &module_info))
    {
        printer("Error: Couldn't get module list!");
        return;
    }

    // Table header:
    // (Print two tables side-by side, since our console has very few lines).
    printer("|    IOP module name    | id |    IOP module name    | id |\n");
    module_name_str[64] = '\0';
    do
    {
        SyncDCache(module_name_str, module_name_str + 64);

        if (SifRpcGetOtherData(&rpc_data, module_info.name, module_name_str, 64, 0) >= 0)
        {
            if (module_name_str[0] == '\0')
            {
                // Unnamed module.
                strcpy(module_name_str, "???");
            }
            else
            {
                // Truncate to 21 chars, the size of the name column:
                module_name_str[21] = '\0';
            }

            // Print a table row (we print two tables side-by-side to save lines):
            if (!(evens_odds++ & 1))
            {
                printer("| %-21s | %-2u |", module_name_str, module_info.id);
            }
            else
            {
                printer(" %-21s | %-2u |\n", module_name_str, module_info.id);
            }

            if (++listed_count == max_modules)
            {
                break;
            }
        }
    } while (smod_get_next_mod(&module_info, &module_info) != 0);

    if (evens_odds & 1)
    {
        printer("\n");
    }
    printer(">> Listed %u modules\n", listed_count);
}

/*
================
Sys_UnloadGame
================
*/
void Sys_UnloadGame(void)
{
    // Nothing to do here, since we are
    // not dealing with dynamic link libraries.
}

/*
================
Sys_GetGameAPI
================
*/
void * Sys_GetGameAPI(void * parms)
{
    // In the original Quake2, id Software used a DLL for the
    // game code, while the Engine code was in the executable.
    // This function was where the engine loaded the game DLL
    // and then called GetGameAPI from the DLL. Since in this PS2
    // port we always statically link everything in the same ELF
    // executable, its easier to just forward this call to the
    // GetGameAPI function.
    return GetGameAPI((game_import_t *)parms);
}

/*
================
'curtime' is the value from last call to Sys_Milliseconds.
'sys_frame_time' is saved by each Sys_SendKeyEvents call.
================
*/
int curtime = 0;
unsigned int sys_frame_time = 0;

static inline int Sys_MillisecondsSinceStartup(void)
{
	return clock() / (CLOCKS_PER_SEC / 1000);
}

/*
================
Sys_Milliseconds
================
*/
int Sys_Milliseconds(void)
{
	static int baseTime = 0;
	static qboolean timerInitialized = false;

	if (!timerInitialized)
	{
		baseTime = Sys_MillisecondsSinceStartup();
		timerInitialized = true;
	}

	curtime = Sys_MillisecondsSinceStartup() - baseTime;
	return curtime;
}

/*
================
Sys_ConsoleInput
================
*/
char * Sys_ConsoleInput(void)
{
    return NULL; // Not available on PS2
}

/*
================
Sys_ConsoleOutput
================
*/
void Sys_ConsoleOutput(const char * string)
{
    (void)string;
    //Dbg_ScrPrintf("%s", string);
    // Reserve the debug screen for fatal error reporting only.
}

/*
================
Sys_SendKeyEvents
================
*/
void Sys_SendKeyEvents(void)
{
    sys_frame_time = Sys_Milliseconds();
}

/*
================
Sys_AppActivate
================
*/
void Sys_AppActivate(void)
{
    // Not available on PS2
}

/*
================
Sys_CopyProtect
================
*/
void Sys_CopyProtect(void)
{
    // Not available on PS2
}

/*
================
Sys_GetClipboardData
================
*/
char * Sys_GetClipboardData(void)
{
    return NULL; // Not available on PS2
}

/*
==============
Sys_HashString

OAT - One-At-a-Time hash of the input string.
See: https://en.wikipedia.org/wiki/Jenkins_hash_function
==============
*/
u32 Sys_HashString(const char * str)
{
    u32 hash = 0;

    while (*str != '\0')
    {
        hash += *str++;
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }

    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);

    return hash;
}

//=============================================================================
//
// Misc file system utilities:
//
//=============================================================================

/*
================
Sys_LoadBinaryFile

Remarks: Uses the fio PS2DEV SDK API.
================
*/
qboolean Sys_LoadBinaryFile(const char * filename, int * size_bytes, void ** data_ptr)
{
    int fd;
    int file_len;
    int num_read;
    void * file_data   = NULL;
    qboolean had_error = false;

    fd = fioOpen(filename, O_RDONLY);
    if (fd < 0)
    {
        had_error = true;
        goto BAIL;
    }

    file_len = fioLseek(fd, 0, SEEK_END);
    if (file_len <= 0)
    {
        had_error = true;
        goto BAIL;
    }

    fioLseek(fd, 0, SEEK_SET); // rewind

    // Alloc or fail with a Sys_Error.
    file_data = PS2_MemAlloc(file_len, MEMTAG_MISC);

    num_read = fioRead(fd, file_data, file_len);
    if (num_read != file_len)
    {
        had_error = true;
        goto BAIL;
    }

BAIL:

    if (fd >= 0)
    {
        fioClose(fd);
    }

    if (had_error)
    {
        if (file_data != NULL)
        {
            PS2_MemFree(file_data, file_len, MEMTAG_MISC);
            file_data = NULL;
        }
        file_len = 0;
    }

    *size_bytes = file_len;
    *data_ptr   = file_data;
    return !had_error;
}

/*
================
Sys_Mkdir
================
*/
void Sys_Mkdir(const char * path)
{
    (void)path;
    // Not available on PS2
}

/*
================
Sys_FindFirst
================
*/
char * Sys_FindFirst(const char * path, unsigned musthave, unsigned canthave)
{
    (void)path;
    (void)musthave;
    (void)canthave;
    return NULL; // Not available on PS2
}

/*
================
Sys_FindNext
================
*/
char * Sys_FindNext(unsigned musthave, unsigned canthave)
{
    (void)musthave;
    (void)canthave;
    return NULL; // Not available on PS2
}

/*
================
Sys_FindClose
================
*/
void Sys_FindClose(void)
{
    // Not available on PS2
}

//=============================================================================
//
// Fix C-library gaps on the PlayStation-2:
//
//=============================================================================

struct tm * localtime(const time_t * timep)
{
    // This is missing on the PS2
    (void)timep;
    static struct tm dummy;
    return &dummy;
}

int rename(const char * a, const char * b)
{
    // This is missing on the PS2 as well
    (void)a;
    (void)b;
    return -1;
}
