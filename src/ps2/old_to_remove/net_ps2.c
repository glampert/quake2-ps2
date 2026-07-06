
/* ================================================================================================
 * -*- C -*-
 * File: net_ps2.c
 * Author: Guilherme R. Lampert
 * Created on: 27/10/15
 * Brief: Implementation of Quake2 NET_ functions (network module) for the PS2.
 *
 * This source code is released under the GNU GPL v2 license.
 * Check the accompanying LICENSE file for details.
 * ================================================================================================ */

#include "common/q_common.h"
#include "ps2/defs_ps2.h"

//
// Local loopback (localhost) buffers, adapted from net_wins.c
//

enum
{
    MAX_LOOPBACK = 4
};

typedef struct
{
    int datalen;
    byte data[MAX_MSGLEN];
} loopmsg_t;

typedef struct
{
    int get;
    int send;
    loopmsg_t msgs[MAX_LOOPBACK];
} loopback_t;

// Alignment is not strictly necessary here, but might boost memcpy/memset perf.
static loopback_t loopbacks[2] PS2_ALIGN(16);

//=============================================================================
//
// Misc NET helpers:
//
//=============================================================================

/*
===================
NET_CompareAdr
===================
*/
qboolean NET_CompareAdr(netadr_t a, netadr_t b)
{
    if (a.type == NA_LOOPBACK)
    {
        return true;
    }

    if (a.type != b.type)
    {
        return false;
    }

    if (a.type == NA_IP)
    {
        if (a.ip[0] == b.ip[0] &&
            a.ip[1] == b.ip[1] &&
            a.ip[2] == b.ip[2] &&
            a.ip[3] == b.ip[3] &&
            a.port == b.port)
        {
            return true;
        }
        return false;
    }

    if (a.type == NA_IPX)
    {
        if ((memcmp(a.ipx, b.ipx, 10) == 0) && a.port == b.port)
        {
            return true;
        }
        return false;
    }
}

/*
===================
NET_CompareBaseAdr
===================
*/
qboolean NET_CompareBaseAdr(netadr_t a, netadr_t b)
{
    // Compares without the port num.

    if (a.type == NA_LOOPBACK)
    {
        return true;
    }

    if (a.type != b.type)
    {
        return false;
    }

    if (a.type == NA_IP)
    {
        if (a.ip[0] == b.ip[0] &&
            a.ip[1] == b.ip[1] &&
            a.ip[2] == b.ip[2] &&
            a.ip[3] == b.ip[3])
        {
            return true;
        }
        return false;
    }

    if (a.type == NA_IPX)
    {
        if ((memcmp(a.ipx, b.ipx, 10) == 0))
        {
            return true;
        }
        return false;
    }
}

/*
===================
NET_AdrToString
===================
*/
char * NET_AdrToString(netadr_t addr)
{
// FIXME temp but fine, since we are currently localhost bound
#define ntohs(x) (x)

    static char s[64];

    if (addr.type == NA_LOOPBACK)
    {
        Com_sprintf(s, sizeof(s), "loopback");
    }
    else if (addr.type == NA_IP)
    {
        Com_sprintf(s, sizeof(s), "%i.%i.%i.%i:%i",
            addr.ip[0], addr.ip[1], addr.ip[2], addr.ip[3],
            ntohs(addr.port));
    }
    else
    {
        Com_sprintf(s, sizeof(s), "%02x%02x%02x%02x:%02x%02x%02x%02x%02x%02x:%i",
            addr.ipx[0], addr.ipx[1], addr.ipx[2], addr.ipx[3], addr.ipx[4],
            addr.ipx[5], addr.ipx[6], addr.ipx[7], addr.ipx[8], addr.ipx[9],
            ntohs(addr.port));
    }

    return s;

#undef ntohs
}

/*
=============
NET_StringToAdr
=============
*/
qboolean NET_StringToAdr(const char * s, netadr_t * addr)
{
    if (strcmp(s, "localhost") == 0 || strcmp(s, "loopback") == 0)
    {
        memset(addr, 0, sizeof(*addr));
        addr->type = NA_LOOPBACK;
        return true;
    }

    // TODO Only localhost is currently supported!
    memset(addr, 0, sizeof(*addr));
    return false;
}

/*
=============
NET_IsLocalAddress
=============
*/
qboolean NET_IsLocalAddress(netadr_t adr)
{
    return adr.type == NA_LOOPBACK;
}

//=============================================================================
//
// Loopback buffers for the local player:
//
//=============================================================================

/*
====================
NET_GetLoopPacket
====================
*/
static qboolean NET_GetLoopPacket(netsrc_t sock, netadr_t * net_from, sizebuf_t * net_message)
{
    // Same code from net_wins.c, Win32 Quake2.

    loopback_t * loop = &loopbacks[sock];

    if (loop->send - loop->get > MAX_LOOPBACK)
    {
        loop->get = loop->send - MAX_LOOPBACK;
    }

    if (loop->get >= loop->send)
    {
        return false;
    }

    int i = loop->get & (MAX_LOOPBACK - 1);
    loop->get++;

    memcpy(net_message->data, loop->msgs[i].data, loop->msgs[i].datalen);
    net_message->cursize = loop->msgs[i].datalen;

    memset(net_from, 0, sizeof(*net_from));
    net_from->type = NA_LOOPBACK;

    return true;
}

/*
====================
NET_GetPacket
====================
*/
qboolean NET_GetPacket(netsrc_t sock, netadr_t * net_from, sizebuf_t * net_message)
{
    if (NET_GetLoopPacket(sock, net_from, net_message))
    {
        return true;
    }

    // TODO Only localhost is currently supported!
    return false;
}

/*
====================
NET_SendLoopPacket
====================
*/
static void NET_SendLoopPacket(netsrc_t sock, int length, const void * data, netadr_t to)
{
    loopback_t * loop = &loopbacks[sock ^ 1];

    int i = loop->send & (MAX_LOOPBACK - 1);
    loop->send++;

    memcpy(loop->msgs[i].data, data, length);
    loop->msgs[i].datalen = length;
}

/*
====================
NET_SendPacket
====================
*/
void NET_SendPacket(netsrc_t sock, int length, const void * data, netadr_t to)
{
    if (to.type == NA_LOOPBACK)
    {
        NET_SendLoopPacket(sock, length, data, to);
        return;
    }

    // TODO Only localhost is currently supported!
}

//=============================================================================
//
// NET startup/shutdown:
//
//=============================================================================

/*
====================
NET_Config
====================
*/
void NET_Config(qboolean multiplayer)
{
    Com_DPrintf("---- NET_Config ----\n");

    // A single player game will only use the loopback code
    if (multiplayer)
    {
        Com_Error(ERR_DROP, "Quake2 multiplayer currently unsupported!\n");
    }
}

/*
====================
NET_Sleep
====================
*/
void NET_Sleep(int msec)
{
    // TODO This is where Quake calls select() with msec timeout.
    (void)msec;
}

/*
====================
NET_Init
====================
*/
void NET_Init(void)
{
    Com_DPrintf("---- NET_Init ----\n");
}

/*
====================
NET_Shutdown
====================
*/
void NET_Shutdown(void)
{
    Com_DPrintf("---- NET_Shutdown ----\n");
}
