/* ================================================================================================
 * File: net_ps2.cpp
 * Brief: NET_* seam for the PS2. Implements the in-process loopback that Quake II
 *        uses for a local (single-player / listen-server) game. Real UDP/IPX is
 *        not wired yet, so remote sends are dropped - multiplayer is a later phase.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/qcommon.h"
#include <cstring>

namespace {

constexpr int MAX_LOOPBACK = 4;

struct LoopMsg
{
    byte data[MAX_MSGLEN];
    int  datalen;
};

struct LoopBack
{
    LoopMsg msgs[MAX_LOOPBACK];
    int get;
    int send;
};

// One queue per socket direction (indexed by netsrc_t: NS_CLIENT / NS_SERVER).
static LoopBack s_loopbacks[2];

bool NET_GetLoopPacket(netsrc_t sock, netadr_t * out_from, sizebuf_t * out_msg)
{
    LoopBack * loop = &s_loopbacks[sock];

    if (loop->send - loop->get > MAX_LOOPBACK)
    {
        loop->get = loop->send - MAX_LOOPBACK;
    }
    if (loop->get >= loop->send)
    {
        return false;
    }

    const int i = loop->get & (MAX_LOOPBACK - 1);
    loop->get++;

    std::memcpy(out_msg->data, loop->msgs[i].data, static_cast<std::size_t>(loop->msgs[i].datalen));
    out_msg->cursize = loop->msgs[i].datalen;

    std::memset(out_from, 0, sizeof(*out_from));
    out_from->type = NA_LOOPBACK;
    return true;
}

void NET_SendLoopPacket(netsrc_t sock, int length, const void * data)
{
    LoopBack * loop = &s_loopbacks[sock ^ 1];

    const int i = loop->send & (MAX_LOOPBACK - 1);
    loop->send++;

    int n = length;
    if (n > MAX_MSGLEN) { n = MAX_MSGLEN; }
    std::memcpy(loop->msgs[i].data, data, static_cast<std::size_t>(n));
    loop->msgs[i].datalen = n;
}

} // namespace

extern "C" {

void NET_Init() { std::memset(s_loopbacks, 0, sizeof(s_loopbacks)); }
void NET_Shutdown() {}
void NET_Config(qboolean multiplayer) { (void)multiplayer; }
void NET_Sleep(int msec) { (void)msec; }

qboolean NET_GetPacket(netsrc_t sock, netadr_t * out_from, sizebuf_t * out_msg)
{
    return NET_GetLoopPacket(sock, out_from, out_msg) ? true : false;
}

void NET_SendPacket(netsrc_t sock, int length, const void * data, netadr_t to)
{
    if (to.type == NA_LOOPBACK)
    {
        NET_SendLoopPacket(sock, length, data);
        return;
    }
    // No real UDP/IPX backend yet: silently drop remote packets.
}

qboolean NET_CompareAdr(netadr_t a, netadr_t b)
{
    if (a.type != b.type) { return false; }
    if (a.type == NA_LOOPBACK) { return true; }
    if (a.type == NA_IP)
    {
        return (std::memcmp(a.ip, b.ip, sizeof(a.ip)) == 0 && a.port == b.port) ? true : false;
    }
    if (a.type == NA_IPX)
    {
        return (std::memcmp(a.ipx, b.ipx, sizeof(a.ipx)) == 0 && a.port == b.port) ? true : false;
    }
    return false;
}

qboolean NET_CompareBaseAdr(netadr_t a, netadr_t b)
{
    if (a.type != b.type) { return false; }
    if (a.type == NA_LOOPBACK) { return true; }
    if (a.type == NA_IP)  { return (std::memcmp(a.ip, b.ip, sizeof(a.ip)) == 0) ? true : false; }
    if (a.type == NA_IPX) { return (std::memcmp(a.ipx, b.ipx, sizeof(a.ipx)) == 0) ? true : false; }
    return false;
}

qboolean NET_IsLocalAddress(netadr_t adr)
{
    return (adr.type == NA_LOOPBACK) ? true : false;
}

char * NET_AdrToString(netadr_t addr)
{
    static char s[64];
    if (addr.type == NA_LOOPBACK)
    {
        Com_sprintf(s, sizeof(s), "loopback");
    }
    else if (addr.type == NA_IP)
    {
        Com_sprintf(s, sizeof(s), "%i.%i.%i.%i:%i",
                    addr.ip[0], addr.ip[1], addr.ip[2], addr.ip[3], addr.port);
    }
    else
    {
        Com_sprintf(s, sizeof(s), "(unsupported)");
    }
    return s;
}

qboolean NET_StringToAdr(const char * s, netadr_t * addr)
{
    std::memset(addr, 0, sizeof(*addr));
    if (std::strcmp(s, "localhost") == 0 || std::strcmp(s, "loopback") == 0)
    {
        addr->type = NA_LOOPBACK;
        return true;
    }
    // No name/number resolution without a real network stack.
    return false;
}

} // extern "C"
