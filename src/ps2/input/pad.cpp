/* ================================================================================================
 * File: pad.cpp
 * Brief: GamePad implementation - the libpad connection lifecycle, per-frame
 *        polling, analog stick normalisation and the vibration motors. See pad.h
 *        for the interface.
 *
 * This source code is released under the GNU GPL v2 license.
 * ================================================================================================ */

#include "ps2/input/pad.h"
#include "ps2/system/iop_boot.h"
#include "ps2/common.h"

namespace ps2::input {
namespace {

constexpr int kPadSlot = 0; // First (only) slot - no multitap

// Motor mapping for padSetActAlign: byte 0 of the padSetActDirect array drives
// actuator 0 (the small motor), byte 1 actuator 1 (the large one); 0xFF leaves
// the other four bytes unmapped.
constexpr char kMotorAlign[6] = {
    0, 1, static_cast<char>(0xFF), static_cast<char>(0xFF), static_cast<char>(0xFF), static_cast<char>(0xFF)
};

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
    // The pad driver lives on the IOP: the boot code brings up the serial and pad modules
    // and libpad (or finds them running), then this opens both connectors. Connection and
    // analog mode switching are asynchronous and polled per frame in Update().
    if (!ps2::sys::StartPadDriver())
    {
        Com_Printf("WARNING: no pad driver - gamepad disabled!\n");
        return false;
    }

    bool anyOpen = false;
    for (int port = 0; port < kPortCount; ++port)
    {
        m_portOpen[port] = (padPortOpen(port, kPadSlot, m_dmaArea[port]) != 0);
        if (m_portOpen[port])
        {
            anyOpen = true;
        }
        else
        {
            Com_Printf("WARNING: padPortOpen failed on port %d!\n", port + 1);
        }
    }
    if (!anyOpen)
    {
        Com_Printf("WARNING: no pad port opened - gamepad disabled!\n");
        return false;
    }

    m_status = Status::Disconnected;
    return true;
}

void GamePad::Shutdown()
{
    if (m_status != Status::Unavailable)
    {
        SetMotors(false, 0);
        for (int port = 0; port < kPortCount; ++port)
        {
            if (m_portOpen[port])
            {
                padPortClose(port, kPadSlot);
                m_portOpen[port] = false;
            }
        }
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

    if (m_status == Status::Disconnected)
    {
        // No pad in use: take the first port, in connector order, that has one.
        int found = -1;
        for (int port = 0; port < kPortCount && found < 0; ++port)
        {
            if (m_portOpen[port] && Connected(padGetState(port, kPadSlot)))
            {
                found = port;
            }
        }
        if (found < 0)
        {
            return;
        }
        m_port = found;
    }

    const int state = padGetState(m_port, kPadSlot);

    switch (m_status)
    {
    case Status::Disconnected:
        if (Connected(state)) // Found above; the state is from the same poll.
        {
            // Pads boot in digital mode; request DualShock (analog sticks) if the
            // device supports it, locked so the MODE button can't switch it back.
            if (padInfoMode(m_port, kPadSlot, PAD_MODETABLE, -1) > 0)
            {
                padSetMainMode(m_port, kPadSlot, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
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
        if (state == PAD_STATE_DISCONN)
        {
            // Unplugged mid-switch. Back to watching both ports, or a pad left in the
            // other one would never be picked up.
            m_status = Status::Disconnected;
            break;
        }
        const unsigned char request = padGetReqState(m_port, kPadSlot);
        if ((request == PAD_RSTAT_COMPLETE || request == PAD_RSTAT_FAILED) && Connected(state))
        {
            Com_DPrintf("Gamepad connected on port %d.\n", m_port + 1);

            // Only a pad with an analog mode can have motors, and they take their
            // mapping after the mode is set - the mode switch resets it.
            if (padInfoAct(m_port, kPadSlot, -1, 0) > 0 &&
                padSetActAlign(m_port, kPadSlot, kMotorAlign) == 1)
            {
                m_status = Status::SettingMotors;
            }
            else
            {
                m_status = Status::Ready;
            }
        }
        break;
    }

    case Status::SettingMotors:
        if (state == PAD_STATE_DISCONN)
        {
            m_status = Status::Disconnected;
        }
        else if (Connected(state)) // No longer executing the request.
        {
            m_hasMotors = (padGetReqState(m_port, kPadSlot) == PAD_RSTAT_COMPLETE);
            m_sentSmall = kMotorsUnknown;
            m_status = Status::Ready;
            Com_DPrintf("Gamepad vibration motors %s.\n", m_hasMotors ? "enabled" : "failed to enable");
        }
        break;

    case Status::Ready:
        if (state == PAD_STATE_DISCONN)
        {
            m_buttons = 0; // The input layer releases anything still held.
            m_hasMotors = false;
            m_status = Status::Disconnected;
        }
        else if (padRead(m_port, kPadSlot, &m_data) != 0)
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

void GamePad::SetMotors(bool smallOn, u8 largeSpeed)
{
    if (m_status != Status::Ready || !m_hasMotors)
    {
        return;
    }

    const u8 small = smallOn ? 1 : 0;
    if (small == m_sentSmall && largeSpeed == m_sentLarge)
    {
        return;
    }

    // Laid out per kMotorAlign. Not a request like padSetActAlign: padman just
    // stores the values and sends them to the motors with every poll from then on.
    const char direct[6] = { static_cast<char>(small), static_cast<char>(largeSpeed), 0, 0, 0, 0 };
    padSetActDirect(m_port, kPadSlot, direct);

    m_sentSmall = small;
    m_sentLarge = largeSpeed;
}

} // namespace ps2::input
