/* ================================================================================================
 * File: pad.cpp
 * Brief: GamePad implementation - the libpad connection lifecycle, per-frame
 *        polling and analog stick normalisation. See pad.h for the interface.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/input/pad.h"
#include "ps2/common.h"

#include <sifrpc.h>
#include <loadfile.h>

namespace ps2::input {
namespace {

constexpr int kPadPort = 0; // Controller connector 1
constexpr int kPadSlot = 0; // First (only) slot - no multitap

// Maps a raw 0..255 stick byte (centre ~128) to a normalised -1..+1 axis.
inline float NormalizeAxis(unsigned char raw)
{
    const float v = (static_cast<float>(raw) - 128.0f) / 127.0f;
    if (v < -1.0f) { return -1.0f; }
    if (v >  1.0f) { return  1.0f; }
    return v;
}

} // namespace

bool GamePad::Connected(int state)
{
    return (state == PAD_STATE_STABLE) || (state == PAD_STATE_FINDCTP1);
}

bool GamePad::Init()
{
    // The pad driver lives on the IOP: bring up SIF RPC and load the ROM-resident
    // serial + pad modules, then open connector 1. Connection and analog mode
    // switching are asynchronous and polled per frame in Update().
    SifInitRpc(0);
    if (SifLoadModule("rom0:SIO2MAN", 0, nullptr) < 0 ||
        SifLoadModule("rom0:PADMAN",  0, nullptr) < 0)
    {
        Com_Printf("WARNING: failed to load pad IOP modules - gamepad disabled!\n");
        return false;
    }
    if (padInit(0) != 1)
    {
        Com_Printf("WARNING: padInit failed - gamepad disabled!\n");
        return false;
    }
    if (padPortOpen(kPadPort, kPadSlot, m_dmaArea) == 0)
    {
        Com_Printf("WARNING: padPortOpen failed - gamepad disabled!\n");
        return false;
    }

    m_status = Status::Disconnected;
    return true;
}

void GamePad::Shutdown()
{
    if (m_status != Status::Unavailable)
    {
        padPortClose(kPadPort, kPadSlot);
        padEnd();
        m_status = Status::Unavailable;
    }
}

void GamePad::Update()
{
    m_analogValid = false;

    if (m_status == Status::Unavailable)
    {
        return;
    }

    const int state = padGetState(kPadPort, kPadSlot);

    switch (m_status)
    {
    case Status::Disconnected:
        if (Connected(state))
        {
            // Pads boot in digital mode; request DualShock (analog sticks) if the
            // device supports it, locked so the MODE button can't switch it back.
            if (padInfoMode(kPadPort, kPadSlot, PAD_MODETABLE, -1) > 0)
            {
                padSetMainMode(kPadPort, kPadSlot, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
                m_status = Status::SettingMode;
            }
            else
            {
                m_status = Status::Ready; // Digital-only pad: buttons still work.
            }
        }
        break;

    case Status::SettingMode:
    {
        const unsigned char request = padGetReqState(kPadPort, kPadSlot);
        if ((request == PAD_RSTAT_COMPLETE || request == PAD_RSTAT_FAILED) && Connected(state))
        {
            Com_DPrintf("Gamepad connected.\n");
            m_status = Status::Ready;
        }
        break;
    }

    case Status::Ready:
        if (state == PAD_STATE_DISCONN)
        {
            m_buttons = 0; // The input layer releases anything still held.
            m_status = Status::Disconnected;
        }
        else if (padRead(kPadPort, kPadSlot, &m_data) != 0)
        {
            m_buttons = static_cast<u16>(m_data.btns ^ 0xFFFFu); // Active-low.

            const int padType = m_data.mode >> 4;
            m_analogValid = (padType == PAD_TYPE_ANALOG) || (padType == PAD_TYPE_DUALSHOCK);
        }
        // A failed read keeps the previous button state (transient RPC hiccup).
        break;

    case Status::Unavailable:
        break;
    }
}

float GamePad::LeftStickX()  const { return NormalizeAxis(m_data.ljoy_h); }
float GamePad::LeftStickY()  const { return NormalizeAxis(m_data.ljoy_v); }
float GamePad::RightStickX() const { return NormalizeAxis(m_data.rjoy_h); }
float GamePad::RightStickY() const { return NormalizeAxis(m_data.rjoy_v); }

} // namespace ps2::input
