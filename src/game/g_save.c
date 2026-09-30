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

#include "g_local.h"

/*
==============================================================================

[PS2_QUAKE]: SAVE FORMAT

id's saves stored function pointers as offsets from InitGame and mmove_t pointers as
offsets from a dummy mmove_t, and stamped the file with __DATE__, so a save only loaded
into the very build that wrote it. Pointers to game functions and animations are now
stored as a hash of the pointee's name instead, looked up in tables generated from the
game's objects at build time (src/tools/scripts/gen_save_tables.py), and every save
file starts with a header naming the format it was written in.

The header's fingerprint (G_SaveFingerprint) covers the size of every struct written
out whole, the field tables below and the item list, so most changes that would make
an old save unreadable reject it cleanly instead of loading garbage. It can't see a
change that keeps the sizes - two members of the same type swapped, say. For those,
bump SAVE_FORMAT_VERSION by hand.

==============================================================================
*/

#define SAVE_FORMAT_VERSION 1

#define SAVE_MAGIC(a, b, c, d) ((a) | ((b) << 8) | ((c) << 16) | ((d) << 24))
#define SAVE_MAGIC_GAME        SAVE_MAGIC('Q', '2', 'G', 'M') // game.ssv
#define SAVE_MAGIC_LEVEL       SAVE_MAGIC('Q', '2', 'L', 'V') // <map>.sav

typedef struct
{
    int magic;
    int version;
    unsigned int fingerprint;
} saveheader_t;

// The generated tables (build/<config>/gen/g_save_tables.c), each sorted by hash.
typedef struct
{
    unsigned int hash;
    const void * ptr;
} g_save_ptr_t;

extern const g_save_ptr_t g_saveFuncs[];
extern const int g_saveNumFuncs;
extern const g_save_ptr_t g_saveMmoves[];
extern const int g_saveNumMmoves;

// The same entries sorted by address, built once by G_InitSaveTables.
extern g_save_ptr_t g_saveFuncsByPtr[];
extern g_save_ptr_t g_saveMmovesByPtr[];

// Set by anything that fails while a save is being written; see WriteGame/WriteLevel.
static qboolean save_write_failed;

static int G_SavePtrCompareHash(const void * a, const void * b)
{
    const unsigned int ha = ((const g_save_ptr_t *)a)->hash;
    const unsigned int hb = ((const g_save_ptr_t *)b)->hash;
    return (ha > hb) - (ha < hb);
}

static int G_SavePtrCompareAddress(const void * a, const void * b)
{
    const size_t pa = (size_t)((const g_save_ptr_t *)a)->ptr;
    const size_t pb = (size_t)((const g_save_ptr_t *)b)->ptr;
    return (pa > pb) - (pa < pb);
}

static void G_InitSaveTables(void)
{
    static qboolean built = false;

    if (built)
        return;

    memcpy(g_saveFuncsByPtr, g_saveFuncs, g_saveNumFuncs * sizeof(g_save_ptr_t));
    qsort(g_saveFuncsByPtr, g_saveNumFuncs, sizeof(g_save_ptr_t), G_SavePtrCompareAddress);

    memcpy(g_saveMmovesByPtr, g_saveMmoves, g_saveNumMmoves * sizeof(g_save_ptr_t));
    qsort(g_saveMmovesByPtr, g_saveNumMmoves, sizeof(g_save_ptr_t), G_SavePtrCompareAddress);

    built = true;
}

// Name hash of a game function or mmove_t, or 0 if the address is not one the tables know.
static unsigned int G_SaveHashForPtr(const g_save_ptr_t * byPtr, int count, const void * ptr)
{
    g_save_ptr_t key;
    const g_save_ptr_t * found;

    key.hash = 0;
    key.ptr = ptr;
    found = bsearch(&key, byPtr, count, sizeof(g_save_ptr_t), G_SavePtrCompareAddress);
    return found ? found->hash : 0;
}

// The reverse of G_SaveHashForPtr: NULL if no game function or mmove_t has that name hash.
static const void * G_SavePtrForHash(const g_save_ptr_t * byHash, int count, unsigned int hash)
{
    g_save_ptr_t key;
    const g_save_ptr_t * found;

    key.hash = hash;
    key.ptr = NULL;
    found = bsearch(&key, byHash, count, sizeof(g_save_ptr_t), G_SavePtrCompareHash);
    return found ? found->ptr : NULL;
}

static unsigned int G_HashBytes(unsigned int h, const void * data, size_t size)
{
    const byte * p = data;
    while (size--)
    {
        h ^= *p++;
        h *= 16777619u; // FNV-1a
    }
    return h;
}

static unsigned int G_HashFieldTable(unsigned int h, const field_t * field)
{
    for (; field->name; field++)
    {
        h = G_HashBytes(h, field->name, strlen(field->name));
        h = G_HashBytes(h, &field->ofs, sizeof(field->ofs));
        h = G_HashBytes(h, &field->type, sizeof(field->type));
        h = G_HashBytes(h, &field->flags, sizeof(field->flags));
    }
    return h;
}

static void G_SaveWrite(FILE * f, const void * data, int size)
{
    if (size > 0 && fwrite(data, size, 1, f) != 1)
        save_write_failed = true;
}

static void G_SaveRead(FILE * f, void * data, int size)
{
    if (size > 0 && fread(data, size, 1, f) != 1)
        gi.error("Savegame is truncated or corrupt");
}

static void G_WriteSaveHeader(FILE * f, int magic)
{
    saveheader_t header;

    header.magic = magic;
    header.version = SAVE_FORMAT_VERSION;
    header.fingerprint = G_SaveFingerprint();
    G_SaveWrite(f, &header, sizeof(header));
}

static void G_ReadSaveHeader(FILE * f, int magic)
{
    saveheader_t header;

    G_SaveRead(f, &header, sizeof(header));
    if (header.magic != magic)
        gi.error("Not a Quake II savegame");
    if (header.version != SAVE_FORMAT_VERSION || header.fingerprint != G_SaveFingerprint())
        gi.error("Savegame is from an incompatible version of the game");
}

field_t fields[] = {
    { "classname", FOFS(classname), F_LSTRING },
    { "model", FOFS(model), F_LSTRING },
    { "spawnflags", FOFS(spawnflags), F_INT },
    { "speed", FOFS(speed), F_FLOAT },
    { "accel", FOFS(accel), F_FLOAT },
    { "decel", FOFS(decel), F_FLOAT },
    { "target", FOFS(target), F_LSTRING },
    { "targetname", FOFS(targetname), F_LSTRING },
    { "pathtarget", FOFS(pathtarget), F_LSTRING },
    { "deathtarget", FOFS(deathtarget), F_LSTRING },
    { "killtarget", FOFS(killtarget), F_LSTRING },
    { "combattarget", FOFS(combattarget), F_LSTRING },
    { "message", FOFS(message), F_LSTRING },
    { "team", FOFS(team), F_LSTRING },
    { "wait", FOFS(wait), F_FLOAT },
    { "delay", FOFS(delay), F_FLOAT },
    { "random", FOFS(random), F_FLOAT },
    { "move_origin", FOFS(move_origin), F_VECTOR },
    { "move_angles", FOFS(move_angles), F_VECTOR },
    { "style", FOFS(style), F_INT },
    { "count", FOFS(count), F_INT },
    { "health", FOFS(health), F_INT },
    { "sounds", FOFS(sounds), F_INT },
    { "light", 0, F_IGNORE },
    { "dmg", FOFS(dmg), F_INT },
    { "mass", FOFS(mass), F_INT },
    { "volume", FOFS(volume), F_FLOAT },
    { "attenuation", FOFS(attenuation), F_FLOAT },
    { "map", FOFS(map), F_LSTRING },
    { "origin", FOFS(s.origin), F_VECTOR },
    { "angles", FOFS(s.angles), F_VECTOR },
    { "angle", FOFS(s.angles), F_ANGLEHACK },

    { "goalentity", FOFS(goalentity), F_EDICT, FFL_NOSPAWN },
    { "movetarget", FOFS(movetarget), F_EDICT, FFL_NOSPAWN },
    { "enemy", FOFS(enemy), F_EDICT, FFL_NOSPAWN },
    { "oldenemy", FOFS(oldenemy), F_EDICT, FFL_NOSPAWN },
    { "activator", FOFS(activator), F_EDICT, FFL_NOSPAWN },
    { "groundentity", FOFS(groundentity), F_EDICT, FFL_NOSPAWN },
    { "teamchain", FOFS(teamchain), F_EDICT, FFL_NOSPAWN },
    { "teammaster", FOFS(teammaster), F_EDICT, FFL_NOSPAWN },
    { "owner", FOFS(owner), F_EDICT, FFL_NOSPAWN },
    { "mynoise", FOFS(mynoise), F_EDICT, FFL_NOSPAWN },
    { "mynoise2", FOFS(mynoise2), F_EDICT, FFL_NOSPAWN },
    { "target_ent", FOFS(target_ent), F_EDICT, FFL_NOSPAWN },
    { "chain", FOFS(chain), F_EDICT, FFL_NOSPAWN },

    { "prethink", FOFS(prethink), F_FUNCTION, FFL_NOSPAWN },
    { "think", FOFS(think), F_FUNCTION, FFL_NOSPAWN },
    { "blocked", FOFS(blocked), F_FUNCTION, FFL_NOSPAWN },
    { "touch", FOFS(touch), F_FUNCTION, FFL_NOSPAWN },
    { "use", FOFS(use), F_FUNCTION, FFL_NOSPAWN },
    { "pain", FOFS(pain), F_FUNCTION, FFL_NOSPAWN },
    { "die", FOFS(die), F_FUNCTION, FFL_NOSPAWN },

    { "stand", FOFS(monsterinfo.stand), F_FUNCTION, FFL_NOSPAWN },
    { "idle", FOFS(monsterinfo.idle), F_FUNCTION, FFL_NOSPAWN },
    { "search", FOFS(monsterinfo.search), F_FUNCTION, FFL_NOSPAWN },
    { "walk", FOFS(monsterinfo.walk), F_FUNCTION, FFL_NOSPAWN },
    { "run", FOFS(monsterinfo.run), F_FUNCTION, FFL_NOSPAWN },
    { "dodge", FOFS(monsterinfo.dodge), F_FUNCTION, FFL_NOSPAWN },
    { "attack", FOFS(monsterinfo.attack), F_FUNCTION, FFL_NOSPAWN },
    { "melee", FOFS(monsterinfo.melee), F_FUNCTION, FFL_NOSPAWN },
    { "sight", FOFS(monsterinfo.sight), F_FUNCTION, FFL_NOSPAWN },
    { "checkattack", FOFS(monsterinfo.checkattack), F_FUNCTION, FFL_NOSPAWN },
    { "currentmove", FOFS(monsterinfo.currentmove), F_MMOVE, FFL_NOSPAWN },

    { "endfunc", FOFS(moveinfo.endfunc), F_FUNCTION, FFL_NOSPAWN },

    // temp spawn vars -- only valid when the spawn function is called
    { "lip", STOFS(lip), F_INT, FFL_SPAWNTEMP },
    { "distance", STOFS(distance), F_INT, FFL_SPAWNTEMP },
    { "height", STOFS(height), F_INT, FFL_SPAWNTEMP },
    { "noise", STOFS(noise), F_LSTRING, FFL_SPAWNTEMP },
    { "pausetime", STOFS(pausetime), F_FLOAT, FFL_SPAWNTEMP },
    { "item", STOFS(item), F_LSTRING, FFL_SPAWNTEMP },

    //need for item field in edict struct, FFL_SPAWNTEMP item will be skipped on saves
    { "item", FOFS(item), F_ITEM },

    { "gravity", STOFS(gravity), F_LSTRING, FFL_SPAWNTEMP },
    { "sky", STOFS(sky), F_LSTRING, FFL_SPAWNTEMP },
    { "skyrotate", STOFS(skyrotate), F_FLOAT, FFL_SPAWNTEMP },
    { "skyaxis", STOFS(skyaxis), F_VECTOR, FFL_SPAWNTEMP },
    { "minyaw", STOFS(minyaw), F_FLOAT, FFL_SPAWNTEMP },
    { "maxyaw", STOFS(maxyaw), F_FLOAT, FFL_SPAWNTEMP },
    { "minpitch", STOFS(minpitch), F_FLOAT, FFL_SPAWNTEMP },
    { "maxpitch", STOFS(maxpitch), F_FLOAT, FFL_SPAWNTEMP },
    { "nextmap", STOFS(nextmap), F_LSTRING, FFL_SPAWNTEMP },

    { 0, 0, 0, 0 }
};

field_t levelfields[] =
{
  { "changemap", LLOFS(changemap), F_LSTRING },

  { "sight_client", LLOFS(sight_client), F_EDICT },
  { "sight_entity", LLOFS(sight_entity), F_EDICT },
  { "sound_entity", LLOFS(sound_entity), F_EDICT },
  { "sound2_entity", LLOFS(sound2_entity), F_EDICT },

  { NULL, 0, F_INT }
};

field_t clientfields[] =
{
  { "pers.weapon", CLOFS(pers.weapon), F_ITEM },
  { "pers.lastweapon", CLOFS(pers.lastweapon), F_ITEM },
  { "newweapon", CLOFS(newweapon), F_ITEM },

  { NULL, 0, F_INT }
};

/*
============
G_SaveFingerprint

[PS2_QUAKE]: See the SAVE FORMAT notes at the top of the file.
============
*/
unsigned int G_SaveFingerprint(void)
{
    static unsigned int fingerprint = 0;

    if (!fingerprint)
    {
        const int version = SAVE_FORMAT_VERSION;
        const int sizes[] = {
            sizeof(edict_t), sizeof(gclient_t), sizeof(game_locals_t), sizeof(level_locals_t)
        };
        const gitem_t * item;
        unsigned int h = 2166136261u; // FNV-1a basis

        h = G_HashBytes(h, &version, sizeof(version));
        h = G_HashBytes(h, sizes, sizeof(sizes));
        h = G_HashFieldTable(h, fields);
        h = G_HashFieldTable(h, levelfields);
        h = G_HashFieldTable(h, clientfields);

        // F_ITEM fields and the inventory are indexes into itemlist.
        for (item = itemlist + 1; item->classname; item++)
            h = G_HashBytes(h, item->classname, strlen(item->classname) + 1);

        fingerprint = h ? h : 1;
    }
    return fingerprint;
}

/*
============
InitGame

This will be called when the dll is first loaded, which
only happens when a new game is started or a save game
is loaded.
============
*/
void InitGame(void)
{
    gi.dprintf("==== InitGame ====\n");

    gun_x = gi.cvar("gun_x", "0", 0);
    gun_y = gi.cvar("gun_y", "0", 0);
    gun_z = gi.cvar("gun_z", "0", 0);

    //FIXME: sv_ prefix is wrong for these
    sv_rollspeed = gi.cvar("sv_rollspeed", "200", 0);
    sv_rollangle = gi.cvar("sv_rollangle", "2", 0);
    sv_maxvelocity = gi.cvar("sv_maxvelocity", "2000", 0);
    sv_gravity = gi.cvar("sv_gravity", "800", 0);

    // noset vars
    dedicated = gi.cvar("dedicated", "0", CVAR_NOSET);

    // latched vars
    sv_cheats = gi.cvar("cheats", "0", CVAR_SERVERINFO | CVAR_LATCH);
    gi.cvar("gamename", GAMEVERSION, CVAR_SERVERINFO | CVAR_LATCH);
    gi.cvar("gamedate", __DATE__, CVAR_SERVERINFO | CVAR_LATCH);

    maxclients = gi.cvar("maxclients", "4", CVAR_SERVERINFO | CVAR_LATCH);
    maxspectators = gi.cvar("maxspectators", "4", CVAR_SERVERINFO);
    deathmatch = gi.cvar("deathmatch", "0", CVAR_LATCH);
    coop = gi.cvar("coop", "0", CVAR_LATCH);
    skill = gi.cvar("skill", "1", CVAR_LATCH);
    maxentities = gi.cvar("maxentities", "1024", CVAR_LATCH);

    // change anytime vars
    dmflags = gi.cvar("dmflags", "0", CVAR_SERVERINFO);
    fraglimit = gi.cvar("fraglimit", "0", CVAR_SERVERINFO);
    timelimit = gi.cvar("timelimit", "0", CVAR_SERVERINFO);
    password = gi.cvar("password", "", CVAR_USERINFO);
    spectator_password = gi.cvar("spectator_password", "", CVAR_USERINFO);
    filterban = gi.cvar("filterban", "1", 0);

    g_select_empty = gi.cvar("g_select_empty", "0", CVAR_ARCHIVE);

    run_pitch = gi.cvar("run_pitch", "0.002", 0);
    run_roll = gi.cvar("run_roll", "0.005", 0);
    bob_up = gi.cvar("bob_up", "0.005", 0);
    bob_pitch = gi.cvar("bob_pitch", "0.002", 0);
    bob_roll = gi.cvar("bob_roll", "0.002", 0);

    // flood control
    flood_msgs = gi.cvar("flood_msgs", "4", 0);
    flood_persecond = gi.cvar("flood_persecond", "4", 0);
    flood_waitdelay = gi.cvar("flood_waitdelay", "10", 0);

    // dm map list
    sv_maplist = gi.cvar("sv_maplist", "", 0);

    // items
    InitItems();

    G_InitSaveTables(); // [PS2_QUAKE]

    // [PS2_QUAKE]: Why not memset in the fist place???
    memset(game.helpmessage1, 0, sizeof(game.helpmessage1));
    memset(game.helpmessage2, 0, sizeof(game.helpmessage2));
    //Com_sprintf(game.helpmessage1, sizeof(game.helpmessage1), "");
    //Com_sprintf(game.helpmessage2, sizeof(game.helpmessage2), "");

    // initialize all entities for this game
    game.maxentities = maxentities->value;
    g_edicts = gi.TagMalloc(game.maxentities * sizeof(g_edicts[0]), TAG_GAME);
    globals.edicts = g_edicts;
    globals.max_edicts = game.maxentities;

    // initialize all clients for this game
    game.maxclients = maxclients->value;
    game.clients = gi.TagMalloc(game.maxclients * sizeof(game.clients[0]), TAG_GAME);
    globals.num_edicts = game.maxclients + 1;
}

//=========================================================

void WriteField1(FILE * f, field_t * field, byte * base)
{
    void * p;
    int len;
    int index;

    if (field->flags & FFL_SPAWNTEMP)
        return;

    p = (void *)(base + field->ofs);
    switch (field->type)
    {
    case F_INT:
    case F_FLOAT:
    case F_ANGLEHACK:
    case F_VECTOR:
    case F_IGNORE:
        break;

    case F_LSTRING:
    case F_GSTRING:
        if (*(char **)p)
            len = strlen(*(char **)p) + 1;
        else
            len = 0;
        *(int *)p = len;
        break;
    case F_EDICT:
        if (*(edict_t **)p == NULL)
            index = -1;
        else
            index = *(edict_t **)p - g_edicts;
        *(int *)p = index;
        break;
    case F_CLIENT:
        if (*(gclient_t **)p == NULL)
            index = -1;
        else
            index = *(gclient_t **)p - game.clients;
        *(int *)p = index;
        break;
    case F_ITEM:
        if (*(edict_t **)p == NULL)
            index = -1;
        else
            index = *(gitem_t **)p - itemlist;
        *(int *)p = index;
        break;

    // [PS2_QUAKE]: by name hash, see the SAVE FORMAT notes at the top
    case F_FUNCTION:
        if (*(byte **)p == NULL)
            index = 0;
        else if (!(index = G_SaveHashForPtr(g_saveFuncsByPtr, g_saveNumFuncs, *(void **)p)))
        {
            gi.dprintf("WriteField: %s points at %p, which is not a game function the save tables know\n",
                       field->name, *(void **)p);
            save_write_failed = true;
        }
        *(int *)p = index;
        break;

    case F_MMOVE:
        if (*(byte **)p == NULL)
            index = 0;
        else if (!(index = G_SaveHashForPtr(g_saveMmovesByPtr, g_saveNumMmoves, *(void **)p)))
        {
            gi.dprintf("WriteField: %s points at %p, which is not a mmove_t the save tables know\n",
                       field->name, *(void **)p);
            save_write_failed = true;
        }
        *(int *)p = index;
        break;

    default:
        gi.error("WriteEdict: unknown field type");
    }
}

void WriteField2(FILE * f, field_t * field, byte * base)
{
    int len;
    void * p;

    if (field->flags & FFL_SPAWNTEMP)
        return;

    p = (void *)(base + field->ofs);
    switch (field->type)
    {
    case F_LSTRING:
        if (*(char **)p)
        {
            len = strlen(*(char **)p) + 1;
            G_SaveWrite(f, *(char **)p, len);
        }
        break;
    }
}

void ReadField(FILE * f, field_t * field, byte * base)
{
    void * p;
    int len;
    int index;

    if (field->flags & FFL_SPAWNTEMP)
        return;

    p = (void *)(base + field->ofs);
    switch (field->type)
    {
    case F_INT:
    case F_FLOAT:
    case F_ANGLEHACK:
    case F_VECTOR:
    case F_IGNORE:
        break;

    case F_LSTRING:
        len = *(int *)p;
        if (!len)
            *(char **)p = NULL;
        else
        {
            *(char **)p = gi.TagMalloc(len, TAG_LEVEL);
            G_SaveRead(f, *(char **)p, len);
        }
        break;
    case F_EDICT:
        index = *(int *)p;
        if (index == -1)
            *(edict_t **)p = NULL;
        else
            *(edict_t **)p = &g_edicts[index];
        break;
    case F_CLIENT:
        index = *(int *)p;
        if (index == -1)
            *(gclient_t **)p = NULL;
        else
            *(gclient_t **)p = &game.clients[index];
        break;
    case F_ITEM:
        index = *(int *)p;
        if (index == -1)
            *(gitem_t **)p = NULL;
        else
            *(gitem_t **)p = &itemlist[index];
        break;

    // [PS2_QUAKE]: by name hash, see the SAVE FORMAT notes at the top
    case F_FUNCTION:
        index = *(int *)p;
        if (index == 0)
            *(const void **)p = NULL;
        else if (!(*(const void **)p = G_SavePtrForHash(g_saveFuncs, g_saveNumFuncs, index)))
            gi.error("Savegame refers to a game function this version doesn't have (%s %08x)", field->name, index);
        break;

    case F_MMOVE:
        index = *(int *)p;
        if (index == 0)
            *(const void **)p = NULL;
        else if (!(*(const void **)p = G_SavePtrForHash(g_saveMmoves, g_saveNumMmoves, index)))
            gi.error("Savegame refers to an animation this version doesn't have (%s %08x)", field->name, index);
        break;

    default:
        gi.error("ReadEdict: unknown field type");
    }
}

//=========================================================

/*
==============
WriteClient

All pointer variables (except function pointers) must be handled specially.
==============
*/
void WriteClient(FILE * f, gclient_t * client)
{
    field_t * field;
    gclient_t temp;

    // all of the ints, floats, and vectors stay as they are
    temp = *client;

    // change the pointers to lengths or indexes
    for (field = clientfields; field->name; field++)
    {
        WriteField1(f, field, (byte *)&temp);
    }

    // write the block
    G_SaveWrite(f, &temp, sizeof(temp));

    // now write any allocated data following the edict
    for (field = clientfields; field->name; field++)
    {
        WriteField2(f, field, (byte *)client);
    }
}

/*
==============
ReadClient

All pointer variables (except function pointers) must be handled specially.
==============
*/
void ReadClient(FILE * f, gclient_t * client)
{
    field_t * field;

    G_SaveRead(f, client, sizeof(*client));

    for (field = clientfields; field->name; field++)
    {
        ReadField(f, field, (byte *)client);
    }
}

/*
============
WriteGame

This will be called whenever the game goes to a new level,
and when the user explicitly saves the game.

Game information include cross level data, like multi level
triggers, help computer info, and all client states.

A single player death will automatically restore from the
last save position.

[PS2_QUAKE]: Writes to a stream the server opened and returns whether all of it was
written; a failure is reported by the server, the game carries on regardless.
============
*/
qboolean WriteGame(FILE * f, qboolean autosave)
{
    int i;

    if (!autosave)
        SaveClientData();

    save_write_failed = false;
    G_WriteSaveHeader(f, SAVE_MAGIC_GAME);

    game.autosaved = autosave;
    G_SaveWrite(f, &game, sizeof(game));
    game.autosaved = false;

    for (i = 0; i < game.maxclients; i++)
        WriteClient(f, &game.clients[i]);

    return !save_write_failed;
}

void ReadGame(FILE * f)
{
    int i;

    G_ReadSaveHeader(f, SAVE_MAGIC_GAME);

    gi.FreeTags(TAG_GAME);

    g_edicts = gi.TagMalloc(game.maxentities * sizeof(g_edicts[0]), TAG_GAME);
    globals.edicts = g_edicts;

    G_SaveRead(f, &game, sizeof(game));
    game.clients = gi.TagMalloc(game.maxclients * sizeof(game.clients[0]), TAG_GAME);
    for (i = 0; i < game.maxclients; i++)
        ReadClient(f, &game.clients[i]);
}

//==========================================================

/*
==============
WriteEdict

All pointer variables (except function pointers) must be handled specially.
==============
*/
void WriteEdict(FILE * f, edict_t * ent)
{
    field_t * field;
    edict_t temp;

    // all of the ints, floats, and vectors stay as they are
    temp = *ent;

    // change the pointers to lengths or indexes
    for (field = fields; field->name; field++)
    {
        WriteField1(f, field, (byte *)&temp);
    }

    // write the block
    G_SaveWrite(f, &temp, sizeof(temp));

    // now write any allocated data following the edict
    for (field = fields; field->name; field++)
    {
        WriteField2(f, field, (byte *)ent);
    }
}

/*
==============
WriteLevelLocals

All pointer variables (except function pointers) must be handled specially.
==============
*/
void WriteLevelLocals(FILE * f)
{
    field_t * field;
    level_locals_t temp;

    // all of the ints, floats, and vectors stay as they are
    temp = level;

    // change the pointers to lengths or indexes
    for (field = levelfields; field->name; field++)
    {
        WriteField1(f, field, (byte *)&temp);
    }

    // write the block
    G_SaveWrite(f, &temp, sizeof(temp));

    // now write any allocated data following the edict
    for (field = levelfields; field->name; field++)
    {
        WriteField2(f, field, (byte *)&level);
    }
}

/*
==============
ReadEdict

All pointer variables (except function pointers) must be handled specially.
==============
*/
void ReadEdict(FILE * f, edict_t * ent)
{
    field_t * field;

    G_SaveRead(f, ent, sizeof(*ent));

    for (field = fields; field->name; field++)
    {
        ReadField(f, field, (byte *)ent);
    }
}

/*
==============
ReadLevelLocals

All pointer variables (except function pointers) must be handled specially.
==============
*/
void ReadLevelLocals(FILE * f)
{
    field_t * field;

    G_SaveRead(f, &level, sizeof(level));

    for (field = levelfields; field->name; field++)
    {
        ReadField(f, field, (byte *)&level);
    }
}

/*
=================
WriteLevel

[PS2_QUAKE]: Writes to a stream the server opened and returns whether all of it was
written. The header replaces id's edict size and InitGame address checks.
=================
*/
qboolean WriteLevel(FILE * f)
{
    int i;
    edict_t * ent;

    save_write_failed = false;
    G_WriteSaveHeader(f, SAVE_MAGIC_LEVEL);

    // write out level_locals_t
    WriteLevelLocals(f);

    // write out all the entities
    for (i = 0; i < globals.num_edicts; i++)
    {
        ent = &g_edicts[i];
        if (!ent->inuse)
            continue;
        G_SaveWrite(f, &i, sizeof(i));
        WriteEdict(f, ent);
    }
    i = -1;
    G_SaveWrite(f, &i, sizeof(i));

    return !save_write_failed;
}

/*
=================
ReadLevel

SpawnEntities will allready have been called on the
level the same way it was when the level was saved.

That is necessary to get the baselines
set up identically.

The server will have cleared all of the world links before
calling ReadLevel.

No clients are connected yet.
=================
*/
void ReadLevel(FILE * f)
{
    int entnum;
    int i;
    edict_t * ent;

    // [PS2_QUAKE]: replaces id's edict size and InitGame address checks
    G_ReadSaveHeader(f, SAVE_MAGIC_LEVEL);

    // free any dynamic memory allocated by loading the level
    // base state
    gi.FreeTags(TAG_LEVEL);

    // wipe all the entities
    memset(g_edicts, 0, game.maxentities * sizeof(g_edicts[0]));
    globals.num_edicts = maxclients->value + 1;

    // load the level locals
    ReadLevelLocals(f);

    // load all the entities
    while (1)
    {
        G_SaveRead(f, &entnum, sizeof(entnum));
        if (entnum == -1)
            break;
        if (entnum < 0 || entnum >= game.maxentities) // [PS2_QUAKE]
            gi.error("ReadLevel: bad entity number %i", entnum);
        if (entnum >= globals.num_edicts)
            globals.num_edicts = entnum + 1;

        ent = &g_edicts[entnum];
        ReadEdict(f, ent);

        // let the server rebuild world links for this ent
        memset(&ent->area, 0, sizeof(ent->area));
        gi.linkentity(ent);
    }

    // mark all clients as unconnected
    for (i = 0; i < maxclients->value; i++)
    {
        ent = &g_edicts[i + 1];
        ent->client = game.clients + i;
        ent->client->pers.connected = false;
    }

    // do any load time things at this point
    for (i = 0; i < globals.num_edicts; i++)
    {
        ent = &g_edicts[i];

        if (!ent->inuse)
            continue;

        // fire any cross-level triggers
        if (ent->classname)
        {
            if (strcmp(ent->classname, "target_crosslevel_target") == 0)
                ent->nextthink = level.time + ent->delay;
        }
    }
}
