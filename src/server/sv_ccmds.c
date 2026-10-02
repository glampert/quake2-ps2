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

#include "server.h"

#include <ctype.h> // [PS2_QUAKE]: isalnum, SV_SaveNameIsValid

/*
===============================================================================

OPERATOR CONSOLE ONLY COMMANDS

These commands can only be entered from stdin or by a remote operator datagram
===============================================================================
*/

/*
====================
SV_SetMaster_f

Specify a list of master servers
====================
*/
void SV_SetMaster_f(void)
{
    int i, slot;

    // only dedicated servers send heartbeats
    if (!dedicated->value)
    {
        Com_Printf("Only dedicated servers use masters.\n");
        return;
    }

    // make sure the server is listed public
    Cvar_Set("public", "1");

    for (i = 1; i < MAX_MASTERS; i++)
    {
        memset(&master_adr[i], 0, sizeof(master_adr[i]));
    }

    slot = 1; // slot 0 will always contain the id master
    for (i = 1; i < Cmd_Argc(); i++)
    {
        if (slot == MAX_MASTERS)
            break;

        if (!NET_StringToAdr(Cmd_Argv(i), &master_adr[i]))
        {
            Com_Printf("Bad address: %s\n", Cmd_Argv(i));
            continue;
        }
        if (master_adr[slot].port == 0)
        {
            master_adr[slot].port = BigShort(PORT_MASTER);
        }

        Com_Printf("Master server at %s\n", NET_AdrToString(master_adr[slot]));

        Com_Printf("Sending a ping.\n");

        Netchan_OutOfBandPrint(NS_SERVER, master_adr[slot], "ping");

        slot++;
    }

    svs.last_heartbeat = -9999999;
}

/*
==================
SV_SetPlayer

Sets sv_client and sv_player to the player with idnum Cmd_Argv(1)
==================
*/
qboolean SV_SetPlayer(void)
{
    client_t * cl;
    int i;
    int idnum;
    const char * s;

    if (Cmd_Argc() < 2)
        return false;

    s = Cmd_Argv(1);

    // numeric values are just slot numbers
    if (s[0] >= '0' && s[0] <= '9')
    {
        idnum = atoi(Cmd_Argv(1));
        if (idnum < 0 || idnum >= maxclients->value)
        {
            Com_Printf("Bad client slot: %i\n", idnum);
            return false;
        }

        sv_client = &svs.clients[idnum];
        sv_player = sv_client->edict;
        if (!sv_client->state)
        {
            Com_Printf("Client %i is not active\n", idnum);
            return false;
        }
        return true;
    }

    // check for a name match
    for (i = 0, cl = svs.clients; i < maxclients->value; i++, cl++)
    {
        if (!cl->state)
            continue;
        if (!strcmp(cl->name, s))
        {
            sv_client = cl;
            sv_player = sv_client->edict;
            return true;
        }
    }

    Com_Printf("Userid %s is not on the server\n", s);
    return false;
}

/*
===============================================================================

SAVEGAME FILES

[PS2_QUAKE]: These files no longer live in <gamedir>/save/<savedir>/. "current" is
the save working set, kept in RAM, and any other savedir is a save slot on the
memory card or in host files; see Sys_SaveOpen in q_common.h and src/ps2/save/.
The file names and what goes in them are still id's.

===============================================================================
*/

// [PS2_QUAKE]: The comment SV_WriteServerFile wrote last, which a slot keeps for the menus.
static char sv_savecomment[32];

// [PS2_QUAKE]: Set once the player has been told an autosave failed; see SV_AutosaveFailed.
static qboolean sv_autosave_warned;

// [PS2_QUAKE]: Set while a save game is being read back; see SV_AbortSaveRead.
static qboolean sv_reading_save;

/*
=====================
SV_AbortSaveRead

[PS2_QUAKE]: Called by Com_Error when a drop unwinds the frame. Whatever read the
save never finishes, so the state is cleared here.
=====================
*/
qboolean SV_AbortSaveRead(void)
{
    const qboolean wasReading = sv_reading_save;
    sv_reading_save = false;
    return wasReading;
}

/*
=====================
SV_SaveReadBlock

[PS2_QUAKE]: A read that drops back to the console when the save runs short, where
FS_Read would halt the game (ERR_FATAL).
=====================
*/
static void SV_SaveReadBlock(void * buffer, int len, FILE * f)
{
    if (fread(buffer, 1, len, f) != (size_t)len)
        Com_Error(ERR_DROP, "The save game is damaged.");
}

/*
=====================
SV_SaveNameIsValid

[PS2_QUAKE]: A savedir becomes a file name on a memory card: short, and only plain
characters. Replaces id's check for "..", "/" and "\", which also forgot to return.
=====================
*/
static qboolean SV_SaveNameIsValid(const char * name)
{
    int len;

    for (len = 0; name[len]; len++)
    {
        if (!isalnum((unsigned char)name[len]) && name[len] != '_')
            return false;
    }
    return len > 0 && len <= 24;
}

/*
=====================
SV_WipeSavegame

Delete save/<XXX>/

[PS2_QUAKE]: Only "current" is ever wiped. A slot is replaced whole when written.
=====================
*/
void SV_WipeSavegame(const char * savename)
{
    Com_DPrintf("SV_WipeSaveGame(%s)\n", savename);

    if (!strcmp(savename, "current"))
        Sys_SaveClearCurrent();
}

/*
================
SV_CopySaveGame

[PS2_QUAKE]: Writes the working set out to a slot, or reads a slot into it, and
returns whether that worked; Sys_SaveLastError says why not.
================
*/
qboolean SV_CopySaveGame(const char * src, const char * dst)
{
    Com_DPrintf("SV_CopySaveGame(%s, %s)\n", src, dst);

    if (!strcmp(src, "current"))
        return Sys_SaveStoreSlot(dst, sv_savecomment);
    if (!strcmp(dst, "current"))
        return Sys_SaveRestoreSlot(src);

    Sys_SaveSetError("Save games can only be copied to or from 'current'.");
    return false;
}

/*
==============
SV_WriteLevelFile

[PS2_QUAKE]: Returns false if the level couldn't be saved, Sys_SaveLastError says why.
==============
*/
qboolean SV_WriteLevelFile(void)
{
    char name[MAX_OSPATH];
    FILE * f;

    Com_DPrintf("SV_WriteLevelFile()\n");

    Com_sprintf(name, sizeof(name), "%s.sv2", sv.name);
    f = Sys_SaveOpen(name, "wb");
    if (!f)
        return false;
    fwrite(sv.configstrings, sizeof(sv.configstrings), 1, f);
    CM_WritePortalState(f);
    if (!Sys_SaveClose(f, true))
        return false;

    Com_sprintf(name, sizeof(name), "%s.sav", sv.name);
    f = Sys_SaveOpen(name, "wb");
    if (!f)
        return false;
    if (!ge->WriteLevel(f))
    {
        Sys_SaveClose(f, false);
        if (!Sys_SaveLastError()[0])
            Sys_SaveSetError("Part of the level could not be saved (see the console).");
        return false;
    }
    return Sys_SaveClose(f, true);
}

/*
==============
SV_ReadLevelFile

==============
*/
void SV_ReadLevelFile(void)
{
    char name[MAX_OSPATH];
    FILE * f;
    const qboolean wasReading = sv_reading_save; // [PS2_QUAKE]

    Com_DPrintf("SV_ReadLevelFile()\n");

    sv_reading_save = true;

    Com_sprintf(name, sizeof(name), "%s.sv2", sv.name);
    f = Sys_SaveOpen(name, "rb");
    if (!f)
    {
        Com_Printf("Failed to open %s: %s\n", name, Sys_SaveLastError());
        sv_reading_save = wasReading;
        return;
    }
    SV_SaveReadBlock(sv.configstrings, sizeof(sv.configstrings), f);
    CM_ReadPortalState(f);
    Sys_SaveClose(f, true);

    Com_sprintf(name, sizeof(name), "%s.sav", sv.name);
    f = Sys_SaveOpen(name, "rb");
    if (!f)
        Com_Error(ERR_DROP, "%s", Sys_SaveLastError());
    ge->ReadLevel(f);
    Sys_SaveClose(f, true);

    sv_reading_save = wasReading;
}

/*
==============
SV_WriteServerFile

[PS2_QUAKE]: Returns false if the game couldn't be saved, Sys_SaveLastError says why.
==============
*/
qboolean SV_WriteServerFile(qboolean autosave)
{
    FILE * f;
    cvar_t * var;
    char name[MAX_OSPATH], string[128];
    char comment[32];
    time_t aclock;
    struct tm * newtime;

    Com_DPrintf("SV_WriteServerFile(%s)\n", autosave ? "true" : "false");

    f = Sys_SaveOpen("server.ssv", "wb");
    if (!f)
        return false;

    // write the comment field
    memset(comment, 0, sizeof(comment));

    if (!autosave)
    {
        time(&aclock);
        newtime = localtime(&aclock);
        Com_sprintf(comment, sizeof(comment), "%2i:%i%i %2i/%2i  ",
                    newtime->tm_hour, newtime->tm_min / 10, newtime->tm_min % 10,
                    newtime->tm_mon + 1, newtime->tm_mday);

        strncat(comment, sv.configstrings[CS_NAME], sizeof(comment) - 1 - strlen(comment));
    }
    else
    { // autosaved
        Com_sprintf(comment, sizeof(comment), "ENTERING %s", sv.configstrings[CS_NAME]);
    }

    memcpy(sv_savecomment, comment, sizeof(sv_savecomment)); // [PS2_QUAKE]
    fwrite(comment, 1, sizeof(comment), f);

    // write the mapcmd
    fwrite(svs.mapcmd, 1, sizeof(svs.mapcmd), f);

    // write all CVAR_LATCH cvars
    // these will be things like coop, skill, deathmatch, etc
    for (var = cvar_vars; var; var = var->next)
    {
        if (!(var->flags & CVAR_LATCH))
        {
            continue;
        }
        if (strlen(var->name) >= sizeof(name) - 1 || strlen(var->string) >= sizeof(string) - 1)
        {
            Com_Printf("Cvar too long: %s = %s\n", var->name, var->string);
            continue;
        }
        memset(name, 0, sizeof(name));
        memset(string, 0, sizeof(string));
        strcpy(name, var->name);
        strcpy(string, var->string);
        fwrite(name, 1, sizeof(name), f);
        fwrite(string, 1, sizeof(string), f);
    }

    if (!Sys_SaveClose(f, true))
        return false;

    // write game state
    f = Sys_SaveOpen("game.ssv", "wb");
    if (!f)
        return false;
    if (!ge->WriteGame(f, autosave))
    {
        Sys_SaveClose(f, false);
        if (!Sys_SaveLastError()[0])
            Sys_SaveSetError("Part of the game could not be saved (see the console).");
        return false;
    }
    return Sys_SaveClose(f, true);
}

/*
==============
SV_ReadServerFile

==============
*/
void SV_ReadServerFile(void)
{
    FILE * f;
    char name[MAX_OSPATH], string[128];
    char comment[32];
    char mapcmd[MAX_TOKEN_CHARS];

    Com_DPrintf("SV_ReadServerFile()\n");

    f = Sys_SaveOpen("server.ssv", "rb");
    if (!f)
        Com_Error(ERR_DROP, "%s", Sys_SaveLastError()); // [PS2_QUAKE]: id printed and carried on
    // read the comment field
    SV_SaveReadBlock(comment, sizeof(comment), f);

    // read the mapcmd
    SV_SaveReadBlock(mapcmd, sizeof(mapcmd), f);

    // read all CVAR_LATCH cvars
    // these will be things like coop, skill, deathmatch, etc
    while (1)
    {
        if (!fread(name, 1, sizeof(name), f))
            break;
        SV_SaveReadBlock(string, sizeof(string), f);
        Com_DPrintf("Set %s = %s\n", name, string);
        Cvar_ForceSet(name, string);
    }

    if (!Sys_SaveClose(f, true))
        Com_Error(ERR_DROP, "%s", Sys_SaveLastError());

    // start a new game fresh with new cvars
    SV_InitGame();

    strcpy(svs.mapcmd, mapcmd);

    // read game state
    f = Sys_SaveOpen("game.ssv", "rb");
    if (!f)
        Com_Error(ERR_DROP, "%s", Sys_SaveLastError());
    ge->ReadGame(f);
    Sys_SaveClose(f, true);
}

/*
==============
SV_AutosaveFailed

[PS2_QUAKE]: Every failure goes to the console, but only the first one in a session
interrupts the game with a message box: without a memory card, every level change
would otherwise bring one up. Saving successfully re-arms it.
==============
*/
static void SV_AutosaveFailed(void)
{
    Com_Printf("Autosave failed: %s\n", Sys_SaveLastError());

    if (!sv_autosave_warned)
    {
        sv_autosave_warned = true;
        M_Popup("Autosave failed", va("%s\n\nIf you die, you will go back to your last save instead.", Sys_SaveLastError()));
    }
}

//=========================================================

/*
==================
SV_DemoMap_f

Puts the server in demo mode on a specific map/cinematic
==================
*/
void SV_DemoMap_f(void)
{
    SV_Map(true, Cmd_Argv(1), false);
}

/*
==================
SV_GameMap_f

Saves the state of the map just being exited and goes to a new map.

If the initial character of the map string is '*', the next map is
in a new unit, so the current savegame directory is cleared of
map files.

Example:

*inter.cin+jail

Clears the archived maps, plays the inter.cin cinematic, then
goes to map jail.bsp.
==================
*/
void SV_GameMap_f(void)
{
    const char * map;
    int i;
    client_t * cl;
    qboolean * savedInuse;

    if (Cmd_Argc() != 2)
    {
        Com_Printf("USAGE: gamemap <map>\n");
        return;
    }

    Com_DPrintf("SV_GameMap(%s)\n", Cmd_Argv(1));

    // check for clearing the current savegame
    map = Cmd_Argv(1);
    if (map[0] == '*')
    {
        // wipe all the *.sav files
        SV_WipeSavegame("current");
    }
    else
    { // save the map just exited
        if (sv.state == ss_game)
        {
            // clear all the client inuse flags before saving so that
            // when the level is re-entered, the clients will spawn
            // at spawn points instead of occupying body shells
            savedInuse = Z_Malloc(maxclients->value * sizeof(qboolean));
            for (i = 0, cl = svs.clients; i < maxclients->value; i++, cl++)
            {
                savedInuse[i] = cl->edict->inuse;
                cl->edict->inuse = false;
            }

            if (!SV_WriteLevelFile()) // [PS2_QUAKE]
                SV_AutosaveFailed();

            // we must restore these for clients to transfer over correctly
            for (i = 0, cl = svs.clients; i < maxclients->value; i++, cl++)
            {
                cl->edict->inuse = savedInuse[i];
            }

            Z_Free(savedInuse);
        }
    }

    // start up the next map
    SV_Map(false, Cmd_Argv(1), false);

    // archive server state
    strncpy(svs.mapcmd, Cmd_Argv(1), sizeof(svs.mapcmd) - 1);

    // copy off the level to the autosave slot
    if (!dedicated->value)
    {
        if (!SV_WriteServerFile(true) || !SV_CopySaveGame("current", "save0")) // [PS2_QUAKE]
            SV_AutosaveFailed();
    }
}

/*
==================
SV_Map_f

Goes directly to a given map without any savegame archiving.
For development work
==================
*/
void SV_Map_f(void)
{
    const char * map;
    char expanded[MAX_QPATH];

    // if not a pcx, demo, or cinematic, check to make sure the level exists
    map = Cmd_Argv(1);
    if (!strstr(map, "."))
    {
        Com_sprintf(expanded, sizeof(expanded), "maps/%s.bsp", map);
        if (FS_LoadFile(expanded, NULL) == -1)
        {
            Com_Printf("Can't find %s\n", expanded);
            return;
        }
    }

    sv.state = ss_dead; // don't save current level when changing
    SV_WipeSavegame("current");
    SV_GameMap_f();
}

/*
=====================================================================

  SAVEGAMES

=====================================================================
*/

/*
==============
SV_Loadgame_f

==============
*/
void SV_Loadgame_f(void)
{
    const char * dir;

    if (Cmd_Argc() != 2)
    {
        Com_Printf("USAGE: loadgame <directory>\n");
        return;
    }

    Com_Printf("Loading game...\n");

    dir = Cmd_Argv(1);
    if (!SV_SaveNameIsValid(dir))
    {
        Com_Printf("Bad savedir.\n");
        return;
    }

    // [PS2_QUAKE]: The slot is read and checked in full before the working set - and the
    // game in progress with it - is touched, so a missing, damaged or incompatible save
    // leaves everything as it was. That also covers id's "No such savegame" check.
    if (!SV_CopySaveGame(dir, "current"))
    {
        Com_Printf("Couldn't load %s: %s\n", dir, Sys_SaveLastError());
        M_Popup("Load failed", Sys_SaveLastError());
        return;
    }

    sv_reading_save = true; // [PS2_QUAKE]

    SV_ReadServerFile();

    // go to the map
    sv.state = ss_dead; // don't save current level when changing
    SV_Map(false, svs.mapcmd, true);

    sv_reading_save = false; // [PS2_QUAKE]
}

/*
==============
SV_Savegame_f

==============
*/
void SV_Savegame_f(void)
{
    const char * dir;
    qboolean ok;

    if (sv.state != ss_game)
    {
        Com_Printf("You must be in a game to save.\n");
        return;
    }

    if (Cmd_Argc() != 2)
    {
        Com_Printf("USAGE: savegame <directory>\n");
        return;
    }

    if (Cvar_VariableValue("deathmatch"))
    {
        Com_Printf("Can't savegame in a deathmatch\n");
        return;
    }

    if (!strcmp(Cmd_Argv(1), "current"))
    {
        Com_Printf("Can't save to 'current'\n");
        return;
    }

    if (maxclients->value == 1 && svs.clients[0].edict->client->ps.stats[STAT_HEALTH] <= 0)
    {
        Com_Printf("\nCan't savegame while dead!\n");
        return;
    }

    dir = Cmd_Argv(1);
    if (!SV_SaveNameIsValid(dir))
    {
        Com_Printf("Bad savedir.\n");
        return;
    }

    Com_Printf("Saving game...\n");

    // [PS2_QUAKE]: Writing to a memory card takes a moment and must not be interrupted.
    // The frame is blocked meanwhile, so put the warning up before starting.
    if (Sys_SaveTargetIsCard())
        SCR_SetBusyNotice("Saving...\n\nDo not remove the memory card\nor switch off the console.");

    // archive current level, including all client edicts.
    // when the level is reloaded, they will be shells awaiting
    // a connecting client
    ok = SV_WriteLevelFile();

    // save server state
    ok = ok && SV_WriteServerFile(false);

    // copy it off
    ok = ok && SV_CopySaveGame("current", dir);

    SCR_SetBusyNotice(NULL);

    // [PS2_QUAKE]: Say so when it didn't work, rather than printing "Done." regardless.
    if (!ok)
    {
        Com_Printf("Save failed: %s\n", Sys_SaveLastError());
        M_Popup("Save failed", Sys_SaveLastError());
        return;
    }

    sv_autosave_warned = false;
    Com_Printf("Done.\n");
    SCR_CenterPrint(Sys_SaveTargetIsCard() ? "Game saved to the memory card." : "Game saved.");
}

//===============================================================

/*
==================
SV_Kick_f

Kick a user off of the server
==================
*/
void SV_Kick_f(void)
{
    if (!svs.initialized)
    {
        Com_Printf("No server running.\n");
        return;
    }

    if (Cmd_Argc() != 2)
    {
        Com_Printf("Usage: kick <userid>\n");
        return;
    }

    if (!SV_SetPlayer())
        return;

    SV_BroadcastPrintf(PRINT_HIGH, "%s was kicked\n", sv_client->name);
    // print directly, because the dropped client won't get the
    // SV_BroadcastPrintf message
    SV_ClientPrintf(sv_client, PRINT_HIGH, "You were kicked from the game\n");
    SV_DropClient(sv_client);
    sv_client->lastmessage = svs.realtime; // min case there is a funny zombie
}

/*
================
SV_Status_f
================
*/
void SV_Status_f(void)
{
    int i, j, l;
    client_t * cl;
    char * s;
    int ping;
    if (!svs.clients)
    {
        Com_Printf("No server running.\n");
        return;
    }
    Com_Printf("map              : %s\n", sv.name);

    Com_Printf("num score ping name            lastmsg address               qport \n");
    Com_Printf("--- ----- ---- --------------- ------- --------------------- ------\n");
    for (i = 0, cl = svs.clients; i < maxclients->value; i++, cl++)
    {
        if (!cl->state)
            continue;
        Com_Printf("%3i ", i);
        Com_Printf("%5i ", cl->edict->client->ps.stats[STAT_FRAGS]);

        if (cl->state == cs_connected)
            Com_Printf("CNCT ");
        else if (cl->state == cs_zombie)
            Com_Printf("ZMBI ");
        else
        {
            ping = cl->ping < 9999 ? cl->ping : 9999;
            Com_Printf("%4i ", ping);
        }

        Com_Printf("%s", cl->name);
        l = 16 - strlen(cl->name);
        for (j = 0; j < l; j++)
            Com_Printf(" ");

        Com_Printf("%7i ", svs.realtime - cl->lastmessage);

        s = NET_AdrToString(cl->netchan.remote_address);
        Com_Printf("%s", s);
        l = 22 - strlen(s);
        for (j = 0; j < l; j++)
            Com_Printf(" ");

        Com_Printf("%5i", cl->netchan.qport);

        Com_Printf("\n");
    }
    Com_Printf("\n");
}

/*
==================
SV_ConSay_f
==================
*/
void SV_ConSay_f(void)
{
    client_t * client;
    int j;
    const char * p;
    size_t len, room;
    char text[1024];

    if (Cmd_Argc() < 2)
        return;

    strcpy(text, "console: ");
    p = Cmd_Args();
    len = strlen(p);

    // Strip leading/trailing quotes:
    if (*p == '"' && len >= 2)
    {
        ++p;      // skip the first
        len -= 2; // and the last (")
    }

    // [PS2_QUAKE]: the args can be as long as text itself (MAX_STRING_CHARS),
    // so the copy is clamped to the room left after the prefix.
    room = sizeof(text) - strlen(text) - 1;
    strncat(text, p, len < room ? len : room);

    for (j = 0, client = svs.clients; j < maxclients->value; j++, client++)
    {
        if (client->state != cs_spawned)
            continue;

        SV_ClientPrintf(client, PRINT_CHAT, "%s\n", text);
    }
}

/*
==================
SV_Heartbeat_f
==================
*/
void SV_Heartbeat_f(void)
{
    svs.last_heartbeat = -9999999;
}

/*
===========
SV_Serverinfo_f

  Examine or change the serverinfo string
===========
*/
void SV_Serverinfo_f(void)
{
    Com_Printf("Server info settings:\n");
    Info_Print(Cvar_Serverinfo());
}

/*
===========
SV_DumpUser_f

Examine all a users info strings
===========
*/
void SV_DumpUser_f(void)
{
    if (Cmd_Argc() != 2)
    {
        Com_Printf("Usage: info <userid>\n");
        return;
    }

    if (!SV_SetPlayer())
        return;

    Com_Printf("userinfo\n");
    Com_Printf("--------\n");
    Info_Print(sv_client->userinfo);
}

/*
==============
SV_ServerRecord_f

Begins server demo recording.  Every entity and every message will be
recorded, but no playerinfo will be stored.  Primarily for demo merging.
==============
*/
void SV_ServerRecord_f(void)
{
    char name[MAX_OSPATH];
    char buf_data[32768];
    sizebuf_t buf;
    int len;
    int i;

    if (Cmd_Argc() != 2)
    {
        Com_Printf("serverrecord <demoname>\n");
        return;
    }

    if (svs.demofile)
    {
        Com_Printf("Already recording.\n");
        return;
    }

    if (sv.state != ss_game)
    {
        Com_Printf("You must be in a level to record.\n");
        return;
    }

    //
    // open the demo file
    //
    Com_sprintf(name, sizeof(name), "%s/demos/%s.dm2", FS_Gamedir(), Cmd_Argv(1));

    Com_Printf("recording to %s.\n", name);
    FS_CreatePath(name);
    svs.demofile = fopen(name, "wb");
    if (!svs.demofile)
    {
        Com_Printf("ERROR: couldn't open.\n");
        return;
    }

    // setup a buffer to catch all multicasts
    SZ_Init(&svs.demo_multicast, svs.demo_multicast_buf, sizeof(svs.demo_multicast_buf));

    //
    // write a single giant fake message with all the startup info
    //
    SZ_Init(&buf, buf_data, sizeof(buf_data));

    //
    // serverdata needs to go over for all types of servers
    // to make sure the protocol is right, and to set the gamedir
    //
    // send the serverdata
    MSG_WriteByte(&buf, svc_serverdata);
    MSG_WriteLong(&buf, PROTOCOL_VERSION);
    MSG_WriteLong(&buf, svs.spawncount);
    // 2 means server demo
    MSG_WriteByte(&buf, 2); // demos are always attract loops
    MSG_WriteString(&buf, Cvar_VariableString("gamedir"));
    MSG_WriteShort(&buf, -1);
    // send full levelname
    MSG_WriteString(&buf, sv.configstrings[CS_NAME]);

    for (i = 0; i < MAX_CONFIGSTRINGS; i++)
        if (sv.configstrings[i][0])
        {
            MSG_WriteByte(&buf, svc_configstring);
            MSG_WriteShort(&buf, i);
            MSG_WriteString(&buf, sv.configstrings[i]);
        }

    // write it to the demo file
    Com_DPrintf("signon message length: %i\n", buf.cursize);
    len = LittleLong(buf.cursize);
    fwrite(&len, 4, 1, svs.demofile);
    fwrite(buf.data, buf.cursize, 1, svs.demofile);

    // the rest of the demo file will be individual frames
}

/*
==============
SV_ServerStop_f

Ends server demo recording
==============
*/
void SV_ServerStop_f(void)
{
    if (!svs.demofile)
    {
        Com_Printf("Not doing a serverrecord.\n");
        return;
    }
    fclose(svs.demofile);
    svs.demofile = NULL;
    Com_Printf("Recording completed.\n");
}

/*
===============
SV_KillServer_f

Kick everyone off, possibly in preparation for a new game

===============
*/
void SV_KillServer_f(void)
{
    if (!svs.initialized)
        return;
    SV_Shutdown("Server was killed.\n", false);
    NET_Config(false); // close network sockets
}

/*
===============
SV_ServerCommand_f

Let the game dll handle a command
===============
*/
void SV_ServerCommand_f(void)
{
    if (!ge)
    {
        Com_Printf("No game loaded.\n");
        return;
    }

    ge->ServerCommand();
}

//===========================================================

/*
==================
SV_InitOperatorCommands
==================
*/
void SV_InitOperatorCommands(void)
{
    Cmd_AddCommand("heartbeat", SV_Heartbeat_f);
    Cmd_AddCommand("kick", SV_Kick_f);
    Cmd_AddCommand("status", SV_Status_f);
    Cmd_AddCommand("serverinfo", SV_Serverinfo_f);
    Cmd_AddCommand("dumpuser", SV_DumpUser_f);

    Cmd_AddCommand("map", SV_Map_f);
    Cmd_AddCommand("demomap", SV_DemoMap_f);
    Cmd_AddCommand("gamemap", SV_GameMap_f);
    Cmd_AddCommand("setmaster", SV_SetMaster_f);

    if (dedicated->value)
    {
        Cmd_AddCommand("say", SV_ConSay_f);
    }

    Cmd_AddCommand("serverrecord", SV_ServerRecord_f);
    Cmd_AddCommand("serverstop", SV_ServerStop_f);
    Cmd_AddCommand("save", SV_Savegame_f);
    Cmd_AddCommand("load", SV_Loadgame_f);
    Cmd_AddCommand("killserver", SV_KillServer_f);
    Cmd_AddCommand("sv", SV_ServerCommand_f);
}
